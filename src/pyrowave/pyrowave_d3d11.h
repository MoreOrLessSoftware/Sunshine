/**
 * @file src/pyrowave/pyrowave_d3d11.h
 * @brief Declarations for the PyroWave encoder fed from Direct3D11 textures.
 */
#pragma once
#ifdef _WIN32

  // standard includes
  #include <array>
  #include <chrono>
  #include <cstdint>
  #include <memory>
  #include <string_view>
  #include <vector>

  // platform includes
  #include <comdef.h>
  #include <d3d11_4.h>
  #include <vulkan/vulkan_core.h>

  // local includes
  #include "src/frame_trace.h"

// Handle type of an image in the PyroWave C API (pyrowave_image)
struct pyrowave_image_opaque;
// Where one packet of an encoded frame is in the bitstream, in the PyroWave C API
struct pyrowave_packet;

namespace pyrowave {

  #ifdef DOXYGEN
  /**
   * @brief COM smart pointer for ID3D11Device5.
   */
  using ID3D11Device5Ptr = ID3D11Device5 *;
  /**
   * @brief COM smart pointer for ID3D11DeviceContext4.
   */
  using ID3D11DeviceContext4Ptr = ID3D11DeviceContext4 *;
  /**
   * @brief COM smart pointer for ID3D11Fence.
   */
  using ID3D11FencePtr = ID3D11Fence *;
  /**
   * @brief COM smart pointer for ID3D11Texture2D.
   */
  using ID3D11Texture2DPtr = ID3D11Texture2D *;
  /**
   * @brief COM smart pointer for ID3D11Query.
   */
  using ID3D11QueryPtr = ID3D11Query *;
  #else
  _COM_SMARTPTR_TYPEDEF(ID3D11Device5, IID_ID3D11Device5);
  _COM_SMARTPTR_TYPEDEF(ID3D11DeviceContext4, IID_ID3D11DeviceContext4);
  _COM_SMARTPTR_TYPEDEF(ID3D11Fence, IID_ID3D11Fence);
  _COM_SMARTPTR_TYPEDEF(ID3D11Texture2D, IID_ID3D11Texture2D);
  _COM_SMARTPTR_TYPEDEF(ID3D11Query, IID_ID3D11Query);
  #endif

  struct api_t;

  /**
   * @brief Check whether the PyroWave library can be loaded.
   *
   * The library (libpyrowave-shared-0.dll) is loaded on first use from the directory of
   * the executable or the DLL search path. Nothing is linked against it at build time.
   *
   * @return True when the library loaded and its API version matches the one built against.
   */
  bool library_available();

  /**
   * @brief Move packets of an encoded frame together so they follow one another.
   *
   * PyroWave places each packet at its own offset in the bitstream. Blocks are
   * self-delimiting, so the frame is the packets back to back.
   *
   * @param frame Bitstream the packets were written to. Shrunk to the joined frame.
   * @param packets Packets in the order they were written, which is by increasing offset.
   * @param count Number of packets.
   */
  void join_packets(std::vector<std::uint8_t> &frame, const ::pyrowave_packet *packets, std::size_t count);

  /**
   * @brief Name a Vulkan queue priority for the log.
   *
   * @param priority Queue priority.
   * @return Lowercase name, or "unknown".
   */
  std::string_view priority_name(VkQueueGlobalPriority priority);

  /**
   * @brief Average length of GPU intervals measured with timestamps, for the debug log.
   */
  class interval_average_t {
  public:
    /**
     * @brief Add one interval.
     *
     * @param start Timestamp at the start, in ticks.
     * @param end Timestamp at the end, in ticks. Ignored if before start.
     * @param frequency Ticks per second. Ignored if zero.
     */
    void add(std::uint64_t start, std::uint64_t end, std::uint64_t frequency);

    /**
     * @brief Count the intervals added since the last reset.
     *
     * @return Number of intervals.
     */
    std::size_t count() const;

    /**
     * @brief Get the average interval.
     *
     * @return Average in milliseconds, or 0 without intervals.
     */
    double average_ms() const;

    /**
     * @brief Forget the intervals added so far.
     */
    void reset();

  private:
    double total_seconds = 0.0;  ///< Sum of the intervals.
    std::size_t intervals = 0;  ///< Number of intervals.
  };

  /**
   * @brief PyroWave encoder that reads frames from D3D11 textures.
   *
   * For 4:2:0, the caller draws each frame's Y plane into y_texture() and its interleaved UV
   * plane into uv_texture() with its own D3D11 device. For 4:4:4, it draws Y, U and V into the
   * R, G and B channels of y_texture(), and there is no uv_texture(). It then calls
   * encode_frame(). PyroWave runs in Vulkan on the same GPU and reads the textures through
   * shared handles; a shared D3D11 fence orders the two.
   */
  class d3d11_encoder {
  public:
    d3d11_encoder();
    ~d3d11_encoder();

    d3d11_encoder(const d3d11_encoder &) = delete;
    d3d11_encoder &operator=(const d3d11_encoder &) = delete;

    /**
     * @brief Create the PyroWave device for the GPU behind a D3D11 device.
     *
     * @param device D3D11 device that draws the encoder input.
     * @param device_ctx Immediate context of that device.
     * @return True on success.
     */
    bool init_device(ID3D11Device *device, ID3D11DeviceContext *device_ctx);

