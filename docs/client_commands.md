# Client Commands
Client commands are buttons that Sunshine adds to a Moonlight client's in-stream overlay menu. Picking one runs it
on this host: it either types a sequence of key chords, as if from a keyboard, or starts a program or script.
Commands are defined once here and show up in every client that supports them.

> [!NOTE]
> Only Moonlight clients with the overlay menu ask for client commands. Other clients ignore them.

## Using client commands
Open the **Client Commands** page in the web UI.

* **All Streams** holds the commands shown whatever is being streamed. They appear in the menu's row of commands,
  after the client's own custom commands.
* Each application under **Applications** can have commands of its own. While that application is being streamed,
  the menu shows a button named after it, which opens a page of its commands. Applications without commands are
  picked from the list underneath to add some.
* Each group under **Programs** has a name, an icon and the file names of one or more programs, such as
  `photoshop.exe`. While one of those programs is in the foreground on this host when the client opens its menu, the
  menu shows a button with the group's name, which opens a page of its commands. This works on Windows only for now.
* **Plugins** lists the commands that come with [plugins](plugins.md#client-commands), such as Nyrna's *Suspend*. What
  they do comes with the plugin, but the pencil button lets you give one your own label, icon and programs. While a plugin is
  reporting what a command will do next, such as Nyrna's *Resume (name)*, its label and icon show instead. They're
  offered while the plugin is active and not turned off for the application being streamed: those marked *Top level*
  with the menu's own commands, and the rest on a page named after the plugin. A plugin can limit a command to certain
  programs, shown as *Only with ...*, which works like a group under **Programs**. *Show for* changes that for any
  command: keep the plugin's setting, show it for any program, or limit it to programs you list.

For each command, set:

* **Label** and **Icon** — the button's text, and an icon from the Material Symbols set.
* **Type**:
  * **Keys** — key chords, typed in order. *Record* captures the keys you press on the page. The browser can't capture
    some combinations, such as those with the Windows key; add those with the chord picker underneath.
  * **Command Line** — a command started on this host and left running, optionally in a working directory and
    elevated. Its environment has `SUNSHINE_CLIENT_COMMAND_ID` and `SUNSHINE_CLIENT_COMMAND_LABEL`, the name of the
    client that picked it in `SUNSHINE_CLIENT_NAME`, and the application being streamed in `SUNSHINE_APP_ID` and
    `SUNSHINE_APP_NAME`. It starts without a console window.
* **Close the menu after running it** — turn this off for commands you may want to run several times in a row.

Select **Save** to save every list at once. Changes apply the next time a client opens its menu.

A command only runs while the client asking for it is streaming, and a key sequence still being typed has to finish
before another one starts. If a command fails, the client shows why on its button.

## Files
The global commands are kept in the `commands` array of `client_commands.json` in the config directory, the program
groups in its `program-groups` array, your changes to plugin commands in its `plugin-commands` object (such
as `{"nyrna/toggle": {"label": "Pause Game", "icon": "material:pause", "programs": ["game.exe"]}}`, where an empty
`programs` list means any program), and an application's commands in a `client-commands` array on it in `apps.json`.
A program group looks like this:

```json
{"id": "3b0d...", "label": "Photoshop", "icon": "material:brush", "programs": ["photoshop.exe"], "commands": [...]}
```

Each command looks like one of these:

```json
{"id": "6f1c...", "label": "Task Manager", "icon": "material:monitoring", "close-menu": true,
 "type": "keys", "steps": [[162, 160, 27]]}
```

```json
{"id": "9a2e...", "label": "Notepad", "icon": "material:description", "close-menu": true,
 "type": "script", "cmd": "notepad.exe", "working-dir": "", "elevated": false}
```

`steps` holds chords of Windows virtual-key codes. Each chord's keys are pressed in order, 50 ms apart, then released
in reverse; the next chord starts 100 ms later.

## Client protocol
Clients use two requests on the paired HTTPS port:

* `GET /clientcommands` lists the commands offered now:
  `{"version": 1, "commands": [...], "groups": [{"kind": "app", "label": "...", "icon": "", "commands": [...]}]}`,
  where a group's `kind` is `app`, `program` or `plugin`, and each command has `id`, `label`, `icon` and `close-menu`.
* `GET /runclientcommand?id=<id>` runs one and answers `{"status": "ok"}`, or `{"status": "error", "message": "..."}`
  with 403 when the client isn't streaming, 404 when the command is no longer offered, 409 while another key sequence
  is being typed, and 500 when a command line couldn't start.
