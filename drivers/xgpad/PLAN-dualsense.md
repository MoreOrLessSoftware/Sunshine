# Plan: DualSense profile for xgpad

Goal: the xgpad driver presents either an Xbox Series pad (unchanged) or a USB DualSense (`054C:0CE6`), chosen per pad.
PlayStation clients and clients with gyro or a touchpad (DS5, DS4, Switch Pro, Joy-Con) get a DualSense with motion,
touchpad, lightbar, player LEDs, rumble and adaptive triggers; everything else stays Xbox. Clients can switch their
controllers between the two from the in-stream menu. One driver, one plugin, one certificate.

## 1. Shared contract (`xgpad_shared.h`)

- `XGPAD_PROFILE_PROPKEY` (same GUID, pid 3, `DEVPROP_TYPE_UINT32`): `XGPAD_PROFILE_XBOX_SERIES = 0`,
  `XGPAD_PROFILE_DUALSENSE = 1`. Read in DeviceAdd (the report descriptor is requested before the section is
  guaranteed to exist). The producer also writes `profile` into `XGPAD_SHARED`; the driver refuses a mismatch.
- Input ring of generic entries `XGPAD_REPORT { id; length; data[64] }` (63-byte DualSense `0x01`, 16-byte Xbox).
- Output ring (16 entries, `outputCount`) instead of the single seqlock slot: DualSense hosts send several `0x02`
  reports with different valid flags in a row, and a latest-only slot would lose lightbar/trigger updates.
- `XGPAD_SWD_ENUMERATOR_DUALSENSE = L"VID_054C&PID_0CE6&REV_0100"`, DualSense payload structs.
- `XGPAD_SHARED_VERSION` 4.

## 2. Driver (`driver/xgpad.c`, `xgpad.inf`)

- Per-profile description: report descriptor, VID/PID/version, input/output report IDs and sizes, strings, feature
  replies.
