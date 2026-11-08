// Copyright 2026 Google Inc. All Rights Reserved.
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

#ifndef THIRD_PARTY_SNAPPY_SNAPPY_ISADETECTION_H_
#define THIRD_PARTY_SNAPPY_SNAPPY_ISADETECTION_H_

#include <cstdint>

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
#define SNAPPY_IS_X86_64 1
#else
#define SNAPPY_IS_X86_64 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#define SNAPPY_IS_AARCH64 1
#else
#define SNAPPY_IS_AARCH64 0
#endif

#define SNAPPY_STRINGIFY_IMPL_(a) #a
#define SNAPPY_STRINGIFY(a) SNAPPY_STRINGIFY_IMPL_(a)

#if defined(__clang__)
#define SNAPPY_TARGET_REGION(T) \
  _Pragma(SNAPPY_STRINGIFY(     \
      clang attribute push(__attribute__((target(T))), apply_to = function)))
#define SNAPPY_UNTARGET_REGION _Pragma("clang attribute pop")
#elif defined(__GNUC__)
#define SNAPPY_TARGET_REGION(T) \
  _Pragma("GCC push_options") _Pragma(SNAPPY_STRINGIFY(GCC target(T)))
#define SNAPPY_UNTARGET_REGION _Pragma("GCC pop_options")
#else
#define SNAPPY_TARGET_REGION(T)
#define SNAPPY_UNTARGET_REGION
#endif

#if SNAPPY_IS_X86_64
#define SNAPPY_TARGET_SSSE3 SNAPPY_TARGET_REGION("ssse3")
#define SNAPPY_TARGET_SSE42 SNAPPY_TARGET_REGION("ssse3,sse4.2")
#define SNAPPY_TARGET_AVX2_BMI2 \
  SNAPPY_TARGET_REGION("ssse3,sse4.2,avx,avx2,bmi,bmi2")
#endif

#if SNAPPY_IS_AARCH64
#define SNAPPY_TARGET_ARM_CRC32 SNAPPY_TARGET_REGION("+crc")
#endif

namespace snappy {
namespace internal {

enum SnappyInstructionSet : uint32_t {
  kIsaDefault = 0x0,
  kIsaSSSE3 = 1u << 0,
  kIsaSSE42 = 1u << 1,
  kIsaAVX = 1u << 2,
  kIsaAVX2 = 1u << 3,
  kIsaBMI1 = 1u << 4,
  kIsaBMI2 = 1u << 5,
  kIsaNEON = 1u << 6,
  kIsaNEON_CRC32 = 1u << 7,
  kIsaUninitialized = 1u << 31,
};

constexpr uint32_t kIsaX86_SSE42Tier = kIsaSSSE3 | kIsaSSE42;
constexpr uint32_t kIsaX86_AVX2Tier =
    kIsaSSSE3 | kIsaSSE42 | kIsaAVX | kIsaAVX2 | kIsaBMI1 | kIsaBMI2;

inline constexpr bool HasX86AvxSupport(uint32_t leaf1_ecx,
                                       uint64_t xcr0) noexcept {
  return ((leaf1_ecx & ((1u << 27) | (1u << 28))) ==
          ((1u << 27) | (1u << 28))) &&
         ((xcr0 & 0x6) == 0x6);
}

inline constexpr uint32_t EvaluateX86CpuidBits(uint32_t max_basic_leaf,
                                               uint32_t leaf1_ecx,
                                               uint64_t xcr0,
                                               uint32_t leaf7_ebx) noexcept {
  return (max_basic_leaf < 1)
             ? kIsaDefault
             : (((leaf1_ecx & (1u << 9)) ? kIsaSSSE3 : 0u) |
                ((leaf1_ecx & (1u << 20)) ? kIsaSSE42 : 0u) |
                (HasX86AvxSupport(leaf1_ecx, xcr0) ? kIsaAVX : 0u) |
                ((max_basic_leaf >= 7)
                     ? (((leaf7_ebx & (1u << 3)) ? kIsaBMI1 : 0u) |
                        ((leaf7_ebx & (1u << 8)) ? kIsaBMI2 : 0u) |
                        ((HasX86AvxSupport(leaf1_ecx, xcr0) &&
                          (leaf7_ebx & (1u << 5)))
                             ? kIsaAVX2
                             : 0u))
                     : 0u));
}

uint32_t DetectSupportedArchitecturesSlow() noexcept;
uint32_t GetSupportedArchitectures() noexcept;
void SetArchitectureMaskForTesting(uint32_t mask) noexcept;
void ResetArchitectureMaskForTesting() noexcept;

}  // namespace internal
}  // namespace snappy

#endif  // THIRD_PARTY_SNAPPY_SNAPPY_ISADETECTION_H_
