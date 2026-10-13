/**
 * @file        ppc/intrinsics.h
 * @brief       SIMD intrinsic helpers and vector lookup tables for PPC AltiVec emulation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp SIMD patterns and simde
 */

#pragma once

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <simde/x86/avx.h>
#include <simde/x86/avx2.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include <rex/types.h>

namespace rex::ppc {

//=============================================================================
// Vector Load/Store Mask Tables
//=============================================================================
// These tables are used for lvlx/lvrx (load vector left/right) and
// stvlx/stvrx (store vector left/right) instructions.

inline uint8_t VectorMaskL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

inline uint8_t VectorMaskR[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

inline uint8_t VectorShiftTableL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

inline uint8_t VectorShiftTableR[] = {
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

//=============================================================================
// SIMD Helper Functions
//=============================================================================

// Xenon vmsum3fp128/vmsum4fp128 return QNaN when a dot product of finite
// inputs overflows its single-precision result. simde_mm_dp_ps instead returns
// +/-infinity, which changes ordered comparisons in guest collision code.
inline float simde_mm_vmsum_narrow_result(double accumulator) noexcept {
  const float result = static_cast<float>(accumulator);
  if (std::isinf(result) && !std::isinf(accumulator)) {
    return std::bit_cast<float>(uint32_t{0x7FC00000});
  }

  // The generated instruction path runs with FTZ/DAZ enabled. Keep the same
  // output behavior when the scalar-double implementation produces a denormal.
  const uint32_t result_bits = std::bit_cast<uint32_t>(result);
  if ((result_bits & 0x7FFFFFFFu) != 0 && (result_bits & 0x7F800000u) == 0) {
    return std::bit_cast<float>(result_bits & 0x80000000u);
  }
  return result;
}

// Guest vector elements are reversed relative to host lanes. Accumulate exact
// float products in double and preserve Xenia's hardware-verified reduction
// order: (x + z) + y for vmsum3fp128.
inline simde__m128 simde_mm_vmsum3fp128(simde__m128 a, simde__m128 b) noexcept {
  alignas(16) float a_lanes[4];
  alignas(16) float b_lanes[4];
  simde_mm_store_ps(a_lanes, a);
  simde_mm_store_ps(b_lanes, b);

  const double x = static_cast<double>(a_lanes[3]) * static_cast<double>(b_lanes[3]);
  const double y = static_cast<double>(a_lanes[2]) * static_cast<double>(b_lanes[2]);
  const double z = static_cast<double>(a_lanes[1]) * static_cast<double>(b_lanes[1]);
  return simde_mm_set1_ps(simde_mm_vmsum_narrow_result((x + z) + y));
}

// vmsum4fp128 uses the pairwise reduction (x + z) + (y + w).
inline simde__m128 simde_mm_vmsum4fp128(simde__m128 a, simde__m128 b) noexcept {
  alignas(16) float a_lanes[4];
  alignas(16) float b_lanes[4];
  simde_mm_store_ps(a_lanes, a);
  simde_mm_store_ps(b_lanes, b);

  const double x = static_cast<double>(a_lanes[3]) * static_cast<double>(b_lanes[3]);
  const double y = static_cast<double>(a_lanes[2]) * static_cast<double>(b_lanes[2]);
  const double z = static_cast<double>(a_lanes[1]) * static_cast<double>(b_lanes[1]);
  const double w = static_cast<double>(a_lanes[0]) * static_cast<double>(b_lanes[0]);
  return simde_mm_set1_ps(simde_mm_vmsum_narrow_result((x + z) + (y + w)));
}

// Unsigned 32-bit saturating add
inline simde__m128i simde_mm_adds_epu32(simde__m128i a, simde__m128i b) {
  return simde_mm_add_epi32(
      a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
}

// Signed 8-bit average (rounds towards zero)
inline simde__m128i simde_mm_avg_epi8(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu8(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

// Signed 16-bit average
inline simde__m128i simde_mm_avg_epi16(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu16(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

// Signed 32-bit average
inline simde__m128i simde_mm_avg_epi32(simde__m128i a, simde__m128i b) {
  simde__m128i sum = simde_mm_add_epi32(simde_mm_srai_epi32(a, 1), simde_mm_srai_epi32(b, 1));
  return simde_mm_add_epi32(sum,
                            simde_mm_and_si128(simde_mm_or_si128(a, b), simde_mm_set1_epi32(1)));
}

// Convert unsigned 32-bit integers to floats
inline simde__m128 simde_mm_cvtepu32_ps_(simde__m128i src1) {
  simde__m128i xmm1 = simde_mm_add_epi32(src1, simde_mm_set1_epi32(127));
  simde__m128i xmm0 = simde_mm_slli_epi32(src1, 31 - 8);
  xmm0 = simde_mm_srli_epi32(xmm0, 31);
  xmm0 = simde_mm_add_epi32(xmm0, xmm1);
  xmm0 = simde_mm_srai_epi32(xmm0, 8);
  xmm0 = simde_mm_add_epi32(xmm0, simde_mm_set1_epi32(0x4F800000));
  simde__m128 xmm2 = simde_mm_cvtepi32_ps(src1);
  return simde_mm_blendv_ps(xmm2, simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(src1));
}

// Permute bytes from two vectors based on control vector
inline simde__m128i simde_mm_perm_epi8_(simde__m128i a, simde__m128i b, simde__m128i c) {
  simde__m128i d = simde_mm_set1_epi8(0xF);
  simde__m128i e = simde_mm_sub_epi8(d, simde_mm_and_si128(c, d));
  return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, e), simde_mm_shuffle_epi8(b, e),
                              simde_mm_slli_epi32(c, 3));
}

// Unsigned 8-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu8(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_cmpgt_epi8(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

// Unsigned 16-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu16(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_cmpgt_epi16(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

// Vector Convert To Signed Fixed-Point Word Saturate
inline simde__m128i simde_mm_vctsxs(simde__m128 src1) {
  simde__m128 xmm2 = simde_mm_cmpunord_ps(src1, src1);
  simde__m128i xmm0 = simde_mm_cvttps_epi32(src1);
  simde__m128i xmm1 = simde_mm_cmpeq_epi32(xmm0, simde_mm_set1_epi32(INT_MIN));
  xmm1 = simde_mm_andnot_si128(simde_mm_castps_si128(src1), xmm1);
  simde__m128 dest = simde_mm_blendv_ps(simde_mm_castsi128_ps(xmm0),
                                        simde_mm_castsi128_ps(simde_mm_set1_epi32(INT_MAX)),
                                        simde_mm_castsi128_ps(xmm1));
  return simde_mm_andnot_si128(simde_mm_castps_si128(xmm2), simde_mm_castps_si128(dest));
}

// Vector Convert To Unsigned Fixed-Point Word Saturate
// Convert float to unsigned int with saturation to [0, UINT_MAX]
// NaN -> 0, negative -> 0, > UINT_MAX -> UINT_MAX
inline simde__m128i simde_mm_vctuxs(simde__m128 src1) {
  simde__m128 nan_mask = simde_mm_cmpunord_ps(src1, src1);
  simde__m128 neg_mask = simde_mm_cmplt_ps(src1, simde_mm_setzero_ps());
  simde__m128 max_val = simde_mm_set1_ps(4294967295.0f);  // UINT_MAX as float
  simde__m128 overflow_mask = simde_mm_cmpge_ps(src1, max_val);

  // Clamp to [0, UINT_MAX]
  simde__m128 clamped = simde_mm_max_ps(src1, simde_mm_setzero_ps());
  clamped = simde_mm_min_ps(clamped, max_val);

  // Convert to signed int first (will handle values up to INT_MAX correctly)
  // For values > INT_MAX, we need special handling
  simde__m128 half_range = simde_mm_set1_ps(2147483648.0f);  // 2^31
  simde__m128 high_bit_mask = simde_mm_cmpge_ps(clamped, half_range);

  // For values >= 2^31, subtract 2^31 before conversion and add it back after
  simde__m128 adjusted = simde_mm_sub_ps(clamped, simde_mm_and_ps(high_bit_mask, half_range));
  simde__m128i low_bits = simde_mm_cvttps_epi32(adjusted);
  simde__m128i high_bit = simde_mm_and_si128(simde_mm_castps_si128(high_bit_mask),
                                             simde_mm_set1_epi32(int(0x80000000u)));
  simde__m128i result = simde_mm_or_si128(low_bits, high_bit);

  // Apply saturation: NaN -> 0, overflow -> UINT_MAX
  result = simde_mm_andnot_si128(simde_mm_castps_si128(nan_mask), result);
  result = simde_mm_andnot_si128(simde_mm_castps_si128(neg_mask), result);
  result = simde_mm_or_si128(
      simde_mm_andnot_si128(simde_mm_castps_si128(overflow_mask), result),
      simde_mm_and_si128(simde_mm_castps_si128(overflow_mask), simde_mm_set1_epi32(-1)));

  return result;
}

// Vector Shift Right
// Vector Shift Right - shift the whole 128-bit vector right by the low 3 bits
// of vB's last byte (big-endian byte 15, host index 0), carrying across the
// two 64-bit halves.
inline simde__m128i simde_mm_vsr(simde__m128i a, simde__m128i b) {
  int shift = simde_mm_extract_epi8(b, 0) & 0x7;
  if (shift == 0)
    return a;
  // Variable counts go through the vector-count forms: the immediate forms
  // need a constant on NEON.
  simde__m128i shifted = simde_mm_srl_epi64(a, simde_mm_cvtsi32_si128(shift));
  // The high half's low bits move into the low half's high bits.
  simde__m128i carry =
      simde_mm_srli_si128(simde_mm_sll_epi64(a, simde_mm_cvtsi32_si128(64 - shift)), 8);
  return simde_mm_or_si128(shifted, carry);
}

// Vector Shift Left - shift entire 128-bit vector left by bits in low 3 bits of b
inline simde__m128i simde_mm_vsl(simde__m128i a, simde__m128i b) {
  // The count is the low 3 bits of vB's last byte (big-endian byte 15, host
  // index 0), as the hardware reads it when the bytes disagree.
  int shift = simde_mm_extract_epi8(b, 0) & 0x7;
  if (shift == 0)
    return a;

#if defined(__x86_64__) || defined(_M_X64)
  // Split into high and low 64-bit parts
  simde__m128i low_shifted = simde_mm_slli_epi64(a, shift);
  simde__m128i high_carry = simde_mm_srli_epi64(a, 64 - shift);
  // Shift the carry from low qword to high qword position
  high_carry = simde_mm_slli_si128(high_carry, 8);
  return simde_mm_or_si128(low_shifted, high_carry);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using vld1/vst1 for conversion
  uint64_t vals[2];
  uint64_t res[2] = {0, 0};

  // Store simde__m128i to memory
  simde_mm_store_si128((simde__m128i*)vals, a);

  // Load as NEON vector
  uint64x2_t va = vld1q_u64(vals);

  // vshlq_u64 accepts variable shift per lane
  int64x2_t shift_vector = vdupq_n_s64(shift);
  uint64x2_t low_shifted = vshlq_u64(va, shift_vector);

  // NEON vshl uses negative counts for right shifts.
  int64x2_t rshift_vector = vdupq_n_s64(shift - 64);
  uint64x2_t high_carry = vshlq_u64(va, rshift_vector);

  // Combine results
  uint64x2_t result_vec = vdupq_n_u64(0);
  result_vec = vsetq_lane_u64(vgetq_lane_u64(low_shifted, 0), result_vec, 0);
  result_vec =
      vsetq_lane_u64(vgetq_lane_u64(low_shifted, 1) | vgetq_lane_u64(high_carry, 0), result_vec, 1);

  // Store back to memory and reload as simde__m128i
  vst1q_u64(res, result_vec);
  return simde_mm_load_si128((simde__m128i*)res);
#else
#error "Unsupported architecture for simde_mm_vsl (only x86_64 and ARM64 supported)"
#endif
}

// Vector Maximum / Minimum Floating Point with the AltiVec rules the SSE
// min/max do not have: a NaN operand is the result (vA first), and between
// zeros of either sign the maximum is +0 and the minimum is -0.
// A denormal input is a signed zero to the hardware (non-Java mode), and a
// NaN result is quiet. The flush is explicit because DAZ only makes the
// compare treat a denormal as zero while its bits stay in the operand.
inline simde__m128 simde_mm_vflushfp(simde__m128 x) {
  simde__m128i bits = simde_mm_castps_si128(x);
  simde__m128i exponent = simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7F800000));
  simde__m128i denormal = simde_mm_cmpeq_epi32(exponent, simde_mm_setzero_si128());
  simde__m128i sign = simde_mm_and_si128(bits, simde_mm_set1_epi32(int(0x80000000)));
  return simde_mm_castsi128_ps(simde_mm_blendv_epi8(bits, sign, denormal));
}

inline simde__m128 simde_mm_vquietnan(simde__m128 x) {
  simde__m128 nan = simde_mm_cmpunord_ps(x, x);
  simde__m128 quiet = simde_mm_or_ps(x, simde_mm_castsi128_ps(simde_mm_set1_epi32(0x00400000)));
  return simde_mm_blendv_ps(x, quiet, nan);
}

inline simde__m128 simde_mm_vmaxfp(simde__m128 a, simde__m128 b) {
  a = simde_mm_vflushfp(a);
  b = simde_mm_vflushfp(b);
  simde__m128 r = simde_mm_max_ps(a, b);
  // Equal values (both zeros among them): the sign bit of the result is set
  // only when both are negative, which the AND of the two gives.
  r = simde_mm_blendv_ps(r, simde_mm_and_ps(a, b), simde_mm_cmpeq_ps(a, b));
  r = simde_mm_blendv_ps(r, b, simde_mm_cmpunord_ps(b, b));
  r = simde_mm_blendv_ps(r, a, simde_mm_cmpunord_ps(a, a));
  return simde_mm_vquietnan(r);
}

inline simde__m128 simde_mm_vminfp(simde__m128 a, simde__m128 b) {
  a = simde_mm_vflushfp(a);
  b = simde_mm_vflushfp(b);
  simde__m128 r = simde_mm_min_ps(a, b);
  r = simde_mm_blendv_ps(r, simde_mm_or_ps(a, b), simde_mm_cmpeq_ps(a, b));
  r = simde_mm_blendv_ps(r, b, simde_mm_cmpunord_ps(b, b));
  r = simde_mm_blendv_ps(r, a, simde_mm_cmpunord_ps(a, a));
  return simde_mm_vquietnan(r);
}

// Vector Shift Left by Octet - shift entire vector left by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC left shift = shift towards MSB (lower PPC addresses) = shift towards higher x86 addresses
inline simde__m128i simde_mm_vslo(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

#if defined(__x86_64__) || defined(_M_X64)
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst + shift_bytes, src, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using memory for conversion
  uint8_t src[16];
  uint8_t dst[16] = {0};

  simde_mm_store_si128((simde__m128i*)src, a);
  memcpy(dst + shift_bytes, src, 16 - shift_bytes);

  return simde_mm_load_si128((simde__m128i*)dst);
#else
#error "Unsupported architecture for simde_mm_vslo (only x86_64 and ARM64 supported)"
#endif
}

// Vector Shift Right by Octet - shift entire vector right by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC right shift = shift towards LSB (higher PPC addresses) = shift towards lower x86 addresses
inline simde__m128i simde_mm_vsro(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

#if defined(__x86_64__) || defined(_M_X64)
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst, src + shift_bytes, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using memory for conversion
  uint8_t src[16];
  uint8_t dst[16] = {0};

  simde_mm_store_si128((simde__m128i*)src, a);
  memcpy(dst, src + shift_bytes, 16 - shift_bytes);

  return simde_mm_load_si128((simde__m128i*)dst);
#else
#error "Unsupported architecture for simde_mm_vsro (only x86_64 and ARM64 supported)"
#endif
}

// Variable 16-bit shift left: widen to 32-bit, shift, narrow back
inline simde__m128i simde_mm_sllv_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi32(a_hi, s_hi);
  simde__m128i mask16 = simde_mm_set1_epi32(0xFFFF);
  r_lo = simde_mm_and_si128(r_lo, mask16);
  r_hi = simde_mm_and_si128(r_hi, mask16);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

// Variable 16-bit logical right shift: widen to 32-bit, shift, narrow back
inline simde__m128i simde_mm_srlv_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srlv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srlv_epi32(a_hi, s_hi);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

// Variable 16-bit arithmetic right shift: sign-extend to 32-bit, shift, narrow back
inline simde__m128i simde_mm_srav_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  // Sign-extend a: duplicate each 16-bit lane, then arithmetic shift right by 16
  simde__m128i a_lo = simde_mm_srai_epi32(simde_mm_unpacklo_epi16(a, a), 16);
  simde__m128i a_hi = simde_mm_srai_epi32(simde_mm_unpackhi_epi16(a, a), 16);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srav_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srav_epi32(a_hi, s_hi);
  return simde_mm_packs_epi32(r_lo, r_hi);
}

// Variable 8-bit shift left: widen to 16-bit, shift, narrow back
inline simde__m128i simde_mm_sllv_epi8(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi8(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi8(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi8(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi8(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi16(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi16(a_hi, s_hi);
  simde__m128i mask8 = simde_mm_set1_epi16(0xFF);
  r_lo = simde_mm_and_si128(r_lo, mask8);
  r_hi = simde_mm_and_si128(r_hi, mask8);
  return simde_mm_packus_epi16(r_lo, r_hi);
}

}  // namespace rex::ppc

//=============================================================================
// Global Aliases for Generated Code
//=============================================================================
// Vector mask tables accessible from global scope for generated code
using rex::ppc::VectorMaskL;
using rex::ppc::VectorMaskR;
using rex::ppc::VectorShiftTableL;
using rex::ppc::VectorShiftTableR;
