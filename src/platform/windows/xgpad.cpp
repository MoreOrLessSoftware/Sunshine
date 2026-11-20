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
#include <format>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "playstation_report.h"
#include "src/client_commands.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/utility.h"
#include "xgpad.h"
#include "xgpad_ds5_report.h"
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
    constexpr ULONG devprop_type_uint32 = 0x07;  ///< DEVPROP_TYPE_UINT32

    /**
     * @brief Custom property carrying the per-device object name token to the driver.
     */
    constexpr DEVPROPKEY token_propkey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_TOKEN_PROPKEY_PID};

    /**
     * @brief Custom property carrying the pad's profile (XGPAD_PROFILE_*) to the driver.
     */
    constexpr DEVPROPKEY profile_propkey = {XGPAD_TOKEN_PROPKEY_GUID, XGPAD_PROFILE_PROPKEY_PID};

    /**
     * @brief HID device class GUID {745a17a0-74d3-11d0-b6fe-00a0c90f57da}.
     */
    constexpr GUID hid_class_guid = {0x745a17a0, 0x74d3, 0x11d0, {0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda}};

    /**
     * @brief Hardware ID of Xbox Series pads (BLE-style tail, see drivers/xgpad/driver/xgpad.inf).
     */
    constexpr wchar_t hardware_ids[] = XGPAD_HARDWARE_IDS;

    /**
     * @brief Hardware ID of DualSense pads, matched only by drivers that have the DualSense profile.
     */
    constexpr wchar_t dualsense_hardware_ids[] = XGPAD_HARDWARE_IDS_DUALSENSE;

    constexpr auto ds5_repeat_interval = 100ms;  ///< Longest gap between DualSense reports, so the sensor timestamp keeps moving.
    constexpr std::uint16_t motion_report_rate = 100;  ///< Motion events per second requested from clients for DualSense pads.

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
     * The Xbox HID child prefix ends in "&IG_00\": a real Bluetooth pad has its address before "&IG_00" and never
     * matches. The DualSense prefixes carry "&REV_0100", which a real DualSense's instance IDs never have (USB:
     * HID\VID_054C&PID_0CE6&MI_03\..., Bluetooth: HID\{00001124-...}_VID&0002054C_PID&0CE6\...).
     */
    constexpr std::wstring_view xgpad_instance_prefixes[] = {
      L"SWD\\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B13_REV&0509\\",
      L"HID\\{00001812-0000-1000-8000-00805F9B34FB}&DEV&VID_045E&PID_0B13&REV_0509&IG_00\\",
      L"SWD\\VID_054C&PID_0CE6&REV_0100\\",
      L"HID\\VID_054C&PID_0CE6&REV_0100\\",
      L"HID\\VID_054C&PID_0CE6&REV_0100&IG_",
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
    profile_e profile = profile_e::xbox_series;  ///< Controller the pad presents.
    std::string client_uuid;  ///< Client the pad belongs to, or empty when it's unknown.
    gamepad_state_t state {};  ///< Last buttons, sticks and triggers from the client, kept across a profile switch.
    std::wstring token;  ///< Unique token naming this device's shared objects.
    HANDLE section = nullptr;  ///< Shared memory section.
    XGPAD_SHARED *shared = nullptr;  ///< Mapped view of the section.
    HANDLE input_event = nullptr;  ///< Signalled after writing an input report.
    HANDLE output_event = nullptr;  ///< Signalled by the driver after writing an output report.
    HANDLE created_event = nullptr;  ///< Signalled by the SwDeviceCreate callback.
    hswdevice_t device = nullptr;  ///< Software device handle (device lives as long as this handle).
    std::atomic<HRESULT> create_result {E_PENDING};  ///< Result reported by the SwDeviceCreate callback.
    std::wstring instance_id;  ///< Device instance ID reported by PnP.
    std::chrono::steady_clock::time_point created_at {};  ///< When PnP finished creating the device.
    bool attach_checked = false;  ///< Driver attachment has been confirmed or reported as missing.

    feedback_queue_t feedback_queue;  ///< Queue for rumble feedback to the client.
    std::uint8_t client_relative_index = 0;  ///< Controller number as known by the client.
    rumble_state_t rumble {};  ///< Last rumble state applied from the driver.
    rumble_state_t sent {};  ///< Last rumble state sent to the client.
    thread_pool_util::ThreadPool::task_id_t rumble_stop_task = nullptr;  ///< Pending stop for a finite-duration rumble.
    std::uint32_t rumble_generation = 0;  ///< Incremented per output report; stale stop tasks are ignored.
    long output_consumed = 0;  ///< Output reports already read from the shared ring.

    ds5_state_t ds5;  ///< DualSense state kept between client events.
    playstation::touch_slots_t touch_slots;  ///< DualSense touch slot of each active touch pointer.
    std::uint8_t ds5_seq = 0;  ///< DualSense report counter.
    thread_pool_util::ThreadPool::task_id_t repeat_task = nullptr;  ///< Pending repeat of the DualSense report.
    std::optional<std::array<std::uint8_t, 3>> sent_lightbar;  ///< Last lightbar color sent to the client.
    std::optional<std::uint8_t> sent_player_leds;  ///< Last DualSense player LED bits sent to the client.
    std::optional<std::uint8_t> sent_mic_led;  ///< Last mute LED state sent to the client.

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
    std::map<std::string, profile_e, std::less<>> session_profiles;  ///< Profile each client switched to, until its last pad is removed.
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
     * @brief Apply a pad's new output reports and send changed rumble to the client.
     * @param pad Pad whose output event fired; the caller holds the lock.
     */
    void forward_rumble(pad_t &pad) {
      long lost = 0;
      const auto reports = read_output_reports(*pad.shared, pad.output_consumed, &lost);
      if (lost > 0) {
        BOOST_LOG(debug) << "xgpad: pad "sv << pad.index << " missed "sv << lost << " output reports"sv;
      }
      if (pad.profile == profile_e::dualsense) {
        forward_ds5_output(pad, reports);
        return;
      }

      bool any = false;
      XGPAD_OUTPUT_REPORT report {};
      for (const auto &entry : reports) {
        if (XGPAD_OUTPUT_REPORT rumble; xbox_output_report(entry, rumble)) {
          pad.rumble = apply_output_report(rumble, pad.rumble);
          report = rumble;
          any = true;
        }
      }
      if (!any) {
        return;
      }

      // Finite-duration effects (games writing HID rumble directly) must stop on their own
      ++pad.rumble_generation;
      if (pad.rumble_stop_task) {
        task_pool.cancel(pad.rumble_stop_task);
        pad.rumble_stop_task = nullptr;
      }
      if (const auto duration_ms = rumble_duration_ms(report); duration_ms > 0) {
        pad.rumble_stop_task = task_pool
                                 .pushDelayed(
                                   [this, slot = pad.index, generation = pad.rumble_generation]() {
                                     stop_rumble(slot, generation);
                                   },
                                   std::chrono::milliseconds(duration_ms)
                                 )
                                 .task_id;
      }

      send_rumble(pad);
    }

    /**
     * @brief Send what a DualSense pad's output reports changed to the client; the caller holds the lock.
     * @param pad DualSense pad.
     * @param reports Output reports from the driver, oldest first.
     */
    static void forward_ds5_output(pad_t &pad, const std::vector<XGPAD_REPORT> &reports) {
      for (const auto &entry : reports) {
        const auto output = parse_ds5_output(entry);
        if (!output) {
          continue;
        }
        if (output->rumble) {
          pad.rumble.low_freq = (*output->rumble)[0];
          pad.rumble.high_freq = (*output->rumble)[1];
        }
        if (!pad.feedback_queue) {
          continue;
        }
        if (const auto &triggers = output->triggers) {
          pad.feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(pad.client_relative_index, triggers->flags, triggers->type_left, triggers->type_right, triggers->left, triggers->right));
        }
        if (output->lightbar && output->lightbar != pad.sent_lightbar) {
          const auto &color = *output->lightbar;
          pad.feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(pad.client_relative_index, color[0], color[1], color[2]));
          pad.sent_lightbar = output->lightbar;
        }
        if (output->player_leds && output->player_leds != pad.sent_player_leds) {
          const auto leds = *output->player_leds;
          pad.feedback_queue->raise(gamepad_feedback_msg_t::make_player_leds(pad.client_relative_index, ds5_player_mask(leds), 0, leds | gamepad_feedback_msg_t::ds5_player_leds_valid));
          pad.sent_player_leds = output->player_leds;
        }
        if (output->mic_led && output->mic_led != pad.sent_mic_led) {
          pad.feedback_queue->raise(gamepad_feedback_msg_t::make_mic_led(pad.client_relative_index, *output->mic_led));
          pad.sent_mic_led = output->mic_led;
        }
      }
      send_rumble(pad);
    }

    /**
     * @brief Stop a finite-duration rumble once it has played out.
     * @param slot Pad slot.
     * @param generation Rumble generation the stop was scheduled for.
     */
    void stop_rumble(const unsigned slot, const std::uint32_t generation) {
      std::lock_guard lock {mutex};
      auto &pad = pads[slot];
      if (!pad || pad->rumble_generation != generation) {
        return;  // pad removed or a newer rumble replaced this one
      }
      pad->rumble_stop_task = nullptr;
      pad->rumble = {};
      send_rumble(*pad);
    }

    /**
     * @brief Send changed rumble state to the client; the caller holds the lock.
     * @param pad Pad whose rumble state changed.
     */
    static void send_rumble(pad_t &pad) {
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
     * @brief Append an input report to a pad's shared ring and wake the driver.
     *
     * Every report is kept (up to XGPAD_INPUT_RING_SIZE outstanding), so short button presses survive even when
     * several reports arrive before HID class reads the next one.
     *
     * @param pad Target pad.
     * @param report Input report payload.
     */
    void write_input(pad_t &pad, const XGPAD_INPUT_REPORT &report) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      pad.shared->inputQpc = now.QuadPart;
      publish_input_report(*pad.shared, report);  // single writer (the input thread)
      SetEvent(pad.input_event);
    }

    /**
     * @brief Convert a QueryPerformanceCounter value to the DualSense sensor timestamp (0.33 us units).
     * @param counter QPC value.
     * @return Timestamp; it wraps around like a real pad's.
     */
    std::uint32_t ds5_timestamp(const std::int64_t counter) {
      static const std::int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
      }();
      constexpr std::int64_t units_per_second = 3'000'000;
      // Split the multiplication so it cannot overflow after a long uptime
      return static_cast<std::uint32_t>((counter / frequency) * units_per_second + (counter % frequency) * units_per_second / frequency);
    }

    void schedule_ds5_repeat(context_t &context, pad_t &pad);

    /**
     * @brief Publish a DualSense pad's current state as a new input report.
     *
     * Runs on the task pool thread, like all input, which keeps the input ring single-writer. A repeat is scheduled
     * so hosts keep receiving reports (with a moving sensor timestamp) while the client sends nothing.
     *
     * @param context xgpad context.
     * @param pad DualSense pad.
     */
    void write_ds5_input(context_t &context, pad_t &pad) {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      pad.shared->inputQpc = now.QuadPart;
      const auto report = make_ds5_input_report(pad.ds5, pad.ds5_seq++, ds5_timestamp(now.QuadPart));
      publish_input_report(*pad.shared, XGPAD_DS_INPUT_REPORT_ID, std::span {reinterpret_cast<const std::uint8_t *>(&report), sizeof(report)});
      SetEvent(pad.input_event);
      schedule_ds5_repeat(context, pad);
    }

    /**
     * @brief Replace a DualSense pad's pending repeat with one ds5_repeat_interval from now.
     * @param context xgpad context.
     * @param pad DualSense pad.
     */
    void schedule_ds5_repeat(context_t &context, pad_t &pad) {
      if (pad.repeat_task) {
        task_pool.cancel(pad.repeat_task);
      }
      pad.repeat_task = task_pool
                          .pushDelayed(
                            [&context, slot = pad.index, owner = &pad]() {
                              // free() cancels the repeat, so a pad in the slot is the one that scheduled it
                              if (!has(&context, static_cast<int>(slot)) || context.pads[slot].get() != owner) {
                                return;
                              }
                              owner->repeat_task = nullptr;
                              write_ds5_input(context, *owner);
                            },
                            ds5_repeat_interval
                          )
                          .task_id;
    }

    /**
     * @brief Ask the client to send motion events for a DualSense pad.
     * @param pad DualSense pad with its feedback queue set.
     */
    void request_motion(pad_t &pad) {
      if (!pad.feedback_queue) {
        return;
      }
      pad.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(pad.client_relative_index, LI_MOTION_TYPE_ACCEL, motion_report_rate));
      pad.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(pad.client_relative_index, LI_MOTION_TYPE_GYRO, motion_report_rate));
    }

    /**
     * @brief Find the DualSense pad in a slot.
     * @param context xgpad context.
     * @param nr Global gamepad index.
     * @return The pad, or null when the slot is empty or holds an Xbox Series pad.
     */
    pad_t *dualsense_pad(context_t &context, const int nr) {
      if (!has(&context, nr) || context.pads[nr]->profile != profile_e::dualsense) {
        return nullptr;
      }
      return context.pads[nr].get();
    }

    /**
     * @brief Log once whether the driver picked up a pad's shared memory.
     *
     * Runs on the input path, so it only reads a shared flag. An outdated driver package enumerates the
     * controller but ignores every report; without this check that failure is silent.
     *
     * @param pad Pad receiving input.
     */
    void check_driver_attached(pad_t &pad) {
      if (pad.attach_checked) {
        return;
      }
      switch (driver_attach_state(pad.shared->driverStatus, std::chrono::steady_clock::now() - pad.created_at)) {
        case attach_state_e::pending:
          return;
        case attach_state_e::attached:
          BOOST_LOG(debug) << "xgpad: driver attached to pad "sv << pad.index;
          break;
        case attach_state_e::missing:
          BOOST_LOG(error) << "xgpad: driver has not attached to pad "sv << pad.index << " after "sv << driver_attach_timeout.count()
                           << " ms; controller input is ignored. The installed xgpad driver probably does not match this Sunshine build "sv
                           << "(shared memory v"sv << XGPAD_SHARED_VERSION << "). Reinstall the xgpad driver."sv;
          break;
      }
      pad.attach_checked = true;
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
      pad.shared->version = XGPAD_SHARED_VERSION;  // the driver starts from a neutral state on its own
      pad.shared->profile = pad.profile == profile_e::dualsense ? XGPAD_PROFILE_DUALSENSE : XGPAD_PROFILE_XBOX_SERIES;
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
      const bool dualsense = pad.profile == profile_e::dualsense;
      info.pszzHardwareIds = dualsense ? dualsense_hardware_ids : hardware_ids;
      info.pszzCompatibleIds = L"\0";
      info.CapabilityFlags = sw_cap_removable | sw_cap_silent_install | sw_cap_driver_required;
      info.pszDeviceDescription = dualsense ? L"Sunshine Virtual DualSense Controller" : L"Sunshine Virtual Xbox Series Controller";

      // Hand the object name token and the profile to the driver.
      std::array<devproperty_t, 2> properties {};
      properties[0].CompKey.Key = token_propkey;
      properties[0].Type = devprop_type_string;
      properties[0].BufferSize = static_cast<ULONG>((pad.token.size() + 1) * sizeof(wchar_t));
      properties[0].Buffer = const_cast<wchar_t *>(pad.token.c_str());
      ULONG profile = dualsense ? XGPAD_PROFILE_DUALSENSE : XGPAD_PROFILE_XBOX_SERIES;
      properties[1].CompKey.Key = profile_propkey;
      properties[1].Type = devprop_type_uint32;
      properties[1].BufferSize = sizeof(profile);
      properties[1].Buffer = &profile;

      HRESULT result = E_FAIL;
      for (int attempt = 0; attempt < create_retries; ++attempt) {
        ResetEvent(pad.created_event);
        pad.create_result = E_PENDING;
        pad.device = nullptr;
        result = api.create(dualsense ? XGPAD_SWD_ENUMERATOR_DUALSENSE : XGPAD_SWD_ENUMERATOR, L"HTREE\\ROOT\\0", &info, static_cast<ULONG>(properties.size()), properties.data(), on_device_created, &pad, &pad.device);
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

    /**
     * @brief What the driver store knows about the xgpad driver.
     */
    struct driver_info_t {
      bool installed = false;  ///< A driver matching the xgpad hardware ID is in the driver store.
      bool dualsense = false;  ///< The driver also matches the DualSense hardware ID.
      std::string version;  ///< Dotted driver version, empty when not installed.
    };

    /**
     * @brief Search the driver store for the xgpad driver.
     * @return Installation state and version.
     */
    driver_info_t query_driver_info() {
      driver_info_t result;
      HDEVINFO set = SetupDiCreateDeviceInfoList(&hid_class_guid, nullptr);
      if (set == INVALID_HANDLE_VALUE) {
        return result;
      }
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
        // Every model line of the INF is its own entry: keep going to find the DualSense one.
        for (DWORD i = 0; SetupDiEnumDriverInfoW(set, nullptr, SPDIT_CLASSDRIVER, i, &driver); ++i) {
          DWORD size = 0;
          SetupDiGetDriverInfoDetailW(set, nullptr, &driver, nullptr, 0, &size);
          if (size < sizeof(SP_DRVINFO_DETAIL_DATA_W)) {
            continue;
          }
          std::vector<std::uint8_t> buffer(size);
          auto *detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W *>(buffer.data());
          detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
          if (SetupDiGetDriverInfoDetailW(set, nullptr, &driver, detail, size, nullptr) &&
              _wcsnicmp(detail->HardwareID, hardware_id_prefix.data(), hardware_id_prefix.size()) == 0) {
            result.installed = true;
            result.version = driver_version_text(driver.DriverVersion);
            result.dualsense |= _wcsicmp(detail->HardwareID, dualsense_hardware_ids) == 0;
          }
        }
        SetupDiDestroyDriverInfoList(set, nullptr, SPDIT_CLASSDRIVER);
      }
      SetupDiDestroyDeviceInfoList(set);
      return result;
    }

    /**
     * @brief Get the driver store state, querying it only when needed.
     * @param refresh Query the driver store even when a result is cached.
     * @return Installation state and version.
     */
    driver_info_t cached_driver_info(const bool refresh) {
      static std::mutex mutex;
      static std::optional<driver_info_t> cache;

      std::lock_guard lock {mutex};
      if (refresh || !cache) {
        auto current = query_driver_info();
        if (!cache || cache->installed != current.installed || cache->dualsense != current.dualsense) {
          BOOST_LOG(info) << "xgpad: driver package "sv << (current.installed ? "found"sv : "not found"sv)
                          << (current.installed ? (current.dualsense ? " (Xbox Series and DualSense)"sv : " (Xbox Series only)"sv) : ""sv);
        }
        cache = std::move(current);
      }
      return *cache;
    }
  }  // namespace

  bool driver_installed(const bool refresh) {
    return cached_driver_info(refresh).installed;
  }

  std::string driver_version() {
    return cached_driver_info(false).version;
  }

  bool driver_supports_dualsense() {
    return cached_driver_info(false).dualsense;
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

  namespace {
    constexpr auto set_profile_action = "xgpad.set_profile";  ///< Client command action that switches a client's pads.
    constexpr auto switch_timeout = 10s;  ///< Longest time a client command waits for its pads to be recreated.

    /**
     * @brief The context client command actions work on.
     */
    struct action_target_t {
      std::mutex mutex;  ///< Held while an action uses the context, and while the context goes away.
      context_t *context = nullptr;  ///< Context created last, or null.
    };

    /**
     * @brief Return the context client command actions work on.
     * @return Shared state.
     */
    action_target_t &action_target() {
      static action_target_t target;
      return target;
    }

    /**
     * @brief Describe the occupied slots for switch_menu_state() and plan_profile_switch().
     * @param context xgpad context; the caller holds its mutex while the result is in use.
     * @return One entry per pad.
     */
    std::vector<pad_slot_t> occupied_slots(const context_t &context) {
      std::vector<pad_slot_t> slots;
      for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
        if (const auto &pad = context.pads[nr]) {
          slots.push_back({nr, pad->client_uuid, pad->profile});
        }
      }
      return slots;
    }

    /**
     * @brief Take a pad out of its slot and close its device.
     *
     * Runs on the input thread. The device nodes are deleted once PnP has removed the device.
     *
     * @param context xgpad context.
     * @param nr Global gamepad index.
     * @return The removed pad, or null when the slot was empty.
     */
    std::unique_ptr<pad_t> remove_pad(context_t &context, const int nr) {
      std::unique_ptr<pad_t> pad;
      {
        std::lock_guard lock {context.mutex};
        if (!has(&context, nr)) {
          return nullptr;
        }
        pad = std::move(context.pads[nr]);
        if (pad->rumble_stop_task) {
          task_pool.cancel(pad->rumble_stop_task);
          pad->rumble_stop_task = nullptr;
        }
        if (pad->repeat_task) {
          task_pool.cancel(pad->repeat_task);
          pad->repeat_task = nullptr;
        }
      }
      SetEvent(context.wake_event);
      if (pad->device) {
        context.api.close(pad->device);
        pad->device = nullptr;
      }
      BOOST_LOG(info) << "xgpad: removed virtual "sv << profile_name(pad->profile) << " controller "sv << nr;

      // Delete the device nodes once PnP has finished removing the device, so none are left in Device Manager.
      if (!pad->instance_id.empty()) {
        {
          std::lock_guard lock {context.cleanup_mutex};
          context.cleanup_pending.push_back(pad->instance_id);
        }
        context.cleanup_cv.notify_one();
      }
      return pad;
    }

    /**
     * @brief Replace a pad with one presenting another profile, keeping its slot, client and state.
     *
     * Runs on the input thread. Games see the controller disconnect and a new one connect. When the new pad can't be
     * created, the old profile is put back.
     *
     * @param context xgpad context.
     * @param nr Global gamepad index.
     * @param target Profile to switch to.
     * @return True when the pad now presents the target profile.
     */
    bool recreate_pad(context_t &context, const int nr, const profile_e target) {
      auto old = remove_pad(context, nr);
      if (!old) {
        return false;
      }

      // Stop what the old controller asked of the client controller
      if (old->feedback_queue) {
        old->feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(old->client_relative_index, 0, 0));
        old->feedback_queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(old->client_relative_index, 0, 0));
        if (old->profile == profile_e::dualsense) {
          old->feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(old->client_relative_index, LI_MOTION_TYPE_ACCEL, 0));
          old->feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(old->client_relative_index, LI_MOTION_TYPE_GYRO, 0));
        }
      }

      const gamepad_id_t id {nr, old->client_relative_index};
      for (const auto profile : {target, old->profile}) {
        if (alloc(context, id, profile, old->feedback_queue, old->client_uuid) == 0) {
          update(context, nr, old->state);
          return profile == target;
        }
      }
      BOOST_LOG(error) << "xgpad: controller "sv << nr << " could not be recreated"sv;
      return false;
    }

    /**
     * @brief Switch all of a client's pads to a profile and remember it for pads the client connects later.
     *
     * Runs on the input thread, like every other change to the pads.
     *
     * @param context xgpad context.
     * @param client_uuid Client that asked.
     * @param target Profile to switch to.
     * @return Outcome for the client's menu.
     */
    client_commands::action_result_t switch_client_pads(context_t &context, const std::string &client_uuid, const profile_e target) {
      if (target == profile_e::dualsense && !driver_supports_dualsense()) {
        return {false, "The installed xgpad driver can't create DualSense controllers"};
      }

      std::vector<int> recreate;
      {
        std::lock_guard lock {context.mutex};
        const auto slots = occupied_slots(context);
        if (switch_menu_state(slots, client_uuid, target, true) == switch_menu_e::hidden) {
          return {false, "This client's controllers don't use the xgpad driver"};
        }
        recreate = plan_profile_switch(slots, client_uuid, target);
        context.session_profiles[client_uuid] = target;
      }

      int failed = 0;
      for (const auto nr : recreate) {
        failed += recreate_pad(context, nr, target) ? 0 : 1;
      }
      BOOST_LOG(info) << "xgpad: switched "sv << static_cast<int>(recreate.size()) - failed << " controller(s) to "sv << profile_name(target);
      if (failed > 0) {
        return {false, std::format("{} controller(s) couldn't be switched", failed)};
      }
      return {};
    }

    /**
     * @brief Run the `xgpad.set_profile` client command action.
     *
     * Called on the HTTPS server thread. The switch runs on the input thread, and this waits for it.
     *
     * @param run Who picked the command.
     * @param args Profile to switch to: "xbox" or "dualsense".
     * @return Outcome for the client's menu.
     */
    client_commands::action_result_t run_set_profile(const client_commands::run_context_t &run, const std::vector<std::string> &args) {
      const auto target = args.empty() ? std::nullopt : profile_from_option(args.front());
      if (!target) {
        return {false, "Unknown controller type"};
      }
      if (run.client_uuid.empty()) {
        return {false, "This client's controllers can't be told apart from other clients'"};
      }

      context_t *context;
      {
        std::scoped_lock lock {action_target().mutex};
        context = action_target().context;
      }
      if (!context) {
        return {false, "The xgpad driver isn't in use"};
      }

      auto promise = std::make_shared<std::promise<client_commands::action_result_t>>();
      auto future = promise->get_future();
      auto work = [context, client_uuid = run.client_uuid, target = *target, promise]() {
        // The context may have gone away while this waited for the input thread
        auto &shared = action_target();
        std::scoped_lock lock {shared.mutex};
        if (shared.context != context) {
          promise->set_value({false, "The xgpad driver isn't in use"});
          return;
        }
        promise->set_value(switch_client_pads(*context, client_uuid, target));
      };
      if (task_pool.running()) {
        task_pool.push(std::move(work));
      } else {
        work();
      }
      if (future.wait_for(switch_timeout) != std::future_status::ready) {
        return {false, "Switching the controllers took too long"};
      }
      return future.get();
    }

    /**
     * @brief Decide how an `xgpad.set_profile` command shows in a client's menu.
     * @param run Who is opening the menu; only the client UUID is set.
     * @param args Profile the command switches to.
     * @return Hidden without pads to switch, marked for the profile all of the client's pads have.
     */
    client_commands::action_display_t set_profile_status(const client_commands::run_context_t &run, const std::vector<std::string> &args) {
      client_commands::action_display_t display;
      const auto target = args.empty() ? std::nullopt : profile_from_option(args.front());
      auto &shared = action_target();
      std::scoped_lock lock {shared.mutex};
      if (!target || !shared.context) {
        display.hidden = true;
        return display;
      }
      std::scoped_lock pads_lock {shared.context->mutex};
      switch (switch_menu_state(occupied_slots(*shared.context), run.client_uuid, *target, driver_supports_dualsense())) {
        case switch_menu_e::hidden:
          display.hidden = true;
          break;
        case switch_menu_e::active:
          display.icon = "material:check_circle";
          break;
        case switch_menu_e::available:
          break;
      }
      return display;
    }
  }  // namespace

  void destroy(context_t *context) {
    if (!context) {
      return;
    }
    {
      // Wait for a running switch, and keep later ones off this context
      auto &shared = action_target();
      std::scoped_lock lock {shared.mutex};
      if (shared.context == context) {
        shared.context = nullptr;
        client_commands::unregister_action(set_profile_action);
      }
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
    {
      std::scoped_lock lock {action_target().mutex};
      action_target().context = context.get();
    }
    client_commands::register_action(set_profile_action, run_set_profile, set_profile_status);
    return context;
  }

  std::optional<profile_e> session_profile(context_t &context, const std::string_view client_uuid) {
    std::lock_guard lock {context.mutex};
    const auto it = context.session_profiles.find(client_uuid);
    if (client_uuid.empty() || it == context.session_profiles.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  int alloc(context_t &context, const gamepad_id_t &id, const profile_e profile, feedback_queue_t feedback_queue, const std::string &client_uuid) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) {
      return -1;
    }

    auto pad = std::make_unique<pad_t>();
    pad->index = static_cast<unsigned>(id.globalIndex);
    pad->profile = profile;
    pad->client_uuid = client_uuid;
    pad->ds5.back_as_touchpad = config::input.ds4_back_as_touchpad_click;
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
    pad->created_at = std::chrono::steady_clock::now();
    const auto create_ms = std::chrono::duration_cast<std::chrono::milliseconds>(pad->created_at - started).count();
    if (FAILED(result)) {
      if (pad->device) {
        context.api.close(pad->device);
        pad->device = nullptr;
      }
      BOOST_LOG(warning) << "xgpad: could not create virtual "sv << profile_name(profile) << " controller "sv << id.globalIndex << " ["sv
                         << util::hex(result).to_string_view() << "]; is the xgpad driver installed and Sunshine elevated?"sv;
      return -1;
    }

    BOOST_LOG(info) << "Gamepad "sv << id.globalIndex << " will be "sv << profile_name(profile) << " controller (xgpad, token "sv
                    << std::string(pad->token.begin(), pad->token.end()) << ", created in "sv << create_ms << " ms)"sv;
    pad_t *created = pad.get();
    {
      std::lock_guard lock {context.mutex};
      if (profile == profile_e::dualsense) {
        request_motion(*created);
      }
      context.pads[id.globalIndex] = std::move(pad);
    }
    SetEvent(context.wake_event);
    if (profile == profile_e::dualsense) {
      write_ds5_input(context, *created);  // hosts see the pad at rest, and the repeat starts
    }
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
    pad.sent_lightbar.reset();
    pad.sent_player_leds.reset();
    pad.sent_mic_led.reset();
    if (pad.profile == profile_e::dualsense) {
      request_motion(pad);
    }
    return 0;
  }

  void update(context_t &context, const int nr, const gamepad_state_t &state) {
    // alloc/free/update/touch/motion/battery all run on the input thread, so the slot cannot change underneath us.
    if (!has(&context, nr)) {
      return;
    }
    auto &pad = *context.pads[nr];
    check_driver_attached(pad);
    pad.state = state;
    if (pad.profile == profile_e::dualsense) {
      pad.ds5.gamepad = state;
      write_ds5_input(context, pad);
    } else {
      write_input(pad, make_input_report(state));
    }
  }

  void touch(context_t &context, const gamepad_touch_t &touch) {
    auto *pad = dualsense_pad(context, touch.id.globalIndex);
    if (!pad) {
      return;
    }
    if (!ds5_apply_touch(pad->ds5, pad->touch_slots, touch)) {
      BOOST_LOG(debug) << "xgpad: ignored touch event "sv << static_cast<unsigned>(touch.eventType) << " for pointer "sv << touch.pointerId
                       << " on pad "sv << pad->index;
      return;
    }
    write_ds5_input(context, *pad);
  }

  void motion(context_t &context, const gamepad_motion_t &motion) {
    auto *pad = dualsense_pad(context, motion.id.globalIndex);
    if (pad && ds5_apply_motion(pad->ds5, motion.motionType, motion.x, motion.y, motion.z)) {
      write_ds5_input(context, *pad);
    }
  }

  void battery(context_t &context, const gamepad_battery_t &battery) {
    if (auto *pad = dualsense_pad(context, battery.id.globalIndex)) {
      pad->ds5.status = ds5_battery_status(pad->ds5.status, battery.state, battery.percentage);
      write_ds5_input(context, *pad);
    }
  }

  void free(context_t &context, const int nr) {
    const auto pad = remove_pad(context, nr);
    if (!pad || pad->client_uuid.empty()) {
      return;
    }

    // A client's switch lasts until its last pad goes, normally when its stream ends
    std::lock_guard lock {context.mutex};
    const auto slots = occupied_slots(context);
    if (std::ranges::none_of(slots, [&pad](const pad_slot_t &slot) {
          return slot.client_uuid == pad->client_uuid;
        })) {
      context.session_profiles.erase(pad->client_uuid);
    }
  }

}  // namespace platf::xgpad