- DualSense: USB report descriptor (273 bytes; verify against a real pad's dump); feature reports via
  `IOCTL_UMDF_HID_GET_FEATURE`: `0x05` calibration chosen so that 16 LSB = 1 deg/s and 8192 LSB = 1 g, `0x09` pairing
  info with a MAC derived from the token, `0x20` firmware info; other declared feature IDs answer zeros; `SET_FEATURE`
  accepted and ignored. Serial number string = the MAC.
- DualSense reports are never merged (motion samples need their own entries); Xbox keeps the digital-change merge.
- INF: `SunshineXgpad\VID_054C&PID_0CE6` uses an `XgpadDs` install section without the `xinputhid` filter.
  `DriverVer` 0.4.0.0.
- `xgpctl --ds5` test pattern (buttons, sticks, gyro sweep, touch drag; prints output reports); `xgpdiag` shows the
  profile and output counters.

## 3. Sunshine runtime

- `xgpad_ds5_report.h` (pure, unit tested): `make_ds5_input_report(state)` (sticks, triggers, counter, hat, buttons
  incl. Create/Options/PS/touchpad/mic, gyro/accel, QPC sensor timestamp, two touch points on 1920x1080, battery);
  `parse_ds5_output(span)` -> rumble, adaptive triggers (`make_adaptive_triggers(flags0 & 0x0C, ...)`), lightbar,
  player LEDs (via virtualhid's `player_led_mask`).
- Shared helpers for DS4/DS5 motion scaling and touch pointer slots (ViGEm DS4 code moves onto them).
- `xgpad::alloc(context, id, profile, queue)`, `xgpad::touch/motion/battery`, output ring drained per profile,
  motion requested on alloc/rebind for DualSense pads.
- `driver_supports_dualsense()` (installed package has the DualSense hardware ID); old driver -> today's fallback.
- `choose_xgpad_profile(choice) -> optional<profile>` replaces `should_allocate_xgpad`:

| Policy / `gamepad`                           | Result                                                  |
|----------------------------------------------|---------------------------------------------------------|
| `xgpad`, `ds5`/`ds4`                         | DualSense (Xbox on an old driver)                       |
| `xgpad`, `auto`, PlayStation-style client    | DualSense (change: Xbox today)                          |
| `all`, `auto`, PlayStation-style client      | xgpad DualSense when supported, else ViGEm DS4          |
| `all`, explicit `ds4`                        | ViGEm DS4                                               |
| Xbox profiles / Xbox or generic client       | Xbox Series (unchanged)                                 |
| virtualhid usable                            | unchanged (virtualhid wins)                             |

  "PlayStation-style" = `LI_CTYPE_PS`, or `motion_as_ds4` with gyro/accel, or `touchpad_as_ds4` with a touchpad.
- `input.cpp`: profile passed through; touch/motion/battery dispatched to xgpad instead of dropped.
- `en` locale and docs: `gamepad_driver_desc`, `gamepad_driver_xgpad`, `rec_xgpad_*`, motion/touchpad option text.

## 4. Switching from the client

- Core: client command `type_e::action` with `action` + `args`; manifest `"action"` only for bundled plugins and
  registered names. `client_commands::register_action(name, handler, status)`; handler gets `run_context_t` + args
  and returns ok or a message; status callback can relabel/hide a command when the menu opens.
- Core: `plugins::resolved_options(id, app_overrides, client_uuid)`; `stream_env` builds on it.
- xgpad plugin option `controller` (`automatic` / `xbox` / `dualsense`, overridable per app and client) = the profile a
  session starts with; it wins over `choose_xgpad_profile` unless `automatic`.
- `xgpad::set_profile(context, client_uuid, profile)`: per-client session override (cleared when the app quits, also
  used for pads plugged in later). For each of the client's xgpad slots with another profile: keep state + feedback
  queue, close the device, recreate it with the new profile and a fresh token, publish neutral then the kept state,
  re-request motion (DualSense) or stop rumble (Xbox). Runs on the input thread via `platf::gamepad_set_profile`.
  Pure helpers: `plan_profile_switch(...)`, profile precedence (session override > plugin option > automatic).
- Commands on a *Controller* page: **Xbox (XInput)** and **DualSense**, the active one with a check icon, hidden when
  the client has no xgpad pads or the driver predates DualSense.

## 5. Plugin (`src_assets/windows/assets/plugins/xgpad/`)

- `plugin.json` 1.1.0 (bundled plugin reinstalls and replaces the 0.3 package), name "Virtual Controller Driver
  (Xbox Series & DualSense)", new description (XInput-only games see a DualSense only through Steam Input; switching
  is a disconnect + connect), `icon`, `client_commands`, `controller` option. `id` stays `xgpad`.
- `xgpad-driver.ps1` and `xgpad_instance_prefixes`: add `SWD\VID_054C&PID_0CE6&REV_0100\` and
  `HID\VID_054C&PID_0CE6&REV_0100` (never a bare `HID\VID_054C&PID_0CE6`, which would match a real pad).

## 6. Tests

- `test_xgpad.cpp`: profile choice table, instance prefixes (a real DualSense never matches), shared ring helpers,
  `plan_profile_switch`, precedence.
- `test_xgpad_ds5.cpp`: buttons/hat, axes, motion scaling and clamping, touch slots, battery, counter/timestamp,
  output parsing per flag and for short reports, struct layout.
- `test_plugins.cpp`, `test_client_commands.cpp`, `sunshine-setup.Tests.ps1`: actions, resolved options, manifest.

## 7. Manual checks

1. SDL3 `testcontroller`: hidapi sees an unknown bus type on an SWD parent; confirm USB handling, feature reports,
   gyro, touchpad, lightbar. Riskiest item; prototype first.
2. Steam Input: DualSense detected, gyro test, PlayStation layout for an XInput-only game.
3. A game with native DualSense support: adaptive triggers reach a DS5 client, lightbar a DS4/DS5 client.
4. Clients: DS5, DS4, Switch Pro (gyro, Capture -> mic) -> DualSense; Xbox -> Xbox Series. Switching both ways.
5. Four mixed pads, disconnect/resume, plugin uninstall leaves no phantom devices; upgrade from plugin 1.0.0.

## Commits

1. `feat(xgpad): profile-aware shared contract and a DualSense driver profile` (header, driver, INF, tools, minimal
   Sunshine adaptation to the new rings)
2. `refactor(input): share DS4/DS5 motion scaling and touch slots`
3. `feat(xgpad): DualSense reports, feedback and profile selection`
4. `feat(client-commands): actions handled by Sunshine for bundled plugins`
5. `feat(xgpad): switch a client's controllers between Xbox and DualSense`
6. `feat(plugins): xgpad plugin 1.1.0`
