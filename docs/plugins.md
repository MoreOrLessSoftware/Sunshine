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
| Virtual Display  | Creates a virtual display matching the client's resolution and frame rate using [sunshine-vdisplay](https://github.com/Zayrick/sunshine-vdisplay), and removes it when the stream ends. The refresh rate can be a multiple of the client frame rate. Each client gets its own virtual monitor unless *Shared virtual monitor for all clients* is on. *Start the stream on a new desktop* creates a temporary Windows desktop first using the [VirtualDesktop](https://github.com/MScholtes/PSVirtualDesktop) PowerShell module, and removes it when the stream ends. If you installed this plugin before that option existed, uninstall and reinstall it to add the module, and turn off *Remove the driver when uninstalling* first to keep the driver. |
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
A plugin is a folder containing a `plugin.json` manifest and the PowerShell scripts it references. Put custom plugins
in the `plugins` folder inside the Sunshine config directory (by default `C:\Program Files\Sunshine\config\plugins`),
then select *Rescan*. A custom plugin cannot reuse the ID of a bundled plugin.

### Manifest

```json
{
  "id": "my-plugin",
  "name": "My Plugin",
  "version": "1.0.0",
  "description": "What the plugin does.",
  "author": "Me",
  "homepage": "https://example.com",
  "order": 500,
  "elevated": false,
  "scripts": {
    "install": "install.ps1",
    "uninstall": "uninstall.ps1",
    "do": "do.ps1",
    "undo": "undo.ps1"
  },
  "actions": [
    {
      "id": "test",
      "label": "Run a test",
      "description": "Shown as the button's tooltip.",
      "script": "test.ps1"
    }
  ],
  "panel": {
    "file": "panel.html",
    "port": 8008,
    "height": 360
  },
  "icon": "material:extension",
  "client_commands": [
    {
      "id": "toggle",
      "label": "Toggle Something",
      "icon": "material:toggle_on",
      "top_level": true,
      "script": "toggle.ps1",
      "args": ["on"]
    }
  ],
  "options": [
    {
      "id": "level",
      "type": "number",
      "label": "Level",
      "description": "Shown under the field in the web UI.",
      "default": 1,
      "min": 0,
      "max": 10,
      "step": 1
    }
  ]
}
```

| Field         | Required | Description                                                                                                         |
|---------------|----------|---------------------------------------------------------------------------------------------------------------------|
| `id`          | Yes      | 1 to 64 lowercase letters, digits, `.`, `_` or `-`, starting with a letter or digit.                                |
| `name`        | No       | Display name. Defaults to `id`.                                                                                     |
| `version`     | No       | Shown in the web UI.                                                                                                |
| `description` | No       | Shown in the web UI.                                                                                                |
| `author`      | No       | Shown in the web UI.                                                                                                |
| `homepage`    | No       | Linked from the web UI.                                                                                             |
| `order`       | No       | Plugins with a lower order run first. Defaults to 1000. The bundled plugins use 50, 100, 200, 300.                  |
| `elevated`    | No       | Run the do, undo, connect and disconnect scripts as administrator. Defaults to `false`. Install and uninstall always run as administrator. |
| `manages_display` | No   | `true` when the plugin creates or arranges displays itself. While it is active for an application, Sunshine skips its *Device configuration* setting when a stream starts or resumes. Defaults to `false`. |
| `scripts`     | No       | Any of `install`, `uninstall`, `do`, `undo`, `connect` and `disconnect`. Each must be a `.ps1` file directly inside the plugin folder. |
| `options`     | No       | Options shown in the web UI.                                                                                        |
| `actions`     | No       | Buttons shown on the plugin's card once it is installed. Each has an `id` (like an option ID), a `script` (a `.ps1` file directly inside the plugin folder), and optionally a `label` and `description`. Action scripts always run as administrator. |
| `panel`       | No       | A page shown on the plugin's card once it is installed: `file` (an `.html` file directly inside the plugin folder), and optionally `port` and `height` (in pixels, 100 to 2000, default 360). |
| `client_commands` | No   | Commands added to Moonlight's in-stream menu while the plugin is active; see [Client commands](#client-commands). |
| `icon`        | No       | Icon of the plugin's group of client commands in the menu, such as `material:extension` (a [Material Symbols](https://fonts.google.com/icons) name). |

Each option has an `id` (lowercase letters, digits and `_`, starting with a letter), a `type`, and optionally a
`label`, `description` and `default`.

