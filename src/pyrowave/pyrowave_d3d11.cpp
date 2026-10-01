/**
 * @file src/pyrowave/pyrowave_d3d11.cpp
 * @brief Definitions for the PyroWave encoder fed from Direct3D11 textures.
 */
#ifdef _WIN32
  // this include
  #include "pyrowave_d3d11.h"

  // standard includes
  #include <algorithm>
  #include <array>
  #include <cstring>
  #include <mutex>

  // lib includes
  #include <vulkan/vulkan_core.h>
// clang-format off
  #include <pyrowave/pyrowave.h>
  // clang-format on

  // local includes
  #include "src/logging.h"
  #include "src/utility.h"

namespace pyrowave {
  using namespace std::literals;

  /**
   * @brief Entry points of the PyroWave shared library, resolved at runtime.
   */
  struct api_t {
    decltype(&::pyrowave_get_api_version) get_api_version;  ///< pyrowave_get_api_version
    decltype(&::pyrowave_create_device_by_compat2) create_device_by_compat2;  ///< pyrowave_create_device_by_compat2
    decltype(&::pyrowave_device_set_queue_type) device_set_queue_type;  ///< pyrowave_device_set_queue_type
    decltype(&::pyrowave_device_destroy) device_destroy;  ///< pyrowave_device_destroy
    decltype(&::pyrowave_sync_object_create) sync_object_create;  ///< pyrowave_sync_object_create
    decltype(&::pyrowave_sync_object_get_semaphore) sync_object_get_semaphore;  ///< pyrowave_sync_object_get_semaphore
    decltype(&::pyrowave_sync_object_destroy) sync_object_destroy;  ///< pyrowave_sync_object_destroy
    decltype(&::pyrowave_image_create) image_create;  ///< pyrowave_image_create
    decltype(&::pyrowave_image_get_image_view) image_get_image_view;  ///< pyrowave_image_get_image_view
    decltype(&::pyrowave_image_destroy) image_destroy;  ///< pyrowave_image_destroy
    decltype(&::pyrowave_encoder_create) encoder_create;  ///< pyrowave_encoder_create
    decltype(&::pyrowave_encoder_encode_gpu_synchronous) encoder_encode_gpu_synchronous;  ///< pyrowave_encoder_encode_gpu_synchronous
    decltype(&::pyrowave_encoder_compute_num_packets) encoder_compute_num_packets;  ///< pyrowave_encoder_compute_num_packets
    decltype(&::pyrowave_encoder_packetize) encoder_packetize;  ///< pyrowave_encoder_packetize
    decltype(&::pyrowave_encoder_destroy) encoder_destroy;  ///< pyrowave_encoder_destroy
  };

  namespace {
    /**
     * @brief Load the PyroWave library once and resolve its entry points.
     *
     * @return Entry points, or nullptr when the library is missing or incompatible.
     */
    const api_t *load_api() {
      static std::once_flag once;
      static api_t api {};
      static bool loaded = false;

      std::call_once(once, []() {
        // Kept loaded for the life of the process
        HMODULE dll = LoadLibraryExW(L"libpyrowave-shared-0.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!dll) {
          BOOST_LOG(info) << "PyroWave: libpyrowave-shared-0.dll not found, the codec is unavailable"sv;
          return;
        }

        bool resolved = true;
        auto resolve = [&](auto &fn, const char *name) {
          fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
          if (!fn) {
            BOOST_LOG(error) << "PyroWave: missing entry point "sv << name;
            resolved = false;
          }
        };

        resolve(api.get_api_version, "pyrowave_get_api_version");
        resolve(api.create_device_by_compat2, "pyrowave_create_device_by_compat2");
        resolve(api.device_set_queue_type, "pyrowave_device_set_queue_type");
        resolve(api.device_destroy, "pyrowave_device_destroy");
        resolve(api.sync_object_create, "pyrowave_sync_object_create");
        resolve(api.sync_object_get_semaphore, "pyrowave_sync_object_get_semaphore");
        resolve(api.sync_object_destroy, "pyrowave_sync_object_destroy");
        resolve(api.image_create, "pyrowave_image_create");
        resolve(api.image_get_image_view, "pyrowave_image_get_image_view");
        resolve(api.image_destroy, "pyrowave_image_destroy");
        resolve(api.encoder_create, "pyrowave_encoder_create");
        resolve(api.encoder_encode_gpu_synchronous, "pyrowave_encoder_encode_gpu_synchronous");
        resolve(api.encoder_compute_num_packets, "pyrowave_encoder_compute_num_packets");
        resolve(api.encoder_packetize, "pyrowave_encoder_packetize");
        resolve(api.encoder_destroy, "pyrowave_encoder_destroy");
        if (!resolved) {
          return;
        }

        // The API and ABI can change between minor versions until 1.0
        uint32_t major, minor, patch;
        api.get_api_version(&major, &minor, &patch);
        if (major != PYROWAVE_API_VERSION_MAJOR || minor != PYROWAVE_API_VERSION_MINOR) {
          BOOST_LOG(error) << "PyroWave: library version "sv << major << '.' << minor << '.' << patch
                           << " doesn't match the "sv << PYROWAVE_API_VERSION_MAJOR << '.' << PYROWAVE_API_VERSION_MINOR
                           << " API Sunshine was built with"sv;
          return;
        }

        BOOST_LOG(info) << "PyroWave: loaded library version "sv << major << '.' << minor << '.' << patch;
        loaded = true;
      });

      return loaded ? &api : nullptr;
    }
  }  // namespace

