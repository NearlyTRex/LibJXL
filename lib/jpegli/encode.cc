// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jpegli/encode.h"

#include <initializer_list>
#include <vector>

#include "lib/jpegli/adaptive_quantization.h"
#include "lib/jpegli/bitstream.h"
#include "lib/jpegli/color_transform.h"
#include "lib/jpegli/dct.h"
#include "lib/jpegli/encode_internal.h"
#include "lib/jpegli/entropy_coding.h"
#include "lib/jpegli/error.h"
#include "lib/jpegli/memory_manager.h"
#include "lib/jpegli/quant.h"
#include "lib/jxl/base/byte_order.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/enc_color_management.h"
#include "lib/jxl/enc_xyb.h"

namespace jpegli {

using ByteSpan = jxl::Span<const uint8_t>;

constexpr unsigned char kICCSignature[12] = {
    0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00};
constexpr int kICCMarker = JPEG_APP0 + 2;
constexpr size_t kMaxBytesInMarker = 65533;

bool GetMarkerPayload(const uint8_t* data, size_t size, ByteSpan* payload) {
  if (size < 4) {
    return false;
  }
  size_t hi = data[2];
  size_t lo = data[3];
  size_t internal_size = (hi << 8u) | lo;
  // First two bytes of marker is not counted towards size.
  if (internal_size != size - 2) {
    return false;
  }
  // cut first two marker bytes and "length" from payload.
  *payload = ByteSpan(data, size);
  payload->remove_prefix(4);
  return true;
}

jxl::Status ParseChunkedMarker(j_compress_ptr cinfo, uint8_t marker_type,
                               const ByteSpan& tag, jxl::PaddedBytes* output,
                               bool allow_permutations = false) {
  output->clear();

  std::vector<ByteSpan> chunks;
  std::vector<bool> presence;
  size_t expected_number_of_parts = 0;
  bool is_first_chunk = true;
  size_t ordinal = 0;
  for (const auto& marker : cinfo->master->special_markers) {
    if (marker.size() < 2 || marker[1] != marker_type) {
      continue;
    }
    ByteSpan payload;
    if (!GetMarkerPayload(marker.data(), marker.size(), &payload)) {
      // Something is wrong with this marker; does not care.
      continue;
    }
    if ((payload.size() < tag.size()) ||
        memcmp(payload.data(), tag.data(), tag.size()) != 0) {
      continue;
    }
    payload.remove_prefix(tag.size());
    if (payload.size() < 2) {
      return JXL_FAILURE("Chunk is too small.");
    }
    uint8_t index = payload[0];
    uint8_t total = payload[1];
    ordinal++;
    if (!allow_permutations) {
      if (index != ordinal) return JXL_FAILURE("Invalid chunk order.");
    }

    payload.remove_prefix(2);

    JXL_RETURN_IF_ERROR(total != 0);
    if (is_first_chunk) {
      is_first_chunk = false;
      expected_number_of_parts = total;
      // 1-based indices; 0-th element is added for convenience.
      chunks.resize(total + 1);
      presence.resize(total + 1);
    } else {
      JXL_RETURN_IF_ERROR(expected_number_of_parts == total);
    }

    if (index == 0 || index > total) {
      return JXL_FAILURE("Invalid chunk index.");
    }

    if (presence[index]) {
      return JXL_FAILURE("Duplicate chunk.");
    }
    presence[index] = true;
    chunks[index] = payload;
  }

  for (size_t i = 0; i < expected_number_of_parts; ++i) {
    // 0-th element is not used.
    size_t index = i + 1;
    if (!presence[index]) {
      return JXL_FAILURE("Missing chunk.");
    }
    output->append(chunks[index]);
  }

  return true;
}

jxl::Status SetColorEncodingFromIccData(j_compress_ptr cinfo,
                                        jxl::ColorEncoding* color_encoding) {
  jxl::PaddedBytes icc_profile;
  if (!ParseChunkedMarker(cinfo, kApp2, ByteSpan(kIccProfileTag),
                          &icc_profile)) {
    JXL_WARNING("ReJPEG: corrupted ICC profile\n");
    icc_profile.clear();
  }

  if (icc_profile.empty()) {
    bool is_gray = (cinfo->num_components == 1);
    *color_encoding = jxl::ColorEncoding::SRGB(is_gray);
    return true;
  }

  return color_encoding->SetICC(std::move(icc_profile));
}

float LinearQualityToDistance(int scale_factor) {
  scale_factor = std::min(5000, std::max(0, scale_factor));
  int quality =
      scale_factor < 100 ? 100 - scale_factor / 2 : 5000 / scale_factor;
  return jpegli_quality_to_distance(quality);
}

float DistanceToLinearQuality(float distance) {
  if (distance <= 0.1f) {
    return 1.0f;
  } else if (distance <= 4.6f) {
    return (200.0f / 9.0f) * (distance - 0.1f);
  } else if (distance <= 6.4f) {
    return 5000.0f / (100.0f - (distance - 0.1f) / 0.09f);
  } else if (distance < 25.0f) {
    return 530000.0f /
           (3450.0f -
            300.0f * std::sqrt((848.0f * distance - 5330.0f) / 120.0f));
  } else {
    return 5000.0f;
  }
}

struct ProgressiveScan {
  int Ss, Se, Ah, Al;
  bool interleaved;
};

template <typename T>
std::vector<uint8_t> CreateICCAppMarker(const T& icc) {
  std::vector<uint8_t> icc_marker(18 + icc.size());
  // See the APP2 marker format for embedded ICC profile at
  // https://www.color.org/technotes/ICC-Technote-ProfileEmbedding.pdf
  icc_marker[0] = 0xff;
  icc_marker[1] = 0xe2;  // APP2 marker
  // ICC marker size (excluding the marker bytes).
  icc_marker[2] = (icc_marker.size() - 2) >> 8;
  icc_marker[3] = (icc_marker.size() - 2) & 0xFF;
  // Byte sequence identifying an APP2 marker containing an icc profile.
  memcpy(&icc_marker[4], "ICC_PROFILE", 12);
  icc_marker[16] = 1;  // Sequence number
  icc_marker[17] = 1;  // Number of chunks.
  memcpy(&icc_marker[18], icc.data(), icc.size());
  return icc_marker;
}

std::vector<uint8_t> CreateXybICCAppMarker() {
  jxl::ColorEncoding c_xyb;
  c_xyb.SetColorSpace(jxl::ColorSpace::kXYB);
  c_xyb.rendering_intent = jxl::RenderingIntent::kPerceptual;
  JXL_CHECK(c_xyb.CreateICC());
  return CreateICCAppMarker(c_xyb.ICC());
}

void SetICCAppMarker(j_compress_ptr cinfo, const std::vector<uint8_t>& icc) {
  std::vector<std::vector<uint8_t>> special_markers;
  bool icc_added = false;
  for (auto& v : cinfo->master->special_markers) {
    JXL_DASSERT(v.size() >= 2);
    if (v[1] != 0xe2) {
      special_markers.emplace_back(std::move(v));
    } else if (!icc_added) {
      // TODO(szabadka) Handle too big icc data.
      special_markers.push_back(icc);
      icc_added = true;
    }
  }
  if (!icc_added) {
    special_markers.push_back(icc);
  }
  std::swap(cinfo->master->special_markers, special_markers);
}

void SetDefaultScanScript(j_compress_ptr cinfo, int max_shift) {
  int level = cinfo->master->progressive_level;
  std::vector<jpegli::ProgressiveScan> progressive_mode;
  if (level == 0) {
    progressive_mode.push_back({0, 63, 0, 0, true});
  } else if (level == 1) {
    progressive_mode.push_back({0, 0, 0, 0, max_shift > 0});
    progressive_mode.push_back({1, 63, 0, 1, false});
    progressive_mode.push_back({1, 63, 1, 0, false});
  } else {
    progressive_mode.push_back({0, 0, 0, 0, max_shift > 0});
    progressive_mode.push_back({1, 2, 0, 0, false});
    progressive_mode.push_back({3, 63, 0, 2, false});
    progressive_mode.push_back({3, 63, 2, 1, false});
    progressive_mode.push_back({3, 63, 1, 0, false});
  }

  cinfo->script_space_size = 0;
  for (const auto& scan : progressive_mode) {
    cinfo->script_space_size += scan.interleaved ? 1 : cinfo->num_components;
  }
  cinfo->script_space =
      jpegli::Allocate<jpeg_scan_info>(cinfo, cinfo->script_space_size);

  jpeg_scan_info* next_scan = cinfo->script_space;
  for (const auto& scan : progressive_mode) {
    if (scan.interleaved) {
      next_scan->Ss = scan.Ss;
      next_scan->Se = scan.Se;
      next_scan->Ah = scan.Ah;
      next_scan->Al = scan.Al;
      next_scan->comps_in_scan = cinfo->num_components;
      for (int c = 0; c < cinfo->num_components; ++c) {
        next_scan->component_index[c] = c;
      }
      ++next_scan;
    } else {
      for (int c = 0; c < cinfo->num_components; ++c) {
        next_scan->Ss = scan.Ss;
        next_scan->Se = scan.Se;
        next_scan->Ah = scan.Ah;
        next_scan->Al = scan.Al;
        next_scan->comps_in_scan = 1;
        next_scan->component_index[0] = c;
        ++next_scan;
      }
    }
  }
  JXL_ASSERT(next_scan - cinfo->script_space == cinfo->script_space_size);
  cinfo->scan_info = cinfo->script_space;
  cinfo->num_scans = cinfo->script_space_size;
}

}  // namespace jpegli

