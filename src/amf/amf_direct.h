/**
 * @file src/amf/amf_direct.h
 * @brief Declarations for encoding AV1 and HEVC through AMD's AMF runtime directly, without FFmpeg.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// local includes
#include "src/frame_trace.h"
#include "src/video_colorspace.h"

#ifdef _WIN32
  // platform includes
  #include <d3d11_4.h>
#endif

namespace video {
  struct config_t;
}  // namespace video

/**
 * @brief Encoding AV1 and HEVC through AMD's AMF runtime directly.
 *
 * @details FFmpeg's AMF encoder hands over whole frames only. Going to AMF directly lets the
 *          encoder hand over each AV1 tile or HEVC slice of a frame as it finishes (output mode
 *          TILE or SLICE), so the frame can be sent while the rest is still encoding, as NVENC
 *          does with slices. It also lets the encoder recover from lost frames with long-term
 *          references.
 */
namespace amf_direct {

  /**
   * @brief The codecs the encoder can encode.
   */
  enum class codec_e {
    av1,  ///< AV1, in tiles.
    hevc,  ///< HEVC, in slices.
  };

  /**
   * @brief Settings for the encoder that come from the user's configuration.
   *
   * @details The usage, quality preset and rate control are the AMF enum values of the codec
   *          encoded, which differ between codecs. The defaults are AV1's.
   */
  struct settings_t {
    codec_e codec = codec_e::av1;  ///< The codec to encode.
    std::int64_t usage = 2;  ///< AMF_VIDEO_ENCODER_AV1_USAGE_ENUM or AMF_VIDEO_ENCODER_HEVC_USAGE_ENUM. AV1 ultra low latency by default.
    std::int64_t quality_preset = 100;  ///< AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_ENUM or AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_ENUM. AV1 speed by default.
    std::int64_t rate_control = 3;  ///< AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_ENUM or AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_ENUM. CBR by default.
    bool preanalysis = false;  ///< Whether AMF's pre-analysis runs.
    bool enforce_hrd = false;  ///< Whether AMF enforces the HRD.
    std::optional<bool> vbaq;  ///< HEVC only. Whether variance-based adaptive quantization runs; unset leaves AMF's default.
    std::optional<int> max_au_size;  ///< HEVC only. Largest access unit in bits; unset leaves AMF's default.
    int parts = 1;  ///< Tiles (AV1) or slices (HEVC) per frame. Above 1, AMF hands each over as it finishes.
    int ltr_frames = 0;  ///< Long-term reference frames kept to recover from lost frames without a key frame. 0 turns them off.
  };

  /**
   * @brief AMF's color profile, transfer characteristic and primaries for a stream's colorspace.
   */
  struct color_t {
    std::int64_t profile;  ///< AMF_VIDEO_CONVERTER_COLOR_PROFILE_ENUM.
    std::int64_t transfer;  ///< ISO/IEC 23001-8 transfer characteristic.
    std::int64_t primaries;  ///< ISO/IEC 23001-8 color primaries.
  };

  /**
   * @brief Work out what AMF signals in the bitstream for a stream's colorspace.
   *
   * @param colorspace The stream's colorspace.
   * @return The color profile, transfer characteristic and primaries.
   */
  color_t color_from_colorspace(const video::sunshine_colorspace_t &colorspace);

  /**
   * @brief Work out the encoder's VBV buffer size, one frame's worth of the bitrate.
   *
   * @details One frame of buffer keeps each frame near its share of the bitrate, so no frame
   *          takes much longer than the others to send, as the FFmpeg path does.
   *
   * @param bitrate_bps Bitrate in bits per second.
   * @param framerate Frames per second.
   * @return Buffer size in bits.
   */
  std::int64_t vbv_buffer_bits(std::int64_t bitrate_bps, int framerate);

  /**
   * @brief Work out how long a frame lasts in AMF's timestamp units of 100 ns.
   *
   * @details AMF's rate control spreads the bitrate over the frames by their timestamps, so
   *          frames must be stamped one frame duration apart.
   *
   * @param fps_num Frame rate numerator.
   * @param fps_den Frame rate denominator.
   * @return Frame duration in 100 ns units.
   */
  std::int64_t frame_duration(int fps_num, int fps_den);

  /**
   * @brief Work out how many frames apart long-term references are kept.
   *
   * @param fps_num Frame rate numerator.
   * @param fps_den Frame rate denominator.
   * @return Frames in an eighth of a second, at least 1.
   */
  std::int64_t ltr_interval(int fps_num, int fps_den);

  /**
   * @brief Work out the HEVC level to ask AMF for, as the FFmpeg path does.
   *
   * @details Up to 4K, level 5.1 or 5.2 when the frame rate fits in it, so the stream doesn't
   *          claim a higher level than it needs.
   *
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @param framerate Frames per second.
   * @return AMF_VIDEO_ENCODER_LEVEL_ENUM value (153 for 5.1, 156 for 5.2), or 0 to leave the
   *         level to AMF.
   */
  std::int64_t hevc_level(int width, int height, int framerate);

