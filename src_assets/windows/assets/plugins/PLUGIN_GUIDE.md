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
options. If the new copy has a different `version`, the plugin shows *Reinstall needed* and sits out of streams until
you select *Reinstall*, which runs its install script again. So change the version whenever what the install script
sets up changes. *Reinstall* is also there for any installed plugin. A custom plugin cannot reuse the ID of a bundled
plugin.

## Publishing a plugin
Others can import a published plugin by entering a URL under *Import plugin* instead of a folder. Sunshine downloads
the plugin's update feed, a `latest.json` file like this:

```json
{
  "version": "1.0.0",
  "url": "my-plugin-1.0.0.zip",
  "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
}
```

| Field     | Description                                                                                         |
|-----------|-----------------------------------------------------------------------------------------------------|
| `version` | The plugin's version, which must match the `version` in the zip file's `plugin.json`.               |
| `url`     | The zip file holding the plugin: an `https://` address, or a path relative to the feed's address.   |
| `sha256`  | The zip file's SHA-256 checksum, as 64 hex digits. Sunshine refuses a zip file that doesn't match.  |

The zip file holds `plugin.json` at its top level, or inside a single top-level folder. It can be up to 20 MB, and
its entries must be stored or deflated, without encryption. Both files must be served over HTTPS.

On GitHub, attach `latest.json` and the zip file to a release. Users can then enter the repository's address, such as
`https://github.com/me/my-plugin`: Sunshine reads `latest.json` from the latest release, and a relative `url` points to
that release's zip file. This builds both files from the plugin's folder:

```powershell
$version = (Get-Content .\my-plugin\plugin.json -Raw | ConvertFrom-Json).version
$zip = "my-plugin-$version.zip"
Compress-Archive -Path .\my-plugin\* -DestinationPath $zip -Force
$sha256 = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
@{version = $version; url = $zip; sha256 = $sha256} | ConvertTo-Json | Set-Content latest.json
```