void jpegli_CreateCompress(j_compress_ptr cinfo, int version,
                           size_t structsize) {
  cinfo->master = nullptr;
  cinfo->mem = nullptr;
  if (structsize != sizeof(*cinfo)) {
    JPEGLI_ERROR("jpegli_compress_struct has wrong size.");
  }
  cinfo->master = new jpeg_comp_master;
  cinfo->mem =
      reinterpret_cast<struct jpeg_memory_mgr*>(new jpegli::MemoryManager);
  cinfo->is_decompressor = FALSE;
  cinfo->dest = nullptr;
  cinfo->restart_interval = 0;
  for (int i = 0; i < NUM_QUANT_TBLS; ++i) {
    cinfo->quant_tbl_ptrs[i] = nullptr;
  }
  cinfo->scan_info = nullptr;
  cinfo->num_scans = 0;
  cinfo->master->cur_marker_data = nullptr;
  cinfo->master->distance = 1.0;
  cinfo->master->xyb_mode = false;
  cinfo->master->use_std_tables = false;
  cinfo->master->use_adaptive_quantization = true;
  cinfo->master->progressive_level = 2;
  cinfo->master->data_type = JPEGLI_TYPE_UINT8;
  cinfo->master->endianness = JPEGLI_NATIVE_ENDIAN;
}

