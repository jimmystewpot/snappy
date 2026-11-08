// Copyright 2005 Google Inc. All Rights Reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "snappy.h"

#include <atomic>

#include "snappy-internal.h"
#include "snappy-isadetection.h"
#include "snappy-sinksource.h"

#if SNAPPY_IS_X86_64
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#elif SNAPPY_IS_AARCH64
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif (defined(__linux__) || defined(__ANDROID__)) && defined(__has_include)
#if __has_include(<sys/auxv.h>)
#define SNAPPY_HAVE_SYS_AUXV 1
#include <sys/auxv.h>
#endif
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif

#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
#ifndef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE 31
#endif
#if !defined(SNAPPY_HAVE_BMI2)
// __BMI2__ is defined by GCC and Clang. Visual Studio doesn't target BMI2
// specifically, but it does define __AVX2__ when AVX2 support is available.
// Fortunately, AVX2 was introduced in Haswell, just like BMI2.
//
// BMI2 is not defined as a subset of AVX2 (unlike SSSE3 and AVX above). So,
// GCC and Clang can build code with AVX2 enabled but BMI2 disabled, in which
// case issuing BMI2 instructions results in a compiler error.
#if defined(__BMI2__) || (defined(_MSC_VER) && defined(__AVX2__))
#define SNAPPY_HAVE_BMI2 1
#else
#define SNAPPY_HAVE_BMI2 0
#endif
#endif  // !defined(SNAPPY_HAVE_BMI2)

#if SNAPPY_IS_X86_64
// Please do not replace with <x86intrin.h> or with headers that assume more
// advanced SSE versions without checking with all the OWNERS.
#include <immintrin.h>
#endif

#if SNAPPY_IS_AARCH64
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if !defined(_MSC_VER) || defined(__clang__)
SNAPPY_TARGET_ARM_CRC32
#include <arm_acle.h>
SNAPPY_UNTARGET_REGION
#endif
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snappy {

namespace internal {

static std::atomic<uint32_t> g_cached_isa_mask{kIsaUninitialized};

uint32_t DetectSupportedArchitecturesSlow() noexcept {
  uint32_t val = kIsaDefault;
#if SNAPPY_IS_X86_64
  uint32_t max_basic_leaf = 0;
  uint32_t leaf1_ecx = 0;
  uint32_t leaf7_ebx = 0;
  uint64_t xcr0 = 0;

#if defined(_MSC_VER)
  int cpu_info[4] = {0};
  __cpuid(cpu_info, 0);
  max_basic_leaf = static_cast<uint32_t>(cpu_info[0]);
  if (max_basic_leaf >= 1) {
    __cpuidex(cpu_info, 1, 0);
    leaf1_ecx = static_cast<uint32_t>(cpu_info[2]);
    if ((leaf1_ecx & ((1u << 27) | (1u << 28))) == ((1u << 27) | (1u << 28))) {
      xcr0 = _xgetbv(0);
    }
  }
  if (max_basic_leaf >= 7) {
    __cpuidex(cpu_info, 7, 0);
    leaf7_ebx = static_cast<uint32_t>(cpu_info[1]);
  }
#else   // GCC / Clang
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
  __cpuid_count(0, 0, eax, ebx, ecx, edx);
  max_basic_leaf = eax;
  if (max_basic_leaf >= 1) {
    __cpuid_count(1, 0, eax, ebx, ecx, edx);
    leaf1_ecx = ecx;
    if ((leaf1_ecx & ((1u << 27) | (1u << 28))) == ((1u << 27) | (1u << 28))) {
      uint32_t xcr0_eax = 0, xcr0_edx = 0;
      asm(".byte 0x0f, 0x01, 0xd0" : "=a"(xcr0_eax), "=d"(xcr0_edx) : "c"(0));
      xcr0 = (static_cast<uint64_t>(xcr0_edx) << 32) | xcr0_eax;
    }
  }
  if (max_basic_leaf >= 7) {
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    leaf7_ebx = ebx;
  }
#endif  // _MSC_VER

  val = EvaluateX86CpuidBits(max_basic_leaf, leaf1_ecx, xcr0, leaf7_ebx);
#elif SNAPPY_IS_AARCH64
  val = kIsaNEON;
#if defined(__APPLE__)
  int sysctl_val = 0;
  size_t len = sizeof(sysctl_val);
  if (sysctlbyname("hw.optional.armv8_crc32", &sysctl_val, &len, nullptr, 0) ==
          0 &&
      sysctl_val != 0) {
    val |= kIsaNEON_CRC32;
  }
#elif (defined(__linux__) || defined(__ANDROID__)) && \
    defined(SNAPPY_HAVE_SYS_AUXV)
  if ((getauxval(AT_HWCAP) & HWCAP_CRC32) != 0) {
    val |= kIsaNEON_CRC32;
  }
#elif defined(_WIN32)
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) {
    val |= kIsaNEON_CRC32;
  }
#endif
#if defined(__ARM_FEATURE_CRC32)
  val |= kIsaNEON_CRC32;
#endif
#endif  // SNAPPY_IS_AARCH64

  g_cached_isa_mask.store(val, std::memory_order_relaxed);
  return val;
}

uint32_t GetSupportedArchitectures() noexcept {
  uint32_t mask = g_cached_isa_mask.load(std::memory_order_relaxed);
  if ((mask & kIsaUninitialized) != 0) {
    return DetectSupportedArchitecturesSlow();
  }
  return mask;
}

void SetArchitectureMaskForTesting(uint32_t mask) noexcept {
  uint32_t host_caps = DetectSupportedArchitecturesSlow();
  g_cached_isa_mask.store((mask & host_caps) & ~kIsaUninitialized,
                          std::memory_order_relaxed);
}

void ResetArchitectureMaskForTesting() noexcept {
  g_cached_isa_mask.store(kIsaUninitialized, std::memory_order_relaxed);
  DetectSupportedArchitecturesSlow();
}

}  // namespace internal

namespace {

// The amount of slop bytes writers are using for unconditional copies.
constexpr int kSlopBytes = 64;

using internal::char_table;
using internal::COPY_1_BYTE_OFFSET;
using internal::COPY_2_BYTE_OFFSET;
using internal::COPY_4_BYTE_OFFSET;
using internal::kMaximumTagLength;
using internal::LITERAL;
#if SNAPPY_HAVE_VECTOR_BYTE_SHUFFLE
using internal::V128;
using internal::V128_DupChar;
using internal::V128_Load;
using internal::V128_LoadU;
using internal::V128_Shuffle;
using internal::V128_StoreU;
#endif

// We translate the information encoded in a tag through a lookup table to a
// format that requires fewer instructions to decode. Effectively we store
// the length minus the tag part of the offset. The lowest significant byte
// thus stores the length. While total length - offset is given by
// entry - ExtractOffset(type). The nice thing is that the subtraction
// immediately sets the flags for the necessary check that offset >= length.
// This folds the cmp with sub. We engineer the long literals and copy-4 to
// always fail this check, so their presence doesn't affect the fast path.
// To prevent literals from triggering the guard against offset < length (offset
// does not apply to literals) the table is giving them a spurious offset of
// 256.
inline constexpr int16_t MakeEntry(int16_t len, int16_t offset) {
  return len - (offset << 8);
}

inline constexpr int16_t LengthMinusOffset(int data, int type) {
  return type == 3   ? 0xFF                    // copy-4 (or type == 3)
         : type == 2 ? MakeEntry(data + 1, 0)  // copy-2
         : type == 1 ? MakeEntry((data & 7) + 4, data >> 3)  // copy-1
         : data < 60 ? MakeEntry(data + 1, 1)  // note spurious offset.
                     : 0xFF;                   // long literal
}

inline constexpr int16_t LengthMinusOffset(uint8_t tag) {
  return LengthMinusOffset(tag >> 2, tag & 3);
}

template <size_t... Ints>
struct index_sequence {};

template <std::size_t N, size_t... Is>
struct make_index_sequence : make_index_sequence<N - 1, N - 1, Is...> {};

template <size_t... Is>
struct make_index_sequence<0, Is...> : index_sequence<Is...> {};

template <size_t... seq>
constexpr std::array<int16_t, 256> MakeTable(index_sequence<seq...>) {
  return std::array<int16_t, 256>{LengthMinusOffset(seq)...};
}

alignas(64) const std::array<int16_t, 256> kLengthMinusOffset =
    MakeTable(make_index_sequence<256>{});

enum class CompressIsaTier {
  kScalar,
  kX86_SSE42,
  kX86_AVX2_BMI2,
  kARM_CRC32,
};

// Given a table of uint16_t whose size is mask / 2 + 1, return a pointer to the
// relevant entry, if any, for the given bytes.  Any hash function will do,
// but a good hash function reduces the number of collisions and thus yields
// better compression for compressible input.
//
// REQUIRES: mask is 2 * (table_size - 1), and table_size is a power of two.
template <CompressIsaTier Tier>
inline uint16_t* TableEntryTier(uint16_t* table, uint32_t bytes, uint32_t mask);

template <>
inline uint16_t* TableEntryTier<CompressIsaTier::kScalar>(uint16_t* table,
                                                          uint32_t bytes,
                                                          uint32_t mask) {
  constexpr uint32_t kMagic = 0x1e35a7bdU;
  const uint32_t hash = (kMagic * bytes) >> (31 - kMaxHashTableBits);
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(table) +
                                     (hash & mask));
}

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSE42
template <>
inline uint16_t* TableEntryTier<CompressIsaTier::kX86_SSE42>(uint16_t* table,
                                                             uint32_t bytes,
                                                             uint32_t mask) {
  const uint32_t hash = _mm_crc32_u32(bytes, mask);
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(table) +
                                     (hash & mask));
}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <>
inline uint16_t* TableEntryTier<CompressIsaTier::kX86_AVX2_BMI2>(
    uint16_t* table, uint32_t bytes, uint32_t mask) {
  const uint32_t hash = _mm_crc32_u32(bytes, mask);
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(table) +
                                     (hash & mask));
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
SNAPPY_TARGET_ARM_CRC32
template <>
inline uint16_t* TableEntryTier<CompressIsaTier::kARM_CRC32>(uint16_t* table,
                                                             uint32_t bytes,
                                                             uint32_t mask) {
  const uint32_t hash = __crc32cw(bytes, mask);
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(table) +
                                     (hash & mask));
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_AARCH64

inline uint16_t* TableEntry4ByteMatch(uint16_t* table, uint32_t bytes,
                                      uint32_t mask) {
  constexpr uint32_t kMagic = 2654435761U;
  const uint32_t hash = (kMagic * bytes) >> (32 - kMaxHashTableBits);
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(table) +
                                     (hash & mask));
}

inline uint16_t* TableEntry8ByteMatch(uint16_t* table, uint64_t bytes,
                                      uint32_t mask) {
  constexpr uint64_t kMagic = 58295818150454627ULL;
  const uint32_t hash = (kMagic * bytes) >> (64 - kMaxHashTableBits);
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(table) +
                                     (hash & mask));
}

}  // namespace

size_t MaxCompressedLength(size_t source_bytes) {
  // Avoid integer overflow that could cause undersized buffer allocations.
  // Return std::numeric_limits<size_t>::max() to force a controlled allocation
  // failure.
  if (source_bytes > (std::numeric_limits<size_t>::max() - 32) / 7 * 6) {
    return std::numeric_limits<size_t>::max();
  }
  // Compressed data can be defined as:
  //    compressed := item* literal*
  //    item       := literal* copy
  //
  // The trailing literal sequence has a space blowup of at most 62/60
  // since a literal of length 60 needs one tag byte + one extra byte
  // for length information.
  //
  // Item blowup is trickier to measure.  Suppose the "copy" op copies
  // 4 bytes of data.  Because of a special check in the encoding code,
  // we produce a 4-byte copy only if the offset is < 65536.  Therefore
  // the copy op takes 3 bytes to encode, and this type of item leads
  // to at most the 62/60 blowup for representing literals.
  //
  // Suppose the "copy" op copies 5 bytes of data.  If the offset is big
  // enough, it will take 5 bytes to encode the copy op.  Therefore the
  // worst case here is a one-byte literal followed by a five-byte copy.
  // I.e., 6 bytes of input turn into 7 bytes of "compressed" data.
  //
  // This last factor dominates the blowup, so the final estimate is:
  return 32 + source_bytes + source_bytes / 6;
}

namespace {

void UnalignedCopy64(const void* src, void* dst) {
  char tmp[8];
  std::memcpy(tmp, src, 8);
  std::memcpy(dst, tmp, 8);
}

void UnalignedCopy128(const void* src, void* dst) {
  // std::memcpy() gets vectorized when the appropriate compiler options are
  // used. For example, x86 compilers targeting SSE2+ will optimize to an SSE2
  // load and store.
  char tmp[16];
  std::memcpy(tmp, src, 16);
  std::memcpy(dst, tmp, 16);
}

template <bool use_16bytes_chunk>
inline void ConditionalUnalignedCopy128(const char* src, char* dst) {
  if (use_16bytes_chunk) {
    UnalignedCopy128(src, dst);
  } else {
    UnalignedCopy64(src, dst);
    UnalignedCopy64(src + 8, dst + 8);
  }
}

// Copy [src, src+(op_limit-op)) to [op, (op_limit-op)) a byte at a time. Used
// for handling COPY operations where the input and output regions may overlap.
// For example, suppose:
//    src       == "ab"
//    op        == src + 2
//    op_limit  == op + 20
// After IncrementalCopySlow(src, op, op_limit), the result will have eleven
// copies of "ab"
//    ababababababababababab
// Note that this does not match the semantics of either std::memcpy() or
// std::memmove().
inline char* IncrementalCopySlow(const char* src, char* op,
                                 char* const op_limit) {
  // TODO: Remove pragma when LLVM is aware this
  // function is only called in cold regions and when cold regions don't get
  // vectorized or unrolled.
#ifdef __clang__
#pragma clang loop unroll(disable)
#endif
  while (op < op_limit) {
    *op++ = *src++;
  }
  return op_limit;
}

#if SNAPPY_HAVE_VECTOR_BYTE_SHUFFLE

// Computes the bytes for shuffle control mask (please read comments on
// 'pattern_generation_masks' as well) for the given index_offset and
// pattern_size. For example, when the 'offset' is 6, it will generate a
// repeating pattern of size 6. So, the first 16 byte indexes will correspond to
// the pattern-bytes {0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3} and the
// next 16 byte indexes will correspond to the pattern-bytes {4, 5, 0, 1, 2, 3,
// 4, 5, 0, 1, 2, 3, 4, 5, 0, 1}. These byte index sequences are generated by
// calling MakePatternMaskBytes(0, 6, index_sequence<16>()) and
// MakePatternMaskBytes(16, 6, index_sequence<16>()) respectively.

template <size_t... indexes>
inline constexpr std::array<char, sizeof...(indexes)> MakePatternMaskBytes(
    int index_offset, int pattern_size, index_sequence<indexes...>) {
  return {static_cast<char>((index_offset + indexes) % pattern_size)...};
}

// Computes the shuffle control mask bytes array for given pattern-sizes and
// returns an array.
template <size_t... pattern_sizes_minus_one>
inline constexpr std::array<std::array<char, sizeof(V128)>,
                            sizeof...(pattern_sizes_minus_one)>
MakePatternMaskBytesTable(int index_offset,
                          index_sequence<pattern_sizes_minus_one...>) {
  return {
      MakePatternMaskBytes(index_offset, pattern_sizes_minus_one + 1,
                           make_index_sequence</*indexes=*/sizeof(V128)>())...};
}
// This is an array of shuffle control masks that can be used as the source
// operand for PSHUFB to permute the contents of the destination XMM register
// into a repeating byte pattern.
alignas(16) constexpr std::array<std::array<char, sizeof(V128)>,
                                 16> pattern_generation_masks =
    MakePatternMaskBytesTable(
        /*index_offset=*/0,
        /*pattern_sizes_minus_one=*/make_index_sequence<16>());

// Similar to 'pattern_generation_masks', this table is used to "rotate" the
// pattern so that we can copy the *next 16 bytes* consistent with the pattern.
// Basically, pattern_reshuffle_masks is a continuation of
// pattern_generation_masks. It follows that, pattern_reshuffle_masks is same as
// pattern_generation_masks for offsets 1, 2, 4, 8 and 16.
alignas(16) constexpr std::array<std::array<char, sizeof(V128)>,
                                 16> pattern_reshuffle_masks =
    MakePatternMaskBytesTable(
        /*index_offset=*/16,
        /*pattern_sizes_minus_one=*/make_index_sequence<16>());

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
#endif

SNAPPY_ATTRIBUTE_ALWAYS_INLINE
static inline V128 LoadPattern(const char* src, const size_t pattern_size) {
  V128 generation_mask = V128_Load(reinterpret_cast<const V128*>(
      pattern_generation_masks[pattern_size - 1].data()));
  // Uninitialized bytes are masked out by the shuffle mask.
  // TODO: remove annotation and macro defs once MSan is fixed.
  SNAPPY_ANNOTATE_MEMORY_IS_INITIALIZED(src + pattern_size, 16 - pattern_size);
  return V128_Shuffle(V128_LoadU(reinterpret_cast<const V128*>(src)),
                      generation_mask);
}

SNAPPY_ATTRIBUTE_ALWAYS_INLINE
static inline std::pair<V128 /* pattern */, V128 /* reshuffle_mask */>
LoadPatternAndReshuffleMask(const char* src, const size_t pattern_size) {
  V128 pattern = LoadPattern(src, pattern_size);

  // This mask will generate the next 16 bytes in-place. Doing so enables us to
  // write data by at most 4 V128_StoreU.
  //
  // For example, suppose pattern is:        abcdefabcdefabcd
  // Shuffling with this mask will generate: efabcdefabcdefab
  // Shuffling again will generate:          cdefabcdefabcdef
  V128 reshuffle_mask = V128_Load(reinterpret_cast<const V128*>(
      pattern_reshuffle_masks[pattern_size - 1].data()));
  return {pattern, reshuffle_mask};
}

#if SNAPPY_IS_X86_64
SNAPPY_UNTARGET_REGION
#endif

#endif  // SNAPPY_HAVE_VECTOR_BYTE_SHUFFLE

}  // namespace

