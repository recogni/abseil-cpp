// Copyright 2020 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_STRINGS_INTERNAL_CORD_INTERNAL_H_
#define ABSL_STRINGS_INTERNAL_CORD_INTERNAL_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "absl/base/internal/invoke.h"
#include "absl/base/optimization.h"
#include "absl/container/internal/compressed_tuple.h"
#include "absl/meta/type_traits.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

extern std::atomic<bool> cord_ring_buffer_enabled;

inline void enable_cord_ring_buffer(bool enable) {
  cord_ring_buffer_enabled.store(enable, std::memory_order_relaxed);
}

enum Constants {
  // The inlined size to use with absl::InlinedVector.
  //
  // Note: The InlinedVectors in this file (and in cord.h) do not need to use
  // the same value for their inlined size. The fact that they do is historical.
  // It may be desirable for each to use a different inlined size optimized for
  // that InlinedVector's usage.
  //
  // TODO(jgm): Benchmark to see if there's a more optimal value than 47 for
  // the inlined vector size (47 exists for backward compatibility).
  kInlinedVectorSize = 47,

  // Prefer copying blocks of at most this size, otherwise reference count.
  kMaxBytesToCopy = 511
};

// Wraps std::atomic for reference counting.
class Refcount {
 public:
  constexpr Refcount() : count_{kRefIncrement} {}
  struct Immortal {};
  explicit constexpr Refcount(Immortal) : count_(kImmortalTag) {}

  // Increments the reference count. Imposes no memory ordering.
  inline void Increment() {
    count_.fetch_add(kRefIncrement, std::memory_order_relaxed);
  }

  // Asserts that the current refcount is greater than 0. If the refcount is
  // greater than 1, decrements the reference count.
  //
  // Returns false if there are no references outstanding; true otherwise.
  // Inserts barriers to ensure that state written before this method returns
  // false will be visible to a thread that just observed this method returning
  // false.
  inline bool Decrement() {
    int32_t refcount = count_.load(std::memory_order_acquire);
    assert(refcount > 0 || refcount & kImmortalTag);
    return refcount != kRefIncrement &&
           count_.fetch_sub(kRefIncrement, std::memory_order_acq_rel) !=
               kRefIncrement;
  }

  // Same as Decrement but expect that refcount is greater than 1.
  inline bool DecrementExpectHighRefcount() {
    int32_t refcount =
        count_.fetch_sub(kRefIncrement, std::memory_order_acq_rel);
    assert(refcount > 0 || refcount & kImmortalTag);
    return refcount != kRefIncrement;
  }

  // Returns the current reference count using acquire semantics.
  inline int32_t Get() const {
    return count_.load(std::memory_order_acquire) >> kImmortalShift;
  }

  // Returns whether the atomic integer is 1.
  // If the reference count is used in the conventional way, a
  // reference count of 1 implies that the current thread owns the
  // reference and no other thread shares it.
  // This call performs the test for a reference count of one, and
  // performs the memory barrier needed for the owning thread
  // to act on the object, knowing that it has exclusive access to the
  // object.
  inline bool IsOne() {
    return count_.load(std::memory_order_acquire) == kRefIncrement;
  }

  bool IsImmortal() const {
    return (count_.load(std::memory_order_relaxed) & kImmortalTag) != 0;
  }

 private:
  // We reserve the bottom bit to tag a reference count as immortal.
  // By making it `1` we ensure that we never reach `0` when adding/subtracting
  // `2`, thus it never looks as if it should be destroyed.
  // These are used for the StringConstant constructor where we do not increase
  // the refcount at construction time (due to constinit requirements) but we
  // will still decrease it at destruction time to avoid branching on Unref.
  enum {
    kImmortalShift = 1,
    kRefIncrement = 1 << kImmortalShift,
    kImmortalTag = kRefIncrement - 1
  };

  std::atomic<int32_t> count_;
};

// The overhead of a vtable is too much for Cord, so we roll our own subclasses
// using only a single byte to differentiate classes from each other - the "tag"
// byte.  Define the subclasses first so we can provide downcasting helper
// functions in the base class.

struct CordRepConcat;
struct CordRepExternal;
struct CordRepFlat;
struct CordRepSubstring;

// Various representations that we allow
enum CordRepKind {
  CONCAT        = 0,
  EXTERNAL      = 1,
  SUBSTRING     = 2,
  RING          = 3,

  // We have different tags for different sized flat arrays,
  // starting with FLAT, and limited to MAX_FLAT_TAG. The 224 value is based on
  // the current 'size to tag' encoding of 8 / 32 bytes. If a new tag is needed
  // in the future, then 'FLAT' and 'MAX_FLAT_TAG' should be adjusted as well
  // as the Tag <---> Size logic so that FLAT stil represents the minimum flat
  // allocation size. (32 bytes as of now).
  FLAT = 4,
  MAX_FLAT_TAG = 224,
};