void jpegli_destroy_compress(j_compress_ptr cinfo) {
  jpegli_destroy(reinterpret_cast<j_common_ptr>(cinfo));
}

void jpegli_set_xyb_mode(j_compress_ptr cinfo) {
  cinfo->master->xyb_mode = true;
}

void jpegli_set_defaults(j_compress_ptr cinfo) {
  if (cinfo->master->xyb_mode &&
      (cinfo->input_components != 3 || cinfo->in_color_space != JCS_RGB)) {
    JPEGLI_ERROR("Only RGB input is supported in XYB mode.");
  }
  cinfo->num_components = cinfo->input_components;
  cinfo->comp_info =
      jpegli::Allocate<jpeg_component_info>(cinfo, cinfo->num_components);
  for (int c = 0; c < cinfo->num_components; ++c) {
    jpeg_component_info* comp = &cinfo->comp_info[c];
    comp->h_samp_factor = 1;
    comp->v_samp_factor = 1;
    comp->quant_tbl_no = c;
    comp->component_index = c;
  }
  if (cinfo->master->xyb_mode) {
    cinfo->comp_info[0].component_id = 'R';
    cinfo->comp_info[1].component_id = 'G';
    cinfo->comp_info[2].component_id = 'B';
    // Subsample blue channel.
    cinfo->comp_info[0].h_samp_factor = cinfo->comp_info[0].v_samp_factor = 2;
    cinfo->comp_info[1].h_samp_factor = cinfo->comp_info[1].v_samp_factor = 2;
    cinfo->comp_info[2].h_samp_factor = cinfo->comp_info[2].v_samp_factor = 1;
  } else {
    for (int i = 0; i < cinfo->num_components; ++i) {
      cinfo->comp_info[i].component_id = i + 1;
    }
  }
  cinfo->scan_info = nullptr;
  cinfo->num_scans = 0;
}