    /**
     * @brief Create the input textures and the PyroWave encoder.
     *
     * @param width Encoded frame width, must be even for 4:2:0.
     * @param height Encoded frame height, must be even for 4:2:0.
     * @param format The input layout. DXGI_FORMAT_NV12 (8-bit) or DXGI_FORMAT_P010 (10-bit)
     *               for 4:2:0, given as those formats' two planes in separate textures.
     *               DXGI_FORMAT_AYUV (8-bit) or DXGI_FORMAT_Y410 (10-bit) for 4:4:4, given
     *               as Y, U and V in the R, G and B channels of an R8G8B8A8_UNORM or
     *               R10G10B10A2_UNORM texture.
     * @return True on success.
     */
    bool create_encoder(int width, int height, DXGI_FORMAT format);

    /**
     * @brief Texture the caller draws each frame into.
     *
     * For 4:2:0, the Y plane (R8 or R16, full size). For 4:4:4, the whole frame as YUVA
     * (R8G8B8A8 or R10G10B10A2, full size).
     *
     * @return Y or YUV texture owned by the encoder.
     */
    ID3D11Texture2D *y_texture() const;

    /**
     * @brief Texture the caller draws each frame's UV plane into (R8G8 or R16G16, half size).
     *
     * @return UV texture owned by the encoder, or nullptr for 4:4:4.
     */
    ID3D11Texture2D *uv_texture() const;

    /**
     * @brief Start timing the D3D11 work that draws a frame into the input textures.
     *
     * Call before drawing, and end_convert() after. The time is logged at debug level with
     * PyroWave's own GPU timings.
     */
    void begin_convert();

    /**
     * @brief Mark where the captured frame became ready to read, between begin_convert() and
     *        end_convert().
     *
     * Splits the conversion into waiting for the capture to hand over the frame, and drawing
     * it. Without this mark, the whole conversion counts as drawing.
     */
    void mark_input_acquired();

    /**
     * @brief Stop timing the D3D11 work started by begin_convert().
     */
    void end_convert();

    /**
     * @brief Encode the frame currently in the input textures.
     *
     * Waits for the D3D11 work that drew the frame, encodes it, and makes further D3D11
     * work on the input textures wait until PyroWave has read them.
     *
     * @param max_frame_size Largest encoded frame in bytes.
     * @param frame Receives the encoded frame. Its storage is reused, so passing in the
     *              buffer of an earlier frame avoids allocating a new one.
     * @param trace Optional. Marked once the frame is flushed to the GPU, submitted, and encoded.
     * @return True on success.
     */
    bool encode_frame(std::size_t max_frame_size, std::vector<std::uint8_t> &frame, frame_trace::trace_t *trace = nullptr);

  private:
    /**
     * @brief Log PyroWave's GPU timings at debug level every few seconds, then start over.
     */
    void report_performance();

    /**
     * @brief Add the conversion times the GPU has finished measuring, without waiting.
     */
    void collect_convert_times();

    /**
     * @brief Timestamp queries that time one conversion on the D3D11 GPU queue.
     */
    struct convert_query_t {
      ID3D11QueryPtr disjoint;  ///< Says whether the timestamps can be compared, and their frequency.
      ID3D11QueryPtr start;  ///< Timestamp before the conversion.
      ID3D11QueryPtr acquired;  ///< Timestamp once the captured frame is ready to read.
      ID3D11QueryPtr end;  ///< Timestamp after drawing.
      bool pending = false;  ///< Whether its results haven't been read yet.
    };

    /**
     * @brief Destroy the PyroWave objects, waiting for the GPU to be done with them.
     */
    void destroy();

    /**
     * @brief Create a shared texture and import it into PyroWave.
     *
     * @param width Texture width.
     * @param height Texture height.
     * @param format D3D11 format.
     * @param vk_format The same format in Vulkan.
     * @param texture Receives the D3D11 texture.
     * @param image Receives the imported PyroWave image.
     * @return True on success.
     */
    bool import_texture(int width, int height, DXGI_FORMAT format, VkFormat vk_format, ID3D11Texture2DPtr &texture, ::pyrowave_image_opaque *&image);

    const api_t *api = nullptr;  ///< PyroWave entry points, null until init_device().
    ID3D11Device5Ptr device;  ///< D3D11 device that draws the input texture.
    ID3D11DeviceContext4Ptr device_ctx;  ///< Immediate context of that device.
    ID3D11FencePtr fence;  ///< Fence shared with PyroWave to order access to the input textures.
    std::uint64_t fence_value = 0;  ///< Last value signaled or waited for on the fence.
    ID3D11Texture2DPtr y_tex;  ///< Shared Y (4:2:0) or YUV (4:4:4) input texture.
    ID3D11Texture2DPtr uv_tex;  ///< Shared UV input texture, 4:2:0 only.
    std::chrono::steady_clock::time_point last_report;  ///< When GPU timings were last logged.
    std::array<convert_query_t, 4> convert_queries;  ///< Queries for the last few conversions, read once the GPU is done with them.
    std::size_t convert_query_index = 0;  ///< Queries for the next conversion.
    bool timing_convert = false;  ///< Whether begin_convert() started timing the current conversion.
    bool input_acquired = false;  ///< Whether mark_input_acquired() was called for the current conversion.
    interval_average_t convert_time;  ///< Conversion times since the last report.
    interval_average_t convert_wait;  ///< Waits for the captured frame since the last report.
    interval_average_t convert_draw;  ///< Drawing times since the last report.
    std::size_t convert_busy = 0;  ///< Conversions not timed since the last report, as every set of queries was still in use.
    std::size_t convert_disjoint = 0;  ///< Conversions whose timestamps D3D11 reported as not comparable since the last report.
    std::size_t convert_unreadable = 0;  ///< Conversions whose query results couldn't be read since the last report.

    struct handles_t;
    std::unique_ptr<handles_t> handles;  ///< PyroWave objects.
  };

}  // namespace pyrowave
#endif
