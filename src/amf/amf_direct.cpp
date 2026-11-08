/**
 * @file src/amf/amf_direct.cpp
 * @brief Definitions for encoding AV1 and HEVC through AMD's AMF runtime directly, without FFmpeg.
 */
// this include
#include "amf_direct.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstring>
#include <format>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/video.h"

#ifdef _WIN32
  // lib includes
  #include <AMF/components/ColorSpace.h>
  #include <AMF/components/VideoEncoderAV1.h>
  #include <AMF/components/VideoEncoderHEVC.h>
  #include <AMF/core/Buffer.h>
  #include <AMF/core/Factory.h>
  #include <AMF/core/Surface.h>
  #include <AMF/core/Version.h>
#endif

using namespace std::literals;

namespace amf_direct {

  color_t color_from_colorspace(const video::sunshine_colorspace_t &colorspace) {
    // AMF_VIDEO_CONVERTER_COLOR_PROFILE_ENUM, and ISO/IEC 23001-8 code points
    constexpr std::int64_t profile_601 = 0;
    constexpr std::int64_t profile_709 = 1;
    constexpr std::int64_t profile_2020 = 2;
    constexpr std::int64_t profile_full_601 = 3;
    constexpr std::int64_t profile_full_709 = 7;
    constexpr std::int64_t profile_full_2020 = 8;

    constexpr std::int64_t transfer_bt709 = 1;
    constexpr std::int64_t transfer_smpte170m = 6;
    constexpr std::int64_t transfer_bt2020_10 = 14;
    constexpr std::int64_t transfer_smpte2084 = 16;

    constexpr std::int64_t primaries_bt709 = 1;
    constexpr std::int64_t primaries_smpte170m = 6;
    constexpr std::int64_t primaries_bt2020 = 9;

    const bool full = colorspace.full_range;
    switch (colorspace.colorspace) {
      case video::colorspace_e::rec601:
        return {full ? profile_full_601 : profile_601, transfer_smpte170m, primaries_smpte170m};
      case video::colorspace_e::rec709:
        return {full ? profile_full_709 : profile_709, transfer_bt709, primaries_bt709};
      case video::colorspace_e::bt2020sdr:
        return {full ? profile_full_2020 : profile_2020, transfer_bt2020_10, primaries_bt2020};
      case video::colorspace_e::bt2020:
      default:
        return {full ? profile_full_2020 : profile_2020, transfer_smpte2084, primaries_bt2020};
    }
  }

  std::int64_t vbv_buffer_bits(std::int64_t bitrate_bps, int framerate) {
    return bitrate_bps / std::max(framerate, 1);
  }

  std::int64_t frame_duration(int fps_num, int fps_den) {
    constexpr std::int64_t second = 10'000'000;
    return second * std::max(fps_den, 1) / std::max(fps_num, 1);
  }

  std::int64_t ltr_interval(int fps_num, int fps_den) {
    // An eighth of a second
    return std::max<std::int64_t>(std::max(fps_num, 1) / (std::int64_t {8} * std::max(fps_den, 1)), 1);
  }

  std::int64_t hevc_level(int width, int height, int framerate) {
    // AMF_VIDEO_ENCODER_LEVEL_ENUM
    constexpr std::int64_t level_5_1 = 153;
    constexpr std::int64_t level_5_2 = 156;

    // Luma samples per picture and per second that levels 5.1 and 5.2 allow
    const auto size = static_cast<std::int64_t>(width) * height;
    const auto rate = size * framerate;
    if (size <= 8'912'896) {
      if (rate <= 534'773'760) {
        return level_5_1;
      }
      if (rate <= 1'069'547'520) {
        return level_5_2;
      }
    }
    return 0;
  }