void jpegli_default_colorspace(j_compress_ptr cinfo) {}

void jpegli_set_colorspace(j_compress_ptr cinfo, J_COLOR_SPACE colorspace) {
  cinfo->master->jpeg_colorspace = colorspace;
}

void jpegli_set_distance(j_compress_ptr cinfo, float distance) {
  cinfo->master->distance = distance;
}

float jpegli_quality_to_distance(int quality) {
  return (quality >= 100  ? 0.01f
          : quality >= 30 ? 0.1f + (100 - quality) * 0.09f
                          : 53.0f / 3000.0f * quality * quality -
                                23.0f / 20.0f * quality + 25.0f);
}

void jpegli_set_quality(j_compress_ptr cinfo, int quality,
                        boolean force_baseline) {
  cinfo->master->distance = jpegli_quality_to_distance(quality);
  cinfo->master->force_baseline = force_baseline;
}

void jpegli_set_linear_quality(j_compress_ptr cinfo, int scale_factor,
                               boolean force_baseline) {
  cinfo->master->distance = jpegli::LinearQualityToDistance(scale_factor);
  cinfo->master->force_baseline = force_baseline;
}

int jpegli_quality_scaling(int quality) {
  quality = std::min(100, std::max(1, quality));
  return quality < 50 ? 5000 / quality : 200 - 2 * quality;
}

void jpegli_add_quant_table(j_compress_ptr cinfo, int which_tbl,
                            const unsigned int* basic_table, int scale_factor,
                            boolean force_baseline) {}

void jpegli_simple_progression(j_compress_ptr cinfo) {
  jpegli_set_progressive_level(cinfo, 2);
}

void jpegli_suppress_tables(j_compress_ptr cinfo, boolean suppress) {}

void jpegli_write_m_header(j_compress_ptr cinfo, int marker,
                           unsigned int datalen) {
  jpeg_comp_master* m = cinfo->master;
  if (datalen > jpegli::kMaxBytesInMarker) {
    JPEGLI_ERROR("Invalid marker length %u", datalen);
  }
  if (marker != 0xfe && (marker < 0xe0 || marker > 0xef)) {
    JPEGLI_ERROR(
        "jpegli_write_m_header: Only APP and COM markers are supported.");
  }
  std::vector<uint8_t> marker_data(4);
  marker_data[0] = 0xff;
  marker_data[1] = marker;
  marker_data[2] = (datalen + 2) >> 8;
  marker_data[3] = (datalen + 2) & 0xff;
  m->special_markers.emplace_back(std::move(marker_data));
  m->cur_marker_data = &m->special_markers.back();
}

void jpegli_write_m_byte(j_compress_ptr cinfo, int val) {
  jpeg_comp_master* m = cinfo->master;
  if (m->cur_marker_data == nullptr) {
    JPEGLI_ERROR("Marker header missing.");
  }
  m->cur_marker_data->push_back(val);
}

void jpegli_write_icc_profile(j_compress_ptr cinfo, const JOCTET* icc_data_ptr,
                              unsigned int icc_data_len) {
  constexpr size_t kMaxIccBytesInMarker =
      jpegli::kMaxBytesInMarker - sizeof jpegli::kICCSignature - 2;
  const int num_markers =
      static_cast<int>(jpegli::DivCeil(icc_data_len, kMaxIccBytesInMarker));
  size_t begin = 0;
  for (int current_marker = 0; current_marker < num_markers; ++current_marker) {
    const size_t length = std::min(kMaxIccBytesInMarker, icc_data_len - begin);
    jpegli_write_m_header(
        cinfo, jpegli::kICCMarker,
        static_cast<unsigned int>(length + sizeof jpegli::kICCSignature + 2));
    for (const unsigned char c : jpegli::kICCSignature) {
      jpegli_write_m_byte(cinfo, c);
    }
    jpegli_write_m_byte(cinfo, current_marker + 1);
    jpegli_write_m_byte(cinfo, num_markers);
    for (size_t i = 0; i < length; ++i) {
      jpegli_write_m_byte(cinfo, icc_data_ptr[begin]);
      ++begin;
    }
  }
}

