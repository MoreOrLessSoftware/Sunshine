/**
 * @file src/platform/windows/xgpad.cpp
 * @brief Virtual Xbox Series X|S controllers backed by the Sunshine xgpad UMDF2 driver.
 */
#ifndef DOXYGEN
  #define WINVER 0x0A00
#endif

// platform includes
#include <Windows.h>

// Windows.h must come first
#include <sddl.h>
#include <setupapi.h>

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/utility.h"
#include "xgpad.h"
#include "xgpad_report.h"

using namespace std::literals;

namespace platf::xgpad {
  namespace {
    // swdevice.h is not shipped by mingw-w64; these mirror the Windows SDK declarations.
    // The functions are resolved from cfgmgr32.dll at runtime.

    /**
     * @brief Opaque software device handle (HSWDEVICE).
     */
    using hswdevice_t = struct hswdevice_s *;

    constexpr ULONG sw_cap_removable = 0x1;  ///< SWDeviceCapabilitiesRemovable
    constexpr ULONG sw_cap_silent_install = 0x2;  ///< SWDeviceCapabilitiesSilentInstall
    constexpr ULONG sw_cap_driver_required = 0x8;  ///< SWDeviceCapabilitiesDriverRequired

    /**
     * @brief Mirror of SW_DEVICE_CREATE_INFO.
     */
    struct sw_device_create_info_t {
      ULONG cbSize;  ///< Structure size.
      PCWSTR pszInstanceId;  ///< Instance ID, unique per enumerator.
      PCZZWSTR pszzHardwareIds;  ///< Hardware IDs (multi-string).
      PCZZWSTR pszzCompatibleIds;  ///< Compatible IDs (multi-string).
      const GUID *pContainerId;  ///< Optional container ID.
      ULONG CapabilityFlags;  ///< SWDeviceCapabilities* flags.
      PCWSTR pszDeviceDescription;  ///< Device description.
      PCWSTR pszDeviceLocation;  ///< Optional location string.
      const SECURITY_DESCRIPTOR *pSecurityDescriptor;  ///< Optional device security.
    };

    /**
     * @brief SW_DEVICE_CREATE_CALLBACK.
     */
    using sw_create_callback_t = VOID(WINAPI *)(hswdevice_t, HRESULT, PVOID, PCWSTR);
    /**
     * @brief SwDeviceCreate.
     */
    using sw_device_create_t = HRESULT(WINAPI *)(PCWSTR, PCWSTR, const sw_device_create_info_t *, ULONG, const void *, sw_create_callback_t, PVOID, hswdevice_t *);
    /**
     * @brief SwDeviceClose.
     */
    using sw_device_close_t = VOID(WINAPI *)(hswdevice_t);

    // devpropdef.h in mingw-w64 lacks DEVPROPCOMPKEY/DEVPROPERTY; these mirror the Windows SDK layout.

    /**
     * @brief Mirror of DEVPROPCOMPKEY.
     */
    struct devprop_compkey_t {
      DEVPROPKEY Key;  ///< Property key.
      int Store;  ///< DEVPROPSTORE (0 = DEVPROP_STORE_SYSTEM).
      PCWSTR LocaleName;  ///< Locale, null for locale-neutral properties.
    };

    /**
     * @brief Mirror of DEVPROPERTY.
     */
    struct devproperty_t {
      devprop_compkey_t CompKey;  ///< Property key, store and locale.
      ULONG Type;  ///< DEVPROPTYPE.
      ULONG BufferSize;  ///< Size of Buffer in bytes.
      PVOID Buffer;  ///< Property value.
    };

    constexpr ULONG devprop_type_string = 0x12;  ///< DEVPROP_TYPE_STRING

    /**
     * @brief Custom property carrying the per-device object name token to the driver.
     */
    constexpr DEVPROPKEY token_propkey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_TOKEN_PROPKEY_PID};

    /**
     * @brief HID device class GUID {745a17a0-74d3-11d0-b6fe-00a0c90f57da}.
     */
    constexpr GUID hid_class_guid = {0x745a17a0, 0x74d3, 0x11d0, {0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda}};