  bool library_available() {
    return load_api() != nullptr;
  }

  /**
   * @brief PyroWave objects owned by the encoder.
   */
  struct d3d11_encoder::handles_t {
    pyrowave_device device = nullptr;  ///< Vulkan device on the same GPU as the D3D11 device.
    pyrowave_sync_object sync = nullptr;  ///< The shared D3D11 fence, imported as a timeline semaphore.
    std::array<pyrowave_image, 2> images {};  ///< The Y and UV input textures, imported through their shared handles.
    pyrowave_encoder encoder = nullptr;  ///< The encoder.
    pyrowave_gpu_buffers buffers {};  ///< Y, Cb and Cr views of the input textures.
  };

  d3d11_encoder::d3d11_encoder():
      handles {std::make_unique<handles_t>()} {
  }

  d3d11_encoder::~d3d11_encoder() {
    destroy();
  }

  void d3d11_encoder::destroy() {
    if (!api) {
      return;
    }

    // Each of these waits for the GPU to be done with it
    if (handles->encoder) {
      api->encoder_destroy(handles->encoder);
      handles->encoder = nullptr;
    }
    for (auto &image : handles->images) {
      if (image) {
        api->image_destroy(image);
        image = nullptr;
      }
    }
    if (handles->sync) {
      api->sync_object_destroy(handles->sync);
      handles->sync = nullptr;
    }
    if (handles->device) {
      api->device_destroy(handles->device);
      handles->device = nullptr;
    }
  }

