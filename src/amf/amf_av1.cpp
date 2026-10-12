/**
 * @file src/amf/amf_av1.cpp
 * @brief Definitions for encoding AV1 through AMD's AMF runtime directly, without FFmpeg.
 */
// this include
#include "amf_av1.h"

// standard includes
#include <algorithm>
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

  void tile_timing_t::add(const std::vector<tile_t> &tiles) {
    if (tiles.empty()) {
      return;
    }

    ++frame_count;
    tile_count += tiles.size();
    first_ms += std::chrono::duration<double, std::milli>(tiles.front().after_submit).count();
    last_ms += std::chrono::duration<double, std::milli>(tiles.back().after_submit).count();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
      bytes_total += tiles[i].bytes;
      if (i + 1 < tiles.size()) {
        bytes_before_last += tiles[i].bytes;
      }
    }
  }

  std::size_t tile_timing_t::frames() const {
    return frame_count;
  }

  std::string tile_timing_t::summary() const {
    if (frame_count == 0) {
      return "no frames";
    }

    const auto frames = static_cast<double>(frame_count);
    const auto percent_before_last = bytes_total ? 100.0 * static_cast<double>(bytes_before_last) / static_cast<double>(bytes_total) : 0.0;
    return std::format("{} frames, {:.1f} tiles: first {:.2f} ms, last {:.2f} ms after submit, {:.0f}% of the bytes before the last", frame_count, static_cast<double>(tile_count) / frames, first_ms / frames, last_ms / frames, percent_before_last);
  }

  void tile_timing_t::reset() {
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
     * @brief Frames between tile timing lines in the log.
     */
    constexpr std::size_t timing_log_frames = 600;
  }  // namespace

  bool runtime_available() {
    return factory() != nullptr;
  }

  /**
   * @brief AMF objects owned by the encoder.
   */
  struct av1_encoder_t::impl_t {
    amf::AMFContextPtr context;  ///< AMF context on the encode device.
    amf::AMFComponentPtr encoder;  ///< AV1 encoder component.
    ID3D11Texture2D *texture = nullptr;  ///< Texture frames are drawn into.
    amf::AMFSurfacePtr surface;  ///< The texture wrapped for AMF once, submitted for every frame.
    ltr_tracker_t ltr;  ///< Long-term references, to recover from lost frames.
    std::size_t recoveries = 0;  ///< Frames that recovered from lost frames with a long-term reference, since the last timing line.
    std::size_t recovery_keys = 0;  ///< Key frames sent for lost frames, with no long-term reference to recover from, since the last timing line.
    std::size_t ltr_misses = 0;  ///< Frames where AMF didn't mark or reference the long-term reference planned, since the last timing line.
    std::chrono::steady_clock::duration prepare_time {};  ///< Time spent setting up frames before SubmitInput(), since the last timing line.
    std::chrono::steady_clock::duration submit_time {};  ///< Time spent in SubmitInput(), since the last timing line.
    bool tile_output = false;  ///< Whether AMF hands over each tile on its own.
    int tiles = 1;  ///< Tiles in each frame.
    bool first_frame = true;  ///< Whether no frame has been encoded yet, as the first is a key frame.
    std::size_t idr_mismatches = 0;  ///< Frames whose key frame flag differed from what was asked for, since the last timing line.
    tile_timing_t timing;  ///< When tiles came out, for the log.
    std::int64_t duration = 0;  ///< Frame duration in AMF's 100 ns units.
    int frames_described = 0;  ///< Frames whose OBUs were written to the log.
    std::size_t leftover_obus = 0;  ///< Tile buffers whose leftover bytes looked like OBUs, since the last timing line.

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
  };

  av1_encoder_t::av1_encoder_t() = default;

  av1_encoder_t::~av1_encoder_t() = default;

  bool av1_encoder_t::create(ID3D11Device *device, const video::config_t &config, const video::sunshine_colorspace_t &colorspace, const settings_t &settings) {
    auto amf_factory = factory();
    if (!amf_factory || !device) {
      return false;
    }

    auto state = std::make_unique<impl_t>();

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
    if (auto res = amf_factory->CreateComponent(state->context, AMFVideoEncoder_AV1, &state->encoder); res != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: couldn't create the AV1 encoder: "sv << res;
      return false;
    }

    const bool ten_bit = colorspace.bit_depth == 10;
    const auto fps = video::framerate_to_rational(config);
    const auto bitrate_kbps = config::video.max_bitrate > 0 ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate;
    const auto bitrate = static_cast<amf_int64>(bitrate_kbps) * 1000;
    const auto color = color_from_colorspace(colorspace);
    auto &encoder = state->encoder;

    // Usage first, as it sets the defaults for everything else
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE, settings.usage);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMESIZE, ::AMFConstructSize(config.width, config.height));
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, ::AMFConstructRate(fps.num, fps.den));
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PROFILE, (amf_int64) AMF_VIDEO_ENCODER_AV1_PROFILE_MAIN);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, (amf_int64) (ten_bit ? AMF_COLOR_BIT_DEPTH_10 : AMF_COLOR_BIT_DEPTH_8));
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, color.profile);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FULL_RANGE_COLOR, colorspace.full_range);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC, color.transfer);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES, color.primaries);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, settings.quality_preset);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);

    // Key frames only when the client asks for one, each with a sequence header
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, (amf_int64) 0);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED);
    if (config.numRefFrames > 0) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_NUM_REFRAMES, (amf_int64) config.numRefFrames);
    }

    // Long-term references are kept until replaced, not only while frames reference them
    if (settings.ltr_frames > 0) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_LTR_FRAMES, (amf_int64) settings.ltr_frames);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_LTR_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_LTR_MODE_KEEP_UNUSED);
    }

    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD, settings.rate_control);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitrate);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitrate);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv_buffer_bits(bitrate, config.framerate));
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_SKIP_FRAME, false);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, settings.enforce_hrd);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE, settings.preanalysis);
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_PREENCODE, settings.preanalysis);

    // Each tile in its own tile group, handed over as it finishes
    if (settings.tiles > 1) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TILES_PER_FRAME, (amf_int64) settings.tiles);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TILE_GROUP_OBU, true);

      // With several tiles, the entropy context AMF saves at the end of a frame isn't the one
      // the frame header says (context_update_tile_id), so decoders that check, such as
      // dav1d, reject every frame after a key frame. Frames start from their reference's
      // context instead.
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_CDF_FRAME_END_UPDATE_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_CDF_FRAME_END_UPDATE_MODE_DISABLE);
      if (encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_OUTPUT_MODE_TILE) == AMF_OK) {
        state->tile_output = true;
      } else {
        BOOST_LOG(warning) << "AMF direct: this driver can't hand over tiles on their own; frames come out whole"sv;
      }
    }

    // QueryOutput() waits in the driver for output rather than returning at once
    encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, query_timeout_ms);

    const auto format = ten_bit ? amf::AMF_SURFACE_P010 : amf::AMF_SURFACE_NV12;
    if (auto res = encoder->Init(format, config.width, config.height); res != AMF_OK) {
      BOOST_LOG(error) << "AMF direct: couldn't initialize the AV1 encoder: "sv << res;
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
      encoder->GetProperty(AMF_VIDEO_ENCODER_AV1_MAX_LTR_FRAMES, &ltr_frames);
    }
    state->ltr = ltr_tracker_t {static_cast<int>(std::max<amf_int64>(ltr_frames, 0)), ltr_interval(fps.num, fps.den)};

    amf_int64 tiles = 1;
    encoder->GetProperty(AMF_VIDEO_ENCODER_AV1_TILES_PER_FRAME, &tiles);
    state->tiles = static_cast<int>(std::max<amf_int64>(tiles, 1));
    BOOST_LOG(info) << "AMF direct: encoding AV1 "sv << config.width << 'x' << config.height << ' ' << colorspace.bit_depth << "-bit at "sv << bitrate_kbps << " kbps, "sv
                    << tiles << (tiles == 1 ? " tile"sv : " tiles"sv) << (state->tile_output ? ", each handed over as it finishes"sv : ", frames handed over whole"sv) << ", "sv
                    << ltr_frames << " long-term references"sv;

    state->duration = frame_duration(fps.num, fps.den);
    impl = std::move(state);
    return true;
  }

  ID3D11Texture2D *av1_encoder_t::input_texture() const {
    return impl ? impl->texture : nullptr;
  }

  bool av1_encoder_t::encode_frame(std::uint64_t frame_index, bool force_idr, encoded_frame_t &frame, frame_trace::trace_t *trace, const tile_callback_t &on_tile) {
    frame.data.clear();
    frame.tiles.clear();
    frame.idr = false;
    if (!impl) {
      return false;
    }

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
      surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, (amf_int64) AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
      surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
    }
    if (plan.mark >= 0) {
      surface->SetProperty(AMF_VIDEO_ENCODER_AV1_MARK_CURRENT_WITH_LTR_INDEX, (amf_int64) plan.mark);
    }
    if (plan.reference >= 0) {
      surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_LTR_REFERENCE_BITFIELD, (amf_int64) (1 << plan.reference));
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
        BOOST_LOG(info) << "AMF direct: frame "sv << frame_index << " output "sv << frame.tiles.size() << ": "sv << bytes << " bytes: "sv << describe_obus(native, bytes);
      }
      const auto used = obu_bytes(native, bytes, impl->tile_output);
      if (impl->tile_output && used != obu_bytes(native, bytes)) {
        ++impl->leftover_obus;
      }
      frame.data.insert(frame.data.end(), native, native + used);
      frame.tiles.push_back({std::chrono::steady_clock::now() - submitted, used});

      amf_int64 frame_type = -1;
      if (data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &frame_type) == AMF_OK && frame_type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY) {
        frame.idr = true;
      }
      data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_MARKED_LTR_INDEX, &marked);
      data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_REFERENCED_LTR_INDEX_BITFIELD, &referenced);

      amf_int64 buffer_type = AMF_VIDEO_ENCODER_AV1_OUTPUT_BUFFER_TYPE_FRAME;
      data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_BUFFER_TYPE, &buffer_type);
      const bool last = !impl->tile_output || buffer_type != AMF_VIDEO_ENCODER_AV1_OUTPUT_BUFFER_TYPE_TILE;
      if (!last && on_tile) {
        // Earlier tiles are sent with the key frame flag that was asked for
        const bool idr = frame.idr;
        frame.idr = idr_expected;
        on_tile(frame, impl->tiles);
        frame.idr = idr;
      }
      if (last) {
        if (frame.idr != idr_expected) {
          ++impl->idr_mismatches;
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
          BOOST_LOG(info) << "AMF direct: frame "sv << frame_index << (frame.idr ? " (key)"sv : ""sv) << " whole: "sv << describe_obus(frame.data.data(), frame.data.size());
        }

        impl->timing.add(frame.tiles);
        if (impl->timing.frames() >= timing_log_frames) {
          const auto frames = static_cast<double>(impl->timing.frames());
          BOOST_LOG(info) << "AMF direct: "sv << impl->timing.summary() << ", "sv << impl->leftover_obus << " tiles followed by bytes that looked like OBUs, "sv << impl->idr_mismatches << " key frame flags not as asked, "sv
                          << std::format("{:.3f} ms preparing and {:.3f} ms submitting each frame, ", std::chrono::duration<double, std::milli>(impl->prepare_time).count() / frames, std::chrono::duration<double, std::milli>(impl->submit_time).count() / frames)
                          << impl->recoveries << " recoveries from long-term references, "sv << impl->recovery_keys << " key frames for lost frames, "sv << impl->ltr_misses << " long-term references not as asked"sv;
          impl->leftover_obus = 0;
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

  void av1_encoder_t::invalidate_ref_frames(std::int64_t first_frame, std::int64_t last_frame) {
    if (impl) {
      impl->ltr.invalidate(first_frame, last_frame);
    }
  }

  void av1_encoder_t::acknowledge_ltr(std::int64_t frame_index) {
    if (impl) {
      impl->ltr.acknowledge(frame_index);
    }
  }
#endif

}  // namespace amf_direct
