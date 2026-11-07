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
| Virtual Display  | Creates a virtual display matching the client's resolution and frame rate using [sunshine-vdisplay](https://github.com/Zayrick/sunshine-vdisplay), and removes it when the stream ends. The refresh rate can be a multiple of the client frame rate. Each client gets its own virtual monitor unless *Shared virtual monitor for all clients* is on. |
| RTSS Frame Limit | Sets the [RivaTuner Statistics Server](https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/) frame rate limit to the client frame rate and restores the previous limit afterwards. RTSS 7.3.7 or newer must already be installed. If RTSS isn't running, it is started for the stream and closed afterwards. The limit is set again when a client resumes, using that client's frame rate, unless *Re-apply the limit when a client resumes* is off. |
| V-Sync Toggle    | Changes the graphics driver's global V-Sync setting and restores the previous setting afterwards. On NVIDIA GPUs it uses [vsync-toggle](https://github.com/xanderfrangos/vsync-toggle). On AMD GPUs it sets the driver's *Wait for Vertical Refresh* setting, which applies to games started after the change. |

Using *Virtual Display* with a refresh rate multiplier together with *RTSS Frame Limit* runs the display faster than
the stream while keeping games capped at the client frame rate.

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
| `order`       | No       | Plugins with a lower order run first. Defaults to 1000. The bundled plugins use 100, 200 and 300.                   |
| `elevated`    | No       | Run the do, undo, connect and disconnect scripts as administrator. Defaults to `false`. Install and uninstall always run as administrator. |
| `scripts`     | No       | Any of `install`, `uninstall`, `do`, `undo`, `connect` and `disconnect`. Each must be a `.ps1` file directly inside the plugin folder. |
| `options`     | No       | Options shown in the web UI.                                                                                        |

Each option has an `id` (lowercase letters, digits and `_`, starting with a letter), a `type`, and optionally a
`label`, `description` and `default`.

| Type      | Value                                    | Extra fields                                                                                  |
|-----------|------------------------------------------|-----------------------------------------------------------------------------------------------|
| `boolean` | `true` or `false`                        |                                                                                               |
| `number`  | A number                                 | `min`, `max` and `step`                                                                       |
| `string`  | Text                                     |                                                                                               |
| `choice`  | One of the listed values                 | `choices`: an array of strings, or of objects with a `value` and a `label`                    |

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