  bool d3d11_encoder::init_device(ID3D11Device *device_in, ID3D11DeviceContext *device_ctx_in) {
    api = load_api();
    if (!api) {
      return false;
    }

    // Fences need D3D11.4 (Windows 10 1703)
    HRESULT status = device_in->QueryInterface(IID_PPV_ARGS(&device));
    if (FAILED(status)) {
      BOOST_LOG(error) << "PyroWave: ID3D11Device5 is unavailable [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }
    status = device_ctx_in->QueryInterface(IID_PPV_ARGS(&device_ctx));
    if (FAILED(status)) {
      BOOST_LOG(error) << "PyroWave: ID3D11DeviceContext4 is unavailable [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    // Find the adapter so PyroWave picks the same GPU in Vulkan
    LUID luid {};
    {
      IDXGIDevice *dxgi_device = nullptr;
      IDXGIAdapter *adapter = nullptr;
      DXGI_ADAPTER_DESC desc {};
      status = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
      if (SUCCEEDED(status)) {
        status = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
      }
      if (SUCCEEDED(status)) {
        status = adapter->GetDesc(&desc);
        adapter->Release();
      }
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: couldn't find the D3D11 device's adapter [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }
      luid = desc.AdapterLuid;
    }

    static_assert(sizeof(LUID) == sizeof(pyrowave_luid));
    pyrowave_luid pyro_luid;
    std::memcpy(pyro_luid.luid, &luid, sizeof(luid));

    // High priority puts PyroWave on an async compute queue, so a busy graphics queue
    // doesn't hold up encoding. Without the privilege for it, the driver gives a lower one.
    auto result = api->create_device_by_compat2(0, 0, nullptr, nullptr, &pyro_luid, VK_QUEUE_GLOBAL_PRIORITY_HIGH, &handles->device);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: couldn't create a Vulkan device for this GPU ("sv << result << ')';
      return false;
    }
    api->device_set_queue_type(handles->device, VK_QUEUE_COMPUTE_BIT);

    // D3D11 signals this when a frame is drawn, and PyroWave when it has read it
    status = device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
    if (FAILED(status)) {
      BOOST_LOG(error) << "PyroWave: couldn't create a shared fence [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    HANDLE fence_handle = nullptr;
    status = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
    if (FAILED(status)) {
      BOOST_LOG(error) << "PyroWave: couldn't share the fence [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    pyrowave_sync_object_create_info sync_info {};
    sync_info.device = handles->device;
    sync_info.external_handle = reinterpret_cast<pyrowave_os_handle>(fence_handle);
    sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
    result = api->sync_object_create(&sync_info, &handles->sync);
    if (result != PYROWAVE_SUCCESS) {
      CloseHandle(fence_handle);
      BOOST_LOG(error) << "PyroWave: couldn't import the shared fence ("sv << result << ')';
      return false;
    }

    return true;
  }

  bool d3d11_encoder::import_texture(int width, int height, DXGI_FORMAT format, VkFormat vk_format, ID3D11Texture2DPtr &texture, pyrowave_image &image) {
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT status = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "PyroWave: couldn't create a shared input texture [0x"sv << util::hex(status).to_string_view() << ']';
      return false;
    }

    HANDLE texture_handle = nullptr;
    {
      IDXGIResource1 *resource = nullptr;
      status = texture->QueryInterface(IID_PPV_ARGS(&resource));
      if (SUCCEEDED(status)) {
        status = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &texture_handle);
        resource->Release();
      }
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: couldn't share an input texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }
    }

    // Must match the D3D11 texture closely enough for the driver to import it
    VkImageCreateInfo image_create_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = vk_format;
    image_create_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    pyrowave_image_create_info image_info {};
    image_info.device = handles->device;
    image_info.external_handle = reinterpret_cast<pyrowave_os_handle>(texture_handle);
    image_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    image_info.image_create_info = &image_create_info;
    auto result = api->image_create(&image_info, &image);
    if (result != PYROWAVE_SUCCESS) {
      // Whether PyroWave closed the handle on failure isn't specified, so it is left open
      BOOST_LOG(error) << "PyroWave: couldn't import a shared input texture ("sv << result << ')';
      return false;
    }

    return true;
  }