void jpegli_start_compress(j_compress_ptr cinfo, boolean write_all_tables) {
  jpeg_comp_master* m = cinfo->master;
  cinfo->next_scanline = 0;
  if (cinfo->scan_info != nullptr) {
    cinfo->progressive_mode =
        cinfo->scan_info->Ss != 0 || cinfo->scan_info->Se != DCTSIZE2 - 1;
  } else {
    cinfo->progressive_mode = cinfo->master->progressive_level > 0;
  }
  cinfo->max_h_samp_factor = cinfo->max_v_samp_factor = 1;
  for (int c = 0; c < cinfo->num_components; ++c) {
    jpeg_component_info* comp = &cinfo->comp_info[c];
    cinfo->max_h_samp_factor =
        std::max(comp->h_samp_factor, cinfo->max_h_samp_factor);
    cinfo->max_v_samp_factor =
        std::max(comp->v_samp_factor, cinfo->max_v_samp_factor);
  }
  m->max_shift = 0;
  for (int c = 0; c < cinfo->num_components; ++c) {
    jpeg_component_info* comp = &cinfo->comp_info[c];
    if (comp->h_samp_factor != comp->v_samp_factor) {
      // TODO(szabadka) Remove this restriction.
      JPEGLI_ERROR(
          "Horizontal- or vertical-only subsampling is not "
          "supported.");
    }
    if (cinfo->max_h_samp_factor % comp->h_samp_factor != 0) {
      JPEGLI_ERROR("Non-integral sampling ratios are not supported.");
    }
    int factor = cinfo->max_h_samp_factor / comp->h_samp_factor;
    bool valid_factor = false;
    int shift = 0;
    for (; shift < 4; ++shift) {
      if (factor == (1 << shift)) {
        valid_factor = true;
        break;
      }
    }
    if (!valid_factor) {
      JPEGLI_ERROR("Invalid sampling factor %d", factor);
    }
    m->max_shift = std::max(shift, m->max_shift);
  }
  m->xsize_blocks = jpegli::DivCeil(cinfo->image_width, DCTSIZE << m->max_shift)
                    << m->max_shift;
  m->ysize_blocks =
      jpegli::DivCeil(cinfo->image_height, DCTSIZE << m->max_shift)
      << m->max_shift;
  m->input = jxl::Image3F(m->xsize_blocks * DCTSIZE, m->ysize_blocks * DCTSIZE);
  m->input.ShrinkTo(cinfo->image_width, cinfo->image_height);
}

JDIMENSION jpegli_write_scanlines(j_compress_ptr cinfo, JSAMPARRAY scanlines,
                                  JDIMENSION num_lines) {
  jpeg_comp_master* m = cinfo->master;
  // TODO(szabadka) Handle CMYK input images.
  if (cinfo->num_components > 3) {
    JPEGLI_ERROR("Invalid number of components.");
  }
  if (num_lines + cinfo->next_scanline > cinfo->image_height) {
    num_lines = cinfo->image_height - cinfo->next_scanline;
  }
  // const int bytes_per_sample = jpegli_bytes_per_sample(m->data_type);
  const int bytes_per_sample = m->data_type == JPEGLI_TYPE_UINT8    ? 1
                               : m->data_type == JPEGLI_TYPE_UINT16 ? 2
                                                                    : 4;
  const int pwidth = cinfo->num_components * bytes_per_sample;
  bool is_little_endian =
      (m->endianness == JPEGLI_LITTLE_ENDIAN ||
       (m->endianness == JPEGLI_NATIVE_ENDIAN && IsLittleEndian()));
  static constexpr double kMul8 = 1.0 / 255.0;
  static constexpr double kMul16 = 1.0 / 65535.0;
  for (int c = 0; c < cinfo->num_components; ++c) {
    for (size_t i = 0; i < num_lines; ++i) {
      float* row = m->input.PlaneRow(c, cinfo->next_scanline + i);
      if (m->data_type == JPEGLI_TYPE_UINT8) {
        uint8_t* p = &scanlines[i][c];
        for (size_t x = 0; x < cinfo->image_width; ++x, p += pwidth) {
          row[x] = p[0] * kMul8;
        }
      } else if (m->data_type == JPEGLI_TYPE_UINT16 && is_little_endian) {
        uint8_t* p = &scanlines[i][c * 2];
        for (size_t x = 0; x < cinfo->image_width; ++x, p += pwidth) {
          row[x] = LoadLE16(p) * kMul16;
        }
      } else if (m->data_type == JPEGLI_TYPE_UINT16 && !is_little_endian) {
        uint8_t* p = &scanlines[i][c * 2];
        for (size_t x = 0; x < cinfo->image_width; ++x, p += pwidth) {
          row[x] = LoadBE16(p) * kMul16;
        }
      } else if (m->data_type == JPEGLI_TYPE_FLOAT && is_little_endian) {
        uint8_t* p = &scanlines[i][c * 4];
        for (size_t x = 0; x < cinfo->image_width; ++x, p += pwidth) {
          row[x] = LoadLEFloat(p);
        }
      } else if (m->data_type == JPEGLI_TYPE_FLOAT && !is_little_endian) {
        uint8_t* p = &scanlines[i][c * 4];
        for (size_t x = 0; x < cinfo->image_width; ++x, p += pwidth) {
          row[x] = LoadBEFloat(p);
        }
      }
    }
  }
  cinfo->next_scanline += num_lines;
  return num_lines;
}

