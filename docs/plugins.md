# Plugins
Plugins automate setup that would otherwise be done with
[preparation commands](configuration.md#global_prep_cmd). A plugin bundles the scripts that run when a stream
starts and stops, a step that downloads whatever the scripts depend on, and a set of options you can change from
the web UI.

> [!NOTE]
> Plugins currently run PowerShell scripts and are only supported on Windows.

## Using plugins
Open the **Plugins** page in the web UI. Each plugin can be:

* **Installed** — *Install & Activate* runs the plugin's install script, which downloads its dependencies, and
  activates the plugin once the script succeeds. The output is shown under *View log*.
* **Activated** — an active plugin runs for every application. Use the *Active* switch to turn it off or back on
  without uninstalling. To skip a plugin for one application, uncheck it under *Plugins* when editing that
  application, or select *Apps & plugins* to see every application against every active plugin. In that grid,
  the checkbox next to an application or under a plugin changes the whole row or column.
* **Configured** — change the plugin's options and select *Save options*. The new values apply to the next stream.
  Some options take effect right away by running a script as administrator. If that script fails, the card shows the
  failure, nothing is saved, and you can select *Save options* again once the problem is fixed.
* **Tested** — some plugins add buttons to their card, such as *Start test*, and a live view below them. A button
  runs one of the plugin's scripts right away, as administrator; its output is shown under *View log*.
* **Uninstalled** — deactivates the plugin, runs its uninstall script, and deletes the files it downloaded.

Active plugins run before the global and application preparation commands, in plugin order. Their undo scripts
run after the preparation commands are undone, in reverse order. If a plugin's do script fails, the application is
not started, just as with a failing preparation command.

A plugin can also have connect and disconnect scripts, which run like preparation commands with the `connect`
type: the connect script runs when a client resumes the running application, and the disconnect script runs when
the last client leaves it running. They see the frame rate and other `SUNSHINE_CLIENT_*` values of the client that
is resuming.

## Bundled plugins

| Plugin           | What it does                                                                                                                                                                                                                                          |
|------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Virtual Display  | Creates a virtual display matching the client's resolution and frame rate using [sunshine-vdisplay](https://github.com/Zayrick/sunshine-vdisplay), and removes it when the stream ends. The refresh rate can be a multiple of the client frame rate. Each client gets its own virtual monitor unless *Shared virtual monitor for all clients* is on. *Start the stream on a new desktop* creates a temporary Windows desktop first using the [VirtualDesktop](https://github.com/MScholtes/PSVirtualDesktop) PowerShell module, and removes it when the stream ends. If you installed this plugin before that option existed, uninstall and reinstall it to add the module, and turn off *Remove the driver when uninstalling* first to keep the driver. *10-bit SDR* sets the driver's `sdrBits` registry value and turns on Windows' Auto Color Management for the virtual monitor at the start of each stream, since Windows otherwise keeps composing the desktop in 8 bits. Saving a change reloads the driver, so end any stream first. Turning the option off leaves Auto Color Management as it is on monitors that already have it. |
| RTSS Frame Limit | Sets the [RivaTuner Statistics Server](https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/) frame rate limit to the client frame rate and restores the previous limit afterwards. RTSS 7.3.7 or newer must already be installed. If RTSS isn't running, it is started for the stream and closed afterwards. The limit is set again when a client resumes, using that client's frame rate, unless *Re-apply the limit when a client resumes* is off. |
| Nyrna            | Adds a *Suspend* command to the [client commands](client_commands.md) in Moonlight's in-stream menu. It suspends the game or application in the foreground with [Nyrna](https://nyrna.merritt.codes/). While it's suspended, a notification on this PC shows a picture of it with a *Resume* button, and the command becomes *Resume (name)*, which you can change with the *Resume command label* option. If Nyrna is already running here (installed, with its tray icon), the command presses its hotkey, set in the plugin's options. Nyrna runs through scheduled tasks in the *Sunshine* folder of Task Scheduler, as the signed-in user with full rights, so it can also resume games that run as administrator. Games with anti-cheat usually can't be suspended. |
| Stadia Controller | Runs the [Stadiacast](https://github.com/MoreOrLessSoftware/stadiacast) receiver while a stream is running, so a Stadia Controller in Wi-Fi mode becomes a virtual Xbox Series controller (with the *Xbox Series Controller Driver* component) or Xbox 360 controller (with ViGEmBus). Wake the controller once the stream has started. *Start test* runs the receiver without a stream, and the card shows the controller connecting and every button, stick and trigger live. The controller must use this PC as its DNS server; see the [Stadiacast setup guide](https://github.com/MoreOrLessSoftware/stadiacast/blob/main/docs/setup.md). |
| V-Sync Toggle    | Changes the graphics driver's global V-Sync setting and restores the previous setting afterwards. On NVIDIA GPUs it uses [vsync-toggle](https://github.com/xanderfrangos/vsync-toggle). On AMD GPUs it sets the driver's *Wait for Vertical Refresh* setting, which applies to games started after the change. |

Using *Virtual Display* with a refresh rate multiplier together with *RTSS Frame Limit* runs the display faster than
the stream while keeping games capped at the client frame rate.

While *Virtual Display* runs for an application, Sunshine skips its *Device configuration* setting (Audio/Video) for
that stream, so the plugin alone sets up the displays. Applications that exclude the plugin use *Device configuration*
as usual.

## Writing a plugin
The [plugin guide](../src_assets/windows/assets/plugins/PLUGIN_GUIDE.md) explains how to build your own, step by step and for coding agents, and describes everything a plugin can do. Sunshine ships with it, under *Plugin guide* on the **Plugins** page.

<div class="section_buttons">

| Previous                        |                                    Next |
|:--------------------------------|----------------------------------------:|
| [App Examples](app_examples.md) | [Awesome-Sunshine](awesome_sunshine.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
