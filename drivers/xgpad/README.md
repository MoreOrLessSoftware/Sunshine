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
| `scripts/xgpad-driver.ps1` (+ `.bat` wrappers) | Install and uninstall, as used by the Sunshine installer |
| `install-test.ps1` | Developer install/uninstall of a test build |
| `tools/xgpctl.cpp`, `tools/xgpdiag.cpp` | Test producer and shared-memory counter monitor |

## Developer builds

```powershell
.\build.ps1                    # signs with a reusable test certificate in CurrentUser\My
.\install-test.ps1             # elevated: trust the test certificate + pnputil /add-driver
.\out\xgpctl.exe 2 60          # elevated: create two virtual pads and play a test pattern
.\install-test.ps1 -Uninstall  # elevated: remove devices, driver package and certificate
```

## Installer builds

Configure Sunshine with `-DSUNSHINE_BUILD_XGPAD_DRIVER=ON`. This needs Visual Studio with the C++ tools
and PowerShell. The build then runs `build.ps1 -Release`, and CPack adds the optional
**Xbox Series Controller Driver** component, which is unchecked by default.

`-Release` signs each build with a fresh self-signed certificate and deletes that certificate, including
its private key, right after signing. Only the public `xgpad.cer` ships. When the component is selected,
the installer adds it to `LocalMachine\Root` and `LocalMachine\TrustedPublisher`. Because the private key
no longer exists, that trust cannot be used to sign anything else. Installing a newer release replaces
the older certificate and driver package, and uninstalling Sunshine removes both.