Sunshine remembers the address a plugin was imported from, and shows it on the plugin's page. *Check for updates* on
the **Plugins** page reads the feed of every plugin imported from a URL, and a plugin whose feed has a different
`version` offers *Update to* that version, which downloads it like an import. So publish a new release, with a new
`version`, whenever you change the plugin. An installed plugin then needs *Reinstall*, as with any new version.

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
| `version`     | No       | Shown in the web UI. When it changes, an installed plugin needs its install script to run again; see [Notes for coding agents](#notes-for-coding-agents). |
| `description` | No       | Shown in the web UI. A blank line (`\n\n`) starts a new paragraph.                                                  |
| `author`      | No       | Shown in the web UI.                                                                                                |
| `homepage`    | No       | Linked from the web UI.                                                                                             |
| `notice`      | No       | Something users must know or do before the plugin works, shown in a box on its page even before it is installed. |
| `notice_url`  | No       | An `https://` page with more about `notice`, linked after it.                                                      |
| `order`       | No       | Plugins with a lower order run first. Defaults to 1000. The bundled plugins use 50 to 500.                          |
| `elevated`    | No       | Run the do, undo, connect and disconnect scripts as administrator. Defaults to `false`. Install and uninstall always run as administrator. |
| `manages_display` | No   | `true` when the plugin creates or arranges displays itself. While it is active for an application, Sunshine skips its *Device configuration* setting when a stream starts or resumes. When several such plugins are active for an application, only the first in plugin order runs, and activating one turns it off for the applications another such plugin already serves. Defaults to `false`. |
| `idle_only`   | No       | `true` when installing or uninstalling the plugin must wait until no stream is running, such as for a plugin that installs a driver a stream may be using. Defaults to `false`. |
| `restart_after_change` | No | `true` when Sunshine must restart after the plugin is installed or uninstalled for the change to take effect, such as for a driver Sunshine looks for when it starts. The web UI then offers to restart Sunshine. Defaults to `false`. |
| `default_active` | No    | `true` for a bundled plugin that Sunshine installs and activates the first time it starts with it. A user who uninstalls it keeps it uninstalled, and an install that fails isn't tried again. Custom plugins ignore it. Defaults to `false`. |
| `scripts`     | No       | Any of `install`, `uninstall`, `do`, `undo`, `connect`, `disconnect`, `apply` and `status`. Each must be a `.ps1` file directly inside the plugin folder. See [Applying options](#applying-options) for `apply` and [Status and notices](#status-and-notices) for `status`. |
| `options`     | No       | Options shown in the web UI.                                                                                        |
| `actions`     | No       | Buttons shown on the plugin's card once it is installed. Each has an `id` (like an option ID), a `script` (a `.ps1` file directly inside the plugin folder), and optionally a `label` and `description`. Action scripts always run as administrator. |
| `panel`       | No       | A page shown on the plugin's card once it is installed: `file` (an `.html` file directly inside the plugin folder), and optionally `port` and `height`: in pixels from 100 to 2000 (default 360), or `"auto"` to grow and shrink with the page's content, which then shouldn't set its own height to fill the frame. |
| `client_commands` | No   | Commands added to Moonlight's in-stream menu while the plugin is active; see [Client commands](#client-commands). |
| `icon`        | No       | Icon of the plugin's group of client commands in the menu, such as `material:extension` (a [Material Symbols](https://fonts.google.com/icons) name). |

Each option has an `id` (lowercase letters, digits and `_`, starting with a letter), a `type`, and optionally a
`label`, `description`, `default`, `apply` and `scope`.

| Type      | Value                                    | Extra fields                                                                                  |
|-----------|------------------------------------------|-----------------------------------------------------------------------------------------------|
| `boolean` | `true` or `false`                        |                                                                                               |
| `number`  | A number                                 | `min`, `max` and `step`                                                                       |
| `string`  | Text                                     |                                                                                               |
| `choice`  | One of the listed values                 | `choices`: an array of strings, or of objects with a `value`, a `label` and a `group`; and `choices_script` |

A choice option can list more values when the web UI shows it, such as the devices or files on this host, by naming a
`choices_script` (a `.ps1` file directly inside the plugin folder). See [Listing choices](#listing-choices).

## Listing choices
When the web UI shows a choice option with a `choices_script`, Sunshine runs the script as the signed-in user,
without elevation, and the select offers the option's `choices` followed by the values the script lists. A refresh
button next to the select runs the script again. The plugin must be installed first.

The script gets the [script environment](#script-environment) with the plugin's own option values, and two more
variables: `SUNSHINE_PLUGIN_CHOICE_OPTION`, the ID of the option, so one script can serve several options, and
`SUNSHINE_PLUGIN_CHOICES_FILE`, the file to write the list to. The list is a JSON array in the same form as
`choices`; choices with a `group` are listed under it, after those without one. A value can't be empty, longer than
512 bytes or hold control characters, and the list can't hold more than 1000 choices or 256 KiB. The script has 15
seconds to exit with 0, and its output goes to `.choices.log` in its data folder. The list file is in the data
folder too, which only administrators can write to by default, so grant users access to it in the install script:

```powershell
& icacls.exe $env:SUNSHINE_PLUGIN_DATA_DIR /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
```

```powershell
$choices = Get-ChildItem "$env:windir\System32\spool\drivers\color" -Filter *.icc |
    ForEach-Object { @{ value = $_.Name; label = $_.BaseName; group = 'Installed' } }
ConvertTo-Json -InputObject @($choices) -Compress | Set-Content -LiteralPath $env:SUNSHINE_PLUGIN_CHOICES_FILE -Encoding UTF8
```

`choices` must still list at least one value, which the option uses until the list is in and when the script lists
nothing; something like "Don't change" works well. Since the script's list can change, the option accepts any value
the script could list, and a saved value the script no longer lists stays selected, marked as not found. The do and
undo scripts should handle such a value, for example by logging a warning and leaving things as they are.

## Status and notices
A plugin can report its state, such as the version of a driver it manages, with a `status` script. Sunshine runs it
when the plugin's panel asks for `/api/plugins/status?id=<id>` and, for every installed and active plugin, when the home
page loads. Like a [choices script](#listing-choices), it runs as the signed-in user, without elevation, with the
[script environment](#script-environment) and the plugin's own option values. It gets one more variable,
`SUNSHINE_PLUGIN_STATUS_FILE`, the file to write the status to as a JSON object. It has 15 seconds to exit with 0,
the file can't be larger than 256 KiB, and its output goes to `.status.log` in its data folder. The status file is in
the data folder too, so grant users access to it in the install script as for choices scripts.

The object can hold anything the panel needs. Its `notices`, an array of at most 10, are shown on the home page:

| Field    | Required | Description                                                                                      |
|----------|----------|--------------------------------------------------------------------------------------------------|
| `title`  | Yes      | Up to 200 bytes.                                                                                 |
| `text`   | No       | Up to 2000 bytes.                                                                                |
| `level`  | No       | `info` (the default) or `warning`.                                                               |
| `href`   | No       | A page of the web UI starting with `./`, such as `./plugins`, or an `https://` page.             |
| `action` | No       | Text of the button that opens `href`, up to 64 bytes. Defaults to *Open*.                        |

```powershell
$status = @{ version = $version; notices = @() }
if ($outdated) {
    $status.notices += @{ level = 'warning'; title = 'Driver outdated'; text = "Version $version is too old."; href = './plugins' }
}
ConvertTo-Json -InputObject $status -Depth 4 -Compress | Set-Content -LiteralPath $env:SUNSHINE_PLUGIN_STATUS_FILE -Encoding UTF8
```

A status that isn't an object, or that has an invalid notice, is rejected as a whole, and the plugin shows no notices.
Keep the script quick, since the home page waits for it.

## Applying options
An option that needs more than a new value at the next stream, such as a driver setting, can set `"apply": true`.
The manifest must then have an `apply` script. When *Save options* changes such an option on an installed plugin,
the script runs as administrator with the new values in the [script environment](#script-environment).

Every new value of that save is stored only if the script exits with 0. If it fails, the old values stay and the
user can select *Save options* again, so the script should leave things as they were before it exits with an error.
Saves that change no such option don't run the script. An option changed before the plugin is installed doesn't
run it either, so the install script should read the option too.

## Overriding options
Each application and each client can override a plugin's options for their streams. The values are combined in
this order, the last one that sets an option winning: the plugin's own value, then the application's, then the
client's. A client can also turn the plugin off for its streams. The do, undo, connect and disconnect scripts and the
client commands' scripts get the combined values; install, uninstall, action and apply scripts and the panel always
get the plugin's own.

An option marked `apply` can't be overridden, as it changes the host itself. Set `"scope": "host"` on any other
option that only makes sense for the whole host, such as a cache size or a setting the install script uses, to keep
applications and clients from overriding it. The default, `"stream"`, lets them.

The do and undo scripts run with the values of the client that launched the application, even when another client
resumes it later. The connect and disconnect scripts run with the values of the client resuming it.

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
| `action`     | One of   | Reserved for plugins that come with Sunshine: an action Sunshine itself runs, such as the xgpad plugin's switch between Xbox and DualSense controllers. Imported plugins can't use it. |
| `args`       | No       | Arguments passed to `script` or `action`.                                                          |
| `steps`      | One of   | Key chords to type instead of running a script: an array of chords, each an array of Windows virtual-key codes, such as `[[162, 160, 27]]` for Ctrl+Shift+Esc. Each chord's keys are pressed in order, 50 ms apart, then released in reverse; the next chord starts 100 ms later. |
| `elevated`   | No       | Whether `script` runs as administrator. Defaults to the plugin's `elevated`.                       |
| `top_level`  | No       | Show the command with the menu's own commands rather than on the plugin's page. Defaults to `false`. |
| `close_menu` | No       | Whether the client closes its menu after running the command. Defaults to `true`.                  |
| `programs`   | No       | File names of programs, such as `["photoshop.exe"]`. The `.exe` is optional. The command is then only offered while one of them is in the foreground on this host when the client opens its menu, or, for a name starting with `~` such as `~obs64.exe`, while that program is running at all. Windows only for now. |
| `status_file` | No      | A JSON file name in the plugin's data folder. While it exists, its `label` and `icon` replace the command's, so a script can show what the command will do next, such as *Resume* after a *Suspend*. Sunshine reads it each time a client opens its menu. When the command keeps the menu open (`close_menu` is `false`, or the user chose that), Sunshine waits up to 4 seconds for the script to end before telling the client it ran, and clients that reload their menu then show the new label and icon straight away. |

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

A panel can offer to restart Sunshine, for example after the user installs something Sunshine looks for when it
starts. The web UI shows the restart until Sunshine is back, and the promise resolves with `true` once it is, so the
panel can show what changed:

```js
const restart = window.parent.sunshine?.restart;
if (restart) {
  restart().then((restarted) => restarted && refresh()).catch((error) => console.error(error));
}
```

## Script environment
Scripts run with `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File <script>`. A script fails
when it exits with a non-zero code, including when it throws. Every script receives:

| Variable                     | Value                                                                                         |
|------------------------------|-----------------------------------------------------------------------------------------------|
| `SUNSHINE_PLUGIN_ID`         | The plugin ID.                                                                                |
| `SUNSHINE_PLUGIN_DIR`        | The plugin folder. `$PSScriptRoot` gives the same path.                                       |
| `SUNSHINE_PLUGIN_DATA_DIR`   | A folder for downloaded dependencies and saved state. It is deleted after a successful uninstall. |
| `SUNSHINE_PLUGIN_OPT_<ID>`   | The value of each option, with the option ID in upper case. Booleans are `true` or `false`. Stream scripts get the values the application and client [override](#overriding-options). |

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
The output of install, uninstall, action, apply, client command, do, undo, connect and disconnect scripts goes to
`<id>.log` in the `plugin-data` folder of the config directory, and *View log* on the plugin's page shows it. Each run
of a do, undo, connect or disconnect script starts with a line naming it, the application and the time, and the log
starts over once it grows past 1 MB. Installing, uninstalling, applying options or running an action also starts it
over. The output of a choices script goes to `.choices.log` and that of a status script to `.status.log`, both in
the plugin's data folder.

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
* The install script must work when it runs over an earlier install, since a new `version` runs it again without
  running the old uninstall script first. Replace files in place rather than deleting folders that also hold settings
  or state, stop anything the plugin left running that holds its files, and remove a firewall rule or scheduled task
  before adding it again. If the new version no longer needs something an earlier one set up, remove it here.
  Bundled plugins are reinstalled this way automatically when Sunshine starts after an update; custom plugins wait
  for *Reinstall*.
* Don't assume a command is on `PATH` or what the working directory is. Build paths from `$PSScriptRoot` and
  `$env:SUNSHINE_PLUGIN_DATA_DIR`.