| Type      | Value                                    | Extra fields                                                                                  |
|-----------|------------------------------------------|-----------------------------------------------------------------------------------------------|
| `boolean` | `true` or `false`                        |                                                                                               |
| `number`  | A number                                 | `min`, `max` and `step`                                                                       |
| `string`  | Text                                     |                                                                                               |
| `choice`  | One of the listed values                 | `choices`: an array of strings, or of objects with a `value` and a `label`                    |

### Client commands
A plugin can add commands to the [client commands](client_commands.md) that Moonlight clients show in their in-stream
menu. They're offered while the plugin is installed and active and not turned off for the application being
streamed. By default a plugin's commands are on a page of their own in the menu, opened by a button named after the
plugin; a command with `"top_level": true` is shown with the menu's own commands instead.

| Field        | Required | Description                                                                                        |
|--------------|----------|----------------------------------------------------------------------------------------------------|
| `id`         | Yes      | Like an option ID, unique within the plugin.                                                       |
| `label`      | No       | The button's text. Defaults to `id`.                                                               |
| `icon`       | No       | The button's icon, such as `material:pause_circle`.                                                |
| `script`     | One of   | A `.ps1` file directly inside the plugin folder, run with the [script environment](#script-environment) in the plugin folder, without a console window, and left running. |
| `args`       | No       | Arguments passed to `script`.                                                                      |
| `steps`      | One of   | Key chords to type instead of running a script, as in [client commands](client_commands.md#files). |
| `elevated`   | No       | Whether `script` runs as administrator. Defaults to the plugin's `elevated`.                       |
| `top_level`  | No       | Show the command with the menu's own commands rather than on the plugin's page. Defaults to `false`. |
| `close_menu` | No       | Whether the client closes its menu after running the command. Defaults to `true`.                  |
| `status_file` | No      | A JSON file name in the plugin's data folder. While it exists, its `label` and `icon` replace the command's, so a script can show what the command will do next, such as *Resume* after a *Suspend*. Sunshine reads it each time a client opens its menu. |

Client commands only run while a stream is running, so they can rely on the plugin's do script having run.

### Panels
A panel is an HTML page from the plugin folder that the web UI shows in a frame on the plugin's card, for example a
live view of a service the plugin starts. It is served from `/api/plugins/panel?id=<id>` and can only be framed by
Sunshine's own pages.

A panel cannot reach local services directly, since the web UI is served over HTTPS. When the manifest sets a
`port`, the panel can read from that port on this computer through Sunshine instead:

```js
const response = await fetch('./proxy?id=my-plugin&path=' + encodeURIComponent('/status'));
```

Sunshine forwards the GET request to `http://127.0.0.1:<port>/status` and returns the response, or answers
`502 Bad Gateway` when nothing is listening. Only GET requests are forwarded, only to the declared port.

To receive updates as they happen instead of polling, have the service send
[server-sent events](https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events) and add `stream=1`:

```js
const events = new EventSource('./proxy?id=my-plugin&stream=1&path=' + encodeURIComponent('/events'));
events.addEventListener('message', (event) => console.log(event.data));
```

A `text/event-stream` response is passed on as it arrives. Sunshine ends the stream after a few minutes, and
`EventSource` reconnects on its own; a service can send `retry: 250` to make that reconnect quick.

### Script environment
Scripts run with `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File <script>`. A script fails
when it exits with a non-zero code, including when it throws. Besides the `SUNSHINE_*` variables described under
[global_prep_cmd](configuration.md#global_prep_cmd), every script receives:

| Variable                     | Value                                                                                         |
|------------------------------|-----------------------------------------------------------------------------------------------|
| `SUNSHINE_PLUGIN_ID`         | The plugin ID.                                                                                |
| `SUNSHINE_PLUGIN_DIR`        | The plugin folder. `$PSScriptRoot` gives the same path.                                       |
| `SUNSHINE_PLUGIN_DATA_DIR`   | A folder for downloaded dependencies and saved state. It is deleted after a successful uninstall. |
| `SUNSHINE_PLUGIN_OPT_<ID>`   | The value of each option, with the option ID in upper case. Booleans are `true` or `false`.   |

> [!TIP]
> The data folder is created by the install script, which runs as administrator. If a do or undo script that is
> not elevated needs to write there, grant users access to it during install.

<div class="section_buttons">

| Previous                        |                                    Next |
|:--------------------------------|----------------------------------------:|
| [App Examples](app_examples.md) | [Awesome-Sunshine](awesome_sunshine.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