  /**
   * @brief Measure the OBUs at the start of an AMF output buffer.
   *
   * @details In tile output mode, AMF hands over each tile in a buffer larger than the tile,
   *          with leftover bytes of earlier output after it. AV1 decoders don't skip bytes
   *          between OBUs as H.264 and HEVC decoders skip zero bytes between NAL units, so only
   *          the OBUs are kept. The leftover bytes can look like OBUs too, so a tile's buffer is
   *          cut after its tile group: each tile comes in its own tile group.
   *
   * @param data The buffer.
   * @param size Size of the buffer in bytes.
   * @param through_first_tile Whether to stop after the first tile group or frame OBU.
   * @return Bytes taken by the OBUs that start the buffer, each with a size field, up to the
   *         first byte that doesn't start one, or the end of the first tile.
   */
  std::size_t obu_bytes(const std::uint8_t *data, std::size_t size, bool through_first_tile = false);

  /**
   * @brief Tiles an AV1 tile group OBU carries, or a frame OBU, whose tiles aren't read.
   */
  struct tile_group_t {
    int start;  ///< First tile, -1 for a frame OBU.
    int end;  ///< Last tile, -1 for a frame OBU.
  };

  /**
   * @brief The tile groups in an AV1 bitstream, to check that a frame sent all its tiles.
   *
   * @param data The bitstream.
   * @param size Size of the bitstream in bytes.
   * @param tile_bits Bits that number a tile: log2 of the tile count, which must be a power of two.
   * @return The tile groups and frame OBUs in order, up to the first byte that doesn't start an
   *         OBU with a size field.
   */
  std::vector<tile_group_t> tile_groups(const std::uint8_t *data, std::size_t size, int tile_bits);

  /**
   * @brief Describe the OBUs in an AV1 bitstream, for the log.
   *
   * @param data The bitstream.
   * @param size Size of the bitstream in bytes.
   * @return The OBUs in order with their payload sizes, such as "TD:0 SEQ:12 FH:30 TG:5120",
   *         ending with "invalid at N" if the data at byte N isn't an OBU.
   */
  std::string describe_obus(const std::uint8_t *data, std::size_t size);

  /**
   * @brief Describe the NAL units in an Annex B HEVC bitstream, for the log.
   *
   * @param data The bitstream.
   * @param size Size of the bitstream in bytes.
   * @return The NAL units in order with their sizes after the start code, such as
   *         "AUD:3 VPS:23 SPS:52 PPS:7 IDR_N_LP:5120", with other types by number, starting
   *         with "invalid at 0" if the data doesn't start with a start code.
   */
  std::string describe_nals(const std::uint8_t *data, std::size_t size);

  /**
   * @brief When each part (AV1 tile or HEVC slice) of a frame came out of the encoder.
   */
  struct part_t {
    std::chrono::steady_clock::duration after_submit;  ///< Time from submitting the frame to the part coming out.
    std::size_t bytes;  ///< Size of the part's output.
  };

  /**
   * @brief Summarizes when parts come out of the encoder, to tell whether they come out as each
   *        finishes or all together at the end of the frame.
   */
  class part_timing_t {
  public:
    /**
     * @brief Add a frame's parts.
     *
     * @param parts The frame's parts, in the order they came out.
     */
    void add(const std::vector<part_t> &parts);

    /**
     * @brief Get the number of frames added since the last reset().
     *
     * @return Frames added.
     */
    std::size_t frames() const;

    /**
     * @brief Describe the frames added since the last reset().
     *
     * @return A line for the log, such as "120 frames, 4.0 parts: first 1.10 ms, last 3.05 ms
     *         after submit, 64% of the bytes before the last".
     */
    std::string summary() const;

    /**
     * @brief Forget the frames added.
     */
    void reset();

  private:
    std::size_t frame_count = 0;
    std::size_t part_count = 0;
    double first_ms = 0.0;
    double last_ms = 0.0;
    std::uint64_t bytes_before_last = 0;
    std::uint64_t bytes_total = 0;
  };

  /**
   * @brief What to do with long-term references for one frame.
   */
  struct ltr_plan_t {
    bool key = false;  ///< Whether the frame must be a key frame.
    int mark = -1;  ///< Slot to keep the frame in as a long-term reference, or -1.
    int reference = -1;  ///< Slot whose frame alone the frame references, to recover from lost frames, or -1.
  };

  /**
   * @brief Decides which frames become long-term references, and which one to recover from
   *        when the client loses frames.
   *
   * @details A frame is kept as a long-term reference at regular intervals. The client confirms
   *          each one it receives whole. When the client reports lost frames, the next frame
   *          references only the newest confirmed one from before the loss, so the client can
   *          decode it without the lost frames, instead of waiting for a key frame. The newest
   *          confirmed reference is never replaced by one not yet confirmed.
   */
  class ltr_tracker_t {
  public:
    /**
     * @brief Create a tracker.
     *
     * @param slots Long-term references the encoder keeps. 0 recovers with key frames only.
     * @param interval Frames between long-term references. At least 1.
     */
    ltr_tracker_t(int slots = 0, std::int64_t interval = 1);

