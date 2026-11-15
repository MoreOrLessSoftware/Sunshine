# xgpad: virtual Xbox Series X|S controller driver

A UMDF2 HID minidriver that presents an Xbox Series X|S controller (Bluetooth LE report layout,
`045E:0B13`) underneath the inbox `xinputhid` filter. XInput, Windows.Gaming.Input and GameInput see a real
Series controller, including Guide, Share (capture) and impulse trigger rumble. No kernel-mode code and no
Microsoft signature are involved.

Sunshine creates one software device per client controller (`gamepad_driver = xgpad`) and exchanges HID
reports with the driver through shared memory (`xgpad_shared.h`).

## Layout

| Path | Purpose |
|---|---|
| `driver/xgpad.c`, `driver/xgpad.inf` | UMDF2 driver and INF (stack: xinputhid → mshidumdf → WUDFRd → SWD) |
| `xgpad_shared.h` | Shared-memory contract with Sunshine (`src/platform/windows/xgpad.cpp`) |
| `build.ps1` | Builds and signs the package with MSVC and the NuGet WDK (cached in `.wdk/`) |
| `src_assets/windows/assets/plugins/xgpad/` | The xgpad plugin: installing it installs the driver, uninstalling it removes the driver (`xgpad-driver.ps1`) |
| `scripts/uninstall-xgpad-driver.bat` | Removes the driver when Sunshine is uninstalled |
| `install-test.ps1` | Developer install/uninstall of a test build |
| `tools/xgpctl.cpp`, `tools/xgpdiag.cpp` | Test producer and shared-memory counter monitor |

## Developer builds

```powershell
.\build.ps1                    # signs with a reusable test certificate in CurrentUser\My
.\install-test.ps1             # elevated: trust the test certificate + pnputil /add-driver
.\out\xgpctl.exe 2 60          # elevated: create two virtual pads and play a test pattern
.\install-test.ps1 -Uninstall  # elevated: remove devices, driver package and certificate
```

## Release builds

Configure Sunshine with `-DSUNSHINE_BUILD_XGPAD_DRIVER=ON`. This needs Visual Studio with the C++ tools
and PowerShell. The build then runs `build.ps1 -Release` and puts the package in the xgpad plugin's
`driver` folder. Without the option, the plugin is left out.

`-Release` signs each build with a fresh self-signed certificate and deletes that certificate, including
its private key, right after signing. Only the public `xgpad.cer` ships. Installing the plugin adds it to
`LocalMachine\Root` and `LocalMachine\TrustedPublisher`. Because the private key
no longer exists, that trust cannot be used to sign anything else. Reinstalling the plugin replaces
an older certificate and driver package, and uninstalling the plugin or Sunshine removes both. Bundled plugins
are reinstalled automatically only when their `version` changes, so bump the `version` in the plugin's
`plugin.json` when the driver or `xgpad_shared.h` changes.