enum class DecompressIsaTier {
  kScalar,
  kX86_SSSE3,
  kX86_AVX2_BMI2,
  kARM_NEON,
};

template <DecompressIsaTier Tier>
inline uint32_t ExtractLowBytesTier(const uint32_t& v, int n);

template <>
inline uint32_t ExtractLowBytesTier<DecompressIsaTier::kScalar>(
    const uint32_t& v, int n) {
  assert(n >= 0);
  assert(n <= 4);
  uint64_t mask = 0xffffffff;
  return v & ~(mask << (8 * n));
}

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
template <>
inline uint32_t ExtractLowBytesTier<DecompressIsaTier::kX86_SSSE3>(
    const uint32_t& v, int n) {
  assert(n >= 0);
  assert(n <= 4);
  uint64_t mask = 0xffffffff;
  return v & ~(mask << (8 * n));
}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <>
inline uint32_t ExtractLowBytesTier<DecompressIsaTier::kX86_AVX2_BMI2>(
    const uint32_t& v, int n) {
  assert(n >= 0);
  assert(n <= 4);
  return _bzhi_u32(v, 8 * n);
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
template <>
inline uint32_t ExtractLowBytesTier<DecompressIsaTier::kARM_NEON>(
    const uint32_t& v, int n) {
  assert(n >= 0);
  assert(n <= 4);
  uint64_t mask = 0xffffffff;
  return v & ~(mask << (8 * n));
}
#endif  // SNAPPY_IS_AARCH64

inline uint32_t ExtractLowBytes(const uint32_t& v, int n) {
  return ExtractLowBytesTier<DecompressIsaTier::kScalar>(v, n);
}

template <DecompressIsaTier Tier>
inline void MaybeZeroUpperTier();

template <>
inline void MaybeZeroUpperTier<DecompressIsaTier::kScalar>() {}

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
template <>
inline void MaybeZeroUpperTier<DecompressIsaTier::kX86_SSSE3>() {}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <>
inline void MaybeZeroUpperTier<DecompressIsaTier::kX86_AVX2_BMI2>() {
  _mm256_zeroupper();
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
template <>
inline void MaybeZeroUpperTier<DecompressIsaTier::kARM_NEON>() {}
#endif  // SNAPPY_IS_AARCH64

template <DecompressIsaTier Tier>
inline void MemCopy64Tier(char* dst, const void* src, size_t size);

template <DecompressIsaTier Tier>
inline void MemCopy64Tier(ptrdiff_t dst, const void* src, size_t size) {
  (void)dst;
  (void)src;
  (void)size;
}

template <>
inline void MemCopy64Tier<DecompressIsaTier::kScalar>(char* dst,
                                                      const void* src,
                                                      size_t size) {
  constexpr int kShortMemCopy = 32;
  (void)kShortMemCopy;
  assert(size <= 64);
  assert(std::less_equal<const void*>()(static_cast<const char*>(src) + size,
                                        dst) ||
         std::less_equal<const void*>()(dst + size, src));
#if defined(__riscv) && SNAPPY_HAVE_RVV
  unsigned char* dst_ptr = reinterpret_cast<unsigned char*>(dst);
  const unsigned char* src_ptr = reinterpret_cast<const unsigned char*>(src);
  size_t remaining_bytes = size;
  while (remaining_bytes > 0) {
    size_t vl = VSETVL_E8M2(remaining_bytes);
    vuint8m2_t vec = VLE8_V_U8M2(src_ptr, vl);
    VSE8_V_U8M2(dst_ptr, vec, vl);
    src_ptr += vl;
    dst_ptr += vl;
    remaining_bytes -= vl;
  }
#else
  std::memmove(dst, src, kShortMemCopy);
  if (SNAPPY_PREDICT_FALSE(size > kShortMemCopy)) {
    std::memmove(dst + kShortMemCopy,
                 static_cast<const uint8_t*>(src) + kShortMemCopy,
                 64 - kShortMemCopy);
  }
#endif
}

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
template <>
inline void MemCopy64Tier<DecompressIsaTier::kX86_SSSE3>(char* dst,
                                                         const void* src,
                                                         size_t size) {
  constexpr int kShortMemCopy = 32;
  (void)kShortMemCopy;
  assert(size <= 64);
  assert(std::less_equal<const void*>()(static_cast<const char*>(src) + size,
                                        dst) ||
         std::less_equal<const void*>()(dst + size, src));
  std::memmove(dst, src, kShortMemCopy);
  if (SNAPPY_PREDICT_FALSE(size > kShortMemCopy)) {
    std::memmove(dst + kShortMemCopy,
                 static_cast<const uint8_t*>(src) + kShortMemCopy,
                 64 - kShortMemCopy);
  }
}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <>
inline void MemCopy64Tier<DecompressIsaTier::kX86_AVX2_BMI2>(char* dst,
                                                             const void* src,
                                                             size_t size) {
  constexpr int kShortMemCopy = 32;
  (void)kShortMemCopy;
  assert(size <= 64);
  assert(std::less_equal<const void*>()(static_cast<const char*>(src) + size,
                                        dst) ||
         std::less_equal<const void*>()(dst + size, src));
  __m256i data = _mm256_lddqu_si256(static_cast<const __m256i*>(src));
  _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), data);
  if (SNAPPY_PREDICT_FALSE(size > kShortMemCopy)) {
    data = _mm256_lddqu_si256(static_cast<const __m256i*>(src) + 1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst) + 1, data);
  }
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
template <>
inline void MemCopy64Tier<DecompressIsaTier::kARM_NEON>(char* dst,
                                                        const void* src,
                                                        size_t size) {
  constexpr int kShortMemCopy = 32;
  (void)kShortMemCopy;
  assert(size <= 64);
  assert(std::less_equal<const void*>()(static_cast<const char*>(src) + size,
                                        dst) ||
         std::less_equal<const void*>()(dst + size, src));
  std::memmove(dst, src, kShortMemCopy);
  if (SNAPPY_PREDICT_FALSE(size > kShortMemCopy)) {
    std::memmove(dst + kShortMemCopy,
                 static_cast<const uint8_t*>(src) + kShortMemCopy,
                 64 - kShortMemCopy);
  }
}
#endif  // SNAPPY_IS_AARCH64

inline void MemCopy64(char* dst, const void* src, size_t size) {
  MemCopy64Tier<DecompressIsaTier::kScalar>(dst, src, size);
}

inline void MemCopy64(ptrdiff_t dst, const void* src, size_t size) {
  MemCopy64Tier<DecompressIsaTier::kScalar>(dst, src, size);
}

template <DecompressIsaTier Tier>
inline bool Copy64BytesWithPatternExtensionTier(char* dst, size_t offset);

template <DecompressIsaTier Tier>
inline bool Copy64BytesWithPatternExtensionTier(ptrdiff_t dst, size_t offset) {
  (void)dst;
  return offset != 0;
}

inline bool Copy64BytesWithPatternExtensionScalar(char* dst, size_t offset) {
  if (SNAPPY_PREDICT_TRUE(offset < 16)) {
    if (SNAPPY_PREDICT_FALSE(offset == 0)) return false;
    for (int i = 0; i < 16; i++) dst[i] = (dst - offset)[i];
    static std::array<uint8_t, 16> pattern_sizes = []() {
      std::array<uint8_t, 16> res;
      for (int i = 1; i < 16; i++) res[i] = (16 / i + 1) * i;
      return res;
    }();
    offset = pattern_sizes[offset];
    for (int i = 1; i < 4; i++) {
      std::memcpy(dst + i * 16, dst + i * 16 - offset, 16);
    }
    return true;
  }
  for (int i = 0; i < 4; i++) {
    std::memcpy(dst + i * 16, dst + i * 16 - offset, 16);
  }
  return true;
}

template <>
inline bool Copy64BytesWithPatternExtensionTier<DecompressIsaTier::kScalar>(
    char* dst, size_t offset) {
  return Copy64BytesWithPatternExtensionScalar(dst, offset);
}