  std::size_t obu_bytes(const std::uint8_t *data, std::size_t size, bool through_first_tile) {
    std::size_t end = 0;
    while (end < size) {
      const auto header = data[end];
      // The forbidden bit, a reserved type 0 (as a zero byte is), or no size field ends the OBUs
      if ((header & 0x80) || ((header >> 3) & 0x0F) == 0 || !(header & 0x02)) {
        break;
      }

      std::size_t offset = end + 1 + ((header & 0x04) ? 1 : 0);
      std::uint64_t payload = 0;
      int shift = 0;
      bool size_read = false;
      while (offset < size && shift <= 56) {
        const auto byte = data[offset++];
        payload |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
          size_read = true;
          break;
        }
      }
      if (!size_read || offset > size || payload > size - offset) {
        break;
      }
      end = offset + payload;

      // A tile group (4) or a frame (6) carries the tile
      const auto type = (header >> 3) & 0x0F;
      if (through_first_tile && (type == 4 || type == 6)) {
        break;
      }
    }
    return end;
  }

  std::vector<tile_group_t> tile_groups(const std::uint8_t *data, std::size_t size, int tile_bits) {
    std::vector<tile_group_t> groups;
    std::size_t offset = 0;
    while (offset < size) {
      const auto header = data[offset];
      if ((header & 0x80) || ((header >> 3) & 0x0F) == 0 || !(header & 0x02)) {
        break;
      }

      std::size_t start = offset + 1 + ((header & 0x04) ? 1 : 0);
      std::uint64_t payload = 0;
      int shift = 0;
      bool size_read = false;
      while (start < size && shift <= 56) {
        const auto byte = data[start++];
        payload |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
          size_read = true;
          break;
        }
      }
      if (!size_read || start > size || payload > size - start) {
        break;
      }

      const auto type = (header >> 3) & 0x0F;
      if (type == 6) {
        groups.push_back({-1, -1, offset, start + payload - offset});
      } else if (type == 4) {
        // tile_start_and_end_present_flag, then tg_start and tg_end of tile_bits each, read
        // from the most significant bit. One tile, or no flag, means all the tiles.
        const auto last = (1 << tile_bits) - 1;
        tile_group_t group {0, last, offset, start + payload - offset};
        if (tile_bits > 0 && payload > 0 && (data[start] & 0x80)) {
          std::size_t bit = 1;
          auto read = [&](int bits) {
            int value = 0;
            for (int i = 0; i < bits; ++i, ++bit) {
              const auto byte = bit / 8;
              const auto bit_value = byte < payload ? (data[start + byte] >> (7 - bit % 8)) & 1 : 0;
              value = (value << 1) | bit_value;
            }
            return value;
          };
          group.start = read(tile_bits);
          group.end = read(tile_bits);
        }
        groups.push_back(group);
      }
      offset = start + payload;
    }
    return groups;
  }

  std::string describe_obus(const std::uint8_t *data, std::size_t size) {
    std::string result;
    std::size_t offset = 0;
    while (offset < size) {
      const auto start = offset;
      const auto header = data[offset++];
      const auto type = (header >> 3) & 0x0F;
      const bool extension = header & 0x04;
      const bool has_size = header & 0x02;
      if (header & 0x80) {
        return result + std::format("{}invalid at {}", result.empty() ? "" : " ", start);
      }
      if (extension) {
        ++offset;
      }

      std::uint64_t payload = 0;
      if (has_size) {
        // leb128
        int shift = 0;
        while (true) {
          if (offset >= size || shift > 56) {
            return result + std::format("{}invalid at {}", result.empty() ? "" : " ", start);
          }
          const auto byte = data[offset++];
          payload |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
          shift += 7;
          if (!(byte & 0x80)) {
            break;
          }
        }
      } else {
        payload = offset <= size ? size - offset : 0;
      }
      if (offset > size || payload > size - offset) {
        return result + std::format("{}invalid at {}", result.empty() ? "" : " ", start);
      }

      static constexpr const char *names[16] {"0", "SEQ", "TD", "FH", "TG", "META", "FRAME", "RFH", "TL", "9", "10", "11", "12", "13", "14", "PAD"};
      result += std::format("{}{}{}:{}", result.empty() ? "" : " ", names[type], has_size ? "" : "(no size)", payload);
      offset += payload;
    }
    return result;
  }

  std::string describe_nals(const std::uint8_t *data, std::size_t size) {
    // Where each NAL unit's header starts, after its 3 byte start code
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 <= size; ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        starts.push_back(i + 3);
        i += 2;
      }
    }

    std::string result;
    if (starts.empty() || starts.front() > 4 || (starts.front() == 4 && data[0] != 0)) {
      result = "invalid at 0";
    }
    for (std::size_t k = 0; k < starts.size(); ++k) {
      const auto start = starts[k];
      auto end = k + 1 < starts.size() ? starts[k + 1] - 3 : size;
      // A 4 byte start code's leading zero belongs to it, not to the NAL unit before
      if (k + 1 < starts.size() && end > start && data[end - 1] == 0) {
        --end;
      }

      std::string name;
      if (start >= size) {
        name = "?";
      } else {
        switch (const auto type = (data[start] >> 1) & 0x3F) {
          case 0:
            name = "TRAIL_N";
            break;
          case 1:
            name = "TRAIL_R";
            break;
          case 19:
            name = "IDR_W_RADL";
            break;
          case 20:
            name = "IDR_N_LP";
            break;
          case 21:
            name = "CRA";
            break;
          case 32:
            name = "VPS";
            break;
          case 33:
            name = "SPS";
            break;
          case 34:
            name = "PPS";
            break;
          case 35:
            name = "AUD";
            break;
          case 39:
          case 40:
            name = "SEI";
            break;
          default:
            name = std::to_string(type);
            break;
        }
      }
      result += std::format("{}{}:{}", result.empty() ? "" : " ", name, end - start);
    }
    return result;
  }

  void part_timing_t::add(const std::vector<part_t> &parts) {
    if (parts.empty()) {
      return;
    }

    ++frame_count;
    part_count += parts.size();
    first_ms += std::chrono::duration<double, std::milli>(parts.front().after_submit).count();
    last_ms += std::chrono::duration<double, std::milli>(parts.back().after_submit).count();
    for (std::size_t i = 0; i < parts.size(); ++i) {
      bytes_total += parts[i].bytes;
      if (i + 1 < parts.size()) {
        bytes_before_last += parts[i].bytes;
      }
    }
  }

  std::size_t part_timing_t::frames() const {
    return frame_count;
  }

  std::string part_timing_t::summary() const {
    if (frame_count == 0) {
      return "no frames";
    }

    const auto frames = static_cast<double>(frame_count);
    const auto percent_before_last = bytes_total ? 100.0 * static_cast<double>(bytes_before_last) / static_cast<double>(bytes_total) : 0.0;
    return std::format("{} frames, {:.1f} parts: first {:.2f} ms, last {:.2f} ms after submit, {:.0f}% of the bytes before the last", frame_count, static_cast<double>(part_count) / frames, first_ms / frames, last_ms / frames, percent_before_last);
  }

  void part_timing_t::reset() {
    *this = {};
  }

  ltr_tracker_t::ltr_tracker_t(int slots, std::int64_t interval):
      slot_list(static_cast<std::size_t>(std::max(slots, 0))),
      interval {std::max<std::int64_t>(interval, 1)} {
  }

  int ltr_tracker_t::slots() const {
    return static_cast<int>(slot_list.size());
  }

  ltr_plan_t ltr_tracker_t::plan(std::int64_t frame_index, bool key_requested) {
    ltr_plan_t plan;

    if (lost && !key_requested) {
      const auto [first, last] = *lost;
      if (last >= recovered_at) {
        // Frames from the first lost one on may depend on it, so the client can't use them
        for (auto &slot : slot_list) {
          if (slot.frame >= first) {
            slot = {};
          }
        }

        plan.reference = newest_acknowledged();
        if (plan.reference < 0) {
          key_requested = true;
        }
        recovered_at = frame_index;
      }
    }
    lost.reset();

    if (key_requested) {
      clear();
      plan.key = true;
      recovered_at = frame_index;
    }

    // A recovering frame is left unmarked, as it references only an older reference
    if (!slot_list.empty() && plan.reference < 0 && (plan.key || last_mark < 0 || frame_index - last_mark >= interval)) {
      plan.mark = slot_to_mark();
      slot_list[plan.mark] = {frame_index, false};
      last_mark = frame_index;
    }
    return plan;
  }

  void ltr_tracker_t::acknowledge(std::int64_t frame_index) {
    for (auto &slot : slot_list) {
      if (slot.frame == frame_index) {
        slot.acknowledged = true;
      }
    }
  }

  void ltr_tracker_t::invalidate(std::int64_t first_frame, std::int64_t last_frame) {
    if (lost) {
      first_frame = std::min(first_frame, lost->first);
      last_frame = std::max(last_frame, lost->second);
    }
    lost = std::make_pair(first_frame, last_frame);
  }

  void ltr_tracker_t::forget(int slot) {
    if (slot >= 0 && slot < slots()) {
      slot_list[slot] = {};
    }
  }

  std::int64_t ltr_tracker_t::frame_in(int slot) const {
    return slot >= 0 && slot < slots() ? slot_list[slot].frame : -1;
  }

  int ltr_tracker_t::newest_acknowledged() const {
    int newest = -1;
    for (int i = 0; i < slots(); ++i) {
      if (slot_list[i].acknowledged && (newest < 0 || slot_list[i].frame > slot_list[newest].frame)) {
        newest = i;
      }
    }
    return newest;
  }

  int ltr_tracker_t::slot_to_mark() const {
    // An empty slot, or else the oldest one that isn't the newest the client has
    const auto keep = slots() > 1 ? newest_acknowledged() : -1;
    int oldest = -1;
    for (int i = 0; i < slots(); ++i) {
      if (slot_list[i].frame < 0) {
        return i;
      }
      if (i != keep && (oldest < 0 || slot_list[i].frame < slot_list[oldest].frame)) {
        oldest = i;
      }
    }
    return oldest;
  }

  void ltr_tracker_t::clear() {
    for (auto &slot : slot_list) {
      slot = {};
    }
  }

