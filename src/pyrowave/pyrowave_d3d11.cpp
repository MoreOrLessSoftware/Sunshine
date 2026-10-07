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
  #include <iomanip>
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
    decltype(&::pyrowave_device_get_global_priority) device_get_global_priority;  ///< pyrowave_device_get_global_priority
    decltype(&::pyrowave_device_report_performance_stats) device_report_performance_stats;  ///< pyrowave_device_report_performance_stats
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
        resolve(api.device_get_global_priority, "pyrowave_device_get_global_priority");
        resolve(api.device_report_performance_stats, "pyrowave_device_report_performance_stats");
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

  void join_packets(std::vector<std::uint8_t> &frame, const ::pyrowave_packet *packets, std::size_t count) {
    std::size_t size = 0;
    for (std::size_t i = 0; i < count; i++) {
      // A packet never starts before the end of the ones already moved, so it only moves back
      if (packets[i].offset != size) {
        std::memmove(frame.data() + size, frame.data() + packets[i].offset, packets[i].size);
      }
      size += packets[i].size;
    }
    frame.resize(size);
  }

  std::string_view priority_name(VkQueueGlobalPriority priority) {
    switch (priority) {
      case VK_QUEUE_GLOBAL_PRIORITY_LOW:
        return "low"sv;
      case VK_QUEUE_GLOBAL_PRIORITY_MEDIUM:
        return "medium"sv;
      case VK_QUEUE_GLOBAL_PRIORITY_HIGH:
        return "high"sv;
      case VK_QUEUE_GLOBAL_PRIORITY_REALTIME:
        return "realtime"sv;
      default:
        return "unknown"sv;
    }
  }

  void interval_average_t::add(std::uint64_t start, std::uint64_t end, std::uint64_t frequency) {
    if (end < start || frequency == 0) {
      return;
    }
    total_seconds += static_cast<double>(end - start) / static_cast<double>(frequency);
    intervals++;
  }

  std::size_t interval_average_t::count() const {
    return intervals;
  }

  double interval_average_t::average_ms() const {
    return intervals ? total_seconds * 1000.0 / static_cast<double>(intervals) : 0.0;
  }

  void interval_average_t::reset() {
    total_seconds = 0.0;
    intervals = 0;
  }

  /**
   * @brief PyroWave objects owned by the encoder.
   */
  struct d3d11_encoder::handles_t {
    pyrowave_device device = nullptr;  ///< Vulkan device on the same GPU as the D3D11 device.
    pyrowave_sync_object sync = nullptr;  ///< The shared D3D11 fence, imported as a timeline semaphore.
    std::array<pyrowave_image, 2> images {};  ///< The Y and UV (4:2:0) or the YUV (4:4:4, only the first) input textures, imported through their shared handles.
    pyrowave_encoder encoder = nullptr;  ///< The encoder.
    pyrowave_gpu_buffers buffers {};  ///< Y, Cb and Cr views of the input textures, each picking one channel.
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
    constexpr auto requested_priority = VK_QUEUE_GLOBAL_PRIORITY_HIGH;
    auto result = api->create_device_by_compat2(0, 0, nullptr, nullptr, &pyro_luid, requested_priority, &handles->device);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: couldn't create a Vulkan device for this GPU ("sv << result << ')';
      return false;
    }

    const auto priority = api->device_get_global_priority(handles->device);
    if (priority < requested_priority) {
      BOOST_LOG(info) << "PyroWave: the driver gave the compute queue "sv << priority_name(priority) << " priority rather than "sv
                      << priority_name(requested_priority) << ", so encoding can wait behind other GPU work"sv;
    } else {
      BOOST_LOG(info) << "PyroWave: encoding on a compute queue at "sv << priority_name(priority) << " priority"sv;
    }
    last_report = std::chrono::steady_clock::now();

    // Only for the debug log, so the stream works without them
    for (auto &queries : convert_queries) {
      D3D11_QUERY_DESC disjoint_desc {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      D3D11_QUERY_DESC timestamp_desc {D3D11_QUERY_TIMESTAMP, 0};
      if (FAILED(device->CreateQuery(&disjoint_desc, &queries.disjoint)) ||
          FAILED(device->CreateQuery(&timestamp_desc, &queries.start)) ||
          FAILED(device->CreateQuery(&timestamp_desc, &queries.acquired)) ||
          FAILED(device->CreateQuery(&timestamp_desc, &queries.end))) {
        BOOST_LOG(debug) << "PyroWave: couldn't create timestamp queries, so the D3D11 conversion isn't timed"sv;
        for (auto &created : convert_queries) {
          created = {};
        }
        break;
      }
    }

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

    if (format != DXGI_FORMAT_NV12 && format != DXGI_FORMAT_P010 && format != DXGI_FORMAT_AYUV && format != DXGI_FORMAT_Y410) {
      BOOST_LOG(error) << "PyroWave: unsupported input format "sv << format;
      return false;
    }
    const bool yuv444 = format == DXGI_FORMAT_AYUV || format == DXGI_FORMAT_Y410;
    const bool ten_bit = format == DXGI_FORMAT_P010 || format == DXGI_FORMAT_Y410;

    if (!yuv444 && ((width & 1) || (height & 1))) {
      BOOST_LOG(error) << "PyroWave: 4:2:0 needs an even width and height, not "sv << width << 'x' << height;
      return false;
    }

    pyrowave_result result;
    if (yuv444) {
      // One texture with Y, U and V in its R, G and B channels, drawn in a single pass.
      // Each plane is a view of it that picks one channel.
      const auto yuv_format = ten_bit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
      const auto vk_yuv_format = ten_bit ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM;
      if (!import_texture(width, height, yuv_format, vk_yuv_format, y_tex, handles->images[0])) {
        return false;
      }

      pyrowave_image_view view;
      result = api->image_get_image_view(handles->images[0], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &view);
      if (result == PYROWAVE_SUCCESS) {
        constexpr std::array swizzles {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B};
        for (std::size_t i = 0; i < swizzles.size(); i++) {
          handles->buffers.planes[i] = view;
          handles->buffers.planes[i].swizzle = swizzles[i];
        }
      }
    } else {
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
      result = api->image_get_image_view(handles->images[0], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &handles->buffers.planes[0]);
      if (result == PYROWAVE_SUCCESS) {
        result = api->image_get_image_view(handles->images[1], VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &handles->buffers.planes[1]);
      }
      if (result == PYROWAVE_SUCCESS) {
        result = api->image_get_image_view(handles->images[1], VK_IMAGE_ASPECT_PLANE_2_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &handles->buffers.planes[2]);
      }
    }
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: couldn't view the input textures ("sv << result << ')';
      return false;
    }

    pyrowave_encoder_create_info encoder_info {};
    encoder_info.device = handles->device;
    encoder_info.width = width;
    encoder_info.height = height;
    encoder_info.chroma = yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    result = api->encoder_create(&encoder_info, &handles->encoder);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave: couldn't create the encoder ("sv << result << ')';
      return false;
    }

    BOOST_LOG(info) << "PyroWave: encoding "sv << width << 'x' << height << (ten_bit ? " 10-bit"sv : " 8-bit"sv) << (yuv444 ? " 4:4:4"sv : " 4:2:0"sv);
    return true;
  }

  ID3D11Texture2D *d3d11_encoder::y_texture() const {
    return y_tex;
  }

  ID3D11Texture2D *d3d11_encoder::uv_texture() const {
    return uv_tex;
  }

  void d3d11_encoder::begin_convert() {
    collect_convert_times();

    auto &queries = convert_queries[convert_query_index];
    timing_convert = false;
    if (!queries.disjoint) {
      return;
    }
    if (queries.pending) {
      // The GPU hasn't finished with this set yet, so this frame goes untimed
      convert_busy++;
      return;
    }

    device_ctx->Begin(queries.disjoint);
    device_ctx->End(queries.start);
    timing_convert = true;
    input_acquired = false;
  }

  void d3d11_encoder::mark_input_acquired() {
    if (!timing_convert || input_acquired) {
      return;
    }

    device_ctx->End(convert_queries[convert_query_index].acquired);
    input_acquired = true;
  }

  void d3d11_encoder::end_convert() {
    if (!timing_convert) {
      return;
    }

    auto &queries = convert_queries[convert_query_index];
    if (!input_acquired) {
      // Nothing to wait for, like a blank frame, so it all counts as drawing
      device_ctx->End(queries.acquired);
    }
    device_ctx->End(queries.end);
    device_ctx->End(queries.disjoint);
    queries.pending = true;
    convert_query_index = (convert_query_index + 1) % convert_queries.size();
    timing_convert = false;
  }

  void d3d11_encoder::collect_convert_times() {
    for (auto &queries : convert_queries) {
      if (!queries.pending) {
        continue;
      }

      // The disjoint query ends last, so once it's ready the timestamps are too
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
      const auto status = device_ctx->GetData(queries.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (status == S_FALSE) {
        continue;
      }

      // An error won't clear up, so the set is freed rather than left waiting forever
      queries.pending = false;
      if (FAILED(status)) {
        convert_unreadable++;
        continue;
      }
      if (disjoint.Disjoint) {
        convert_disjoint++;
        continue;
      }

      UINT64 start = 0;
      UINT64 acquired = 0;
      UINT64 end = 0;
      if (device_ctx->GetData(queries.start, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          device_ctx->GetData(queries.acquired, &acquired, sizeof(acquired), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          device_ctx->GetData(queries.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        convert_unreadable++;
        continue;
      }
      convert_time.add(start, end, disjoint.Frequency);
      convert_wait.add(start, acquired, disjoint.Frequency);
      convert_draw.add(acquired, end, disjoint.Frequency);
    }
  }

  bool d3d11_encoder::encode_frame(std::size_t max_frame_size, std::vector<std::uint8_t> &frame, frame_trace::trace_t *trace) {
    if (!handles->encoder) {
      return false;
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
    std::array<pyrowave_gpu_external_reference, 2> refs {};
    std::size_t num_refs = 0;
    for (auto image : handles->images) {
      // 4:4:4 has a single input texture
      if (image) {
        refs[num_refs++] = {image, VK_QUEUE_FAMILY_EXTERNAL};
      }
    }

    pyrowave_gpu_sync_operation acquire {};
    acquire.images = refs.data();
    acquire.num_images = num_refs;
    acquire.sync = {semaphore, drawn_value};

    pyrowave_gpu_sync_operation release {};
    release.images = refs.data();
    release.num_images = num_refs;
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
      return false;
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
      return false;
    }

    // A reused buffer already has its pages, and only what it grows by is cleared. A new
    // one this large would come from the OS and fault in every page on every frame.
    std::vector<pyrowave_packet> packets(num_packets);
    frame.resize(num_packets * boundary);
    std::size_t out_packets = 0;
    result = api->encoder_packetize(handles->encoder, packets.data(), boundary, &out_packets, frame.data(), frame.size());
    if (result != PYROWAVE_SUCCESS || out_packets == 0) {
      BOOST_LOG(error) << "PyroWave: couldn't packetize the encoded frame ("sv << result << ')';
      return false;
    }
    join_packets(frame, packets.data(), out_packets);

    report_performance();
    return true;
  }

  void d3d11_encoder::report_performance() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report < 10s) {
      return;
    }
    last_report = now;

    // Averages per frame since the last report: each PyroWave pass, then GPU memory.
    // Total spans PyroWave's command buffer, from after it waits for the conversion to
    // after the encoded frame is copied back for the CPU.
    api->device_report_performance_stats(
      handles->device,
      [](void *, const char *msg) {
        BOOST_LOG(debug) << "PyroWave: "sv << msg;
      },
      nullptr,
      true
    );

    // Says why frames weren't timed too, as that has happened without an obvious cause
    if (convert_queries[0].disjoint) {
      BOOST_LOG(debug) << "PyroWave: D3D11 convert: "sv << std::fixed << std::setprecision(3) << convert_time.average_ms() << " ms per frame"sv
                       << " (waiting for the capture "sv << convert_wait.average_ms() << " ms, drawing "sv << convert_draw.average_ms() << " ms; "sv
                       << convert_time.count() << " timed, "sv << convert_busy << " skipped while the queries were in use, "sv
                       << convert_disjoint << " disjoint, "sv << convert_unreadable << " unreadable)"sv;
    }
    convert_time.reset();
    convert_wait.reset();
    convert_draw.reset();
    convert_busy = 0;
    convert_disjoint = 0;
    convert_unreadable = 0;
  }

}  // namespace pyrowave
#endif