struct CordRep {
  CordRep() = default;
  constexpr CordRep(Refcount::Immortal immortal, size_t l)
      : length(l), refcount(immortal), tag(EXTERNAL), data{} {}

  // The following three fields have to be less than 32 bytes since
  // that is the smallest supported flat node size.
  size_t length;
  Refcount refcount;
  // If tag < FLAT, it represents CordRepKind and indicates the type of node.
  // Otherwise, the node type is CordRepFlat and the tag is the encoded size.
  uint8_t tag;
  char data[1];  // Starting point for flat array: MUST BE LAST FIELD of CordRep

  inline CordRepConcat* concat();
  inline const CordRepConcat* concat() const;
  inline CordRepSubstring* substring();
  inline const CordRepSubstring* substring() const;
  inline CordRepExternal* external();
  inline const CordRepExternal* external() const;
  inline CordRepFlat* flat();
  inline const CordRepFlat* flat() const;

  // --------------------------------------------------------------------
  // Memory management

  // This internal routine is called from the cold path of Unref below. Keeping
  // it in a separate routine allows good inlining of Unref into many profitable
  // call sites. However, the call to this function can be highly disruptive to
  // the register pressure in those callers. To minimize the cost to callers, we
  // use a special LLVM calling convention that preserves most registers. This
  // allows the call to this routine in cold paths to not disrupt the caller's
  // register pressure. This calling convention is not available on all
  // platforms; we intentionally allow LLVM to ignore the attribute rather than
  // attempting to hardcode the list of supported platforms.
#if defined(__clang__) && !defined(__i386__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wattributes"
  __attribute__((preserve_most))
#pragma clang diagnostic pop
#endif
  static void Destroy(CordRep* rep);

  // Increments the reference count of `rep`.
  // Requires `rep` to be a non-null pointer value.
  static inline CordRep* Ref(CordRep* rep);

  // Decrements the reference count of `rep`. Destroys rep if count reaches
  // zero. Requires `rep` to be a non-null pointer value.
  static inline void Unref(CordRep* rep);
};

struct CordRepConcat : public CordRep {
  CordRep* left;
  CordRep* right;

  uint8_t depth() const { return static_cast<uint8_t>(data[0]); }
  void set_depth(uint8_t depth) { data[0] = static_cast<char>(depth); }
};

struct CordRepSubstring : public CordRep {
  size_t start;  // Starting offset of substring in child
  CordRep* child;
};

// Type for function pointer that will invoke the releaser function and also
// delete the `CordRepExternalImpl` corresponding to the passed in
// `CordRepExternal`.
using ExternalReleaserInvoker = void (*)(CordRepExternal*);

// External CordReps are allocated together with a type erased releaser. The
// releaser is stored in the memory directly following the CordRepExternal.
struct CordRepExternal : public CordRep {
  CordRepExternal() = default;
  explicit constexpr CordRepExternal(absl::string_view str)
      : CordRep(Refcount::Immortal{}, str.size()),
        base(str.data()),
        releaser_invoker(nullptr) {}

  const char* base;
  // Pointer to function that knows how to call and destroy the releaser.
  ExternalReleaserInvoker releaser_invoker;

  // Deletes (releases) the external rep.
  // Requires rep != nullptr and rep->tag == EXTERNAL
  static void Delete(CordRep* rep);
};

struct Rank1 {};
struct Rank0 : Rank1 {};

template <typename Releaser, typename = ::absl::base_internal::invoke_result_t<
                                 Releaser, absl::string_view>>
void InvokeReleaser(Rank0, Releaser&& releaser, absl::string_view data) {
  ::absl::base_internal::invoke(std::forward<Releaser>(releaser), data);
}

template <typename Releaser,
          typename = ::absl::base_internal::invoke_result_t<Releaser>>
void InvokeReleaser(Rank1, Releaser&& releaser, absl::string_view) {
  ::absl::base_internal::invoke(std::forward<Releaser>(releaser));
}

// We use CompressedTuple so that we can benefit from EBCO.
template <typename Releaser>
struct CordRepExternalImpl
    : public CordRepExternal,
      public ::absl::container_internal::CompressedTuple<Releaser> {
  // The extra int arg is so that we can avoid interfering with copy/move
  // constructors while still benefitting from perfect forwarding.
  template <typename T>
  CordRepExternalImpl(T&& releaser, int)
      : CordRepExternalImpl::CompressedTuple(std::forward<T>(releaser)) {
    this->releaser_invoker = &Release;
  }

  ~CordRepExternalImpl() {
    InvokeReleaser(Rank0{}, std::move(this->template get<0>()),
                   absl::string_view(base, length));
  }

  static void Release(CordRepExternal* rep) {
    delete static_cast<CordRepExternalImpl*>(rep);
  }
};

