/*
 * Copyright 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ultrahdr/gainmapmath.h"

#include <arm_neon.h>
#include <cassert>

namespace ultrahdr {

// Scale all coefficients by 2^14 to avoid needing floating-point arithmetic. This can cause an off
// by one error compared to the scalar floating-point implementation.

// Removing conversion coefficients 1 and 0 from the group for each standard leaves 6 coefficients.
// Pack them into a single 128-bit vector as follows, zeroing the remaining elements:
// {Y1, Y2, U1, U2, V1, V2, 0, 0}

// Yuv Bt709 -> Yuv Bt601
// Y' = (1.0f * Y) + ( 0.101579f * U) + ( 0.196076f * V)
// U' = (0.0f * Y) + ( 0.989854f * U) + (-0.110653f * V)
// V' = (0.0f * Y) + (-0.072453f * U) + ( 0.983398f * V)
__attribute__((aligned(16)))
const int16_t kYuv709To601_coeffs_neon[8] = {1664, 3213, 16218, -1813, -1187, 16112, 0, 0};

// Yuv Bt709 -> Yuv Bt2100
// Y' = (1.0f * Y) + (-0.016969f * U) + ( 0.096312f * V)
// U' = (0.0f * Y) + ( 0.995306f * U) + (-0.051192f * V)
// V' = (0.0f * Y) + ( 0.011507f * U) + ( 1.002637f * V)
__attribute__((aligned(16)))
const int16_t kYuv709To2100_coeffs_neon[8] = {-278, 1578, 16307, -839, 189, 16427, 0, 0};

// Yuv Bt601 -> Yuv Bt709
// Y' = (1.0f * Y) + (-0.118188f * U) + (-0.212685f * V),
// U' = (0.0f * Y) + ( 1.018640f * U) + ( 0.114618f * V),
// V' = (0.0f * Y) + ( 0.075049f * U) + ( 1.025327f * V);
__attribute__((aligned(16)))
const int16_t kYuv601To709_coeffs_neon[8] = {-1936, -3485, 16689, 1878, 1230, 16799, 0, 0};

// Yuv Bt601 -> Yuv Bt2100
// Y' = (1.0f * Y) + (-0.128245f * U) + (-0.115879f * V)
// U' = (0.0f * Y) + ( 1.010016f * U) + ( 0.061592f * V)
// V' = (0.0f * Y) + ( 0.086969f * U) + ( 1.029350f * V)
__attribute__((aligned(16)))
const int16_t kYuv601To2100_coeffs_neon[8] = {-2101, -1899, 16548, 1009, 1425, 16865, 0, 0};

// Yuv Bt2100 -> Yuv Bt709
// Y' = (1.0f * Y) + ( 0.018149f * U) + (-0.095132f * V)
// U' = (0.0f * Y) + ( 1.004123f * U) + ( 0.051267f * V)
// V' = (0.0f * Y) + (-0.011524f * U) + ( 0.996782f * V)
__attribute__((aligned(16)))
const int16_t kYuv2100To709_coeffs_neon[8] = {297, -1559, 16452, 840, -189, 16331, 0, 0};

// Yuv Bt2100 -> Yuv Bt601
// Y' = (1.0f * Y) + ( 0.117887f * U) + ( 0.105521f * V)
// U' = (0.0f * Y) + ( 0.995211f * U) + (-0.059549f * V)
// V' = (0.0f * Y) + (-0.084085f * U) + ( 0.976518f * V)
__attribute__((aligned(16)))
const int16_t kYuv2100To601_coeffs_neon[8] = {1931, 1729, 16306, -976, -1378, 15999, 0, 0};

static inline int16x8_t yConversion_neon(uint8x8_t y, int16x8_t u, int16x8_t v, int16x8_t coeffs) {
  int32x4_t lo = vmull_laneq_s16(vget_low_s16(u), coeffs, 0);
  int32x4_t hi = vmull_laneq_s16(vget_high_s16(u), coeffs, 0);
  lo = vmlal_laneq_s16(lo, vget_low_s16(v), coeffs, 1);
  hi = vmlal_laneq_s16(hi, vget_high_s16(v), coeffs, 1);

  // Descale result to account for coefficients being scaled by 2^14.
  uint16x8_t y_output =
      vreinterpretq_u16_s16(vcombine_s16(vqrshrn_n_s32(lo, 14), vqrshrn_n_s32(hi, 14)));
  return vreinterpretq_s16_u16(vaddw_u8(y_output, y));
}

static inline int16x8_t uConversion_neon(int16x8_t u, int16x8_t v, int16x8_t coeffs) {
  int32x4_t u_lo = vmull_laneq_s16(vget_low_s16(u), coeffs, 2);
  int32x4_t u_hi = vmull_laneq_s16(vget_high_s16(u), coeffs, 2);
  u_lo = vmlal_laneq_s16(u_lo, vget_low_s16(v), coeffs, 3);
  u_hi = vmlal_laneq_s16(u_hi, vget_high_s16(v), coeffs, 3);

  // Descale result to account for coefficients being scaled by 2^14.
  const int16x8_t u_output = vcombine_s16(vqrshrn_n_s32(u_lo, 14), vqrshrn_n_s32(u_hi, 14));
  return u_output;
}

static inline int16x8_t vConversion_neon(int16x8_t u, int16x8_t v, int16x8_t coeffs) {
  int32x4_t v_lo = vmull_laneq_s16(vget_low_s16(u), coeffs, 4);
  int32x4_t v_hi = vmull_laneq_s16(vget_high_s16(u), coeffs, 4);
  v_lo = vmlal_laneq_s16(v_lo, vget_low_s16(v), coeffs, 5);
  v_hi = vmlal_laneq_s16(v_hi, vget_high_s16(v), coeffs, 5);

  // Descale result to account for coefficients being scaled by 2^14.
  const int16x8_t v_output = vcombine_s16(vqrshrn_n_s32(v_lo, 14), vqrshrn_n_s32(v_hi, 14));
  return v_output;
}

int16x8x3_t yuvConversion_neon(uint8x8_t y, int16x8_t u, int16x8_t v, int16x8_t coeffs) {
  const int16x8_t y_output = yConversion_neon(y, u, v, coeffs);
  const int16x8_t u_output = uConversion_neon(u, v, coeffs);
  const int16x8_t v_output = vConversion_neon(u, v, coeffs);
  return {y_output, u_output, v_output};
}

void transformYuv420_neon(jr_uncompressed_ptr image, const int16_t* coeffs_ptr) {
  // Implementation assumes image buffer is multiple of 16.
  assert(image->width % 16 == 0);
  uint8_t* y0_ptr = static_cast<uint8_t*>(image->data);
  uint8_t* y1_ptr = y0_ptr + image->luma_stride;
  uint8_t* u_ptr = static_cast<uint8_t*>(image->chroma_data);
  uint8_t* v_ptr = u_ptr + image->chroma_stride * (image->height / 2);

  const int16x8_t coeffs = vld1q_s16(coeffs_ptr);
  const uint16x8_t uv_bias = vreinterpretq_u16_s16(vdupq_n_s16(-128));
  size_t h = 0;
  do {
    size_t w = 0;
    do {
      uint8x16_t y0 = vld1q_u8(y0_ptr + w * 2);
      uint8x16_t y1 = vld1q_u8(y1_ptr + w * 2);
      uint8x8_t u = vld1_u8(u_ptr + w);
      uint8x8_t v = vld1_u8(v_ptr + w);

      // 128 bias for UV given we are using libjpeg; see:
      // https://github.com/kornelski/libjpeg/blob/master/structure.doc
      int16x8_t u_wide_s16 = vreinterpretq_s16_u16(vaddw_u8(uv_bias, u));  // -128 + u
      int16x8_t v_wide_s16 = vreinterpretq_s16_u16(vaddw_u8(uv_bias, v));  // -128 + v

      const int16x8_t u_wide_lo = vzip1q_s16(u_wide_s16, u_wide_s16);
      const int16x8_t u_wide_hi = vzip2q_s16(u_wide_s16, u_wide_s16);
      const int16x8_t v_wide_lo = vzip1q_s16(v_wide_s16, v_wide_s16);
      const int16x8_t v_wide_hi = vzip2q_s16(v_wide_s16, v_wide_s16);

      const int16x8_t y0_lo = yConversion_neon(vget_low_u8(y0), u_wide_lo, v_wide_lo, coeffs);
      const int16x8_t y0_hi = yConversion_neon(vget_high_u8(y0), u_wide_hi, v_wide_hi, coeffs);
      const int16x8_t y1_lo = yConversion_neon(vget_low_u8(y1), u_wide_lo, v_wide_lo, coeffs);
      const int16x8_t y1_hi = yConversion_neon(vget_high_u8(y1), u_wide_hi, v_wide_hi, coeffs);

      const int16x8_t new_u = uConversion_neon(u_wide_s16, v_wide_s16, coeffs);
      const int16x8_t new_v = vConversion_neon(u_wide_s16, v_wide_s16, coeffs);

      // Narrow from 16-bit to 8-bit with saturation.
      const uint8x16_t y0_output = vcombine_u8(vqmovun_s16(y0_lo), vqmovun_s16(y0_hi));
      const uint8x16_t y1_output = vcombine_u8(vqmovun_s16(y1_lo), vqmovun_s16(y1_hi));
      const uint8x8_t u_output = vqmovun_s16(vaddq_s16(new_u, vdupq_n_s16(128)));
      const uint8x8_t v_output = vqmovun_s16(vaddq_s16(new_v, vdupq_n_s16(128)));

      vst1q_u8(y0_ptr + w * 2, y0_output);
      vst1q_u8(y1_ptr + w * 2, y1_output);
      vst1_u8(u_ptr + w, u_output);
      vst1_u8(v_ptr + w, v_output);

      w += 8;
    } while (w < image->width / 2);
    y0_ptr += image->luma_stride * 2;
    y1_ptr += image->luma_stride * 2;
    u_ptr += image->chroma_stride;
    v_ptr += image->chroma_stride;
  } while (++h < image->height / 2);
}

status_t convertYuv_neon(jr_uncompressed_ptr image, ultrahdr_color_gamut src_encoding,
                         ultrahdr_color_gamut dst_encoding) {
  if (image == nullptr) {
    return ERROR_JPEGR_BAD_PTR;
  }
  if (src_encoding == ULTRAHDR_COLORGAMUT_UNSPECIFIED ||
      dst_encoding == ULTRAHDR_COLORGAMUT_UNSPECIFIED) {
    return ERROR_JPEGR_INVALID_COLORGAMUT;
  }

  const int16_t* coeffs = nullptr;
  switch (src_encoding) {
    case ULTRAHDR_COLORGAMUT_BT709:
      switch (dst_encoding) {
        case ULTRAHDR_COLORGAMUT_BT709:
          return JPEGR_NO_ERROR;
        case ULTRAHDR_COLORGAMUT_P3:
          coeffs = kYuv709To601_coeffs_neon;
          break;
        case ULTRAHDR_COLORGAMUT_BT2100:
          coeffs = kYuv709To2100_coeffs_neon;
          break;
        default:
          // Should be impossible to hit after input validation
          return ERROR_JPEGR_INVALID_COLORGAMUT;
      }
      break;
    case ULTRAHDR_COLORGAMUT_P3:
      switch (dst_encoding) {
        case ULTRAHDR_COLORGAMUT_BT709:
          coeffs = kYuv601To709_coeffs_neon;
          break;
        case ULTRAHDR_COLORGAMUT_P3:
          return JPEGR_NO_ERROR;
        case ULTRAHDR_COLORGAMUT_BT2100:
          coeffs = kYuv601To2100_coeffs_neon;
          break;
        default:
          // Should be impossible to hit after input validation
          return ERROR_JPEGR_INVALID_COLORGAMUT;
      }
      break;
    case ULTRAHDR_COLORGAMUT_BT2100:
      switch (dst_encoding) {
        case ULTRAHDR_COLORGAMUT_BT709:
          coeffs = kYuv2100To709_coeffs_neon;
          break;
        case ULTRAHDR_COLORGAMUT_P3:
          coeffs = kYuv2100To601_coeffs_neon;
          break;
        case ULTRAHDR_COLORGAMUT_BT2100:
          return JPEGR_NO_ERROR;
        default:
          // Should be impossible to hit after input validation
          return ERROR_JPEGR_INVALID_COLORGAMUT;
      }
      break;
    default:
      // Should be impossible to hit after input validation
      return ERROR_JPEGR_INVALID_COLORGAMUT;
  }

  if (coeffs == nullptr) {
    // Should be impossible to hit after input validation
    return ERROR_JPEGR_INVALID_COLORGAMUT;
  }

  transformYuv420_neon(image, coeffs);
  return JPEGR_NO_ERROR;
}

}  // namespace ultrahdr