  bool d3d11_encoder::create_encoder(int width, int height, DXGI_FORMAT format) {
    if (!handles->device) {
      return false;
    }

    if ((width & 1) || (height & 1)) {
      BOOST_LOG(error) << "PyroWave: 4:2:0 needs an even width and height, not "sv << width << 'x' << height;
      return false;
    }

    if (format != DXGI_FORMAT_NV12 && format != DXGI_FORMAT_P010) {
      BOOST_LOG(error) << "PyroWave: unsupported input format "sv << format;
      return false;
    }
    const bool ten_bit = format == DXGI_FORMAT_P010;

    // Y and UV go in separate textures rather than one NV12/P010 texture. Importing
    // planar D3D11 textures into Vulkan is where drivers disagree about the plane layout.
    const auto y_format = ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const auto uv_format = ten_bit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const auto vk_y_format = ten_bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    const auto vk_uv_format = ten_bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;
    if (!import_texture(width, height, y_format, vk_y_format, y_tex, handles->images[0]) ||
        !import_texture(width / 2, height / 2, uv_format, vk_uv_format, uv_tex, handles->images[1])) {
      return false;
    }

    // Cb and Cr are the R and G channels of the UV texture
    auto result = api->image_get_image_view(handles->images[0], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &handles->buffers.planes[0]);
    if (result == PYROWAVE_SUCCESS) {
      result = api->image_get_image_view(handles->images[1], VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &handles->buffers.planes[1]);
    }
    if (result == PYROWAVE_SUCCESS) {
      result = api->image_get_image_view(handles->images[1], VK_IMAGE_ASPECT_PLANE_2_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &handles->buffers.planes[2]);
    }
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: couldn't view the input textures ("sv << result << ')';
      return false;
    }

    pyrowave_encoder_create_info encoder_info {};
    encoder_info.device = handles->device;
    encoder_info.width = width;
    encoder_info.height = height;
    encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    result = api->encoder_create(&encoder_info, &handles->encoder);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: couldn't create the encoder ("sv << result << ')';
      return false;
    }

    BOOST_LOG(info) << "PyroWave: encoding "sv << width << 'x' << height << (ten_bit ? " 10-bit"sv : " 8-bit"sv) << " 4:2:0"sv;
    return true;
  }

  ID3D11Texture2D *d3d11_encoder::y_texture() const {
    return y_tex;
  }

  ID3D11Texture2D *d3d11_encoder::uv_texture() const {
    return uv_tex;
  }

  std::vector<std::uint8_t> d3d11_encoder::encode_frame(std::size_t max_frame_size, frame_trace::trace_t *trace) {
    if (!handles->encoder) {
      return {};
    }

    // PyroWave waits for the D3D11 work that drew the frame. The signal only reaches the
    // GPU once the context is flushed.
    const auto drawn_value = ++fence_value;
    const auto read_value = ++fence_value;
    device_ctx->Signal(fence, drawn_value);
    device_ctx->Flush();
    if (trace) {
      trace->mark(frame_trace::point_e::encode_flushed);
    }

    auto semaphore = api->sync_object_get_semaphore(handles->sync);
    std::array<pyrowave_gpu_external_reference, 2> refs {{
      {handles->images[0], VK_QUEUE_FAMILY_EXTERNAL},
      {handles->images[1], VK_QUEUE_FAMILY_EXTERNAL},
    }};

    pyrowave_gpu_sync_operation acquire {};
    acquire.images = refs.data();
    acquire.num_images = refs.size();
    acquire.sync = {semaphore, drawn_value};

    pyrowave_gpu_sync_operation release {};
    release.images = refs.data();
    release.num_images = refs.size();
    release.sync = {semaphore, read_value};

    pyrowave_rate_control rate_control {max_frame_size};
    auto result = api->encoder_encode_gpu_synchronous(handles->encoder, &acquire, &release, &handles->buffers, &rate_control);
    if (trace) {
      trace->mark(frame_trace::point_e::encode_submitted);
    }

    // Drawing the next frame into the textures waits until PyroWave has read this one
    device_ctx->Wait(fence, read_value);

    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: encoding failed ("sv << result << ')';
      return {};
    }

    // One packet holds the whole frame: the RTP layer splits it up and adds FEC. The
    // boundary leaves room for the frame header on top of the rate-controlled payload.
    const std::size_t boundary = max_frame_size + 64 * 1024;

    // Waits for the encode to finish
    std::size_t num_packets = 0;
    result = api->encoder_compute_num_packets(handles->encoder, boundary, &num_packets);
    if (trace) {
      trace->mark(frame_trace::point_e::encode_finished);
    }
    if (result != PYROWAVE_SUCCESS || num_packets == 0) {
      BOOST_LOG(error) << "PyroWave: couldn't size the encoded frame ("sv << result << ')';
      return {};
    }

    std::vector<pyrowave_packet> packets(num_packets);
    std::vector<std::uint8_t> bitstream(num_packets * boundary);
    std::size_t out_packets = 0;
    result = api->encoder_packetize(handles->encoder, packets.data(), boundary, &out_packets, bitstream.data(), bitstream.size());
    if (result != PYROWAVE_SUCCESS || out_packets == 0) {
      BOOST_LOG(error) << "PyroWave: couldn't packetize the encoded frame ("sv << result << ')';
      return {};
    }

    if (out_packets == 1 && packets[0].offset == 0) {
      bitstream.resize(packets[0].size);
      return bitstream;
    }

    // Blocks are self-delimiting, so packets can simply follow one another
    std::vector<std::uint8_t> frame;
    for (std::size_t i = 0; i < out_packets; i++) {
      auto begin = std::begin(bitstream) + packets[i].offset;
      frame.insert(std::end(frame), begin, begin + packets[i].size);
    }
    return frame;
  }

}  // namespace pyrowave
#endif