inline void CordRepExternal::Delete(CordRep* rep) {
  assert(rep != nullptr && rep->tag == EXTERNAL);
  auto* rep_external = static_cast<CordRepExternal*>(rep);
  assert(rep_external->releaser_invoker != nullptr);
  rep_external->releaser_invoker(rep_external);
}

template <typename Str>
struct ConstInitExternalStorage {
  ABSL_CONST_INIT static CordRepExternal value;
};

template <typename Str>
CordRepExternal ConstInitExternalStorage<Str>::value(Str::value);

enum {
  kMaxInline = 15,
  // Tag byte & kMaxInline means we are storing a pointer.
  kTreeFlag = 1 << 4,
  // Tag byte & kProfiledFlag means we are profiling the Cord.
  kProfiledFlag = 1 << 5
};

// If the data has length <= kMaxInline, we store it in `as_chars`, and
// store the size in `tagged_size`.
// Else we store it in a tree and store a pointer to that tree in
// `as_tree.rep` and store a tag in `tagged_size`.
struct AsTree {
  absl::cord_internal::CordRep* rep;
  char padding[kMaxInline + 1 - sizeof(absl::cord_internal::CordRep*) - 1];
  char tagged_size;
};

constexpr char GetOrNull(absl::string_view data, size_t pos) {
  return pos < data.size() ? data[pos] : '\0';
}

union InlineData {
  constexpr InlineData() : as_chars{} {}
  explicit constexpr InlineData(AsTree tree) : as_tree(tree) {}
  explicit constexpr InlineData(absl::string_view chars)
      : as_chars{GetOrNull(chars, 0),  GetOrNull(chars, 1),
                 GetOrNull(chars, 2),  GetOrNull(chars, 3),
                 GetOrNull(chars, 4),  GetOrNull(chars, 5),
                 GetOrNull(chars, 6),  GetOrNull(chars, 7),
                 GetOrNull(chars, 8),  GetOrNull(chars, 9),
                 GetOrNull(chars, 10), GetOrNull(chars, 11),
                 GetOrNull(chars, 12), GetOrNull(chars, 13),
                 GetOrNull(chars, 14), static_cast<char>(chars.size())} {}

  AsTree as_tree;
  char as_chars[kMaxInline + 1];
};
static_assert(sizeof(InlineData) == kMaxInline + 1, "");
static_assert(sizeof(AsTree) == sizeof(InlineData), "");
static_assert(offsetof(AsTree, tagged_size) == kMaxInline, "");

inline CordRepConcat* CordRep::concat() {
  assert(tag == CONCAT);
  return static_cast<CordRepConcat*>(this);
}

inline const CordRepConcat* CordRep::concat() const {
  assert(tag == CONCAT);
  return static_cast<const CordRepConcat*>(this);
}

inline CordRepSubstring* CordRep::substring() {
  assert(tag == SUBSTRING);
  return static_cast<CordRepSubstring*>(this);
}

inline const CordRepSubstring* CordRep::substring() const {
  assert(tag == SUBSTRING);
  return static_cast<const CordRepSubstring*>(this);
}

inline CordRepExternal* CordRep::external() {
  assert(tag == EXTERNAL);
  return static_cast<CordRepExternal*>(this);
}

inline const CordRepExternal* CordRep::external() const {
  assert(tag == EXTERNAL);
  return static_cast<const CordRepExternal*>(this);
}

inline CordRepFlat* CordRep::flat() {
  assert(tag >= FLAT && tag <= MAX_FLAT_TAG);
  return reinterpret_cast<CordRepFlat*>(this);
}

inline const CordRepFlat* CordRep::flat() const {
  assert(tag >= FLAT && tag <= MAX_FLAT_TAG);
  return reinterpret_cast<const CordRepFlat*>(this);
}

inline CordRep* CordRep::Ref(CordRep* rep) {
  assert(rep != nullptr);
  rep->refcount.Increment();
  return rep;
}

inline void CordRep::Unref(CordRep* rep) {
  assert(rep != nullptr);
  // Expect refcount to be 0. Avoiding the cost of an atomic decrement should
  // typically outweigh the cost of an extra branch checking for ref == 1.
  if (ABSL_PREDICT_FALSE(!rep->refcount.DecrementExpectHighRefcount())) {
    Destroy(rep);
  }
}

}  // namespace cord_internal

ABSL_NAMESPACE_END
}  // namespace absl
#endif  // ABSL_STRINGS_INTERNAL_CORD_INTERNAL_H_
