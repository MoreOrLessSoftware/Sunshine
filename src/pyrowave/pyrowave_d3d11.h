/**
 * @file src/pyrowave/pyrowave_d3d11.h
 * @brief Declarations for the PyroWave encoder fed from Direct3D11 textures.
 */
#pragma once
#ifdef _WIN32

  // standard includes
  #include <cstdint>
  #include <memory>
  #include <vector>

  // platform includes
  #include <comdef.h>
  #include <d3d11_4.h>
  #include <vulkan/vulkan_core.h>

// Handle type of an image in the PyroWave C API (pyrowave_image)
struct pyrowave_image_opaque;

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
  #else
  _COM_SMARTPTR_TYPEDEF(ID3D11Device5, IID_ID3D11Device5);
  _COM_SMARTPTR_TYPEDEF(ID3D11DeviceContext4, IID_ID3D11DeviceContext4);
  _COM_SMARTPTR_TYPEDEF(ID3D11Fence, IID_ID3D11Fence);
  _COM_SMARTPTR_TYPEDEF(ID3D11Texture2D, IID_ID3D11Texture2D);
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
   * @brief PyroWave encoder that reads frames from D3D11 textures.
   *
   * The caller draws each frame's Y plane into y_texture() and its interleaved UV plane into
   * uv_texture() with its own D3D11 device, then calls encode_frame(). PyroWave runs in Vulkan
   * on the same GPU and reads the textures through shared handles; a shared D3D11 fence
   * orders the two.
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
     * @param width Encoded frame width, must be even.
     * @param height Encoded frame height, must be even.
     * @param format DXGI_FORMAT_NV12 for 8-bit or DXGI_FORMAT_P010 for 10-bit input, given as
     *               those formats' two planes in separate textures.
     * @return True on success.
     */
    bool create_encoder(int width, int height, DXGI_FORMAT format);

    /**
     * @brief Texture the caller draws each frame's Y plane into (R8 or R16, full size).
     *
     * @return Y texture owned by the encoder.
     */
    ID3D11Texture2D *y_texture() const;

    /**
     * @brief Texture the caller draws each frame's UV plane into (R8G8 or R16G16, half size).
     *
     * @return UV texture owned by the encoder.
     */
    ID3D11Texture2D *uv_texture() const;

    /**
     * @brief Encode the frame currently in the input textures.
     *
     * Waits for the D3D11 work that drew the frame, encodes it, and makes further D3D11
     * work on the input textures wait until PyroWave has read them.
     *
     * @param max_frame_size Largest encoded frame in bytes.
     * @return Encoded frame, or an empty vector on failure.
     */
    std::vector<std::uint8_t> encode_frame(std::size_t max_frame_size);

  private:
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
    ID3D11Texture2DPtr y_tex;  ///< Shared Y input texture.
    ID3D11Texture2DPtr uv_tex;  ///< Shared UV input texture.

    struct handles_t;
    std::unique_ptr<handles_t> handles;  ///< PyroWave objects.
  };

}  // namespace pyrowave
#endif