    /**
     * @brief Hardware ID the xgpad INF matches (BLE-style tail, see drivers/xgpad/driver/xgpad.inf).
     */
    constexpr wchar_t hardware_ids[] = L"SunshineXgpad\\{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&02045e_PID&0b13\0";

    /**
     * @brief Prefix shared by all hardware IDs in the xgpad INF.
     */
    constexpr std::wstring_view hardware_id_prefix = L"SunshineXgpad\\";

    constexpr auto create_timeout = 10s;  ///< Maximum time to wait for PnP to create a pad.
    constexpr int create_retries = 20;  ///< Retries while a just-removed instance ID is still registered.

    /**
     * @brief Dynamically loaded software device API.
     */
    struct sw_api_t {
      sw_device_create_t create = nullptr;  ///< SwDeviceCreate.
      sw_device_close_t close = nullptr;  ///< SwDeviceClose.

      /**
       * @brief Resolve the functions from cfgmgr32.dll.
       * @return True when both functions are available.
       */
      bool load() {
        HMODULE module = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
          return false;
        }
        create = reinterpret_cast<sw_device_create_t>(GetProcAddress(module, "SwDeviceCreate"));
        close = reinterpret_cast<sw_device_close_t>(GetProcAddress(module, "SwDeviceClose"));
        return create && close;
      }
    };

    /**
     * @brief Instance ID prefixes of xgpad device nodes (upper case).
     *
     * The HID child prefix ends in "&IG_00\": a real Bluetooth pad has its address before "&IG_00" and never matches.
     */
    constexpr std::wstring_view xgpad_instance_prefixes[] = {
      L"SWD\\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B13_REV&0509\\",
      L"HID\\{00001812-0000-1000-8000-00805F9B34FB}&DEV&VID_045E&PID_0B13&REV_0509&IG_00\\",
      // Naming used by earlier xgpad test builds.
      L"SWD\\SUNSHINEXGPAD\\",
      L"HID\\SUNSHINEXGPAD&IG_00\\",
      L"SWD\\VID_045E&PID_0B13&REV_0509\\",
      L"HID\\VID_045E&PID_0B13&REV_0509&IG_00\\",
    };

    constexpr DWORD cr_success = 0;  ///< CR_SUCCESS
    constexpr DWORD device_id_max = 200;  ///< MAX_DEVICE_ID_LEN
    constexpr ULONG cm_locate_devnode_phantom = 0x1;  ///< CM_LOCATE_DEVNODE_PHANTOM
    constexpr auto removal_wait = 3s;  ///< How long shutdown waits for closed pads to become non-present.

    /**
     * @brief CM_Locate_DevNodeW.
     */
    using cm_locate_devnode_t = DWORD(WINAPI *)(DWORD *, const wchar_t *, ULONG);
    /**
     * @brief CM_Uninstall_DevNode (not exported by the mingw-w64 import library).
     */
    using cm_uninstall_devnode_t = DWORD(WINAPI *)(DWORD, ULONG);

    /**
     * @brief List xgpad device node instance IDs of the HID class.
     * @param present_only Only include devices that are currently present.
     * @return Matching instance IDs.
     */
    std::vector<std::wstring> list_xgpad_instances(const bool present_only) {
      std::vector<std::wstring> ids;
      HDEVINFO set = SetupDiGetClassDevsW(&hid_class_guid, nullptr, nullptr, present_only ? DIGCF_PRESENT : 0);
      if (set == INVALID_HANDLE_VALUE) {
        return ids;
      }
      SP_DEVINFO_DATA data {};
      data.cbSize = sizeof(data);
      for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
        wchar_t id[device_id_max];
        if (SetupDiGetDeviceInstanceIdW(set, &data, id, device_id_max, nullptr) && is_xgpad_instance_id(id)) {
          ids.emplace_back(id);
        }
      }
      SetupDiDestroyDeviceInfoList(set);
      return ids;
    }

    /**
     * @brief Format a wide-character name with a pad index.
     * @param fmt printf-style format containing one %u.
     * @param index Pad index.
     * @return Formatted name.
     */
    std::wstring format_name(const wchar_t *fmt, const unsigned index) {
      wchar_t buffer[96];
      std::swprintf(buffer, std::size(buffer), fmt, index);
      return buffer;
    }

    /**
     * @brief Format a shared object name with a device token.
     * @param fmt printf-style format containing one %ls.
     * @param token Per-device token.
     * @return Formatted name.
     */
    std::wstring format_name(const wchar_t *fmt, const std::wstring &token) {
      wchar_t buffer[128];
      std::swprintf(buffer, std::size(buffer), fmt, token.c_str());
      return buffer;
    }

    /**
     * @brief Create a token that is unique for every device Sunshine creates.
     * @param index Pad index, kept as a readable prefix.
     * @return Token such as "0_5f3a9c1e7b2d4a60".
     */
    std::wstring make_token(const unsigned index) {
      static std::atomic<std::uint32_t> counter {0};
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      const auto mix = static_cast<std::uint64_t>(now.QuadPart) ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 40) ^
                       (static_cast<std::uint64_t>(counter.fetch_add(1)) << 20);
      wchar_t buffer[XGPAD_TOKEN_MAX];
      std::swprintf(buffer, std::size(buffer), L"%u_%016llx", index, static_cast<unsigned long long>(mix));
      return buffer;
    }
  }  // namespace

  /**
   * @brief One virtual pad: software device, shared section and events.
   */
  struct pad_t {
    unsigned index = 0;  ///< Global gamepad index; also the device instance number.
    std::wstring token;  ///< Unique token naming this device's shared objects.
    HANDLE section = nullptr;  ///< Shared memory section.
    XGPAD_SHARED *shared = nullptr;  ///< Mapped view of the section.
    HANDLE input_event = nullptr;  ///< Signalled after writing an input report.
    HANDLE output_event = nullptr;  ///< Signalled by the driver after writing an output report.
    HANDLE created_event = nullptr;  ///< Signalled by the SwDeviceCreate callback.
    hswdevice_t device = nullptr;  ///< Software device handle (device lives as long as this handle).
    std::atomic<HRESULT> create_result {E_PENDING};  ///< Result reported by the SwDeviceCreate callback.
    std::wstring instance_id;  ///< Device instance ID reported by PnP.

    feedback_queue_t feedback_queue;  ///< Queue for rumble feedback to the client.
    std::uint8_t client_relative_index = 0;  ///< Controller number as known by the client.
    rumble_state_t rumble {};  ///< Last rumble state applied from the driver.
    rumble_state_t sent {};  ///< Last rumble state sent to the client.

    /**
     * @brief Release all handles; the device must already be closed.
     */
    ~pad_t() {
      if (shared) {
        UnmapViewOfFile(shared);
      }
      for (HANDLE handle : {section, input_event, output_event, created_event}) {
        if (handle) {
          CloseHandle(handle);
        }
      }
    }
  };

  /**
   * @brief Process-wide xgpad state: pad slots and the feedback thread.
   */
  class context_t {
  public:
    sw_api_t api;  ///< Software device API.
    std::mutex mutex;  ///< Guards pad slots and per-pad feedback state.
    std::array<std::unique_ptr<pad_t>, MAX_GAMEPADS> pads;  ///< Pads indexed by global gamepad index.
    HANDLE wake_event = nullptr;  ///< Wakes the feedback thread when slots change or on shutdown.
    std::atomic<bool> stopping {false};  ///< Set when the feedback thread must exit.
    std::thread feedback_thread;  ///< Thread forwarding rumble from the driver to clients.

    std::mutex pnp_mutex;  ///< Serializes device creation with stale device node removal.
    std::mutex cleanup_mutex;  ///< Guards the cleanup request state.
    std::condition_variable cleanup_cv;  ///< Wakes the cleanup thread.
    std::vector<std::wstring> cleanup_pending;  ///< Instance IDs of freed pads waiting to become non-present.
    std::thread cleanup_thread;  ///< Removes device nodes of freed pads once PnP has removed the devices.

    /**
     * @brief Delete the device nodes of freed pads once they are no longer present.
     *
     * Runs off the input thread: SwDeviceClose removes devices asynchronously, so each request waits (bounded)
     * for the device to disappear before its phantom device nodes are deleted.
     */
    void cleanup_loop() {
      std::unique_lock lock {cleanup_mutex};
      while (true) {
        cleanup_cv.wait(lock, [this] {
          return stopping || !cleanup_pending.empty();
        });
        if (stopping) {
          return;
        }
        auto freed = std::move(cleanup_pending);
        cleanup_pending.clear();
        lock.unlock();

        const auto deadline = std::chrono::steady_clock::now() + removal_wait;
        while (!stopping && std::chrono::steady_clock::now() < deadline) {
          const auto present = list_xgpad_instances(true);
          const bool any_present = std::ranges::any_of(freed, [&](const std::wstring &id) {
            return std::ranges::any_of(present, [&](const std::wstring &p) {
              return _wcsicmp(p.c_str(), id.c_str()) == 0;
            });
          });
          if (!any_present) {
            break;
          }
          std::this_thread::sleep_for(50ms);
        }
        {
          std::lock_guard pnp {pnp_mutex};
          remove_stale_devices();
        }

        lock.lock();
      }
    }

    /**
     * @brief Wait for output reports from any pad and forward them as rumble feedback.
     */
    void feedback_loop() {
      std::vector<HANDLE> handles;
      std::vector<pad_t *> owners;

      while (!stopping) {
        // Snapshot the output events. Handles are duplicated so a concurrent free() cannot close them
        // while we wait; pads are re-validated by identity under the lock before use.
        handles.assign(1, wake_event);
        owners.assign(1, nullptr);
        {
          std::lock_guard lock {mutex};
          for (auto &pad : pads) {
            HANDLE duplicate = nullptr;
            if (pad && DuplicateHandle(GetCurrentProcess(), pad->output_event, GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0)) {
              handles.push_back(duplicate);
              owners.push_back(pad.get());
            }
          }
        }

        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (wait > WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + handles.size()) {
          std::lock_guard lock {mutex};
          pad_t *owner = owners[wait - WAIT_OBJECT_0];
          for (auto &pad : pads) {
            if (pad.get() == owner) {
              forward_rumble(*pad);
              break;
            }
          }
        } else if (wait == WAIT_FAILED) {
          BOOST_LOG(error) << "xgpad: waiting for rumble events failed: "sv << GetLastError();
          stopping = true;
        }

        for (std::size_t i = 1; i < handles.size(); ++i) {
          CloseHandle(handles[i]);
        }
      }
    }

    /**
     * @brief Read a pad's latest output report and send changed rumble to the client.
     * @param pad Pad whose output event fired; the caller holds the lock.
     */
    static void forward_rumble(pad_t &pad) {
      XGPAD_OUTPUT_REPORT report;
      long seq;
      do {
        seq = pad.shared->outputSeq;
        MemoryBarrier();
        std::memcpy(&report, const_cast<const XGPAD_OUTPUT_REPORT *>(&pad.shared->output), sizeof(report));
        MemoryBarrier();
      } while ((seq & 1) || seq != pad.shared->outputSeq);

      pad.rumble = apply_output_report(report, pad.rumble);
      if (!pad.feedback_queue) {
        return;
      }

      if (pad.rumble.low_freq != pad.sent.low_freq || pad.rumble.high_freq != pad.sent.high_freq) {
        pad.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(pad.client_relative_index, pad.rumble.low_freq, pad.rumble.high_freq));
      }
      if (pad.rumble.left_trigger != pad.sent.left_trigger || pad.rumble.right_trigger != pad.sent.right_trigger) {
        pad.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(pad.client_relative_index, pad.rumble.left_trigger, pad.rumble.right_trigger));
      }
      pad.sent = pad.rumble;
    }
  };

  namespace {
    /**
     * @brief SwDeviceCreate completion callback.
     * @param result Creation result.
     * @param context The pad being created.
     * @param instance_id Device instance ID on success.
     */
    VOID WINAPI on_device_created(hswdevice_t, HRESULT result, PVOID context, PCWSTR instance_id) {
      auto *pad = static_cast<pad_t *>(context);
      if (instance_id) {
        pad->instance_id = instance_id;
      }
      pad->create_result = result;
      SetEvent(pad->created_event);
    }

    /**
     * @brief Write an input report into a pad's shared memory (seqlock) and wake the driver.
     * @param pad Target pad.
     * @param report Input report payload.
     */
    void write_input(pad_t &pad, const XGPAD_INPUT_REPORT &report) {
      XGPAD_SHARED *shared = pad.shared;
      InterlockedIncrement(&shared->inputSeq);  // odd: write in progress
      MemoryBarrier();
      std::memcpy(const_cast<XGPAD_INPUT_REPORT *>(&shared->input), &report, sizeof(report));
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      shared->inputQpc = now.QuadPart;
      MemoryBarrier();
      InterlockedIncrement(&shared->inputSeq);  // even: complete
      SetEvent(pad.input_event);
    }

    /**
     * @brief Create the named section and events for a pad.
     * @param pad Pad to initialize.
     * @return True on success.
     */
    bool create_shared_objects(pad_t &pad) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(XGPAD_OBJECT_SDDL, SDDL_REVISION_1, &descriptor, nullptr)) {
        BOOST_LOG(error) << "xgpad: invalid security descriptor: "sv << GetLastError();
        return false;
      }
      SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};

      pad.section = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(XGPAD_SHARED), format_name(XGPAD_SECTION_NAME_FMT, pad.token).c_str());
      pad.input_event = CreateEventW(&attributes, FALSE, FALSE, format_name(XGPAD_INPUT_EVENT_NAME_FMT, pad.token).c_str());
      pad.output_event = CreateEventW(&attributes, FALSE, FALSE, format_name(XGPAD_OUTPUT_EVENT_NAME_FMT, pad.token).c_str());
      pad.created_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      LocalFree(descriptor);

      if (!pad.section || !pad.input_event || !pad.output_event || !pad.created_event) {
        BOOST_LOG(error) << "xgpad: failed to create shared objects for pad "sv << pad.index << ": "sv << GetLastError();
        return false;
      }

      pad.shared = static_cast<XGPAD_SHARED *>(MapViewOfFile(pad.section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(XGPAD_SHARED)));
      if (!pad.shared) {
        BOOST_LOG(error) << "xgpad: failed to map shared memory for pad "sv << pad.index << ": "sv << GetLastError();
        return false;
      }

      std::memset(pad.shared, 0, sizeof(*pad.shared));
      pad.shared->magic = XGPAD_SHARED_MAGIC;
      pad.shared->version = XGPAD_SHARED_VERSION;
      const auto neutral = make_input_report(gamepad_state_t {});
      std::memcpy(const_cast<XGPAD_INPUT_REPORT *>(&pad.shared->input), &neutral, sizeof(neutral));
      return true;
    }

    /**
     * @brief Create the software device for a pad and wait for PnP to finish.
     * @param api Software device API.
     * @param pad Pad with shared objects already created.
     * @return S_OK on success, otherwise the failure HRESULT.
     */
    HRESULT create_device(const sw_api_t &api, pad_t &pad) {
      const auto instance = format_name(XGPAD_SWD_INSTANCE_FMT, pad.index);
      sw_device_create_info_t info {};
      info.cbSize = sizeof(info);
      info.pszInstanceId = instance.c_str();
      info.pszzHardwareIds = hardware_ids;
      info.pszzCompatibleIds = L"\0";
      info.CapabilityFlags = sw_cap_removable | sw_cap_silent_install | sw_cap_driver_required;
      info.pszDeviceDescription = L"Sunshine Virtual Xbox Series Controller";

      // Hand the object name token to the driver.
      devproperty_t token_property {};
      token_property.CompKey.Key = token_propkey;
      token_property.Type = devprop_type_string;
      token_property.BufferSize = static_cast<ULONG>((pad.token.size() + 1) * sizeof(wchar_t));
      token_property.Buffer = const_cast<wchar_t *>(pad.token.c_str());

      HRESULT result = E_FAIL;
      for (int attempt = 0; attempt < create_retries; ++attempt) {
        ResetEvent(pad.created_event);
        pad.create_result = E_PENDING;
        pad.device = nullptr;
        result = api.create(XGPAD_SWD_ENUMERATOR, L"HTREE\\ROOT\\0", &info, 1, &token_property, on_device_created, &pad, &pad.device);
        if (SUCCEEDED(result)) {
          if (WaitForSingleObject(pad.created_event, static_cast<DWORD>(std::chrono::milliseconds(create_timeout).count())) != WAIT_OBJECT_0) {
            result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
          } else {
            result = pad.create_result;
          }
        }
        if (result != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
          break;
        }
        // The previous device with this instance ID is still being removed.
        if (pad.device) {
          api.close(pad.device);
          pad.device = nullptr;
        }
        std::this_thread::sleep_for(100ms);
      }
      return result;
    }
  }  // namespace

  bool driver_installed() {
    static const bool installed = [] {
      HDEVINFO set = SetupDiCreateDeviceInfoList(&hid_class_guid, nullptr);
      if (set == INVALID_HANDLE_VALUE) {
        return false;
      }
      bool found = false;
      // Without ALLOWEXCLUDEDDRVS the class driver list comes back empty for HIDClass.
      SP_DEVINSTALL_PARAMS_W params {};
      params.cbSize = sizeof(params);
      if (SetupDiGetDeviceInstallParamsW(set, nullptr, &params)) {
        params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        SetupDiSetDeviceInstallParamsW(set, nullptr, &params);
      }
      if (SetupDiBuildDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER)) {
        SP_DRVINFO_DATA_W driver {};
        driver.cbSize = sizeof(driver);
        for (DWORD i = 0; !found && SetupDiEnumDriverInfoW(set, nullptr, SPDIT_CLASSDRIVER, i, &driver); ++i) {
          DWORD size = 0;
          SetupDiGetDriverInfoDetailW(set, nullptr, &driver, nullptr, 0, &size);
          if (size < sizeof(SP_DRVINFO_DETAIL_DATA_W)) {
            continue;
          }
          std::vector<std::uint8_t> buffer(size);
          auto *detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W *>(buffer.data());
          detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
          if (SetupDiGetDriverInfoDetailW(set, nullptr, &driver, detail, size, nullptr)) {
            found = _wcsnicmp(detail->HardwareID, hardware_id_prefix.data(), hardware_id_prefix.size()) == 0;
          }
        }
        SetupDiDestroyDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER);
      }
      SetupDiDestroyDeviceInfoList(set);
      BOOST_LOG(info) << "xgpad: driver package "sv << (found ? "found"sv : "not found"sv);
      return found;
    }();
    return installed;
  }

  bool is_xgpad_instance_id(const std::wstring_view instance_id) {
    return std::ranges::any_of(xgpad_instance_prefixes, [&](const std::wstring_view prefix) {
      return instance_id.size() > prefix.size() && _wcsnicmp(instance_id.data(), prefix.data(), prefix.size()) == 0;
    });
  }

  int remove_stale_devices() {
    HMODULE module = LoadLibraryExW(L"cfgmgr32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
      return 0;
    }
    const auto locate = reinterpret_cast<cm_locate_devnode_t>(GetProcAddress(module, "CM_Locate_DevNodeW"));
    const auto uninstall = reinterpret_cast<cm_uninstall_devnode_t>(GetProcAddress(module, "CM_Uninstall_DevNode"));
    if (!locate || !uninstall) {
      return 0;
    }

    const auto present = list_xgpad_instances(true);
    int removed = 0;
    for (const auto &id : list_xgpad_instances(false)) {
      if (std::ranges::find(present, id) != present.end()) {
        continue;  // never touch a device that is in use
      }
      DWORD devinst = 0;
      if (locate(&devinst, id.c_str(), cm_locate_devnode_phantom) == cr_success && uninstall(devinst, 0) == cr_success) {
        ++removed;
      } else {
        BOOST_LOG(debug) << "xgpad: could not remove stale device node "sv << std::string(id.begin(), id.end());
      }
    }
    if (removed) {
      BOOST_LOG(info) << "xgpad: removed "sv << removed << " stale device node(s)"sv;
    }
    return removed;
  }

  void destroy(context_t *context) {
    if (!context) {
      return;
    }
    {
      std::lock_guard lock {context->cleanup_mutex};
      context->stopping = true;
    }
    context->cleanup_cv.notify_all();
    SetEvent(context->wake_event);
    if (context->feedback_thread.joinable()) {
      context->feedback_thread.join();
    }
    if (context->cleanup_thread.joinable()) {
      context->cleanup_thread.join();
    }
    bool had_pads = false;
    for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
      had_pads |= has(context, nr);
      free(*context, nr);
    }
    CloseHandle(context->wake_event);
    delete context;

    if (had_pads) {
      // SwDeviceClose removes devices asynchronously; give PnP a moment before deleting the leftovers.
      const auto deadline = std::chrono::steady_clock::now() + removal_wait;
      while (!list_xgpad_instances(true).empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
      }
    }
    remove_stale_devices();
  }

  context_ptr create() {
    // Clean up after previous runs (including crashes and service stops) before creating new pads.
    remove_stale_devices();

    context_ptr context {new context_t {}, &destroy};
    if (!context->api.load()) {
      BOOST_LOG(warning) << "xgpad: software device API is unavailable"sv;
      return {nullptr, &destroy};
    }
    context->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!context->wake_event) {
      return {nullptr, &destroy};
    }
    context->feedback_thread = std::thread {&context_t::feedback_loop, context.get()};
    context->cleanup_thread = std::thread {&context_t::cleanup_loop, context.get()};
    return context;
  }

  int alloc(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) {
      return -1;
    }

    auto pad = std::make_unique<pad_t>();
    pad->index = static_cast<unsigned>(id.globalIndex);
    pad->token = make_token(pad->index);
    pad->client_relative_index = id.clientRelativeIndex;
    pad->feedback_queue = std::move(feedback_queue);
    if (!create_shared_objects(*pad)) {
      return -1;
    }

    const auto started = std::chrono::steady_clock::now();
    HRESULT result;
    {
      // Never create a pad while a freed pad's device nodes are being deleted.
      std::lock_guard pnp {context.pnp_mutex};
      result = create_device(context.api, *pad);
    }
    const auto create_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (FAILED(result)) {
      if (pad->device) {
        context.api.close(pad->device);
        pad->device = nullptr;
      }
      BOOST_LOG(warning) << "xgpad: could not create virtual Xbox Series controller "sv << id.globalIndex << " ["sv
                         << util::hex(result).to_string_view() << "]; is the xgpad driver installed and Sunshine elevated?"sv;
      return -1;
    }

    BOOST_LOG(info) << "Gamepad "sv << id.globalIndex << " will be Xbox Series controller (xgpad, token "sv
                    << std::string(pad->token.begin(), pad->token.end()) << ", created in "sv << create_ms << " ms)"sv;
    {
      std::lock_guard lock {context.mutex};
      context.pads[id.globalIndex] = std::move(pad);
    }
    SetEvent(context.wake_event);
    return 0;
  }

  bool has(const context_t *context, const int nr) {
    return context && nr >= 0 && nr < MAX_GAMEPADS && context->pads[nr];
  }

  int rebind(context_t &context, const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    std::lock_guard lock {context.mutex};
    if (!has(&context, id.globalIndex)) {
      return -1;
    }
    auto &pad = *context.pads[id.globalIndex];
    pad.client_relative_index = id.clientRelativeIndex;
    pad.feedback_queue = std::move(feedback_queue);
    pad.sent = {};  // resend the current rumble state to the resumed client on the next change
    return 0;
  }

  void update(context_t &context, const int nr, const gamepad_state_t &state) {
    // alloc/free/update all run on the input thread, so the slot cannot change underneath us.
    if (!has(&context, nr)) {
      return;
    }
    write_input(*context.pads[nr], make_input_report(state));
  }

  void free(context_t &context, const int nr) {
    std::unique_ptr<pad_t> pad;
    {
      std::lock_guard lock {context.mutex};
      if (!has(&context, nr)) {
        return;
      }
      pad = std::move(context.pads[nr]);
    }
    SetEvent(context.wake_event);
    if (pad->device) {
      context.api.close(pad->device);
    }
    BOOST_LOG(info) << "xgpad: removed virtual Xbox Series controller "sv << nr;

    // Delete the device nodes once PnP has finished removing the device, so none are left in Device Manager.
    if (!pad->instance_id.empty()) {
      {
        std::lock_guard lock {context.cleanup_mutex};
        context.cleanup_pending.push_back(pad->instance_id);
      }
      context.cleanup_cv.notify_one();
    }
  }

}  // namespace platf::xgpad