void jpegli_finish_compress(j_compress_ptr cinfo) {
  jpeg_comp_master* m = cinfo->master;

  const bool use_xyb = m->xyb_mode;
  const bool use_aq = m->use_adaptive_quantization;
  const bool use_std_tables = m->use_std_tables;
  jpegli::QuantMode quant_mode = use_xyb          ? jpegli::QUANT_XYB
                                 : use_std_tables ? jpegli::QUANT_STD
                                                  : jpegli::QUANT_YUV;

  if (use_xyb && cinfo->num_components != 3) {
    JPEGLI_ERROR("Only RGB input is supported in XYB mode.");
  }
  if (cinfo->num_components == 1) {
    CopyImageTo(m->input.Plane(0), &m->input.Plane(1));
    CopyImageTo(m->input.Plane(0), &m->input.Plane(2));
  }
  jxl::ColorEncoding color_encoding;
  if (!jpegli::SetColorEncodingFromIccData(cinfo, &color_encoding)) {
    JPEGLI_ERROR("Could not parse ICC profile.");
  }
  if (use_xyb) {
    jpegli::SetICCAppMarker(cinfo, jpegli::CreateXybICCAppMarker());
  }
  jxl::Image3F& input = m->input;
  float distance = m->distance;

  if (use_xyb) {
    // Convert input to XYB colorspace.
    jxl::Image3F opsin(m->xsize_blocks * DCTSIZE, m->ysize_blocks * DCTSIZE);
    opsin.ShrinkTo(cinfo->image_width, cinfo->image_height);
    jxl::Image3FToXYB(input, color_encoding, 255.0, nullptr, &opsin,
                      jxl::GetJxlCms());
    ScaleXYB(&opsin);
    input.Swap(opsin);
  } else {
    for (size_t y = 0; y < cinfo->image_height; ++y) {
      jpegli::RGBToYCbCr(input.PlaneRow(0, y), input.PlaneRow(1, y),
                         input.PlaneRow(2, y), cinfo->image_width);
    }
  }
  PadImageToBlockMultipleInPlace(&input, DCTSIZE << m->max_shift);

  // Compute adaptive quant field.
  jxl::ImageF qf(m->xsize_blocks, m->ysize_blocks);
  if (use_aq) {
    int y_channel = use_xyb ? 1 : 0;
    qf = jpegli::InitialQuantField(distance, input.Plane(y_channel), nullptr,
                                   distance);
  } else {
    FillImage(0.575f, &qf);
  }
  float qfmin, qfmax;
  ImageMinMax(qf, &qfmin, &qfmax);

  // Global scale is chosen in a way that butteraugli 3-norm matches libjpeg
  // with the same quality setting. Fitted for quality 90 on jyrki31 corpus.
  constexpr float kGlobalScaleXYB = 0.86747522f;
  constexpr float kGlobalScaleYCbCr = 1.03148720f;
  constexpr float kGlobalScaleStd = 1.0f;
  constexpr float kGlobalScales[jpegli::NUM_QUANT_MODES] = {
      kGlobalScaleXYB, kGlobalScaleYCbCr, kGlobalScaleStd};
  float global_scale = kGlobalScales[quant_mode];
  float ac_scale, dc_scale;
  if (!use_xyb) {
    if (color_encoding.tf.IsPQ()) {
      global_scale *= .4f;
    } else if (color_encoding.tf.IsHLG()) {
      global_scale *= .5f;
    }
  }
  if (use_xyb || !use_std_tables) {
    ac_scale = global_scale * distance / qfmax;
    dc_scale = global_scale / jpegli::InitialQuantDC(distance);
  } else {
    float linear_scale = 0.01f * jpegli::DistanceToLinearQuality(distance);
    ac_scale = global_scale * linear_scale;
    dc_scale = global_scale * linear_scale;
  }

  //
  // Start writing to the bitstream
  //
  (*cinfo->dest->init_destination)(cinfo);

  // SOI
  jpegli::WriteOutput(cinfo, {0xFF, 0xD8});

  // APPn, COM
  for (const auto& v : m->special_markers) {
    jpegli::WriteOutput(cinfo, v);
  }

  // DQT
  float qm[3 * jpegli::kDCTBlockSize];
  jpegli::AddJpegQuantMatrices(cinfo, quant_mode, dc_scale, ac_scale, qm);
  jpegli::EncodeDQT(cinfo);

  // SOF
  jpegli::EncodeSOF(cinfo);

  for (int c = 0; c < cinfo->num_components; ++c) {
    const size_t factor =
        (cinfo->max_h_samp_factor / cinfo->comp_info[c].h_samp_factor);
    JXL_ASSERT(m->xsize_blocks % factor == 0);
    JXL_ASSERT(m->ysize_blocks % factor == 0);
    // TODO(szabadka): These fields have a different meaning than in libjpeg,
    // make sure it does not cause problems or change it to the libjpeg values.
    cinfo->comp_info[c].width_in_blocks = m->xsize_blocks / factor;
    cinfo->comp_info[c].height_in_blocks = m->ysize_blocks / factor;
  }
  std::vector<std::vector<jpegli::coeff_t>> coeffs;
  jpegli::ComputeDCTCoefficients(cinfo, input, distance, use_xyb, qf, qm,
                                 &coeffs);

  if (cinfo->scan_info == nullptr) {
    jpegli::SetDefaultScanScript(cinfo, m->max_shift);
  }

  std::vector<jpegli::JPEGHuffmanCode> huffman_codes;
  jpegli::OptimizeHuffmanCodes(cinfo, coeffs, &huffman_codes);

  // DRI
  if (cinfo->restart_interval > 0) {
    jpegli::EncodeDRI(cinfo);
  }

  size_t dht_index = 0;
  for (int i = 0; i < cinfo->num_scans; ++i) {
    jpegli::EncodeDHT(cinfo, huffman_codes, &dht_index,
                      m->scan_coding_info[i].num_huffman_codes);
    jpegli::EncodeSOS(cinfo, i);
    if (!jpegli::EncodeScan(cinfo, coeffs, i)) {
      JPEGLI_ERROR("Failed to encode scan.");
    }
  }
  // EOI
  jpegli::WriteOutput(cinfo, {0xFF, 0xD9});
  (*cinfo->dest->term_destination)(cinfo);
}

void jpegli_set_input_format(j_compress_ptr cinfo, JpegliDataType data_type,
                             JpegliEndianness endianness) {
  cinfo->master->data_type = data_type;
  cinfo->master->endianness = endianness;
}

void jpegli_enable_adaptive_quantization(j_compress_ptr cinfo, boolean value) {
  cinfo->master->use_adaptive_quantization = value;
}

void jpegli_set_progressive_level(j_compress_ptr cinfo, int level) {
  if (level < 0) {
    JPEGLI_ERROR("Invalid progressive level %d", level);
  }
  cinfo->master->progressive_level = level;
}

void jpegli_use_standard_quant_tables(j_compress_ptr cinfo) {
  cinfo->master->use_std_tables = true;
}