    /**
     * @brief Get the number of long-term references kept.
     *
     * @return Slots.
     */
    int slots() const;

    /**
     * @brief Decide what the next frame does with long-term references.
     *
     * @param frame_index The frame's index, as the client sees it.
     * @param key_requested Whether a key frame was asked for.
     * @return The plan for the frame. A key frame is planned when one was asked for, or when the
     *         client lost frames and no confirmed reference from before the loss is left.
     */
    ltr_plan_t plan(std::int64_t frame_index, bool key_requested);

    /**
     * @brief Record that the client received a long-term reference whole.
     *
     * @param frame_index The frame's index.
     */
    void acknowledge(std::int64_t frame_index);

    /**
     * @brief Record that the client lost frames, to recover from with the next plan().
     *
     * @details Requests that come before the next plan() are merged. A request only for frames
     *          before the last recovery or key frame is ignored, as that frame already recovered.
     *
     * @param first_frame First frame lost.
     * @param last_frame Last frame lost.
     */
    void invalidate(std::int64_t first_frame, std::int64_t last_frame);

    /**
     * @brief Stop using a slot, as when the encoder didn't keep the frame planned for it.
     *
     * @param slot The slot.
     */
    void forget(int slot);

    /**
     * @brief Get the frame kept in a slot.
     *
     * @param slot The slot.
     * @return The frame's index, or -1 if the slot is empty.
     */
    std::int64_t frame_in(int slot) const;

  private:
    /**
     * @brief A long-term reference.
     */
    struct slot_t {
      std::int64_t frame = -1;  ///< Index of the frame kept, or -1.
      bool acknowledged = false;  ///< Whether the client received the frame whole.
    };

    int newest_acknowledged() const;
    int slot_to_mark() const;
    void clear();

    std::vector<slot_t> slot_list;
    std::int64_t interval;
    std::int64_t last_mark = -1;
    std::int64_t recovered_at = -1;
    std::optional<std::pair<std::int64_t, std::int64_t>> lost;
  };

  /**
   * @brief One encoded frame.
   */
  struct encoded_frame_t {
    std::vector<std::uint8_t> data;  ///< The frame's bitstream, all parts in order.
    bool idr = false;  ///< Whether the frame is a key frame.
    bool after_ref_frame_invalidation = false;  ///< Whether the frame recovers from lost frames by referencing a long-term reference.
    bool ltr = false;  ///< Whether the frame is kept as a long-term reference, for the client to confirm.
    std::vector<part_t> parts;  ///< When each part came out of the encoder.
  };

  /**
   * @brief Called as each part but the last comes out of the encoder.
   *
   * @details Receives the frame so far, all parts out so far in order, and the number of parts
   *          in the frame. Its `idr` is whether a key frame was asked for, as AMF may only say
   *          so with the last part.
   */
  using part_callback_t = std::function<void(const encoded_frame_t &frame, int parts_total)>;

#ifdef _WIN32
  /**
   * @brief Check whether AMD's AMF runtime can be loaded.
   *
   * @return True when amfrt64.dll loaded and initialized.
   */
  bool runtime_available();

  /**
   * @brief AV1 or HEVC encoder that reads a Direct3D 11 texture and encodes it through AMF.
   */
  class encoder_t {
  public:
    encoder_t();
    ~encoder_t();

    encoder_t(const encoder_t &) = delete;
    encoder_t &operator=(const encoder_t &) = delete;

    /**
     * @brief Create the encoder and the texture it reads frames from.
     *
     * @param device Direct3D 11 device the frames are drawn on.
     * @param config Client stream configuration.
     * @param colorspace The stream's colorspace.
     * @param settings Settings from the user's configuration.
     * @return True on success.
     */
    bool create(ID3D11Device *device, const video::config_t &config, const video::sunshine_colorspace_t &colorspace, const settings_t &settings);

    /**
     * @brief Get the texture the next frame is drawn into before encode_frame().
     *
     * @return NV12 or P010 texture, or nullptr before create().
     */
    ID3D11Texture2D *input_texture() const;

    /**
     * @brief Encode the frame in the input texture and wait for all of its parts.
     *
     * @param frame_index Frame index, used as the frame's timestamp.
     * @param force_idr Whether to encode a key frame.
     * @param frame Receives the encoded frame.
     * @param trace Optional. Marked when the frame is submitted and when its last part comes out.
     * @param on_part Optional. Called as each part but the last comes out, when parts are handed
     *                over on their own.
     * @return True on success.
     */
    bool encode_frame(std::uint64_t frame_index, bool force_idr, encoded_frame_t &frame, frame_trace::trace_t *trace, const part_callback_t &on_part = {});

    /**
     * @brief Recover from frames the client lost with the next frame.
     *
     * @param first_frame First frame lost.
     * @param last_frame Last frame lost.
     */
    void invalidate_ref_frames(std::int64_t first_frame, std::int64_t last_frame);

    /**
     * @brief Record that the client received a long-term reference whole.
     *
     * @param frame_index The frame's index.
     */
    void acknowledge_ltr(std::int64_t frame_index);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
#endif

}  // namespace amf_direct