#if SNAPPY_HAVE_VECTOR_BYTE_SHUFFLE
#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
#endif
SNAPPY_ATTRIBUTE_ALWAYS_INLINE
inline bool Copy64BytesWithPatternExtensionVector(char* dst, size_t offset) {
  if (SNAPPY_PREDICT_TRUE(offset <= 16)) {
    switch (offset) {
      case 0:
        return false;
      case 1: {
        V128 pattern = V128_DupChar(dst[-1]);
        for (int i = 0; i < 4; i++) {
          V128_StoreU(reinterpret_cast<V128*>(dst + 16 * i), pattern);
        }
        return true;
      }
      case 2:
      case 4:
      case 8:
      case 16: {
        V128 pattern = LoadPattern(dst - offset, offset);
        for (int i = 0; i < 4; i++) {
          V128_StoreU(reinterpret_cast<V128*>(dst + 16 * i), pattern);
        }
        return true;
      }
      default: {
        auto pattern_and_reshuffle_mask =
            LoadPatternAndReshuffleMask(dst - offset, offset);
        V128 pattern = pattern_and_reshuffle_mask.first;
        V128 reshuffle_mask = pattern_and_reshuffle_mask.second;
        for (int i = 0; i < 4; i++) {
          V128_StoreU(reinterpret_cast<V128*>(dst + 16 * i), pattern);
          pattern = V128_Shuffle(pattern, reshuffle_mask);
        }
        return true;
      }
    }
  }
  for (int i = 0; i < 4; i++) {
    std::memcpy(dst + i * 16, dst + i * 16 - offset, 16);
  }
  return true;
}
#if SNAPPY_IS_X86_64
SNAPPY_UNTARGET_REGION
#endif
#endif  // SNAPPY_HAVE_VECTOR_BYTE_SHUFFLE

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
template <>
inline bool Copy64BytesWithPatternExtensionTier<DecompressIsaTier::kX86_SSSE3>(
    char* dst, size_t offset) {
  return Copy64BytesWithPatternExtensionVector(dst, offset);
}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <>
inline bool
Copy64BytesWithPatternExtensionTier<DecompressIsaTier::kX86_AVX2_BMI2>(
    char* dst, size_t offset) {
  return Copy64BytesWithPatternExtensionVector(dst, offset);
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
template <>
inline bool Copy64BytesWithPatternExtensionTier<DecompressIsaTier::kARM_NEON>(
    char* dst, size_t offset) {
  return Copy64BytesWithPatternExtensionVector(dst, offset);
}
#endif  // SNAPPY_IS_AARCH64

inline bool Copy64BytesWithPatternExtension(char* dst, size_t offset) {
  return Copy64BytesWithPatternExtensionTier<DecompressIsaTier::kScalar>(
      dst, offset);
}

inline bool Copy64BytesWithPatternExtension(ptrdiff_t dst, size_t offset) {
  return Copy64BytesWithPatternExtensionTier<DecompressIsaTier::kScalar>(
      dst, offset);
}

template <DecompressIsaTier Tier>
inline char* IncrementalCopyTier(const char* src, char* op,
                                 char* const op_limit, char* const buf_limit);

template <>
inline char* IncrementalCopyTier<DecompressIsaTier::kScalar>(
    const char* src, char* op, char* const op_limit, char* const buf_limit) {
  constexpr int big_pattern_size_lower_bound = 8;
  assert(src < op);
  assert(op < op_limit);
  assert(op_limit <= buf_limit);
  assert(op_limit - op <= 64);

  size_t pattern_size = op - src;
  if (pattern_size < big_pattern_size_lower_bound) {
    if (SNAPPY_PREDICT_TRUE(op <= buf_limit - 11)) {
      while (pattern_size < 8) {
        UnalignedCopy64(src, op);
        op += pattern_size;
        pattern_size *= 2;
      }
      if (SNAPPY_PREDICT_TRUE(op >= op_limit)) return op_limit;
    } else {
      return IncrementalCopySlow(src, op, op_limit);
    }
  }
  assert(pattern_size >= big_pattern_size_lower_bound);
  constexpr bool use_16bytes_chunk = false;

  if (SNAPPY_PREDICT_TRUE(op_limit <= buf_limit - 15)) {
    ConditionalUnalignedCopy128<use_16bytes_chunk>(src, op);
    if (op + 16 < op_limit) {
      ConditionalUnalignedCopy128<use_16bytes_chunk>(src + 16, op + 16);
    }
    if (op + 32 < op_limit) {
      ConditionalUnalignedCopy128<use_16bytes_chunk>(src + 32, op + 32);
    }
    if (op + 48 < op_limit) {
      ConditionalUnalignedCopy128<use_16bytes_chunk>(src + 48, op + 48);
    }
    return op_limit;
  }

#ifdef __clang__
#pragma clang loop unroll(disable)
#endif
  for (char* op_end = buf_limit - 16; op < op_end; op += 16, src += 16) {
    ConditionalUnalignedCopy128<use_16bytes_chunk>(src, op);
  }
  if (op >= op_limit) return op_limit;

  if (SNAPPY_PREDICT_FALSE(op <= buf_limit - 8)) {
    UnalignedCopy64(src, op);
    src += 8;
    op += 8;
  }
  return IncrementalCopySlow(src, op, op_limit);
}

#if SNAPPY_HAVE_VECTOR_BYTE_SHUFFLE
#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
#endif
SNAPPY_ATTRIBUTE_ALWAYS_INLINE
inline char* IncrementalCopyVector(const char* src, char* op,
                                   char* const op_limit,
                                   char* const buf_limit) {
  constexpr int big_pattern_size_lower_bound = 16;
  assert(src < op);
  assert(op < op_limit);
  assert(op_limit <= buf_limit);
  assert(op_limit - op <= 64);

  size_t pattern_size = op - src;
  if (pattern_size < big_pattern_size_lower_bound) {
    if (SNAPPY_PREDICT_TRUE(op_limit <= buf_limit - 15)) {
      auto pattern_and_reshuffle_mask =
          LoadPatternAndReshuffleMask(src, pattern_size);
      V128 pattern = pattern_and_reshuffle_mask.first;
      V128 reshuffle_mask = pattern_and_reshuffle_mask.second;
      V128_StoreU(reinterpret_cast<V128*>(op), pattern);

      if (op + 16 < op_limit) {
        pattern = V128_Shuffle(pattern, reshuffle_mask);
        V128_StoreU(reinterpret_cast<V128*>(op + 16), pattern);
      }
      if (op + 32 < op_limit) {
        pattern = V128_Shuffle(pattern, reshuffle_mask);
        V128_StoreU(reinterpret_cast<V128*>(op + 32), pattern);
      }
      if (op + 48 < op_limit) {
        pattern = V128_Shuffle(pattern, reshuffle_mask);
        V128_StoreU(reinterpret_cast<V128*>(op + 48), pattern);
      }
      return op_limit;
    }
    char* const op_end = buf_limit - 15;
    if (SNAPPY_PREDICT_TRUE(op < op_end)) {
      auto pattern_and_reshuffle_mask =
          LoadPatternAndReshuffleMask(src, pattern_size);
      V128 pattern = pattern_and_reshuffle_mask.first;
      V128 reshuffle_mask = pattern_and_reshuffle_mask.second;
#ifdef __clang__
#pragma clang loop unroll(disable)
#endif
      do {
        V128_StoreU(reinterpret_cast<V128*>(op), pattern);
        pattern = V128_Shuffle(pattern, reshuffle_mask);
        op += 16;
      } while (SNAPPY_PREDICT_TRUE(op < op_end));
    }
    return IncrementalCopySlow(op - pattern_size, op, op_limit);
  }
  assert(pattern_size >= big_pattern_size_lower_bound);
  constexpr bool use_16bytes_chunk = true;

  if (SNAPPY_PREDICT_TRUE(op_limit <= buf_limit - 15)) {
    ConditionalUnalignedCopy128<use_16bytes_chunk>(src, op);
    if (op + 16 < op_limit) {
      ConditionalUnalignedCopy128<use_16bytes_chunk>(src + 16, op + 16);
    }
    if (op + 32 < op_limit) {
      ConditionalUnalignedCopy128<use_16bytes_chunk>(src + 32, op + 32);
    }
    if (op + 48 < op_limit) {
      ConditionalUnalignedCopy128<use_16bytes_chunk>(src + 48, op + 48);
    }
    return op_limit;
  }

#ifdef __clang__
#pragma clang loop unroll(disable)
#endif
  for (char* op_end = buf_limit - 16; op < op_end; op += 16, src += 16) {
    ConditionalUnalignedCopy128<use_16bytes_chunk>(src, op);
  }
  if (op >= op_limit) return op_limit;

  if (SNAPPY_PREDICT_FALSE(op <= buf_limit - 8)) {
    UnalignedCopy64(src, op);
    src += 8;
    op += 8;
  }
  return IncrementalCopySlow(src, op, op_limit);
}
#if SNAPPY_IS_X86_64
SNAPPY_UNTARGET_REGION
#endif
#endif  // SNAPPY_HAVE_VECTOR_BYTE_SHUFFLE

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
template <>
inline char* IncrementalCopyTier<DecompressIsaTier::kX86_SSSE3>(
    const char* src, char* op, char* const op_limit, char* const buf_limit) {
  return IncrementalCopyVector(src, op, op_limit, buf_limit);
}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <>
inline char* IncrementalCopyTier<DecompressIsaTier::kX86_AVX2_BMI2>(
    const char* src, char* op, char* const op_limit, char* const buf_limit) {
  return IncrementalCopyVector(src, op, op_limit, buf_limit);
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
template <>
inline char* IncrementalCopyTier<DecompressIsaTier::kARM_NEON>(
    const char* src, char* op, char* const op_limit, char* const buf_limit) {
  return IncrementalCopyVector(src, op, op_limit, buf_limit);
}
#endif  // SNAPPY_IS_AARCH64

inline char* IncrementalCopy(const char* src, char* op, char* const op_limit,
                             char* const buf_limit) {
  return IncrementalCopyTier<DecompressIsaTier::kScalar>(src, op, op_limit,
                                                         buf_limit);
}

template <bool allow_fast_path>
static inline char* EmitLiteral(char* op, const char* literal, int len) {
  // The vast majority of copies are below 16 bytes, for which a
  // call to std::memcpy() is overkill. This fast path can sometimes
  // copy up to 15 bytes too much, but that is okay in the
  // main loop, since we have a bit to go on for both sides:
  //
  //   - The input will always have kInputMarginBytes = 15 extra
  //     available bytes, as long as we're in the main loop, and
  //     if not, allow_fast_path = false.
  //   - The output will always have 32 spare bytes (see
  //     MaxCompressedLength).
  assert(len > 0);  // Zero-length literals are disallowed
  int n = len - 1;
  if (allow_fast_path && len <= 16) {
    // Fits in tag byte
    *op++ = LITERAL | (n << 2);

    UnalignedCopy128(literal, op);
    return op + len;
  }

  if (n < 60) {
    // Fits in tag byte
    *op++ = LITERAL | (n << 2);
  } else {
    int count = (Bits::Log2Floor(n) >> 3) + 1;
    assert(count >= 1);
    assert(count <= 4);
    *op++ = LITERAL | ((59 + count) << 2);
    // Encode in upcoming bytes.
    // Write 4 bytes, though we may care about only 1 of them. The output buffer
    // is guaranteed to have at least 3 more spaces left as 'len >= 61' holds
    // here and there is a std::memcpy() of size 'len' below.
    LittleEndian::Store32(op, n);
    op += count;
  }
  // When allow_fast_path is true, we can overwrite up to 16 bytes.
  if (allow_fast_path) {
    char* destination = op;
    const char* source = literal;
    const char* end = destination + len;
    do {
      std::memcpy(destination, source, 16);
      destination += 16;
      source += 16;
    } while (destination < end);
  } else {
    std::memcpy(op, literal, len);
  }
  return op + len;
}

template <bool len_less_than_12>
static inline char* EmitCopyAtMost64(char* op, size_t offset, size_t len) {
  assert(len <= 64);
  assert(len >= 4);
  assert(offset < 65536);
  assert(len_less_than_12 == (len < 12));

  if (len_less_than_12) {
    uint32_t u = (len << 2) + (offset << 8);
    uint32_t copy1 = COPY_1_BYTE_OFFSET - (4 << 2) + ((offset >> 3) & 0xe0);
    uint32_t copy2 = COPY_2_BYTE_OFFSET - (1 << 2);
    // It turns out that offset < 2048 is a difficult to predict branch.
    // `perf record` shows this is the highest percentage of branch misses in
    // benchmarks. This code produces branch free code, the data dependency
    // chain that bottlenecks the throughput is so long that a few extra
    // instructions are completely free (IPC << 6 because of data deps).
    u += offset < 2048 ? copy1 : copy2;
    LittleEndian::Store32(op, u);
    op += offset < 2048 ? 2 : 3;
  } else {
    // Write 4 bytes, though we only care about 3 of them.  The output buffer
    // is required to have some slack, so the extra byte won't overrun it.
    uint32_t u = COPY_2_BYTE_OFFSET + ((len - 1) << 2) + (offset << 8);
    LittleEndian::Store32(op, u);
    op += 3;
  }
  return op;
}

template <bool len_less_than_12>
static inline char* EmitCopy(char* op, size_t offset, size_t len) {
  assert(len_less_than_12 == (len < 12));
  if (len_less_than_12) {
    return EmitCopyAtMost64</*len_less_than_12=*/true>(op, offset, len);
  } else {
    // A special case for len <= 64 might help, but so far measurements suggest
    // it's in the noise.

    // Emit 64 byte copies but make sure to keep at least four bytes reserved.
    while (SNAPPY_PREDICT_FALSE(len >= 68)) {
      op = EmitCopyAtMost64</*len_less_than_12=*/false>(op, offset, 64);
      len -= 64;
    }

    // One or two copies will now finish the job.
    if (len > 64) {
      op = EmitCopyAtMost64</*len_less_than_12=*/false>(op, offset, 60);
      len -= 60;
    }

    // Emit remainder.
    if (len < 12) {
      op = EmitCopyAtMost64</*len_less_than_12=*/true>(op, offset, len);
    } else {
      op = EmitCopyAtMost64</*len_less_than_12=*/false>(op, offset, len);
    }
    return op;
  }
}

bool GetUncompressedLength(const char* start, size_t n, size_t* result) {
  uint32_t v = 0;
  const char* limit = start + n;
  if (Varint::Parse32WithLimit(start, limit, &v) != NULL) {
    *result = v;
    return true;
  } else {
    return false;
  }
}

namespace {
uint32_t CalculateTableSize(uint32_t input_size) {
  static_assert(
      kMaxHashTableSize >= kMinHashTableSize,
      "kMaxHashTableSize should be greater or equal to kMinHashTableSize.");
  if (input_size > kMaxHashTableSize) {
    return kMaxHashTableSize;
  }
  if (input_size < kMinHashTableSize) {
    return kMinHashTableSize;
  }
  // This is equivalent to Log2Ceiling(input_size), assuming input_size > 1.
  // 2 << Log2Floor(x - 1) is equivalent to 1 << (1 + Log2Floor(x - 1)).
  return 2u << Bits::Log2Floor(input_size - 1);
}
}  // namespace

namespace internal {
size_t WorkingMemory::RequiredSize(size_t input_size) {
  const size_t max_fragment_size = std::min(input_size, kBlockSize);
  const size_t table_size = CalculateTableSize(max_fragment_size);
  return table_size * sizeof(uint16_t) + max_fragment_size +
         MaxCompressedLength(max_fragment_size);
}

WorkingMemory::WorkingMemory(size_t input_size)
    : WorkingMemory(input_size,
                    std::allocator<char>().allocate(RequiredSize(input_size))) {
  owns_mem_ = true;
}

WorkingMemory::WorkingMemory(size_t input_size, char* buffer) {
  assert(buffer != nullptr);
  assert(reinterpret_cast<uintptr_t>(buffer) % alignof(uint16_t) == 0);
  const size_t max_fragment_size = std::min(input_size, kBlockSize);
  const size_t table_size = CalculateTableSize(max_fragment_size);
  mem_ = buffer;
  size_ = RequiredSize(input_size);
  owns_mem_ = false;
  table_ = reinterpret_cast<uint16_t*>(mem_);
  input_ = mem_ + table_size * sizeof(*table_);
  output_ = input_ + max_fragment_size;
}

WorkingMemory::~WorkingMemory() {
  if (owns_mem_) {
    std::allocator<char>().deallocate(mem_, size_);
  }
}

uint16_t* WorkingMemory::GetHashTable(size_t fragment_size,
                                      int* table_size) const {
  const size_t htsize = CalculateTableSize(fragment_size);
  memset(table_, 0, htsize * sizeof(*table_));
  *table_size = htsize;
  return table_;
}
}  // end namespace internal

// Flat array compression that does not emit the "uncompressed length"
// prefix. Compresses "input" string to the "*op" buffer.
//
// REQUIRES: "input" is at most "kBlockSize" bytes long.
// REQUIRES: "op" points to an array of memory that is at least
// "MaxCompressedLength(input.size())" in size.
// REQUIRES: All elements in "table[0..table_size-1]" are initialized to zero.
// REQUIRES: "table_size" is a power of two
//
// Returns an "end" pointer into "op" buffer.
// "end - op" is the compressed size of "input".
namespace internal {
template <CompressIsaTier Tier>
SNAPPY_ATTRIBUTE_ALWAYS_INLINE inline char* CompressFragmentImpl(
    const char* input, size_t input_size, char* op, uint16_t* table,
    const int table_size) {
  // "ip" is the input pointer, and "op" is the output pointer.
  const char* ip = input;
  assert(input_size <= kBlockSize);
  assert((table_size & (table_size - 1)) == 0);  // table must be power of two
  const uint32_t mask = 2 * (table_size - 1);
  const char* ip_end = input + input_size;
  const char* base_ip = ip;

  const size_t kInputMarginBytes = 15;
  if (SNAPPY_PREDICT_TRUE(input_size >= kInputMarginBytes)) {
    const char* ip_limit = input + input_size - kInputMarginBytes;

    for (uint32_t preload = LittleEndian::Load32(ip + 1);;) {
      // Bytes in [next_emit, ip) will be emitted as literal bytes.  Or
      // [next_emit, ip_end) after the main loop.
      const char* next_emit = ip++;
      uint64_t data = LittleEndian::Load64(ip);
      // The body of this loop calls EmitLiteral once and then EmitCopy one or
      // more times.  (The exception is that when we're close to exhausting
      // the input we goto emit_remainder.)
      //
      // In the first iteration of this loop we're just starting, so
      // there's nothing to copy, so calling EmitLiteral once is
      // necessary.  And we only start a new iteration when the
      // current iteration has determined that a call to EmitLiteral will
      // precede the next call to EmitCopy (if any).
      //
      // Step 1: Scan forward in the input looking for a 4-byte-long match.
      // If we get close to exhausting the input then goto emit_remainder.
      //
      // Heuristic match skipping: If 32 bytes are scanned with no matches
      // found, start looking only at every other byte. If 32 more bytes are
      // scanned (or skipped), look at every third byte, etc.. When a match is
      // found, immediately go back to looking at every byte. This is a small
      // loss (~5% performance, ~0.1% density) for compressible data due to more
      // bookkeeping, but for non-compressible data (such as JPEG) it's a huge
      // win since the compressor quickly "realizes" the data is incompressible
      // and doesn't bother looking for matches everywhere.
      //
      // The "skip" variable keeps track of how many bytes there are since the
      // last match; dividing it by 32 (ie. right-shifting by five) gives the
      // number of bytes to move ahead for each iteration.
      uint32_t skip = 32;

      const char* candidate;
      if (ip_limit - ip >= 16) {
        auto delta = ip - base_ip;
        for (int j = 0; j < 4; ++j) {
          for (int k = 0; k < 4; ++k) {
            int i = 4 * j + k;
            // These for-loops are meant to be unrolled. So we can freely
            // special case the first iteration to use the value already
            // loaded in preload.
            uint32_t dword = i == 0 ? preload : static_cast<uint32_t>(data);
            assert(dword == LittleEndian::Load32(ip + i));
            uint16_t* table_entry = TableEntryTier<Tier>(table, dword, mask);
            candidate = base_ip + *table_entry;
            assert(candidate >= base_ip);
            assert(candidate < ip + i);
            *table_entry = delta + i;
            if (SNAPPY_PREDICT_FALSE(LittleEndian::Load32(candidate) ==
                                     dword)) {
              *op = LITERAL | (i << 2);
              UnalignedCopy128(next_emit, op + 1);
              ip += i;
              op = op + i + 2;
              goto emit_match;
            }
            data >>= 8;
          }
          data = LittleEndian::Load64(ip + 4 * j + 4);
        }
        ip += 16;
        skip += 16;
      }
      while (true) {
        assert(static_cast<uint32_t>(data) == LittleEndian::Load32(ip));
        uint16_t* table_entry = TableEntryTier<Tier>(table, data, mask);
        uint32_t bytes_between_hash_lookups = skip >> 5;
        skip += bytes_between_hash_lookups;
        const char* next_ip = ip + bytes_between_hash_lookups;
        if (SNAPPY_PREDICT_FALSE(next_ip > ip_limit)) {
          ip = next_emit;
          goto emit_remainder;
        }
        candidate = base_ip + *table_entry;
        assert(candidate >= base_ip);
        assert(candidate < ip);

        *table_entry = ip - base_ip;
        if (SNAPPY_PREDICT_FALSE(static_cast<uint32_t>(data) ==
                                 LittleEndian::Load32(candidate))) {
          break;
        }
        data = LittleEndian::Load32(next_ip);
        ip = next_ip;
      }

      // Step 2: A 4-byte match has been found.  We'll later see if more
      // than 4 bytes match.  But, prior to the match, input
      // bytes [next_emit, ip) are unmatched.  Emit them as "literal bytes."
      assert(next_emit + 16 <= ip_end);
      op = EmitLiteral</*allow_fast_path=*/true>(op, next_emit, ip - next_emit);

      // Step 3: Call EmitCopy, and then see if another EmitCopy could
      // be our next move.  Repeat until we find no match for the
      // input immediately after what was consumed by the last EmitCopy call.
      //
      // If we exit this loop normally then we need to call EmitLiteral next,
      // though we don't yet know how big the literal will be.  We handle that
      // by proceeding to the next iteration of the main loop.  We also can exit
      // this loop via goto if we get close to exhausting the input.
    emit_match:
      do {
        // We have a 4-byte match at ip, and no need to emit any
        // "literal bytes" prior to ip.
        const char* base = ip;
        std::pair<size_t, bool> p =
            FindMatchLength(candidate + 4, ip + 4, ip_end, &data);
        size_t matched = 4 + p.first;
        ip += matched;
        size_t offset = base - candidate;
        assert(0 == memcmp(base, candidate, matched));
        if (p.second) {
          op = EmitCopy</*len_less_than_12=*/true>(op, offset, matched);
        } else {
          op = EmitCopy</*len_less_than_12=*/false>(op, offset, matched);
        }
        if (SNAPPY_PREDICT_FALSE(ip >= ip_limit)) {
          goto emit_remainder;
        }
        // Expect 5 bytes to match
        assert((data & 0xFFFFFFFFFF) ==
               (LittleEndian::Load64(ip) & 0xFFFFFFFFFF));
        // We are now looking for a 4-byte match again.  We read
        // table[Hash(ip, mask)] for that.  To improve compression,
        // we also update table[Hash(ip - 1, mask)] and table[Hash(ip, mask)].
        *TableEntryTier<Tier>(table, LittleEndian::Load32(ip - 1), mask) =
            ip - base_ip - 1;
        uint16_t* table_entry = TableEntryTier<Tier>(table, data, mask);
        candidate = base_ip + *table_entry;
        *table_entry = ip - base_ip;
        // Measurements on the benchmarks have shown the following probabilities
        // for the loop to exit (ie. avg. number of iterations is reciprocal).
        // BM_Flat/6  txt1    p = 0.3-0.4
        // BM_Flat/7  txt2    p = 0.35
        // BM_Flat/8  txt3    p = 0.3-0.4
        // BM_Flat/9  txt3    p = 0.34-0.4
        // BM_Flat/10 pb      p = 0.4
        // BM_Flat/11 gaviota p = 0.1
        // BM_Flat/12 cp      p = 0.5
        // BM_Flat/13 c       p = 0.3
      } while (static_cast<uint32_t>(data) == LittleEndian::Load32(candidate));
      // Because the least significant 5 bytes matched, we can utilize data
      // for the next iteration.
      preload = data >> 8;
    }
  }

emit_remainder:
  // Emit the remaining bytes as a literal
  if (ip < ip_end) {
    op = EmitLiteral</*allow_fast_path=*/false>(op, ip, ip_end - ip);
  }

  return op;
}

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSE42
char* CompressFragment_SSE42(const char* input, size_t input_size, char* op,
                             uint16_t* table, const int table_size) {
  return CompressFragmentImpl<CompressIsaTier::kX86_SSE42>(
      input, input_size, op, table, table_size);
}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
char* CompressFragment_AVX2_BMI2(const char* input, size_t input_size, char* op,
                                 uint16_t* table, const int table_size) {
  return CompressFragmentImpl<CompressIsaTier::kX86_AVX2_BMI2>(
      input, input_size, op, table, table_size);
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
SNAPPY_TARGET_ARM_CRC32
char* CompressFragment_ARM_CRC32(const char* input, size_t input_size, char* op,
                                 uint16_t* table, const int table_size) {
  return CompressFragmentImpl<CompressIsaTier::kARM_CRC32>(
      input, input_size, op, table, table_size);
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_AARCH64

char* CompressFragment_Scalar(const char* input, size_t input_size, char* op,
                              uint16_t* table, const int table_size) {
  return CompressFragmentImpl<CompressIsaTier::kScalar>(input, input_size, op,
                                                        table, table_size);
}

char* CompressFragment(const char* input, size_t input_size, char* op,
                       uint16_t* table, const int table_size) {
  const uint32_t mask = GetSupportedArchitectures();
#if SNAPPY_IS_X86_64
  if ((mask & kIsaX86_AVX2Tier) == kIsaX86_AVX2Tier) {
    return CompressFragment_AVX2_BMI2(input, input_size, op, table, table_size);
  }
  if ((mask & kIsaX86_SSE42Tier) == kIsaX86_SSE42Tier) {
    return CompressFragment_SSE42(input, input_size, op, table, table_size);
  }
#elif SNAPPY_IS_AARCH64
  if (mask & kIsaNEON_CRC32) {
    return CompressFragment_ARM_CRC32(input, input_size, op, table, table_size);
  }
#endif
  return CompressFragment_Scalar(input, input_size, op, table, table_size);
}

char* CompressFragmentDoubleHash(const char* input, size_t input_size, char* op,
                                 uint16_t* table, const int table_size,
                                 uint16_t* table2, const int table_size2) {
  (void)table_size2;
  assert(table_size == table_size2);
  // "ip" is the input pointer, and "op" is the output pointer.
  const char* ip = input;
  assert(input_size <= kBlockSize);
  assert((table_size & (table_size - 1)) == 0);  // table must be power of two
  const uint32_t mask = 2 * (table_size - 1);
  const char* ip_end = input + input_size;
  const char* base_ip = ip;

  const size_t kInputMarginBytes = 15;
  if (SNAPPY_PREDICT_TRUE(input_size >= kInputMarginBytes)) {
    const char* ip_limit = input + input_size - kInputMarginBytes;

    for (;;) {
      const char* next_emit = ip++;
      uint64_t data = LittleEndian::Load64(ip);
      uint32_t skip = 512;

      const char* candidate;
      uint32_t candidate_length;
      while (true) {
        assert(static_cast<uint32_t>(data) == LittleEndian::Load32(ip));
        uint16_t* table_entry2 = TableEntry8ByteMatch(table2, data, mask);
        uint32_t bytes_between_hash_lookups = skip >> 9;
        skip++;
        const char* next_ip = ip + bytes_between_hash_lookups;
        if (SNAPPY_PREDICT_FALSE(next_ip > ip_limit)) {
          ip = next_emit;
          goto emit_remainder;
        }
        candidate = base_ip + *table_entry2;
        assert(candidate >= base_ip);
        assert(candidate < ip);

        *table_entry2 = ip - base_ip;
        if (SNAPPY_PREDICT_FALSE(static_cast<uint32_t>(data) ==
                                 LittleEndian::Load32(candidate))) {
          candidate_length =
              FindMatchLengthPlain(candidate + 4, ip + 4, ip_end) + 4;
          break;
        }

        uint16_t* table_entry = TableEntry4ByteMatch(table, data, mask);
        candidate = base_ip + *table_entry;
        assert(candidate >= base_ip);
        assert(candidate < ip);

        *table_entry = ip - base_ip;
        if (SNAPPY_PREDICT_FALSE(static_cast<uint32_t>(data) ==
                                 LittleEndian::Load32(candidate))) {
          candidate_length =
              FindMatchLengthPlain(candidate + 4, ip + 4, ip_end) + 4;
          table_entry2 =
              TableEntry8ByteMatch(table2, LittleEndian::Load64(ip + 1), mask);
          auto candidate2 = base_ip + *table_entry2;
          size_t candidate_length2 =
              FindMatchLengthPlain(candidate2, ip + 1, ip_end);
          if (candidate_length2 > candidate_length) {
            *table_entry2 = ip - base_ip;
            candidate = candidate2;
            candidate_length = candidate_length2;
            ++ip;
          }
          break;
        }
        data = LittleEndian::Load64(next_ip);
        ip = next_ip;
      }
      // Backtrack to the point it matches fully.
      while (ip > next_emit && candidate > base_ip &&
             *(ip - 1) == *(candidate - 1)) {
        --ip;
        --candidate;
        ++candidate_length;
      }
      *TableEntry8ByteMatch(table2, LittleEndian::Load64(ip + 1), mask) =
          ip - base_ip + 1;
      *TableEntry8ByteMatch(table2, LittleEndian::Load64(ip + 2), mask) =
          ip - base_ip + 2;
      *TableEntry4ByteMatch(table, LittleEndian::Load32(ip + 1), mask) =
          ip - base_ip + 1;
      // Step 2: A 4-byte or 8-byte match has been found.
      // We'll later see if more than 4 bytes match.  But, prior to the match,
      // input bytes [next_emit, ip) are unmatched.  Emit them as
      // "literal bytes."
      assert(next_emit + 16 <= ip_end);
      if (ip - next_emit > 0) {
        op = EmitLiteral</*allow_fast_path=*/true>(op, next_emit,
                                                   ip - next_emit);
      }
      // Step 3: Call EmitCopy, and then see if another EmitCopy could
      // be our next move.  Repeat until we find no match for the
      // input immediately after what was consumed by the last EmitCopy call.
      //
      // If we exit this loop normally then we need to call EmitLiteral next,
      // though we don't yet know how big the literal will be.  We handle that
      // by proceeding to the next iteration of the main loop.  We also can exit
      // this loop via goto if we get close to exhausting the input.
      do {
        // We have a 4-byte match at ip, and no need to emit any
        // "literal bytes" prior to ip.
        const char* base = ip;
        ip += candidate_length;
        size_t offset = base - candidate;
        if (candidate_length < 12) {
          op =
              EmitCopy</*len_less_than_12=*/true>(op, offset, candidate_length);
        } else {
          op = EmitCopy</*len_less_than_12=*/false>(op, offset,
                                                    candidate_length);
        }
        if (SNAPPY_PREDICT_FALSE(ip >= ip_limit)) {
          goto emit_remainder;
        }
        // We are now looking for a 4-byte match again.  We read
        // table[Hash(ip, mask)] for that. To improve compression,
        // we also update several previous table entries.
        if (ip - base_ip > 7) {
          *TableEntry8ByteMatch(table2, LittleEndian::Load64(ip - 7), mask) =
              ip - base_ip - 7;
          *TableEntry8ByteMatch(table2, LittleEndian::Load64(ip - 4), mask) =
              ip - base_ip - 4;
        }
        *TableEntry8ByteMatch(table2, LittleEndian::Load64(ip - 3), mask) =
            ip - base_ip - 3;
        *TableEntry8ByteMatch(table2, LittleEndian::Load64(ip - 2), mask) =
            ip - base_ip - 2;
        *TableEntry4ByteMatch(table, LittleEndian::Load32(ip - 2), mask) =
            ip - base_ip - 2;
        *TableEntry4ByteMatch(table, LittleEndian::Load32(ip - 1), mask) =
            ip - base_ip - 1;

        uint16_t* table_entry =
            TableEntry8ByteMatch(table2, LittleEndian::Load64(ip), mask);
        candidate = base_ip + *table_entry;
        *table_entry = ip - base_ip;
        if (LittleEndian::Load32(ip) == LittleEndian::Load32(candidate)) {
          candidate_length =
              FindMatchLengthPlain(candidate + 4, ip + 4, ip_end) + 4;
          continue;
        }
        table_entry =
            TableEntry4ByteMatch(table, LittleEndian::Load32(ip), mask);
        candidate = base_ip + *table_entry;
        *table_entry = ip - base_ip;
        if (LittleEndian::Load32(ip) == LittleEndian::Load32(candidate)) {
          candidate_length =
              FindMatchLengthPlain(candidate + 4, ip + 4, ip_end) + 4;
          continue;
        }
        break;
      } while (true);
    }
  }

emit_remainder:
  // Emit the remaining bytes as a literal
  if (ip < ip_end) {
    op = EmitLiteral</*allow_fast_path=*/false>(op, ip, ip_end - ip);
  }

  return op;
}
}  // end namespace internal

static inline void Report(int token, const char* algorithm,
                          size_t compressed_size, size_t uncompressed_size) {
  // TODO: Switch to [[maybe_unused]] when we can assume C++17.
  (void)token;
  (void)algorithm;
  (void)compressed_size;
  (void)uncompressed_size;
}

// Signature of output types needed by decompression code.
// The decompression code is templatized on a type that obeys this
// signature so that we do not pay virtual function call overhead in
// the middle of a tight decompression loop.
//
// class DecompressionWriter {
//  public:
//   // Called before decompression
//   void SetExpectedLength(size_t length);
//
//   // For performance a writer may choose to donate the cursor variable to the
//   // decompression function. The decompression will inject it in all its
//   // function calls to the writer. Keeping the important output cursor as a
//   // function local stack variable allows the compiler to keep it in
//   // register, which greatly aids performance by avoiding loads and stores of
//   // this variable in the fast path loop iterations.
//   T GetOutputPtr() const;
//
//   // At end of decompression the loop donates the ownership of the cursor
//   // variable back to the writer by calling this function.
//   void SetOutputPtr(T op);
//
//   // Called after decompression
//   bool CheckLength() const;
//
//   // Called repeatedly during decompression
//   // Each function get a pointer to the op (output pointer), that the writer
//   // can use and update. Note it's important that these functions get fully
//   // inlined so that no actual address of the local variable needs to be
//   // taken.
//   bool Append(const char* ip, size_t length, T* op);
//   bool AppendFromSelf(uint32_t offset, size_t length, T* op);
//
//   // The rules for how TryFastAppend differs from Append are somewhat
//   // convoluted:
//   //
//   //  - TryFastAppend is allowed to decline (return false) at any
//   //    time, for any reason -- just "return false" would be
//   //    a perfectly legal implementation of TryFastAppend.
//   //    The intention is for TryFastAppend to allow a fast path
//   //    in the common case of a small append.
//   //  - TryFastAppend is allowed to read up to <available> bytes
//   //    from the input buffer, whereas Append is allowed to read
//   //    <length>. However, if it returns true, it must leave
//   //    at least five (kMaximumTagLength) bytes in the input buffer
//   //    afterwards, so that there is always enough space to read the
//   //    next tag without checking for a refill.
//   //  - TryFastAppend must always return decline (return false)
//   //    if <length> is 61 or more, as in this case the literal length is not
//   //    decoded fully. In practice, this should not be a big problem,
//   //    as it is unlikely that one would implement a fast path accepting
//   //    this much data.
//   //
//   bool TryFastAppend(const char* ip, size_t available, size_t length, T* op);
// };

static inline bool LeftShiftOverflows(uint8_t value, uint32_t shift) {
  assert(shift < 32);
  static const uint8_t masks[] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  //
      0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe};
  return (value & masks[shift]) != 0;
}

void ClearDeferred(const void** deferred_src, size_t* deferred_length,
                   uint8_t* safe_source) {
  *deferred_src = safe_source;
  *deferred_length = 0;
}

void DeferMemCopy(const void** deferred_src, size_t* deferred_length,
                  const void* src, size_t length) {
  *deferred_src = src;
  *deferred_length = length;
}

SNAPPY_ATTRIBUTE_ALWAYS_INLINE
inline size_t AdvanceToNextTagARMOptimized(const uint8_t** ip_p, size_t* tag) {
  const uint8_t*& ip = *ip_p;
  // This section is crucial for the throughput of the decompression loop.
  // The latency of an iteration is fundamentally constrained by the
  // following data chain on ip.
  // ip -> c = Load(ip) -> delta1 = (c & 3)        -> ip += delta1 or delta2
  //                       delta2 = ((c >> 2) + 1)    ip++
  // This is different from X86 optimizations because ARM has conditional add
  // instruction (csinc) and it removes several register moves.
  const size_t tag_type = *tag & 3;
  const bool is_literal = (tag_type == 0);
  if (is_literal) {
    size_t next_literal_tag = (*tag >> 2) + 1;
    *tag = ip[next_literal_tag];
    ip += next_literal_tag + 1;
  } else {
    *tag = ip[tag_type];
    ip += tag_type + 1;
  }
  return tag_type;
}

SNAPPY_ATTRIBUTE_ALWAYS_INLINE
inline size_t AdvanceToNextTagX86Optimized(const uint8_t** ip_p, size_t* tag) {
  const uint8_t*& ip = *ip_p;
  // This section is crucial for the throughput of the decompression loop.
  // The latency of an iteration is fundamentally constrained by the
  // following data chain on ip.
  // ip -> c = Load(ip) -> ip1 = ip + 1 + (c & 3) -> ip = ip1 or ip2
  //                       ip2 = ip + 2 + (c >> 2)
  // This amounts to 8 cycles.
  // 5 (load) + 1 (c & 3) + 1 (lea ip1, [ip + (c & 3) + 1]) + 1 (cmov)
  size_t literal_len = *tag >> 2;
  size_t tag_type = *tag;
  bool is_literal;
#if defined(__GCC_ASM_FLAG_OUTPUTS__) && defined(__x86_64__)
  // TODO clang misses the fact that the (c & 3) already correctly
  // sets the zero flag.
  asm("and $3, %k[tag_type]\n\t"
      : [tag_type] "+r"(tag_type), "=@ccz"(is_literal)::"cc");
#else
  tag_type &= 3;
  is_literal = (tag_type == 0);
#endif
  // TODO
  // This is code is subtle. Loading the values first and then cmov has less
  // latency then cmov ip and then load. However clang would move the loads
  // in an optimization phase, volatile prevents this transformation.
  // Note that we have enough slop bytes (64) that the loads are always valid.
  size_t tag_literal =
      static_cast<const volatile uint8_t*>(ip)[1 + literal_len];
  size_t tag_copy = static_cast<const volatile uint8_t*>(ip)[tag_type];
  *tag = is_literal ? tag_literal : tag_copy;
  const uint8_t* ip_copy = ip + 1 + tag_type;
  const uint8_t* ip_literal = ip + 2 + literal_len;
  ip = is_literal ? ip_literal : ip_copy;
#if defined(__GNUC__) && defined(__x86_64__)
  // TODO Clang is "optimizing" zero-extension (a totally free
  // operation) this means that after the cmov of tag, it emits another movzb
  // tag, byte(tag). It really matters as it's on the core chain. This dummy
  // asm, persuades clang to do the zero-extension at the load (it's automatic)
  // removing the expensive movzb.
  asm("" ::"r"(tag_copy));
#endif
  return tag_type;
}

SNAPPY_ATTRIBUTE_ALWAYS_INLINE
inline size_t AdvanceToNextTagRVOptimized(const uint8_t** ip_p, size_t* tag) {
  const uint8_t*& ip = *ip_p;
  // This section is crucial for the throughput of the decompression loop.
  // The latency of an iteration is fundamentally constrained by the data chain
  // on ip: ip -> c = *tag -> literal_len = c >> 2, tag_type = c & 3
  //      -> literal_advance = literal_len + 2, copy_advance = tag_type + 1
  //      -> next_ip = ip + literal_advance  OR  ip + copy_advance (literal vs
  //      copy)
  //      -> *tag = byte at (next_ip - 1); ip = next_ip
  //
  // Base RISC-V has no x86-style cmov and no AArch64 csinc on the same shape;
  // this computes both candidate advances and both load offsets, then selects
  // with (is_literal ? ... : ...). With the Zicond extension (czero.eqz /
  // czero.nez), those selections typically lower to branchless conditional-zero
  // ops instead of a hard-to-predict literal/copy branch, which is why this
  // form tends to win there.
  const size_t literal_len = *tag >> 2;
  const size_t tag_type = *tag & 3;
  const bool is_literal = (tag_type == 0);
  const size_t copy_advance = tag_type + 1;
  const size_t literal_advance = literal_len + 2;
  const uint8_t* next_ip =
      is_literal ? (ip + literal_advance) : (ip + copy_advance);
  *tag = is_literal ? ip[literal_advance - 1] : ip[copy_advance - 1];
  ip = next_ip;
  return tag_type;
}

// Extract the offset for copy-1 and copy-2 returns 0 for literals or copy-4.
inline uint32_t ExtractOffset(uint32_t val, size_t tag_type) {
  // For Arm non-static storage works better. For x86 static storage is better.
  // TODO: Once the array is recognized as a register, improve the
  // readability for x86.
#if defined(__x86_64__)
  static constexpr uint64_t kExtractMasksCombined = 0x0000FFFF00FF0000ull;
  uint16_t result;
  memcpy(&result,
         reinterpret_cast<const char*>(&kExtractMasksCombined) + 2 * tag_type,
         sizeof(result));
  return val & result;
  // For AArch64 and RISC-V, use a bit-twiddling trick to extract the mask from
  // a single combined constant instead of a lookup table. The constant packs
  // multiple 16-bit masks based on tag_type (see implementation below). The
  // code calculates the shift amount from tag_type, right-shifts the constant
  // to move the desired mask to the LSB position, then extracts it with &
  // 0xFFFF. This branchless approach is often more performant on modern CPUs.
#elif defined(__aarch64__) || (defined(__riscv) && (__riscv_xlen == 64))
  constexpr uint64_t kExtractMasksCombined = 0x0000FFFF00FF0000ull;
  return val & static_cast<uint32_t>(
                   (kExtractMasksCombined >> (tag_type * 16)) & 0xFFFF);
#else
  static constexpr uint32_t kExtractMasks[4] = {0, 0xFF, 0xFFFF, 0};
  return val & kExtractMasks[tag_type];
#endif
};

// Core decompression loop, when there is enough data available.
// Decompresses the input buffer [ip, ip_limit) into the output buffer
// [op, op_limit_min_slop). Returning when either we are too close to the end
// of the input buffer, or we exceed op_limit_min_slop or when a exceptional
// tag is encountered (literal of length > 60) or a copy-4.
// Returns {ip, op} at the points it stopped decoding.
// TODO This function probably does not need to be inlined, as it
// should decode large chunks at a time. This allows runtime dispatch to
// implementations based on CPU capability (BMI2 / perhaps 32 / 64 byte memcpy).
template <DecompressIsaTier Tier, typename T>
SNAPPY_ATTRIBUTE_ALWAYS_INLINE inline std::pair<const uint8_t*, ptrdiff_t>
DecompressBranchlessImpl(const uint8_t* ip, const uint8_t* ip_limit,
                         ptrdiff_t op, T op_base, ptrdiff_t op_limit_min_slop) {
  // If deferred_src is invalid point it here.
  uint8_t safe_source[64];
  const void* deferred_src;
  size_t deferred_length;
  ClearDeferred(&deferred_src, &deferred_length, safe_source);

  // We unroll the inner loop twice so we need twice the spare room.
  op_limit_min_slop -= kSlopBytes;
  if (2 * (kSlopBytes + 1) < ip_limit - ip && op < op_limit_min_slop) {
    const uint8_t* const ip_limit_min_slop = ip_limit - 2 * kSlopBytes - 1;
    ip++;
    // ip points just past the tag and we are touching at maximum kSlopBytes
    // in an iteration.
    size_t tag = ip[-1];
#if defined(__clang__) && defined(__aarch64__)
    // Workaround for https://bugs.llvm.org/show_bug.cgi?id=51317
    // when loading 1 byte, clang for aarch64 doesn't realize that it(ldrb)
    // comes with free zero-extension, so clang generates another
    // 'and xn, xm, 0xff' before it use that as the offset. This 'and' is
    // redundant and can be removed by adding this dummy asm, which gives
    // clang a hint that we're doing the zero-extension at the load.
    asm("" ::"r"(tag));
#endif
    do {
      // The throughput is limited by instructions, unrolling the inner loop
      // twice reduces the amount of instructions checking limits and also
      // leads to reduced mov's.

      SNAPPY_PREFETCH(ip + 128);
      for (int i = 0; i < 2; i++) {
        const uint8_t* old_ip = ip;
        assert(tag == ip[-1]);
        // For literals tag_type = 0, hence we will always obtain 0 from
        // ExtractLowBytes. For literals offset will thus be kLiteralOffset.
        ptrdiff_t len_minus_offset = kLengthMinusOffset[tag];
        uint32_t next;
#if defined(__aarch64__)
        size_t tag_type = AdvanceToNextTagARMOptimized(&ip, &tag);
        // We never need more than 16 bits. Doing a Load16 allows the compiler
        // to elide the masking operation in ExtractOffset.
        next = LittleEndian::Load16(old_ip);
#elif defined(__riscv)
        size_t tag_type = AdvanceToNextTagRVOptimized(&ip, &tag);
        // We never need more than 16 bits. Doing a Load16 allows the compiler
        // to elide the masking operation in ExtractOffset.
        next = LittleEndian::Load16(old_ip);
#else
        size_t tag_type = AdvanceToNextTagX86Optimized(&ip, &tag);
        next = LittleEndian::Load32(old_ip);
#endif
        size_t len = len_minus_offset & 0xFF;
        ptrdiff_t extracted = ExtractOffset(next, tag_type);
        ptrdiff_t len_min_offset = len_minus_offset - extracted;
        if (SNAPPY_PREDICT_FALSE(len_minus_offset > extracted)) {
          if (SNAPPY_PREDICT_FALSE(len & 0x80)) {
            // Exceptional case (long literal or copy 4).
            // Actually doing the copy here is negatively impacting the main
            // loop due to compiler incorrectly allocating a register for
            // this fallback. Hence we just break.
          break_loop:
            ip = old_ip;
            goto exit;
          }
          // Only copy-1 or copy-2 tags can get here.
          assert(tag_type == 1 || tag_type == 2);
          std::ptrdiff_t delta = (op + deferred_length) + len_min_offset - len;
          // Guard against copies before the buffer start.
          // Execute any deferred MemCopy since we write to dst here.
          MemCopy64Tier<Tier>(op_base + op, deferred_src, deferred_length);
          op += deferred_length;
          ClearDeferred(&deferred_src, &deferred_length, safe_source);
          if (SNAPPY_PREDICT_FALSE(delta < 0 ||
                                   !Copy64BytesWithPatternExtensionTier<Tier>(
                                       op_base + op, len - len_min_offset))) {
            goto break_loop;
          }
          // We aren't deferring this copy so add length right away.
          op += len;
          continue;
        }
        std::ptrdiff_t delta = (op + deferred_length) + len_min_offset - len;
        if (SNAPPY_PREDICT_FALSE(delta < 0)) {
          // Due to the spurious offset in literals have this will trigger
          // at the start of a block when op is still smaller than 256.
          if (tag_type != 0) goto break_loop;
          MemCopy64Tier<Tier>(op_base + op, deferred_src, deferred_length);
          op += deferred_length;
          DeferMemCopy(&deferred_src, &deferred_length, old_ip, len);
          continue;
        }

        // For copies we need to copy from op_base + delta, for literals
        // we need to copy from ip instead of from the stream.
        const void* from =
            tag_type ? reinterpret_cast<void*>(op_base + delta) : old_ip;
        MemCopy64Tier<Tier>(op_base + op, deferred_src, deferred_length);
        op += deferred_length;
        DeferMemCopy(&deferred_src, &deferred_length, from, len);
      }
    } while (ip < ip_limit_min_slop &&
             static_cast<ptrdiff_t>(op + deferred_length) < op_limit_min_slop);
  exit:
    ip--;
    assert(ip <= ip_limit);
  }
  // If we deferred a copy then we can perform.  If we are up to date then we
  // might not have enough slop bytes and could run past the end.
  if (deferred_length) {
    MemCopy64Tier<Tier>(op_base + op, deferred_src, deferred_length);
    op += deferred_length;
    ClearDeferred(&deferred_src, &deferred_length, safe_source);
  }
  MaybeZeroUpperTier<Tier>();
  return {ip, op};
}

template <typename T>
inline std::pair<const uint8_t*, ptrdiff_t> DecompressBranchless(
    const uint8_t* ip, const uint8_t* ip_limit, ptrdiff_t op, T op_base,
    ptrdiff_t op_limit_min_slop) {
  return DecompressBranchlessImpl<DecompressIsaTier::kScalar>(
      ip, ip_limit, op, op_base, op_limit_min_slop);
}

// Helper class for decompression
class SnappyDecompressor {
 private:
  Source* reader_;        // Underlying source of bytes to decompress
  const char* ip_;        // Points to next buffered byte
  const char* ip_limit_;  // Points just past buffered bytes
  // If ip < ip_limit_min_maxtaglen_ it's safe to read kMaxTagLength from
  // buffer.
  const char* ip_limit_min_maxtaglen_;
  uint64_t peeked_;                  // Bytes peeked from reader (need to skip)
  bool eof_;                         // Hit end of input without an error?
  char scratch_[kMaximumTagLength];  // See RefillTag().

  // Ensure that all of the tag metadata for the next tag is available
  // in [ip_..ip_limit_-1].  Also ensures that [ip,ip+4] is readable even
  // if (ip_limit_ - ip_ < 5).
  //
  // Returns true on success, false on error or end of input.
  bool RefillTag();

  void ResetLimit(const char* ip) {
    ip_limit_min_maxtaglen_ =
        ip_limit_ - std::min<ptrdiff_t>(ip_limit_ - ip, kMaximumTagLength - 1);
  }

 public:
  explicit SnappyDecompressor(Source* reader)
      : reader_(reader), ip_(NULL), ip_limit_(NULL), peeked_(0), eof_(false) {}

  ~SnappyDecompressor() {
    // Advance past any bytes we peeked at from the reader
    reader_->Skip(peeked_);
  }

  // Returns true iff we have hit the end of the input without an error.
  bool eof() const { return eof_; }

  // Read the uncompressed length stored at the start of the compressed data.
  // On success, stores the length in *result and returns true.
  // On failure, returns false.
  bool ReadUncompressedLength(uint32_t* result) {
    assert(ip_ == NULL);  // Must not have read anything yet
    // Length is encoded in 1..5 bytes
    *result = 0;
    uint32_t shift = 0;
    while (true) {
      if (shift >= 32) return false;
      size_t n;
      const char* ip = reader_->Peek(&n);
      if (n == 0) return false;
      const unsigned char c = *(reinterpret_cast<const unsigned char*>(ip));
      reader_->Skip(1);
      uint32_t val = c & 0x7f;
      if (LeftShiftOverflows(static_cast<uint8_t>(val), shift)) return false;
      *result |= val << shift;
      if (c < 128) {
        break;
      }
      shift += 7;
    }
    return true;
  }

  template <DecompressIsaTier Tier>
  friend struct DecompressAllTagsImplStruct;

  template <class Writer>
  void DecompressAllTags_Scalar(Writer* writer);

#if SNAPPY_IS_X86_64
  template <class Writer>
  void DecompressAllTags_SSSE3(Writer* writer);

  template <class Writer>
  void DecompressAllTags_AVX2_BMI2(Writer* writer);
#endif

#if SNAPPY_IS_AARCH64
  template <class Writer>
  void DecompressAllTags_ARM_NEON(Writer* writer);
#endif

  template <class Writer>
  void DecompressAllTags(Writer* writer);
};

template <DecompressIsaTier Tier>
struct DecompressAllTagsImplStruct;

#define SNAPPY_DECOMPRESS_ALL_TAGS_BODY(Tier)                                  \
  const char* ip = decompressor->ip_;                                          \
  decompressor->ResetLimit(ip);                                                \
  auto op = writer->GetOutputPtr();                                            \
  do {                                                                         \
    uint32_t preload;                                                          \
    if (SNAPPY_PREDICT_FALSE(ip >= decompressor->ip_limit_min_maxtaglen_)) {   \
      decompressor->ip_ = ip;                                                  \
      if (SNAPPY_PREDICT_FALSE(!decompressor->RefillTag())) goto exit;         \
      ip = decompressor->ip_;                                                  \
      decompressor->ResetLimit(ip);                                            \
    }                                                                          \
    preload = static_cast<uint8_t>(*ip);                                       \
    for (;;) {                                                                 \
      {                                                                        \
        ptrdiff_t op_limit_min_slop;                                           \
        auto op_base = writer->GetBase(&op_limit_min_slop);                    \
        if (op_base) {                                                         \
          auto res = DecompressBranchlessImpl<Tier>(                           \
              reinterpret_cast<const uint8_t*>(ip),                            \
              reinterpret_cast<const uint8_t*>(decompressor->ip_limit_),       \
              op - op_base, op_base, op_limit_min_slop);                       \
          ip = reinterpret_cast<const char*>(res.first);                       \
          op = op_base + res.second;                                           \
          if (SNAPPY_PREDICT_FALSE(ip >=                                       \
                                   decompressor->ip_limit_min_maxtaglen_)) {   \
            decompressor->ip_ = ip;                                            \
            if (SNAPPY_PREDICT_FALSE(!decompressor->RefillTag())) goto exit;   \
            ip = decompressor->ip_;                                            \
            decompressor->ResetLimit(ip);                                      \
          }                                                                    \
          preload = static_cast<uint8_t>(*ip);                                 \
        }                                                                      \
      }                                                                        \
      const uint8_t c = static_cast<uint8_t>(preload);                         \
      ip++;                                                                    \
      if (SNAPPY_PREDICT_FALSE((c & 0x3) == LITERAL)) {                        \
        uint64_t literal_length = (c >> 2) + 1u;                               \
        if (writer->TryFastAppend(ip, decompressor->ip_limit_ - ip,            \
                                  static_cast<size_t>(literal_length), &op)) { \
          assert(literal_length < 61);                                         \
          ip += static_cast<size_t>(literal_length);                           \
          preload = static_cast<uint8_t>(*ip);                                 \
          continue;                                                            \
        }                                                                      \
        if (SNAPPY_PREDICT_FALSE(literal_length >= 61)) {                      \
          const size_t literal_length_length =                                 \
              static_cast<size_t>(literal_length - 60);                        \
          literal_length =                                                     \
              static_cast<uint64_t>(ExtractLowBytesTier<Tier>(                 \
                  LittleEndian::Load32(ip), literal_length_length)) +          \
              uint64_t{1};                                                     \
          ip += literal_length_length;                                         \
        }                                                                      \
        size_t avail = decompressor->ip_limit_ - ip;                           \
        while (avail < literal_length) {                                       \
          if (!writer->Append(ip, avail, &op)) goto exit;                      \
          literal_length -= avail;                                             \
          decompressor->reader_->Skip(decompressor->peeked_);                  \
          size_t n;                                                            \
          ip = decompressor->reader_->Peek(&n);                                \
          avail = n;                                                           \
          decompressor->peeked_ = avail;                                       \
          if (avail == 0) goto exit;                                           \
          decompressor->ip_limit_ = ip + avail;                                \
          decompressor->ResetLimit(ip);                                        \
        }                                                                      \
        if (!writer->Append(ip, static_cast<size_t>(literal_length), &op)) {   \
          goto exit;                                                           \
        }                                                                      \
        ip += static_cast<size_t>(literal_length);                             \
        if (SNAPPY_PREDICT_FALSE(ip >=                                         \
                                 decompressor->ip_limit_min_maxtaglen_)) {     \
          decompressor->ip_ = ip;                                              \
          if (SNAPPY_PREDICT_FALSE(!decompressor->RefillTag())) goto exit;     \
          ip = decompressor->ip_;                                              \
          decompressor->ResetLimit(ip);                                        \
        }                                                                      \
        preload = static_cast<uint8_t>(*ip);                                   \
      } else {                                                                 \
        if (SNAPPY_PREDICT_FALSE((c & 3) == COPY_4_BYTE_OFFSET)) {             \
          const size_t copy_offset = LittleEndian::Load32(ip);                 \
          const size_t length = (c >> 2) + 1;                                  \
          ip += 4;                                                             \
          if (!writer->template AppendFromSelf<Tier>(copy_offset, length,      \
                                                     &op))                     \
            goto exit;                                                         \
        } else {                                                               \
          const ptrdiff_t entry = kLengthMinusOffset[c];                       \
          preload = LittleEndian::Load32(ip);                                  \
          const uint32_t trailer = ExtractLowBytesTier<Tier>(preload, c & 3);  \
          const uint32_t length = entry & 0xff;                                \
          assert(length > 0);                                                  \
          const uint32_t copy_offset = trailer - entry + length;               \
          if (!writer->template AppendFromSelf<Tier>(copy_offset, length,      \
                                                     &op))                     \
            goto exit;                                                         \
          ip += (c & 3);                                                       \
          preload >>= (c & 3) * 8;                                             \
          if (ip < decompressor->ip_limit_min_maxtaglen_) continue;            \
        }                                                                      \
        if (SNAPPY_PREDICT_FALSE(ip >=                                         \
                                 decompressor->ip_limit_min_maxtaglen_)) {     \
          decompressor->ip_ = ip;                                              \
          if (SNAPPY_PREDICT_FALSE(!decompressor->RefillTag())) goto exit;     \
          ip = decompressor->ip_;                                              \
          decompressor->ResetLimit(ip);                                        \
        }                                                                      \
        preload = static_cast<uint8_t>(*ip);                                   \
      }                                                                        \
    }                                                                          \
  } while (false);                                                             \
  exit:                                                                        \
  writer->SetOutputPtr(op);                                                    \
  MaybeZeroUpperTier<Tier>();

template <>
struct DecompressAllTagsImplStruct<DecompressIsaTier::kScalar> {
  template <class Writer>
  SNAPPY_ATTRIBUTE_ALWAYS_INLINE static void Run(
      SnappyDecompressor* decompressor, Writer* writer) {
    SNAPPY_DECOMPRESS_ALL_TAGS_BODY(DecompressIsaTier::kScalar)
  }
};

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
template <>
struct DecompressAllTagsImplStruct<DecompressIsaTier::kX86_SSSE3> {
  template <class Writer>
  SNAPPY_ATTRIBUTE_ALWAYS_INLINE static void Run(
      SnappyDecompressor* decompressor, Writer* writer) {
    SNAPPY_DECOMPRESS_ALL_TAGS_BODY(DecompressIsaTier::kX86_SSSE3)
  }
};
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <>
struct DecompressAllTagsImplStruct<DecompressIsaTier::kX86_AVX2_BMI2> {
  template <class Writer>
  SNAPPY_ATTRIBUTE_ALWAYS_INLINE static void Run(
      SnappyDecompressor* decompressor, Writer* writer) {
    SNAPPY_DECOMPRESS_ALL_TAGS_BODY(DecompressIsaTier::kX86_AVX2_BMI2)
  }
};
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
template <>
struct DecompressAllTagsImplStruct<DecompressIsaTier::kARM_NEON> {
  template <class Writer>
  SNAPPY_ATTRIBUTE_ALWAYS_INLINE static void Run(
      SnappyDecompressor* decompressor, Writer* writer) {
    SNAPPY_DECOMPRESS_ALL_TAGS_BODY(DecompressIsaTier::kARM_NEON)
  }
};
#endif  // SNAPPY_IS_AARCH64

#undef SNAPPY_DECOMPRESS_ALL_TAGS_BODY

template <class Writer>
SNAPPY_ATTRIBUTE_NOINLINE void SnappyDecompressor::DecompressAllTags_Scalar(
    Writer* writer) {
  DecompressAllTagsImplStruct<DecompressIsaTier::kScalar>::Run(this, writer);
}

#if SNAPPY_IS_X86_64
SNAPPY_TARGET_SSSE3
template <class Writer>
SNAPPY_ATTRIBUTE_NOINLINE void SnappyDecompressor::DecompressAllTags_SSSE3(
    Writer* writer) {
  DecompressAllTagsImplStruct<DecompressIsaTier::kX86_SSSE3>::Run(this, writer);
}
SNAPPY_UNTARGET_REGION

SNAPPY_TARGET_AVX2_BMI2
template <class Writer>
SNAPPY_ATTRIBUTE_NOINLINE void SnappyDecompressor::DecompressAllTags_AVX2_BMI2(
    Writer* writer) {
  DecompressAllTagsImplStruct<DecompressIsaTier::kX86_AVX2_BMI2>::Run(this,
                                                                      writer);
}
SNAPPY_UNTARGET_REGION
#endif  // SNAPPY_IS_X86_64

#if SNAPPY_IS_AARCH64
template <class Writer>
SNAPPY_ATTRIBUTE_NOINLINE void SnappyDecompressor::DecompressAllTags_ARM_NEON(
    Writer* writer) {
  DecompressAllTagsImplStruct<DecompressIsaTier::kARM_NEON>::Run(this, writer);
}
#endif  // SNAPPY_IS_AARCH64

template <class Writer>
void SnappyDecompressor::DecompressAllTags(Writer* writer) {
  const uint32_t mask = internal::GetSupportedArchitectures();
#if SNAPPY_IS_X86_64
  if ((mask & internal::kIsaX86_AVX2Tier) == internal::kIsaX86_AVX2Tier) {
    DecompressAllTags_AVX2_BMI2(writer);
    return;
  }
  if ((mask & internal::kIsaSSSE3) != 0) {
    DecompressAllTags_SSSE3(writer);
    return;
  }
#elif SNAPPY_IS_AARCH64
  if ((mask & internal::kIsaNEON) != 0) {
    DecompressAllTags_ARM_NEON(writer);
    return;
  }
#else
  (void)mask;
#endif
  DecompressAllTags_Scalar(writer);
}

constexpr uint32_t CalculateNeeded(uint8_t tag) {
  return ((tag & 3) == 0 && tag >= (60 * 4))
             ? (tag >> 2) - 58
             : (0x05030201 >> ((tag * 8) & 31)) & 0xFF;
}

#if __cplusplus >= 201402L
constexpr bool VerifyCalculateNeeded() {
  for (int i = 0; i < 1; i++) {
    if (CalculateNeeded(i) != static_cast<uint32_t>((char_table[i] >> 11)) + 1)
      return false;
  }
  return true;
}

// Make sure CalculateNeeded is correct by verifying it against the established
// table encoding the number of added bytes needed.
static_assert(VerifyCalculateNeeded(), "");
#endif  // c++14

bool SnappyDecompressor::RefillTag() {
  const char* ip = ip_;
  if (ip == ip_limit_) {
    // Fetch a new fragment from the reader
    reader_->Skip(peeked_);  // All peeked bytes are used up
    size_t n;
    ip = reader_->Peek(&n);
    peeked_ = n;
    eof_ = (n == 0);
    if (eof_) return false;
    ip_limit_ = ip + n;
  }

  // Read the tag character
  assert(ip < ip_limit_);
  const unsigned char c = *(reinterpret_cast<const unsigned char*>(ip));
  // At this point make sure that the data for the next tag is consecutive.
  // For copy 1 this means the next 2 bytes (tag and 1 byte offset)
  // For copy 2 the next 3 bytes (tag and 2 byte offset)
  // For copy 4 the next 5 bytes (tag and 4 byte offset)
  // For all small literals we only need 1 byte buf for literals 60...63 the
  // length is encoded in 1...4 extra bytes.
  const uint32_t needed = CalculateNeeded(c);
  assert(needed <= sizeof(scratch_));

  // Read more bytes from reader if needed
  uint64_t nbuf = ip_limit_ - ip;
  if (nbuf < needed) {
    // Stitch together bytes from ip and reader to form the word
    // contents.  We store the needed bytes in "scratch_".  They
    // will be consumed immediately by the caller since we do not
    // read more than we need.
    std::memmove(scratch_, ip, nbuf);
    reader_->Skip(peeked_);  // All peeked bytes are used up
    peeked_ = 0;
    while (nbuf < needed) {
      size_t length;
      const char* src = reader_->Peek(&length);
      if (length == 0) return false;
      uint64_t to_add = std::min<uint64_t>(needed - nbuf, length);
      std::memcpy(scratch_ + nbuf, src, to_add);
      nbuf += to_add;
      reader_->Skip(to_add);
    }
    assert(nbuf == needed);
    ip_ = scratch_;
    ip_limit_ = scratch_ + needed;
  } else if (nbuf < kMaximumTagLength) {
    // Have enough bytes, but move into scratch_ so that we do not
    // read past end of input
    std::memmove(scratch_, ip, nbuf);
    reader_->Skip(peeked_);  // All peeked bytes are used up
    peeked_ = 0;
    ip_ = scratch_;
    ip_limit_ = scratch_ + nbuf;
  } else {
    // Pass pointer to buffer returned by reader_.
    ip_ = ip;
  }
  return true;
}

template <typename Writer>
static bool InternalUncompress(Source* r, Writer* writer) {
  // Read the uncompressed length from the front of the compressed input
  SnappyDecompressor decompressor(r);
  uint32_t uncompressed_len = 0;
  if (!decompressor.ReadUncompressedLength(&uncompressed_len)) return false;

  return InternalUncompressAllTags(&decompressor, writer, r->Available(),
                                   uncompressed_len);
}

template <typename Writer>
static bool InternalUncompressAllTags(SnappyDecompressor* decompressor,
                                      Writer* writer, uint32_t compressed_len,
                                      uint32_t uncompressed_len) {
  int token = 0;

  writer->SetExpectedLength(uncompressed_len);

  // Process the entire input
  decompressor->DecompressAllTags(writer);
  writer->Flush();
  Report(token, "snappy_uncompress", compressed_len, uncompressed_len);
  return (decompressor->eof() && writer->CheckLength());
}

bool GetUncompressedLength(Source* source, uint32_t* result) {
  SnappyDecompressor decompressor(source);
  return decompressor.ReadUncompressedLength(result);
}

size_t Compress(Source* reader, Sink* writer) {
  return Compress(reader, writer, CompressionOptions{});
}

static size_t InternalCompress(Source* reader, Sink* writer,
                               CompressionOptions options,
                               internal::WorkingMemory* wmem) {
  assert(options.level == 1 || options.level == 2);
  size_t written = 0;
  size_t N = reader->Available();
  // The uncompressed length is a 32-bit varint in the stream format.
  if (static_cast<uint64_t>(N) > std::numeric_limits<uint32_t>::max()) {
    return 0;
  }
  char ulength[Varint::kMax32];
  char* p = Varint::Encode32(ulength, N);
  writer->Append(ulength, p - ulength);
  written += (p - ulength);

  while (N > 0) {
    // Get next block to compress (without copying if possible)
    size_t fragment_size;
    const char* fragment = reader->Peek(&fragment_size);
    assert(fragment_size != 0);  // premature end of input
    const size_t num_to_read = std::min(N, kBlockSize);
    size_t bytes_read = fragment_size;

    size_t pending_advance = 0;
    if (bytes_read >= num_to_read) {
      // Buffer returned by reader is large enough
      pending_advance = num_to_read;
      fragment_size = num_to_read;
    } else {
      char* scratch = wmem->GetScratchInput();
      std::memcpy(scratch, fragment, bytes_read);
      reader->Skip(bytes_read);

      while (bytes_read < num_to_read) {
        fragment = reader->Peek(&fragment_size);
        size_t n = std::min<size_t>(fragment_size, num_to_read - bytes_read);
        std::memcpy(scratch + bytes_read, fragment, n);
        bytes_read += n;
        reader->Skip(n);
      }
      assert(bytes_read == num_to_read);
      fragment = scratch;
      fragment_size = num_to_read;
    }
    assert(fragment_size == num_to_read);

    // Get encoding table for compression
    int table_size;
    uint16_t* table = wmem->GetHashTable(num_to_read, &table_size);

    // Compress input_fragment and append to dest
    int max_output = MaxCompressedLength(num_to_read);

    // Since we encode kBlockSize regions followed by a region
    // which is <= kBlockSize in length, a previously allocated
    // scratch_output[] region is big enough for this iteration.
    // Need a scratch buffer for the output, in case the byte sink doesn't
    // have room for us directly.
    char* dest = writer->GetAppendBuffer(max_output, wmem->GetScratchOutput());
    char* end = nullptr;
    if (options.level == 1) {
      end = internal::CompressFragment(fragment, fragment_size, dest, table,
                                       table_size);
    } else if (options.level == 2) {
      end = internal::CompressFragmentDoubleHash(
          fragment, fragment_size, dest, table, table_size >> 1,
          table + (table_size >> 1), table_size >> 1);
    }

    writer->Append(dest, end - dest);
    written += (end - dest);

    N -= num_to_read;
    reader->Skip(pending_advance);
  }
  return written;
}

size_t Compress(Source* reader, Sink* writer, CompressionOptions options) {
  internal::WorkingMemory wmem(reader->Available());
  return InternalCompress(reader, writer, options, &wmem);
}

size_t Compress(Source* reader, Sink* writer, CompressionOptions options,
                CompressionContext* ctx) {
  assert(ctx != nullptr);
  assert(ctx->working_memory_ != nullptr);
  return InternalCompress(reader, writer, options, ctx->working_memory_);
}

CompressionContext::CompressionContext()
    : working_memory_(new internal::WorkingMemory(kBlockSize)),
      owns_working_memory_(true) {}

size_t CompressionContext::WorkspaceSize() {
  return sizeof(internal::WorkingMemory) +
         internal::WorkingMemory::RequiredSize(kBlockSize);
}

CompressionContext::CompressionContext(void* workspace, size_t workspace_size)
    : owns_working_memory_(false) {
  assert(workspace != nullptr);
  assert(workspace_size >= WorkspaceSize());
  assert(reinterpret_cast<uintptr_t>(workspace) %
             alignof(internal::WorkingMemory) ==
         0);
  (void)workspace_size;
  char* base = static_cast<char*>(workspace);
  working_memory_ = new (base) internal::WorkingMemory(
      kBlockSize, base + sizeof(internal::WorkingMemory));
}

void CompressionContext::Reset() {
  if (working_memory_ == nullptr) return;
  if (owns_working_memory_) {
    delete working_memory_;
  } else {
    working_memory_->~WorkingMemory();
  }
  working_memory_ = nullptr;
}

CompressionContext::~CompressionContext() { Reset(); }

CompressionContext::CompressionContext(CompressionContext&& other) noexcept
    : working_memory_(other.working_memory_),
      owns_working_memory_(other.owns_working_memory_) {
  other.working_memory_ = nullptr;
}

CompressionContext& CompressionContext::operator=(
    CompressionContext&& other) noexcept {
  if (this != &other) {
    Reset();
    working_memory_ = other.working_memory_;
    owns_working_memory_ = other.owns_working_memory_;
    other.working_memory_ = nullptr;
  }
  return *this;
}

// -----------------------------------------------------------------------
// IOVec interfaces
// -----------------------------------------------------------------------

// A `Source` implementation that yields the contents of an `iovec` array. Note
// that `total_size` is the total number of bytes to be read from the elements
// of `iov` (_not_ the total number of elements in `iov`).
class SnappyIOVecReader : public Source {
 public:
  SnappyIOVecReader(const struct iovec* iov, size_t total_size)
      : curr_iov_(iov),
        curr_pos_(total_size > 0 ? reinterpret_cast<const char*>(iov->iov_base)
                                 : nullptr),
        curr_size_remaining_(total_size > 0 ? iov->iov_len : 0),
        total_size_remaining_(total_size) {
    // Skip empty leading `iovec`s.
    if (total_size > 0 && curr_size_remaining_ == 0) Advance();
  }

  ~SnappyIOVecReader() override = default;

  size_t Available() const override { return total_size_remaining_; }

  const char* Peek(size_t* len) override {
    *len = curr_size_remaining_;
    return curr_pos_;
  }

  void Skip(size_t n) override {
    while (n >= curr_size_remaining_ && n > 0) {
      n -= curr_size_remaining_;
      Advance();
    }
    curr_size_remaining_ -= n;
    total_size_remaining_ -= n;
    curr_pos_ += n;
  }

 private:
  // Advances to the next nonempty `iovec` and updates related variables.
  void Advance() {
    do {
      assert(total_size_remaining_ >= curr_size_remaining_);
      total_size_remaining_ -= curr_size_remaining_;
      if (total_size_remaining_ == 0) {
        curr_pos_ = nullptr;
        curr_size_remaining_ = 0;
        return;
      }
      ++curr_iov_;
      curr_pos_ = reinterpret_cast<const char*>(curr_iov_->iov_base);
      curr_size_remaining_ = curr_iov_->iov_len;
    } while (curr_size_remaining_ == 0);
  }

  // The `iovec` currently being read.
  const struct iovec* curr_iov_;
  // The location in `curr_iov_` currently being read.
  const char* curr_pos_;
  // The amount of unread data in `curr_iov_`.
  size_t curr_size_remaining_;
  // The amount of unread data in the entire input array.
  size_t total_size_remaining_;
};

// A type that writes to an iovec.
// Note that this is not a "ByteSink", but a type that matches the
// Writer template argument to SnappyDecompressor::DecompressAllTags().
class SnappyIOVecWriter {
 private:
  // output_iov_end_ is set to iov + count and used to determine when
  // the end of the iovs is reached.
  const struct iovec* output_iov_end_;

#if !defined(NDEBUG)
  const struct iovec* output_iov_;
#endif  // !defined(NDEBUG)

  // Current iov that is being written into.
  const struct iovec* curr_iov_;

  // Pointer to current iov's write location.
  char* curr_iov_output_;

  // Remaining bytes to write into curr_iov_output.
  size_t curr_iov_remaining_;

  // Total bytes decompressed into output_iov_ so far.
  size_t total_written_;

  // Maximum number of bytes that will be decompressed into output_iov_.
  size_t output_limit_;

  static inline char* GetIOVecPointer(const struct iovec* iov, size_t offset) {
    return reinterpret_cast<char*>(iov->iov_base) + offset;
  }

 public:
  // Does not take ownership of iov. iov must be valid during the
  // entire lifetime of the SnappyIOVecWriter.
  inline SnappyIOVecWriter(const struct iovec* iov, size_t iov_count)
      : output_iov_end_(iov + iov_count),
#if !defined(NDEBUG)
        output_iov_(iov),
#endif  // !defined(NDEBUG)
        curr_iov_(iov),
        curr_iov_output_(iov_count ? reinterpret_cast<char*>(iov->iov_base)
                                   : nullptr),
        curr_iov_remaining_(iov_count ? iov->iov_len : 0),
        total_written_(0),
        output_limit_(-1) {
  }

  inline void SetExpectedLength(size_t len) { output_limit_ = len; }

  inline bool CheckLength() const { return total_written_ == output_limit_; }

  inline bool Append(const char* ip, size_t len, char**) {
    if (total_written_ + len > output_limit_) {
      return false;
    }

    return AppendNoCheck(ip, len);
  }

  char* GetOutputPtr() { return nullptr; }
  char* GetBase(ptrdiff_t*) { return nullptr; }
  void SetOutputPtr(char* op) {
    // TODO: Switch to [[maybe_unused]] when we can assume C++17.
    (void)op;
  }

  inline bool AppendNoCheck(const char* ip, size_t len) {
    while (len > 0) {
      if (curr_iov_remaining_ == 0) {
        // This iovec is full. Go to the next one.
        if (curr_iov_ + 1 >= output_iov_end_) {
          return false;
        }
        ++curr_iov_;
        curr_iov_output_ = reinterpret_cast<char*>(curr_iov_->iov_base);
        curr_iov_remaining_ = curr_iov_->iov_len;
      }

      const size_t to_write = std::min(len, curr_iov_remaining_);
      std::memcpy(curr_iov_output_, ip, to_write);
      curr_iov_output_ += to_write;
      curr_iov_remaining_ -= to_write;
      total_written_ += to_write;
      ip += to_write;
      len -= to_write;
    }

    return true;
  }

  inline bool TryFastAppend(const char* ip, size_t available, size_t len,
                            char**) {
    const size_t space_left = output_limit_ - total_written_;
    if (len <= 16 && available >= 16 + kMaximumTagLength && space_left >= 16 &&
        curr_iov_remaining_ >= 16) {
      // Fast path, used for the majority (about 95%) of invocations.
      UnalignedCopy128(ip, curr_iov_output_);
      curr_iov_output_ += len;
      curr_iov_remaining_ -= len;
      total_written_ += len;
      return true;
    }

    return false;
  }

  template <DecompressIsaTier Tier = DecompressIsaTier::kScalar>
  inline bool AppendFromSelf(size_t offset, size_t len, char**) {
    // See SnappyArrayWriter::AppendFromSelf for an explanation of
    // the "offset - 1u" trick.
    if (offset - 1u >= total_written_) {
      return false;
    }
    const size_t space_left = output_limit_ - total_written_;
    if (len > space_left) {
      return false;
    }

    // Locate the iovec from which we need to start the copy.
    const iovec* from_iov = curr_iov_;
    size_t from_iov_offset = curr_iov_->iov_len - curr_iov_remaining_;
    while (offset > 0) {
      if (from_iov_offset >= offset) {
        from_iov_offset -= offset;
        break;
      }

      offset -= from_iov_offset;
      --from_iov;
#if !defined(NDEBUG)
      assert(from_iov >= output_iov_);
#endif  // !defined(NDEBUG)
      from_iov_offset = from_iov->iov_len;
    }

    // Copy <len> bytes starting from the iovec pointed to by from_iov_index to
    // the current iovec.
    while (len > 0) {
      assert(from_iov <= curr_iov_);
      if (from_iov != curr_iov_) {
        const size_t to_copy =
            std::min(from_iov->iov_len - from_iov_offset, len);
        AppendNoCheck(GetIOVecPointer(from_iov, from_iov_offset), to_copy);
        len -= to_copy;
        if (len > 0) {
          ++from_iov;
          from_iov_offset = 0;
        }
      } else {
        size_t to_copy = curr_iov_remaining_;
        if (to_copy == 0) {
          // This iovec is full. Go to the next one.
          if (curr_iov_ + 1 >= output_iov_end_) {
            return false;
          }
          ++curr_iov_;
          curr_iov_output_ = reinterpret_cast<char*>(curr_iov_->iov_base);
          curr_iov_remaining_ = curr_iov_->iov_len;
          continue;
        }
        if (to_copy > len) {
          to_copy = len;
        }
        assert(to_copy > 0);

        IncrementalCopyTier<Tier>(GetIOVecPointer(from_iov, from_iov_offset),
                                  curr_iov_output_, curr_iov_output_ + to_copy,
                                  curr_iov_output_ + curr_iov_remaining_);
        curr_iov_output_ += to_copy;
        curr_iov_remaining_ -= to_copy;
        from_iov_offset += to_copy;
        total_written_ += to_copy;
        len -= to_copy;
      }
    }

    return true;
  }

  inline void Flush() {}
};

bool RawUncompressToIOVec(const char* compressed, size_t compressed_length,
                          const struct iovec* iov, size_t iov_cnt) {
  ByteArraySource reader(compressed, compressed_length);
  return RawUncompressToIOVec(&reader, iov, iov_cnt);
}

bool RawUncompressToIOVec(Source* compressed, const struct iovec* iov,
                          size_t iov_cnt) {
  SnappyIOVecWriter output(iov, iov_cnt);
  return InternalUncompress(compressed, &output);
}

// -----------------------------------------------------------------------
// Flat array interfaces
// -----------------------------------------------------------------------

// A type that writes to a flat array.
// Note that this is not a "ByteSink", but a type that matches the
// Writer template argument to SnappyDecompressor::DecompressAllTags().
class SnappyArrayWriter {
 private:
  char* base_;
  char* op_;
  char* op_limit_;
  // If op < op_limit_min_slop_ then it's safe to unconditionally write
  // kSlopBytes starting at op.
  char* op_limit_min_slop_;

 public:
  inline explicit SnappyArrayWriter(char* dst)
      : base_(dst),
        op_(dst),
        op_limit_(dst),
        op_limit_min_slop_(dst) {}  // Safe default see invariant.

  inline void SetExpectedLength(size_t len) {
    op_limit_ = op_ + len;
    // Prevent pointer from being past the buffer.
    op_limit_min_slop_ = op_limit_ - std::min<size_t>(kSlopBytes - 1, len);
  }

  inline bool CheckLength() const { return op_ == op_limit_; }

  char* GetOutputPtr() { return op_; }
  char* GetBase(ptrdiff_t* op_limit_min_slop) {
    *op_limit_min_slop = op_limit_min_slop_ - base_;
    return base_;
  }
  void SetOutputPtr(char* op) { op_ = op; }

  inline bool Append(const char* ip, size_t len, char** op_p) {
    char* op = *op_p;
    const size_t space_left = op_limit_ - op;
    if (space_left < len) return false;
    std::memcpy(op, ip, len);
    *op_p = op + len;
    return true;
  }

  inline bool TryFastAppend(const char* ip, size_t available, size_t len,
                            char** op_p) {
    char* op = *op_p;
    const size_t space_left = op_limit_ - op;
    if (len <= 16 && available >= 16 + kMaximumTagLength && space_left >= 16) {
      // Fast path, used for the majority (about 95%) of invocations.
      UnalignedCopy128(ip, op);
      *op_p = op + len;
      return true;
    } else {
      return false;
    }
  }

  template <DecompressIsaTier Tier = DecompressIsaTier::kScalar>
  SNAPPY_ATTRIBUTE_ALWAYS_INLINE inline bool AppendFromSelf(size_t offset,
                                                            size_t len,
                                                            char** op_p) {
    assert(len > 0);
    char* const op = *op_p;
    assert(op >= base_);
    char* const op_end = op + len;

    // Check if we try to append from before the start of the buffer.
    if (SNAPPY_PREDICT_FALSE(static_cast<size_t>(op - base_) < offset))
      return false;

    if (SNAPPY_PREDICT_FALSE((kSlopBytes < 64 && len > kSlopBytes) ||
                             op >= op_limit_min_slop_ || offset < len)) {
      if (op_end > op_limit_ || offset == 0) return false;
      *op_p = IncrementalCopyTier<Tier>(op - offset, op, op_end, op_limit_);
      return true;
    }
    std::memmove(op, op - offset, kSlopBytes);
    *op_p = op_end;
    return true;
  }
  inline size_t Produced() const {
    assert(op_ >= base_);
    return op_ - base_;
  }
  inline void Flush() {}
};

bool RawUncompress(const char* compressed, size_t compressed_length,
                   char* uncompressed) {
  ByteArraySource reader(compressed, compressed_length);
  return RawUncompress(&reader, uncompressed);
}

bool RawUncompress(Source* compressed, char* uncompressed) {
  SnappyArrayWriter output(uncompressed);
  return InternalUncompress(compressed, &output);
}

bool Uncompress(const char* compressed, size_t compressed_length,
                std::string* uncompressed) {
  size_t ulength;
  if (!GetUncompressedLength(compressed, compressed_length, &ulength)) {
    return false;
  }
  // On 32-bit builds: max_size() < kuint32max.  Check for that instead
  // of crashing (e.g., consider externally specified compressed data).
  if (ulength > uncompressed->max_size()) {
    return false;
  }
  STLStringResizeUninitialized(uncompressed, ulength);
  return RawUncompress(compressed, compressed_length,
                       string_as_array(uncompressed));
}

// A Writer that drops everything on the floor and just does validation
class SnappyDecompressionValidator {
 private:
  size_t expected_;
  size_t produced_;

 public:
  inline SnappyDecompressionValidator() : expected_(0), produced_(0) {}
  inline void SetExpectedLength(size_t len) { expected_ = len; }
  size_t GetOutputPtr() { return produced_; }
  size_t GetBase(ptrdiff_t* op_limit_min_slop) {
    *op_limit_min_slop = std::numeric_limits<ptrdiff_t>::max() - kSlopBytes + 1;
    return 1;
  }
  void SetOutputPtr(size_t op) { produced_ = op; }
  inline bool CheckLength() const { return expected_ == produced_; }
  inline bool Append(const char* ip, size_t len, size_t* produced) {
    // TODO: Switch to [[maybe_unused]] when we can assume C++17.
    (void)ip;

    *produced += len;
    return *produced <= expected_;
  }
  inline bool TryFastAppend(const char* ip, size_t available, size_t length,
                            size_t* produced) {
    // TODO: Switch to [[maybe_unused]] when we can assume C++17.
    (void)ip;
    (void)available;
    (void)length;
    (void)produced;

    return false;
  }
  template <DecompressIsaTier Tier = DecompressIsaTier::kScalar>
  inline bool AppendFromSelf(size_t offset, size_t len, size_t* produced) {
    // See SnappyArrayWriter::AppendFromSelf for an explanation of
    // the "offset - 1u" trick.
    if (*produced <= offset - 1u) return false;
    *produced += len;
    return *produced <= expected_;
  }
  inline void Flush() {}
};

bool IsValidCompressedBuffer(const char* compressed, size_t compressed_length) {
  ByteArraySource reader(compressed, compressed_length);
  SnappyDecompressionValidator writer;
  return InternalUncompress(&reader, &writer);
}

bool IsValidCompressed(Source* compressed) {
  SnappyDecompressionValidator writer;
  return InternalUncompress(compressed, &writer);
}

void RawCompress(const char* input, size_t input_length, char* compressed,
                 size_t* compressed_length) {
  RawCompress(input, input_length, compressed, compressed_length,
              CompressionOptions{});
}

void RawCompress(const char* input, size_t input_length, char* compressed,
                 size_t* compressed_length, CompressionOptions options) {
  ByteArraySource reader(input, input_length);
  UncheckedByteArraySink writer(compressed);
  Compress(&reader, &writer, options);

  // Compute how many bytes were added
  *compressed_length = (writer.CurrentDestination() - compressed);
}

void RawCompressFromIOVec(const struct iovec* iov, size_t uncompressed_length,
                          char* compressed, size_t* compressed_length) {
  RawCompressFromIOVec(iov, uncompressed_length, compressed, compressed_length,
                       CompressionOptions{});
}

void RawCompressFromIOVec(const struct iovec* iov, size_t uncompressed_length,
                          char* compressed, size_t* compressed_length,
                          CompressionOptions options) {
  SnappyIOVecReader reader(iov, uncompressed_length);
  UncheckedByteArraySink writer(compressed);
  Compress(&reader, &writer, options);

  // Compute how many bytes were added.
  *compressed_length = writer.CurrentDestination() - compressed;
}

void RawCompress(const char* input, size_t input_length, char* compressed,
                 size_t* compressed_length, CompressionOptions options,
                 CompressionContext* ctx) {
  ByteArraySource reader(input, input_length);
  UncheckedByteArraySink writer(compressed);
  Compress(&reader, &writer, options, ctx);

  // Compute how many bytes were added
  *compressed_length = (writer.CurrentDestination() - compressed);
}

size_t Compress(const char* input, size_t input_length,
                std::string* compressed) {
  return Compress(input, input_length, compressed, CompressionOptions{});
}

size_t Compress(const char* input, size_t input_length, std::string* compressed,
                CompressionOptions options) {
  // Pre-grow the buffer to the max length of the compressed output
  STLStringResizeUninitialized(compressed, MaxCompressedLength(input_length));

  size_t compressed_length;
  RawCompress(input, input_length, string_as_array(compressed),
              &compressed_length, options);
  compressed->erase(compressed_length);
  return compressed_length;
}

size_t CompressFromIOVec(const struct iovec* iov, size_t iov_cnt,
                         std::string* compressed) {
  return CompressFromIOVec(iov, iov_cnt, compressed, CompressionOptions{});
}

size_t CompressFromIOVec(const struct iovec* iov, size_t iov_cnt,
                         std::string* compressed, CompressionOptions options) {
  // Compute the number of bytes to be compressed.
  size_t uncompressed_length = 0;
  for (size_t i = 0; i < iov_cnt; ++i) {
    uncompressed_length += iov[i].iov_len;
  }

  // Pre-grow the buffer to the max length of the compressed output.
  STLStringResizeUninitialized(compressed,
                               MaxCompressedLength(uncompressed_length));

  size_t compressed_length;
  RawCompressFromIOVec(iov, uncompressed_length, string_as_array(compressed),
                       &compressed_length, options);
  compressed->erase(compressed_length);
  return compressed_length;
}

// -----------------------------------------------------------------------
// Sink interface
// -----------------------------------------------------------------------

// A type that decompresses into a Sink. The template parameter
// Allocator must export one method "char* Allocate(int size);", which
// allocates a buffer of "size" and appends that to the destination.
template <typename Allocator>
class SnappyScatteredWriter {
  Allocator allocator_;

  // We need random access into the data generated so far.  Therefore
  // we keep track of all of the generated data as an array of blocks.
  // All of the blocks except the last have length kBlockSize.
  std::vector<char*> blocks_;
  size_t expected_;

  // Total size of all fully generated blocks so far
  size_t full_size_;

  // Pointer into current output block
  char* op_base_;   // Base of output block
  char* op_ptr_;    // Pointer to next unfilled byte in block
  char* op_limit_;  // Pointer just past block
  // If op < op_limit_min_slop_ then it's safe to unconditionally write
  // kSlopBytes starting at op.
  char* op_limit_min_slop_;

  inline size_t Size() const { return full_size_ + (op_ptr_ - op_base_); }

  bool SlowAppend(const char* ip, size_t len);
  bool SlowAppendFromSelf(size_t offset, size_t len);

 public:
  inline explicit SnappyScatteredWriter(const Allocator& allocator)
      : allocator_(allocator),
        full_size_(0),
        op_base_(NULL),
        op_ptr_(NULL),
        op_limit_(NULL),
        op_limit_min_slop_(NULL) {}
  char* GetOutputPtr() { return op_ptr_; }
  char* GetBase(ptrdiff_t* op_limit_min_slop) {
    *op_limit_min_slop = op_limit_min_slop_ - op_base_;
    return op_base_;
  }
  void SetOutputPtr(char* op) { op_ptr_ = op; }

  inline void SetExpectedLength(size_t len) {
    assert(blocks_.empty());
    expected_ = len;
  }

  inline bool CheckLength() const { return Size() == expected_; }

  // Return the number of bytes actually uncompressed so far
  inline size_t Produced() const { return Size(); }

  inline bool Append(const char* ip, size_t len, char** op_p) {
    char* op = *op_p;
    size_t avail = op_limit_ - op;
    if (len <= avail) {
      // Fast path
      std::memcpy(op, ip, len);
      *op_p = op + len;
      return true;
    } else {
      op_ptr_ = op;
      bool res = SlowAppend(ip, len);
      *op_p = op_ptr_;
      return res;
    }
  }

  inline bool TryFastAppend(const char* ip, size_t available, size_t length,
                            char** op_p) {
    char* op = *op_p;
    const int space_left = op_limit_ - op;
    if (length <= 16 && available >= 16 + kMaximumTagLength &&
        space_left >= 16) {
      // Fast path, used for the majority (about 95%) of invocations.
      UnalignedCopy128(ip, op);
      *op_p = op + length;
      return true;
    } else {
      return false;
    }
  }

  template <DecompressIsaTier Tier = DecompressIsaTier::kScalar>
  inline bool AppendFromSelf(size_t offset, size_t len, char** op_p) {
    char* op = *op_p;
    assert(op >= op_base_);
    // Check if we try to append from before the start of the buffer.
    if (SNAPPY_PREDICT_FALSE((kSlopBytes < 64 && len > kSlopBytes) ||
                             static_cast<size_t>(op - op_base_) < offset ||
                             op >= op_limit_min_slop_ || offset < len)) {
      if (offset == 0) return false;
      if (SNAPPY_PREDICT_FALSE(static_cast<size_t>(op - op_base_) < offset ||
                               op + len > op_limit_)) {
        op_ptr_ = op;
        bool res = SlowAppendFromSelf(offset, len);
        *op_p = op_ptr_;
        return res;
      }
      *op_p = IncrementalCopyTier<Tier>(op - offset, op, op + len, op_limit_);
      return true;
    }
    // Fast path
    char* const op_end = op + len;
    std::memmove(op, op - offset, kSlopBytes);
    *op_p = op_end;
    return true;
  }

  // Called at the end of the decompress. We ask the allocator
  // write all blocks to the sink.
  inline void Flush() { allocator_.Flush(Produced()); }
};

template <typename Allocator>
bool SnappyScatteredWriter<Allocator>::SlowAppend(const char* ip, size_t len) {
  size_t avail = op_limit_ - op_ptr_;
  while (len > avail) {
    // Completely fill this block
    std::memcpy(op_ptr_, ip, avail);
    op_ptr_ += avail;
    assert(op_limit_ - op_ptr_ == 0);
    full_size_ += (op_ptr_ - op_base_);
    len -= avail;
    ip += avail;

    // Bounds check
    if (full_size_ + len > expected_) return false;

    // Make new block
    size_t bsize = std::min<size_t>(kBlockSize, expected_ - full_size_);
    op_base_ = allocator_.Allocate(bsize);
    op_ptr_ = op_base_;
    op_limit_ = op_base_ + bsize;
    op_limit_min_slop_ = op_limit_ - std::min<size_t>(kSlopBytes - 1, bsize);

    blocks_.push_back(op_base_);
    avail = bsize;
  }

  std::memcpy(op_ptr_, ip, len);
  op_ptr_ += len;
  return true;
}

template <typename Allocator>
bool SnappyScatteredWriter<Allocator>::SlowAppendFromSelf(size_t offset,
                                                          size_t len) {
  // Overflow check
  // See SnappyArrayWriter::AppendFromSelf for an explanation of
  // the "offset - 1u" trick.
  const size_t cur = Size();
  if (offset - 1u >= cur) return false;
  if (expected_ - cur < len) return false;

  // Currently we shouldn't ever hit this path because Compress() chops the
  // input into blocks and does not create cross-block copies. However, it is
  // nice if we do not rely on that, since we can get better compression if we
  // allow cross-block copies and thus might want to change the compressor in
  // the future.
  // TODO Replace this with a properly optimized path. This is not
  // triggered right now. But this is so super slow, that it would regress
  // performance unacceptably if triggered.
  size_t src = cur - offset;
  char* op = op_ptr_;
  while (len-- > 0) {
    char c = blocks_[src >> kBlockLog][src & (kBlockSize - 1)];
    if (!Append(&c, 1, &op)) {
      op_ptr_ = op;
      return false;
    }
    src++;
  }
  op_ptr_ = op;
  return true;
}

class SnappySinkAllocator {
 public:
  explicit SnappySinkAllocator(Sink* dest) : dest_(dest) {}

  char* Allocate(int size) {
    Datablock block(new char[size], size);
    blocks_.push_back(block);
    return block.data;
  }

  // We flush only at the end, because the writer wants
  // random access to the blocks and once we hand the
  // block over to the sink, we can't access it anymore.
  // Also we don't write more than has been actually written
  // to the blocks.
  void Flush(size_t size) {
    size_t size_written = 0;
    for (Datablock& block : blocks_) {
      size_t block_size = std::min<size_t>(block.size, size - size_written);
      dest_->AppendAndTakeOwnership(block.data, block_size,
                                    &SnappySinkAllocator::Deleter, NULL);
      size_written += block_size;
    }
    blocks_.clear();
  }

 private:
  struct Datablock {
    char* data;
    size_t size;
    Datablock(char* p, size_t s) : data(p), size(s) {}
  };

  static void Deleter(void* arg, const char* bytes, size_t size) {
    // TODO: Switch to [[maybe_unused]] when we can assume C++17.
    (void)arg;
    (void)size;

    delete[] bytes;
  }

  Sink* dest_;
  std::vector<Datablock> blocks_;

  // Note: copying this object is allowed
};

size_t UncompressAsMuchAsPossible(Source* compressed, Sink* uncompressed) {
  SnappySinkAllocator allocator(uncompressed);
  SnappyScatteredWriter<SnappySinkAllocator> writer(allocator);
  InternalUncompress(compressed, &writer);
  return writer.Produced();
}

bool Uncompress(Source* compressed, Sink* uncompressed) {
  // Read the uncompressed length from the front of the compressed input
  SnappyDecompressor decompressor(compressed);
  uint32_t uncompressed_len = 0;
  if (!decompressor.ReadUncompressedLength(&uncompressed_len)) {
    return false;
  }

  char c;
  size_t allocated_size;
  char* buf = uncompressed->GetAppendBufferVariable(1, uncompressed_len, &c, 1,
                                                    &allocated_size);

  const size_t compressed_len = compressed->Available();
  // If we can get a flat buffer, then use it, otherwise do block by block
  // uncompression
  if (allocated_size >= uncompressed_len) {
    SnappyArrayWriter writer(buf);
    bool result = InternalUncompressAllTags(&decompressor, &writer,
                                            compressed_len, uncompressed_len);
    uncompressed->Append(buf, writer.Produced());
    return result;
  } else {
    SnappySinkAllocator allocator(uncompressed);
    SnappyScatteredWriter<SnappySinkAllocator> writer(allocator);
    return InternalUncompressAllTags(&decompressor, &writer, compressed_len,
                                     uncompressed_len);
  }
}

}  // namespace snappy
