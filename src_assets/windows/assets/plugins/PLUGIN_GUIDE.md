# Plugin guide
A plugin is a folder containing a `plugin.json` manifest and the PowerShell scripts it references. Plugins are
supported on Windows only.

## Developing a plugin
Sunshine loads custom plugins from the `plugins` folder inside its config directory (by default
`C:\Program Files\Sunshine\config\plugins`). Install and action scripts run as administrator, so only administrators
can change that folder. Keep it that way, and develop a plugin somewhere else:

1. Create a folder for the plugin that you can write to, such as `Documents\SunshinePlugins\my-plugin`.
2. Add a `plugin.json` and its scripts.
3. On the **Plugins** page, select *Import plugin* and enter the folder's full path. Sunshine checks the plugin and
   copies it into the `plugins` folder, leaving out anything whose name starts with a dot, such as `.git`.
4. Select the plugin in the sidebar, then *Install & Activate*.

After changing a plugin, import it again. It replaces the previous copy and keeps the plugin's installed state and
options. Changes to what the install script sets up need the plugin to be uninstalled and installed again. A custom
plugin cannot reuse the ID of a bundled plugin.

## Manifest

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
| `order`       | No       | Plugins with a lower order run first. Defaults to 1000. The bundled plugins use 50 to 500.                          |
| `elevated`    | No       | Run the do, undo, connect and disconnect scripts as administrator. Defaults to `false`. Install and uninstall always run as administrator. |
| `manages_display` | No   | `true` when the plugin creates or arranges displays itself. While it is active for an application, Sunshine skips its *Device configuration* setting when a stream starts or resumes. Defaults to `false`. |
| `scripts`     | No       | Any of `install`, `uninstall`, `do`, `undo`, `connect`, `disconnect` and `apply`. Each must be a `.ps1` file directly inside the plugin folder. See [Applying options](#applying-options) for `apply`. |
| `options`     | No       | Options shown in the web UI.                                                                                        |
| `actions`     | No       | Buttons shown on the plugin's card once it is installed. Each has an `id` (like an option ID), a `script` (a `.ps1` file directly inside the plugin folder), and optionally a `label` and `description`. Action scripts always run as administrator. |
| `panel`       | No       | A page shown on the plugin's card once it is installed: `file` (an `.html` file directly inside the plugin folder), and optionally `port` and `height` (in pixels, 100 to 2000, default 360). |
| `client_commands` | No   | Commands added to Moonlight's in-stream menu while the plugin is active; see [Client commands](#client-commands). |
| `icon`        | No       | Icon of the plugin's group of client commands in the menu, such as `material:extension` (a [Material Symbols](https://fonts.google.com/icons) name). |

Each option has an `id` (lowercase letters, digits and `_`, starting with a letter), a `type`, and optionally a
`label`, `description`, `default` and `apply`.

| Type      | Value                                    | Extra fields                                                                                  |
|-----------|------------------------------------------|-----------------------------------------------------------------------------------------------|
| `boolean` | `true` or `false`                        |                                                                                               |
| `number`  | A number                                 | `min`, `max` and `step`                                                                       |
| `string`  | Text                                     |                                                                                               |
| `choice`  | One of the listed values                 | `choices`: an array of strings, or of objects with a `value` and a `label`                    |

## Applying options
An option that needs more than a new value at the next stream, such as a driver setting, can set `"apply": true`.
The manifest must then have an `apply` script. When *Save options* changes such an option on an installed plugin,
the script runs as administrator with the new values in the [script environment](#script-environment).

Every new value of that save is stored only if the script exits with 0. If it fails, the old values stay and the
user can select *Save options* again, so the script should leave things as they were before it exits with an error.
Saves that change no such option don't run the script. An option changed before the plugin is installed doesn't
run it either, so the install script should read the option too.

## Client commands
A plugin can add commands that Moonlight clients show in their in-stream menu, which run on this host. They're offered while the plugin is installed and active and not turned off for the application being
streamed. By default a plugin's commands are on a page of their own in the menu, opened by a button named after the
plugin; a command with `"top_level": true` is shown with the menu's own commands instead.

| Field        | Required | Description                                                                                        |
|--------------|----------|----------------------------------------------------------------------------------------------------|
| `id`         | Yes      | Like an option ID, unique within the plugin.                                                       |
| `label`      | No       | The button's text. Defaults to `id`.                                                               |
| `icon`       | No       | The button's icon, such as `material:pause_circle`.                                                |
| `script`     | One of   | A `.ps1` file directly inside the plugin folder, run with the [script environment](#script-environment) in the plugin folder, without a console window, and left running. |
| `args`       | No       | Arguments passed to `script`.                                                                      |
| `steps`      | One of   | Key chords to type instead of running a script: an array of chords, each an array of Windows virtual-key codes, such as `[[162, 160, 27]]` for Ctrl+Shift+Esc. Each chord's keys are pressed in order, 50 ms apart, then released in reverse; the next chord starts 100 ms later. |
| `elevated`   | No       | Whether `script` runs as administrator. Defaults to the plugin's `elevated`.                       |
| `top_level`  | No       | Show the command with the menu's own commands rather than on the plugin's page. Defaults to `false`. |
| `close_menu` | No       | Whether the client closes its menu after running the command. Defaults to `true`.                  |
| `programs`   | No       | File names of programs, such as `["photoshop.exe"]`. The `.exe` is optional. The command is then only offered while one of them is in the foreground on this host when the client opens its menu. Windows only for now. |
| `status_file` | No      | A JSON file name in the plugin's data folder. While it exists, its `label` and `icon` replace the command's, so a script can show what the command will do next, such as *Resume* after a *Suspend*. Sunshine reads it each time a client opens its menu. |

Client commands only run while a stream is running, so they can rely on the plugin's do script having run.

## Panels
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

## Script environment
Scripts run with `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File <script>`. A script fails
when it exits with a non-zero code, including when it throws. Every script receives:

| Variable                     | Value                                                                                         |
|------------------------------|-----------------------------------------------------------------------------------------------|
| `SUNSHINE_PLUGIN_ID`         | The plugin ID.                                                                                |
| `SUNSHINE_PLUGIN_DIR`        | The plugin folder. `$PSScriptRoot` gives the same path.                                       |
| `SUNSHINE_PLUGIN_DATA_DIR`   | A folder for downloaded dependencies and saved state. It is deleted after a successful uninstall. |
| `SUNSHINE_PLUGIN_OPT_<ID>`   | The value of each option, with the option ID in upper case. Booleans are `true` or `false`.   |

The data folder is created when the plugin is installed, and only administrators can write to it by default. If a do
or undo script that is not elevated needs to write there, grant users access to it in the install script.

The do, undo, connect and disconnect scripts also receive these, which describe the application and the client that
most recently launched or resumed it:

| Variable                               | Value                                                                       |
|----------------------------------------|-----------------------------------------------------------------------------|
| `SUNSHINE_APP_ID`, `SUNSHINE_APP_NAME` | The application being streamed.                                             |
| `SUNSHINE_CLIENT_NAME`                 | The client's name.                                                          |
| `SUNSHINE_CLIENT_WIDTH`, `SUNSHINE_CLIENT_HEIGHT` | The client's resolution.                                         |
| `SUNSHINE_CLIENT_FPS`                  | The client's frame rate, as a whole number.                                 |
| `SUNSHINE_CLIENT_FPS_X100`, `SUNSHINE_CLIENT_FPS_X1000` | The frame rate times 100 or 1000, for fractional rates.    |
| `SUNSHINE_CLIENT_HDR`                  | `true` or `false`.                                                          |
| `SUNSHINE_CLIENT_GCMAP`                | The bitmap of connected gamepads.                                           |
| `SUNSHINE_CLIENT_HOST_AUDIO`           | `true` when audio plays on this host.                                       |
| `SUNSHINE_CLIENT_ENABLE_SOPS`          | `true` when the client asked for optimized game settings.                   |
| `SUNSHINE_CLIENT_AUDIO_CONFIGURATION`  | `2.0`, `5.1` or `7.1`.                                                      |
| `SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS` | The surround channel parameters.                                           |

A client command's script receives `SUNSHINE_CLIENT_COMMAND_ID`, `SUNSHINE_CLIENT_COMMAND_LABEL`,
`SUNSHINE_CLIENT_NAME`, `SUNSHINE_APP_ID` and `SUNSHINE_APP_NAME`.

## Logs
The output of install, uninstall, action, apply and client command scripts goes to `<id>.log` in the `plugin-data`
folder of the config directory, and *View log* on the plugin's page shows it. The output of do, undo, connect and
disconnect scripts isn't kept, so have them write to their own file if you need a record.

## Notes for coding agents
If you are an AI coding agent writing a plugin, follow these as well as the reference above.

* Work in a folder the user can write to, such as one under their Documents folder. Don't write to the Sunshine
  install folder, and don't change its permissions: that would let any account on the computer run code as
  administrator.
* You can't finish the install yourself. Tell the user the folder you built the plugin in, and that they need to
  select *Import plugin* on the **Plugins** page, enter that folder, and then select *Install & Activate*.
* Scripts run under Windows PowerShell 5.1, not PowerShell 7. Avoid syntax it doesn't have, such as `&&`, `||`, `?:`
  and `??`. Keep scripts to ASCII, or save them as UTF-8 with a BOM, because 5.1 misreads UTF-8 without one.
* Every script must be a `.ps1` file directly inside the plugin folder, including ones you dot-source for shared code.
* Run each script on its own before handing over. Set the variables it reads first, for example
  `$env:SUNSHINE_PLUGIN_DATA_DIR`, `$env:SUNSHINE_PLUGIN_OPT_LEVEL` and `$env:SUNSHINE_CLIENT_FPS`.
* A do script that exits with a non-zero code stops the application from starting, so catch failures in anything
  optional and carry on with a warning.
* Do scripts and undo scripts come in pairs. Undo must put back what do changed, and must be safe to run when do
  didn't finish or when there's nothing to undo.
* Only set `elevated` when the scripts need it. Do scripts that aren't elevated run as the signed-in user, which is
  what most things on the desktop need.
* Download dependencies in the install script. Pin an exact version and verify a SHA-256 checksum, and retry, since
  downloads can fail briefly.
* Don't assume a command is on `PATH` or what the working directory is. Build paths from `$PSScriptRoot` and
  `$env:SUNSHINE_PLUGIN_DATA_DIR`.