#ifdef _WIN32
  namespace {
    /**
     * @brief The AMF runtime, loaded once and kept for the life of the process.
     *
     * @return The AMF factory, or nullptr if the runtime couldn't be loaded.
     */
    amf::AMFFactory *factory() {
      static amf::AMFFactory *const instance = []() -> amf::AMFFactory * {
        HMODULE dll = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) {
          BOOST_LOG(debug) << "AMF direct: couldn't load "sv << AMF_DLL_NAMEA;
          return nullptr;
        }

        auto query_version = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(dll, AMF_QUERY_VERSION_FUNCTION_NAME));
        auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll, AMF_INIT_FUNCTION_NAME));
        if (!query_version || !init) {
          BOOST_LOG(warning) << "AMF direct: "sv << AMF_DLL_NAMEA << " is missing its entry points"sv;
          return nullptr;
        }

        amf_uint64 version = 0;
        query_version(&version);

        amf::AMFFactory *result = nullptr;
        if (init(AMF_FULL_VERSION, &result) != AMF_OK || !result) {
          BOOST_LOG(warning) << "AMF direct: AMFInit() failed"sv;
          return nullptr;
        }

        BOOST_LOG(info) << "AMF direct: loaded runtime "sv << AMF_GET_MAJOR_VERSION(version) << '.' << AMF_GET_MINOR_VERSION(version) << '.' << AMF_GET_SUBMINOR_VERSION(version) << '.' << AMF_GET_BUILD_VERSION(version);
        return result;
      }();
      return instance;
    }

    /**
     * @brief Longest wait for one call to QueryOutput(), in milliseconds.
     */
    constexpr amf_int64 query_timeout_ms = 50;

    /**
     * @brief Most QueryOutput() calls for one frame before giving up on it.
     */
    constexpr int max_queries = 40;

    /**
     * @brief Frames between part timing lines in the log.
     */
    constexpr std::size_t timing_log_frames = 600;

    /**
     * @brief The AMF component, property names and values that differ between codecs.
     *
     * @details The properties mean the same for each codec, so one encoder sets them by these
     *          names. What only one codec has is set by create() on its own.
     */
    struct codec_properties_t {
      std::string_view name;  ///< Codec name for the log.
      std::string_view part_name;  ///< What a part of a frame is called, for the log.
      const wchar_t *component;  ///< AMF component ID.
      const wchar_t *usage;  ///< Usage, set first as it sets the defaults for everything else.
      const wchar_t *frame_size;  ///< Frame size.
      const wchar_t *frame_rate;  ///< Frame rate.
      const wchar_t *profile;  ///< Profile.
      const wchar_t *bit_depth;  ///< Color bit depth.
      const wchar_t *color_profile;  ///< Output color profile.
      const wchar_t *full_range;  ///< Output full range flag.
      const wchar_t *transfer;  ///< Output transfer characteristic.
      const wchar_t *primaries;  ///< Output color primaries.
      const wchar_t *quality_preset;  ///< Quality preset.
      const wchar_t *gop_size;  ///< GOP size.
      const wchar_t *header_insertion_mode;  ///< When sequence headers are written.
      amf_int64 header_insertion_key_aligned;  ///< Header insertion mode that writes them with each key frame.
      const wchar_t *max_num_ref_frames;  ///< Most reference frames.
      const wchar_t *max_ltr_frames;  ///< Most long-term references.
      const wchar_t *ltr_mode;  ///< What happens to long-term references no frame references.
      amf_int64 ltr_mode_keep_unused;  ///< Long-term reference mode that keeps them until replaced.
      const wchar_t *rate_control;  ///< Rate control method.
      const wchar_t *target_bitrate;  ///< Target bitrate.
      const wchar_t *peak_bitrate;  ///< Peak bitrate.
      const wchar_t *vbv_buffer_size;  ///< VBV buffer size.
      const wchar_t *filler_data;  ///< Filler data.
      const wchar_t *skip_frame;  ///< Rate control frame skipping.
      const wchar_t *enforce_hrd;  ///< HRD enforcement.
      const wchar_t *preanalysis;  ///< Pre-analysis.
      const wchar_t *preencode;  ///< Pre-encode assisted rate control.
      const wchar_t *parts_per_frame;  ///< Tiles or slices per frame.
      const wchar_t *output_mode;  ///< Whether frames are handed over whole or in parts.
      amf_int64 output_mode_parts;  ///< Output mode that hands each part over as it finishes.
      const wchar_t *query_timeout;  ///< How long QueryOutput() waits for output.
      const wchar_t *force_key;  ///< Frame property that forces the frame type.
      amf_int64 force_key_value;  ///< Frame type that is a key frame.
      const wchar_t *insert_header;  ///< Frame property that writes the sequence headers with the frame.
      const wchar_t *mark_ltr;  ///< Frame property that keeps the frame as a long-term reference.
      const wchar_t *force_ltr_reference;  ///< Frame property that names the long-term references the frame may reference.
      const wchar_t *output_frame_type;  ///< Output property with the frame's type.
      amf_int64 output_key;  ///< Output frame type of a key frame.
      const wchar_t *output_marked_ltr;  ///< Output property with the long-term reference the frame was kept as.
      const wchar_t *output_referenced_ltr;  ///< Output property with the long-term references the frame references.
      const wchar_t *output_buffer_type;  ///< Output property that says whether the buffer is a part, the last part or a whole frame.
      amf_int64 output_buffer_part;  ///< Output buffer type of a part that isn't the last.
    };

    /**
     * @brief AV1's properties.
     */
    const codec_properties_t av1_properties {
      .name = "AV1"sv,
      .part_name = "tile"sv,
      .component = AMFVideoEncoder_AV1,
      .usage = AMF_VIDEO_ENCODER_AV1_USAGE,
      .frame_size = AMF_VIDEO_ENCODER_AV1_FRAMESIZE,
      .frame_rate = AMF_VIDEO_ENCODER_AV1_FRAMERATE,
      .profile = AMF_VIDEO_ENCODER_AV1_PROFILE,
      .bit_depth = AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH,
      .color_profile = AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE,
      .full_range = AMF_VIDEO_ENCODER_AV1_OUTPUT_FULL_RANGE_COLOR,
      .transfer = AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC,
      .primaries = AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES,
      .quality_preset = AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
      .gop_size = AMF_VIDEO_ENCODER_AV1_GOP_SIZE,
      .header_insertion_mode = AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
      .header_insertion_key_aligned = AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED,
      .max_num_ref_frames = AMF_VIDEO_ENCODER_AV1_MAX_NUM_REFRAMES,
      .max_ltr_frames = AMF_VIDEO_ENCODER_AV1_MAX_LTR_FRAMES,
      .ltr_mode = AMF_VIDEO_ENCODER_AV1_LTR_MODE,
      .ltr_mode_keep_unused = AMF_VIDEO_ENCODER_AV1_LTR_MODE_KEEP_UNUSED,
      .rate_control = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
      .target_bitrate = AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE,
      .peak_bitrate = AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE,
      .vbv_buffer_size = AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE,
      .filler_data = AMF_VIDEO_ENCODER_AV1_FILLER_DATA,
      .skip_frame = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_SKIP_FRAME,
      .enforce_hrd = AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD,
      .preanalysis = AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE,
      .preencode = AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_PREENCODE,
      .parts_per_frame = AMF_VIDEO_ENCODER_AV1_TILES_PER_FRAME,
      .output_mode = AMF_VIDEO_ENCODER_AV1_OUTPUT_MODE,
      .output_mode_parts = AMF_VIDEO_ENCODER_AV1_OUTPUT_MODE_TILE,
      .query_timeout = AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT,
      .force_key = AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE,
      .force_key_value = AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY,
      .insert_header = AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER,
      .mark_ltr = AMF_VIDEO_ENCODER_AV1_MARK_CURRENT_WITH_LTR_INDEX,
      .force_ltr_reference = AMF_VIDEO_ENCODER_AV1_FORCE_LTR_REFERENCE_BITFIELD,
      .output_frame_type = AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE,
      .output_key = AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY,
      .output_marked_ltr = AMF_VIDEO_ENCODER_AV1_OUTPUT_MARKED_LTR_INDEX,
      .output_referenced_ltr = AMF_VIDEO_ENCODER_AV1_OUTPUT_REFERENCED_LTR_INDEX_BITFIELD,
      .output_buffer_type = AMF_VIDEO_ENCODER_AV1_OUTPUT_BUFFER_TYPE,
      .output_buffer_part = AMF_VIDEO_ENCODER_AV1_OUTPUT_BUFFER_TYPE_TILE,
    };

    /**
     * @brief HEVC's properties.
     */
    const codec_properties_t hevc_properties {
      .name = "HEVC"sv,
      .part_name = "slice"sv,
      .component = AMFVideoEncoder_HEVC,
      .usage = AMF_VIDEO_ENCODER_HEVC_USAGE,
      .frame_size = AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
      .frame_rate = AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
      .profile = AMF_VIDEO_ENCODER_HEVC_PROFILE,
      .bit_depth = AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH,
      .color_profile = AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE,
      .full_range = AMF_VIDEO_ENCODER_HEVC_OUTPUT_FULL_RANGE_COLOR,
      .transfer = AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
      .primaries = AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES,
      .quality_preset = AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
      .gop_size = AMF_VIDEO_ENCODER_HEVC_GOP_SIZE,
      .header_insertion_mode = AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
      .header_insertion_key_aligned = AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED,
      .max_num_ref_frames = AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES,
      .max_ltr_frames = AMF_VIDEO_ENCODER_HEVC_MAX_LTR_FRAMES,
      .ltr_mode = AMF_VIDEO_ENCODER_HEVC_LTR_MODE,
      .ltr_mode_keep_unused = AMF_VIDEO_ENCODER_HEVC_LTR_MODE_KEEP_UNUSED,
      .rate_control = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
      .target_bitrate = AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
      .peak_bitrate = AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE,
      .vbv_buffer_size = AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE,
      .filler_data = AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE,
      .skip_frame = AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_SKIP_FRAME_ENABLE,
      .enforce_hrd = AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD,
      .preanalysis = AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE,
      .preencode = AMF_VIDEO_ENCODER_HEVC_PREENCODE_ENABLE,
      .parts_per_frame = AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME,
      .output_mode = AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE,
      .output_mode_parts = AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE_SLICE,
      .query_timeout = AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT,
      .force_key = AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
      .force_key_value = AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR,
      .insert_header = AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER,
      .mark_ltr = AMF_VIDEO_ENCODER_HEVC_MARK_CURRENT_WITH_LTR_INDEX,
      .force_ltr_reference = AMF_VIDEO_ENCODER_HEVC_FORCE_LTR_REFERENCE_BITFIELD,
      .output_frame_type = AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE,
      .output_key = AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR,
      .output_marked_ltr = AMF_VIDEO_ENCODER_HEVC_OUTPUT_MARKED_LTR_INDEX,
      .output_referenced_ltr = AMF_VIDEO_ENCODER_HEVC_OUTPUT_REFERENCED_LTR_INDEX_BITFIELD,
      .output_buffer_type = AMF_VIDEO_ENCODER_HEVC_OUTPUT_BUFFER_TYPE,
      .output_buffer_part = AMF_VIDEO_ENCODER_HEVC_OUTPUT_BUFFER_TYPE_SLICE,
    };
  }  // namespace

  bool runtime_available() {
    return factory() != nullptr;
  }

  /**
   * @brief AMF objects owned by the encoder.
   */
  struct encoder_t::impl_t {
    const codec_properties_t *properties = &av1_properties;  ///< Property names of the codec encoded.
    amf::AMFContextPtr context;  ///< AMF context on the encode device.
    amf::AMFComponentPtr encoder;  ///< Encoder component.
    ID3D11Texture2D *texture = nullptr;  ///< Texture frames are drawn into.
    amf::AMFSurfacePtr surface;  ///< The texture wrapped for AMF once, submitted for every frame.
    ltr_tracker_t ltr;  ///< Long-term references, to recover from lost frames.
    std::size_t recoveries = 0;  ///< Frames that recovered from lost frames with a long-term reference, since the last timing line.
    std::size_t recovery_keys = 0;  ///< Key frames sent for lost frames, with no long-term reference to recover from, since the last timing line.
    std::size_t ltr_misses = 0;  ///< Frames where AMF didn't mark or reference the long-term reference planned, since the last timing line.
    std::chrono::steady_clock::duration prepare_time {};  ///< Time spent setting up frames before SubmitInput(), since the last timing line.
    std::chrono::steady_clock::duration submit_time {};  ///< Time spent in SubmitInput(), since the last timing line.
    bool part_output = false;  ///< Whether AMF hands over each part on its own.
    int parts = 1;  ///< Parts in each frame.
    bool first_frame = true;  ///< Whether no frame has been encoded yet, as the first is a key frame.
    std::size_t idr_mismatches = 0;  ///< Frames whose key frame flag differed from what was asked for, since the last timing line.
    part_timing_t timing;  ///< When parts came out, for the log.
    std::int64_t duration = 0;  ///< Frame duration in AMF's 100 ns units.
    int frames_described = 0;  ///< Frames whose bitstream was described in the log.
    std::size_t leftover_obus = 0;  ///< AV1 tile buffers whose leftover bytes looked like OBUs, since the last timing line.
    int tile_bits = -1;  ///< Bits that number an AV1 tile, -1 where the tiles aren't checked.
    std::size_t incomplete_frames = 0;  ///< AV1 frames sent without all their tiles, since the last timing line.
    std::size_t incomplete_logged = 0;  ///< AV1 frames sent without all their tiles that were described in the log.
    std::size_t joined_tiles = 0;  ///< AV1 tile buffers that held the next tile too, kept, since the last timing line.

    ~impl_t() {
      surface = nullptr;
      if (encoder) {
        encoder->Terminate();
        encoder = nullptr;
      }
      if (context) {
        context->Terminate();
        context = nullptr;
      }
      if (texture) {
        texture->Release();
      }
    }

    /**
     * @brief Whether the codec encoded is AV1.
     *
     * @return True for AV1, false for HEVC.
     */
    bool av1() const {
      return properties == &av1_properties;
    }

    /**
     * @brief Describe a bitstream of the codec encoded, for the log.
     *
     * @param data The bitstream.
     * @param size Size of the bitstream in bytes.
     * @return The OBUs or NAL units in it.
     */
    std::string describe(const std::uint8_t *data, std::size_t size) const {
      return av1() ? describe_obus(data, size) : describe_nals(data, size);
    }
  };

  encoder_t::encoder_t() = default;

  encoder_t::~encoder_t() = default;

  bool encoder_t::create(ID3D11Device *device, const video::config_t &config, const video::sunshine_colorspace_t &colorspace, const settings_t &settings) {
    auto amf_factory = factory();
    if (!amf_factory || !device) {
      return false;
    }

    auto state = std::make_unique<impl_t>();
    const bool av1 = settings.codec == codec_e::av1;
    state->properties = av1 ? &av1_properties : &hevc_properties;
    const auto &p = *state->properties;

    // AMF uses the device from its own threads as well
    ID3D11Multithread *multithread = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_ID3D11Multithread, (void **) &multithread))) {
      multithread->SetMultithreadProtected(TRUE);
      multithread->Release();
    }

    if (amf_factory->CreateContext(&state->context) != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: CreateContext() failed"sv;
      return false;
    }
    if (auto res = state->context->InitDX11(device, amf::AMF_DX11_1); res != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: InitDX11() failed: "sv << res;
      return false;
    }
    if (auto res = amf_factory->CreateComponent(state->context, p.component, &state->encoder); res != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: couldn't create the "sv << p.name << " encoder: "sv << res;
      return false;
    }

    const bool ten_bit = colorspace.bit_depth == 10;
    const auto fps = video::framerate_to_rational(config);
    const auto bitrate_kbps = config::video.max_bitrate > 0 ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate;
    const auto bitrate = static_cast<amf_int64>(bitrate_kbps) * 1000;
    const auto color = color_from_colorspace(colorspace);
    auto &encoder = state->encoder;

    // Usage first, as it sets the defaults for everything else
    encoder->SetProperty(p.usage, settings.usage);
    encoder->SetProperty(p.frame_size, ::AMFConstructSize(config.width, config.height));
    encoder->SetProperty(p.frame_rate, ::AMFConstructRate(fps.num, fps.den));
    if (av1) {
      encoder->SetProperty(p.profile, (amf_int64) AMF_VIDEO_ENCODER_AV1_PROFILE_MAIN);
    } else {
      encoder->SetProperty(p.profile, (amf_int64) (ten_bit ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10 : AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN));
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TIER, (amf_int64) AMF_VIDEO_ENCODER_HEVC_TIER_MAIN);
      if (const auto level = hevc_level(config.width, config.height, config.framerate)) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE_LEVEL, (amf_int64) level);
      }
    }
    encoder->SetProperty(p.bit_depth, (amf_int64) (ten_bit ? AMF_COLOR_BIT_DEPTH_10 : AMF_COLOR_BIT_DEPTH_8));
    encoder->SetProperty(p.color_profile, color.profile);
    encoder->SetProperty(p.full_range, colorspace.full_range);
    encoder->SetProperty(p.transfer, color.transfer);
    encoder->SetProperty(p.primaries, color.primaries);
    encoder->SetProperty(p.quality_preset, settings.quality_preset);
    if (av1) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
    } else {
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
    }

    // Key frames only when the client asks for one, each with the sequence headers
    encoder->SetProperty(p.gop_size, (amf_int64) 0);
    if (!av1) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64) 0);
    }
    encoder->SetProperty(p.header_insertion_mode, p.header_insertion_key_aligned);
    if (config.numRefFrames > 0) {
      encoder->SetProperty(p.max_num_ref_frames, (amf_int64) config.numRefFrames);
    }

    // Long-term references are kept until replaced, not only while frames reference them
    if (settings.ltr_frames > 0) {
      encoder->SetProperty(p.max_ltr_frames, (amf_int64) settings.ltr_frames);
      encoder->SetProperty(p.ltr_mode, p.ltr_mode_keep_unused);
    }

    encoder->SetProperty(p.rate_control, settings.rate_control);
    encoder->SetProperty(p.target_bitrate, bitrate);
    encoder->SetProperty(p.peak_bitrate, bitrate);
    encoder->SetProperty(p.vbv_buffer_size, vbv_buffer_bits(bitrate, config.framerate));
    encoder->SetProperty(p.filler_data, false);
    encoder->SetProperty(p.skip_frame, false);
    encoder->SetProperty(p.enforce_hrd, settings.enforce_hrd);
    encoder->SetProperty(p.preanalysis, settings.preanalysis);
    encoder->SetProperty(p.preencode, settings.preanalysis);
    if (!av1) {
      if (settings.vbaq) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, *settings.vbaq);
      }
      if (settings.max_au_size) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_AU_SIZE, (amf_int64) *settings.max_au_size);
      }
    }

    // Each part handed over as it finishes. HEVC slices come out at their exact size, and AV1
    // tiles each in their own tile group.
    if (settings.parts > 1) {
      encoder->SetProperty(p.parts_per_frame, (amf_int64) settings.parts);
      if (av1) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TILE_GROUP_OBU, true);

        // With several tiles, the entropy context AMF saves at the end of a frame isn't the one
        // the frame header says (context_update_tile_id), so decoders that check, such as
        // dav1d, reject every frame after a key frame. Frames start from their reference's
        // context instead.
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_CDF_FRAME_END_UPDATE_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_CDF_FRAME_END_UPDATE_MODE_DISABLE);
      }
      if (encoder->SetProperty(p.output_mode, p.output_mode_parts) == AMF_OK) {
        state->part_output = true;
      } else {
        BOOST_LOG(warning) << "AMF direct: this driver can't hand over "sv << p.name << ' ' << p.part_name << "s on their own; frames come out whole"sv;
      }
    }

    // QueryOutput() waits in the driver for output rather than returning at once
    encoder->SetProperty(p.query_timeout, query_timeout_ms);

    const auto format = ten_bit ? amf::AMF_SURFACE_P010 : amf::AMF_SURFACE_NV12;
    if (auto res = encoder->Init(format, config.width, config.height); res != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: couldn't initialize the "sv << p.name << " encoder: "sv << res;
      return false;
    }

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = config.width;
    desc.Height = config.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = ten_bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &state->texture))) {
      BOOST_LOG(error) << "AMF direct: couldn't create the input texture"sv;
      return false;
    }
    if (auto res = state->context->CreateSurfaceFromDX11Native(state->texture, &state->surface, nullptr); res != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: couldn't wrap the input texture: "sv << res;
      return false;
    }

    amf_int64 ltr_frames = 0;
    if (settings.ltr_frames > 0) {
      encoder->GetProperty(p.max_ltr_frames, &ltr_frames);
    }
    state->ltr = ltr_tracker_t {static_cast<int>(std::max<amf_int64>(ltr_frames, 0)), ltr_interval(fps.num, fps.den)};

    amf_int64 parts = 1;
    encoder->GetProperty(p.parts_per_frame, &parts);
    state->parts = static_cast<int>(std::max<amf_int64>(parts, 1));

    // With tiles handed over one by one, each frame is checked for all of them. A frame missing
    // one can't be decoded, and FFmpeg's AV1 decoder drops it without an error. Tile numbers are
    // read only for a power of two tiles, whose numbers take log2 of the count in bits however
    // the tiles are laid out.
    if (av1 && state->part_output && (state->parts & (state->parts - 1)) == 0) {
      state->tile_bits = 0;
      while ((1 << state->tile_bits) < state->parts) {
        ++state->tile_bits;
      }
    }
    BOOST_LOG(info) << "AMF direct: encoding "sv << p.name << ' ' << config.width << 'x' << config.height << ' ' << colorspace.bit_depth << "-bit at "sv << bitrate_kbps << " kbps, "sv
                    << parts << ' ' << p.part_name << (parts == 1 ? ""sv : "s"sv) << (state->part_output ? ", each handed over as it finishes"sv : ", frames handed over whole"sv) << ", "sv
                    << ltr_frames << " long-term references"sv;

    state->duration = frame_duration(fps.num, fps.den);
    impl = std::move(state);
    return true;
  }

  ID3D11Texture2D *encoder_t::input_texture() const {
    return impl ? impl->texture : nullptr;
  }

  bool encoder_t::encode_frame(std::uint64_t frame_index, bool force_idr, encoded_frame_t &frame, frame_trace::trace_t *trace, const part_callback_t &on_part) {
    frame.data.clear();
    frame.parts.clear();
    frame.idr = false;
    if (!impl) {
      return false;
    }

    const auto &p = *impl->properties;
    const auto preparing = std::chrono::steady_clock::now();

    // AMF makes key frames only when asked and for the first frame (the GOP size is 0)
    const bool first = impl->first_frame;
    impl->first_frame = false;
    const auto plan = impl->ltr.plan(static_cast<std::int64_t>(frame_index), force_idr || first);
    const bool idr_expected = plan.key;
    if (plan.reference >= 0) {
      ++impl->recoveries;
      BOOST_LOG(info) << "AMF direct: frame "sv << frame_index << " recovers from lost frames with the long-term reference to frame "sv << impl->ltr.frame_in(plan.reference);
    } else if (plan.key && !force_idr && !first) {
      ++impl->recovery_keys;
      BOOST_LOG(info) << "AMF direct: frame "sv << frame_index << " is a key frame, with no long-term reference left to recover from lost frames"sv;
    }
    frame.after_ref_frame_invalidation = plan.reference >= 0;
    frame.ltr = plan.mark >= 0;

    // The surface is reused, so last frame's settings go first
    auto &surface = impl->surface;
    surface->Clear();
    surface->SetPts(static_cast<amf_pts>(frame_index) * impl->duration);
    surface->SetDuration(impl->duration);
    if (plan.key) {
      surface->SetProperty(p.force_key, p.force_key_value);
      surface->SetProperty(p.insert_header, true);
    }
    if (plan.mark >= 0) {
      surface->SetProperty(p.mark_ltr, (amf_int64) plan.mark);
    }
    if (plan.reference >= 0) {
      surface->SetProperty(p.force_ltr_reference, (amf_int64) (1 << plan.reference));
    }

    const auto submitted = std::chrono::steady_clock::now();
    if (auto res = impl->encoder->SubmitInput(surface); res != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: SubmitInput() failed: "sv << res;
      return false;
    }
    impl->prepare_time += submitted - preparing;
    impl->submit_time += std::chrono::steady_clock::now() - submitted;

    // What AMF did with the long-term references, as it may say with any of the frame's buffers
    amf_int64 marked = -1;
    amf_int64 referenced = -1;
    if (trace) {
      trace->mark(frame_trace::point_e::encode_submitted);
    }

    // The next tile each buffer should start with, and what each buffer held, for the log if the
    // frame turns out to be missing a tile. -1 once a frame OBU makes the tiles unknown.
    struct buffer_tiles_t {
      std::size_t kept;  ///< Bytes kept.
      std::size_t size;  ///< Bytes in the buffer.
      int next_tile;  ///< The next tile expected after the tiles kept.
      tile_group_t cut;  ///< The tiles cut off when they were the next expected, otherwise -1.
    };

    std::array<buffer_tiles_t, 16> buffer_tiles;
    std::size_t buffers = 0;
    int next_tile = impl->tile_bits >= 0 ? 0 : -1;

    for (int query = 0; query < max_queries; ++query) {
      amf::AMFDataPtr data;
      const auto res = impl->encoder->QueryOutput(&data);
      if (res == AMF_REPEAT || (res == AMF_OK && !data)) {
        continue;
      }
      if (res != AMF_OK) {
        BOOST_LOG(error) << "AMF direct: QueryOutput() failed: "sv << res;
        return false;
      }

      amf::AMFBufferPtr buffer(data);
      if (!buffer) {
        BOOST_LOG(error) << "AMF direct: output isn't a buffer"sv;
        return false;
      }

      const auto bytes = buffer->GetSize();
      const auto *native = static_cast<const std::uint8_t *>(buffer->GetNative());
      if (impl->frames_described < 2) {
        BOOST_LOG(info) << "AMF direct: frame "sv << frame_index << " output "sv << frame.parts.size() << ": "sv << bytes << " bytes: "sv << impl->describe(native, bytes);
      }

      // An HEVC slice buffer holds the slice alone, but an AV1 tile buffer is followed by
      // leftover bytes that AV1 decoders don't skip
      auto used = bytes;
      if (impl->av1()) {
        used = obu_bytes(native, bytes, impl->part_output);
        if (impl->part_output && used != obu_bytes(native, bytes)) {
          ++impl->leftover_obus;
        }

        if (next_tile >= 0) {
          for (const auto &group : tile_groups(native, used, impl->tile_bits)) {
            if (group.start < 0) {
              next_tile = -1;
              break;
            }
            if (group.start == next_tile) {
              next_tile = group.end + 1;
            }
          }
        }
        if (next_tile >= 0) {
          // AMF sometimes puts the next tile in the same buffer, right after the first. Keep the
          // tile groups that carry on from the tiles kept, from the first byte after them, or the
          // frame is sent without them and the client can't decode it or any frame after it.
          const auto rest = tile_groups(native + used, bytes - used, impl->tile_bits);
          std::size_t joined = 0;
          for (const auto &group : rest) {
            if (group.start != next_tile || group.offset != joined) {
              break;
            }
            joined = group.offset + group.size;
            next_tile = group.end + 1;
          }
          if (joined > 0) {
            used += joined;
            ++impl->joined_tiles;
          }
        }
        if (next_tile >= 0 && buffers < buffer_tiles.size()) {
          // Whether the bytes cut off start with the tile that should come next, which would
          // mean AMF put it in this buffer after something the tiles above didn't carry on past
          const auto cut = tile_groups(native + used, bytes - used, impl->tile_bits);
          const bool next_cut_off = !cut.empty() && cut.front().start == next_tile;
          buffer_tiles[buffers++] = {used, bytes, next_tile, next_cut_off ? cut.front() : tile_group_t {-1, -1, 0, 0}};
        }
      }
      frame.data.insert(frame.data.end(), native, native + used);
      frame.parts.push_back({std::chrono::steady_clock::now() - submitted, used});

      amf_int64 frame_type = -1;
      if (data->GetProperty(p.output_frame_type, &frame_type) == AMF_OK && frame_type == p.output_key) {
        frame.idr = true;
      }
      data->GetProperty(p.output_marked_ltr, &marked);
      data->GetProperty(p.output_referenced_ltr, &referenced);

      amf_int64 buffer_type = 0;
      data->GetProperty(p.output_buffer_type, &buffer_type);
      const bool last = !impl->part_output || buffer_type != p.output_buffer_part;
      if (!last && on_part) {
        // Earlier parts are sent with the key frame flag that was asked for
        const bool idr = frame.idr;
        frame.idr = idr_expected;
        on_part(frame, impl->parts);
        frame.idr = idr;
      }
      if (last) {
        if (frame.idr != idr_expected) {
          ++impl->idr_mismatches;
        }
        if (next_tile >= 0 && next_tile != impl->parts) {
          ++impl->incomplete_frames;
          if (impl->incomplete_logged < 20) {
            ++impl->incomplete_logged;
            std::string notes;
            for (std::size_t i = 0; i < buffers; ++i) {
              const auto &b = buffer_tiles[i];
              notes += std::format(" [{} of {} bytes kept, through tile {}{}]", b.kept, b.size, b.next_tile - 1, b.cut.start >= 0 ? std::format(", cut off tiles {}-{}", b.cut.start, b.cut.end) : std::string {});
            }
            BOOST_LOG(warning) << "AMF direct: frame "sv << frame_index << " was sent with tiles 0-"sv << next_tile - 1 << " of "sv << impl->parts
                               << " in "sv << frame.parts.size() << " buffers:"sv << notes;
          }
        }
        if (plan.mark >= 0 && marked != plan.mark) {
          // Not a reference to recover from after all
          ++impl->ltr_misses;
          impl->ltr.forget(plan.mark);
          frame.ltr = false;
        }
        if (plan.reference >= 0 && referenced != (amf_int64) (1 << plan.reference)) {
          ++impl->ltr_misses;
          BOOST_LOG(warning) << "AMF direct: frame "sv << frame_index << " was to reference only long-term reference "sv << plan.reference << ", but references "sv << referenced;
        }
        if (trace) {
          trace->mark(frame_trace::point_e::encode_finished);
        }

        if (impl->frames_described < 2) {
          ++impl->frames_described;
          BOOST_LOG(info) << "AMF direct: frame "sv << frame_index << (frame.idr ? " (key)"sv : ""sv) << " whole: "sv << impl->describe(frame.data.data(), frame.data.size());
        }

        impl->timing.add(frame.parts);
        if (impl->timing.frames() >= timing_log_frames) {
          const auto frames = static_cast<double>(impl->timing.frames());
          auto leftovers = impl->av1() ? std::format("{} tiles followed by bytes that looked like OBUs, ", impl->leftover_obus) : std::string {};
          if (impl->tile_bits >= 0) {
            leftovers += std::format("{} buffers holding more than one tile, {} frames missing a tile, ", impl->joined_tiles, impl->incomplete_frames);
          }
          BOOST_LOG(info) << "AMF direct: "sv << p.name << ' ' << impl->timing.summary() << ", "sv << leftovers << impl->idr_mismatches << " key frame flags not as asked, "sv
                          << std::format("{:.3f} ms preparing and {:.3f} ms submitting each frame, ", std::chrono::duration<double, std::milli>(impl->prepare_time).count() / frames, std::chrono::duration<double, std::milli>(impl->submit_time).count() / frames)
                          << impl->recoveries << " recoveries from long-term references, "sv << impl->recovery_keys << " key frames for lost frames, "sv << impl->ltr_misses << " long-term references not as asked"sv;
          impl->leftover_obus = 0;
          impl->incomplete_frames = 0;
          impl->joined_tiles = 0;
          impl->idr_mismatches = 0;
          impl->recoveries = 0;
          impl->recovery_keys = 0;
          impl->ltr_misses = 0;
          impl->prepare_time = {};
          impl->submit_time = {};
          impl->timing.reset();
        }
        return true;
      }
    }

    BOOST_LOG(error) << "AMF direct: the encoder didn't finish frame "sv << frame_index;
    return false;
  }

  void encoder_t::invalidate_ref_frames(std::int64_t first_frame, std::int64_t last_frame) {
    if (impl) {
      impl->ltr.invalidate(first_frame, last_frame);
    }
  }

  void encoder_t::acknowledge_ltr(std::int64_t frame_index) {
    if (impl) {
      impl->ltr.acknowledge(frame_index);
    }
  }
#endif

}  // namespace amf_direct
