/**
 * @file src/amf/amf_av1.h
 * @brief Declarations for encoding AV1 through AMD's AMF runtime directly, without FFmpeg.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
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
 * @brief Encoding AV1 through AMD's AMF runtime directly.
 *
 * @details FFmpeg's AMF encoder hands over whole frames only. Going to AMF directly lets the
 *          encoder hand over each tile of a frame as it finishes (AV1OutputMode TILE), so the
 *          frame can be sent while the rest is still encoding, as NVENC does with slices.
 */
namespace amf_direct {

  /**
   * @brief Settings for the encoder that come from the user's configuration.
   */
  struct settings_t {
    std::int64_t usage = 2;  ///< AMF_VIDEO_ENCODER_AV1_USAGE_ENUM. Ultra low latency by default.
    std::int64_t quality_preset = 100;  ///< AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_ENUM. Speed by default.
    std::int64_t rate_control = 3;  ///< AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_ENUM. CBR by default.
    bool preanalysis = false;  ///< Whether AMF's pre-analysis runs.
    bool enforce_hrd = false;  ///< Whether AMF enforces the HRD.
    int tiles = 1;  ///< Tiles per frame. Above 1, AMF hands each tile over as it finishes.
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
   * @brief Describe the OBUs in an AV1 bitstream, for the log.
   *
   * @param data The bitstream.
   * @param size Size of the bitstream in bytes.
   * @return The OBUs in order with their payload sizes, such as "TD:0 SEQ:12 FH:30 TG:5120",
   *         ending with "invalid at N" if the data at byte N isn't an OBU.
   */
  std::string describe_obus(const std::uint8_t *data, std::size_t size);

  /**
   * @brief When each tile of a frame came out of the encoder.
   */
  struct tile_t {
    std::chrono::steady_clock::duration after_submit;  ///< Time from submitting the frame to the tile coming out.
    std::size_t bytes;  ///< Size of the tile's output.
  };

  /**
   * @brief Summarizes when tiles come out of the encoder, to tell whether they come out as each
   *        finishes or all together at the end of the frame.
   */
  class tile_timing_t {
  public:
    /**
     * @brief Add a frame's tiles.
     *
     * @param tiles The frame's tiles, in the order they came out.
     */
    void add(const std::vector<tile_t> &tiles);

    /**
     * @brief Get the number of frames added since the last reset().
     *
     * @return Frames added.
     */
    std::size_t frames() const;

    /**
     * @brief Describe the frames added since the last reset().
     *
     * @return A line for the log, such as "120 frames, 4.0 tiles: first 1.10 ms, last 3.05 ms
     *         after submit, 64% of the bytes before the last".
     */
    std::string summary() const;

    /**
     * @brief Forget the frames added.
     */
    void reset();

  private:
    std::size_t frame_count = 0;
    std::size_t tile_count = 0;
    double first_ms = 0.0;
    double last_ms = 0.0;
    std::uint64_t bytes_before_last = 0;
    std::uint64_t bytes_total = 0;
  };

  /**
   * @brief One encoded frame.
   */
  struct encoded_frame_t {
    std::vector<std::uint8_t> data;  ///< The frame's bitstream, all tiles in order.
    bool idr = false;  ///< Whether the frame is a key frame.
    std::vector<tile_t> tiles;  ///< When each tile came out of the encoder.
  };

#ifdef _WIN32
  /**
   * @brief Check whether AMD's AMF runtime can be loaded.
   *
   * @return True when amfrt64.dll loaded and initialized.
   */
  bool runtime_available();

  /**
   * @brief AV1 encoder that reads a Direct3D 11 texture and encodes it through AMF.
   */
  class av1_encoder_t {
  public:
    av1_encoder_t();
    ~av1_encoder_t();

    av1_encoder_t(const av1_encoder_t &) = delete;
    av1_encoder_t &operator=(const av1_encoder_t &) = delete;

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
     * @brief Encode the frame in the input texture and wait for all of its tiles.
     *
     * @param frame_index Frame index, used as the frame's timestamp.
     * @param force_idr Whether to encode a key frame.
     * @param frame Receives the encoded frame.
     * @param trace Optional. Marked when the frame is submitted and when its last tile comes out.
     * @return True on success.
     */
    bool encode_frame(std::uint64_t frame_index, bool force_idr, encoded_frame_t &frame, frame_trace::trace_t *trace);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
#endif

}  // namespace amf_direct
