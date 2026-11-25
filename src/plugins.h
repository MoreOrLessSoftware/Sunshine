/**
 * @file src/plugins.h
 * @brief Declarations for installable plugins that run PowerShell scripts around a streaming session.
 *
 * A plugin is a directory holding a `plugin.json` manifest and the scripts it references.
 * Bundled plugins live in the assets directory and custom plugins live in the appdata directory.
 * Each plugin can be installed (fetch its dependencies), activated (run its do/undo scripts as
 * preparation commands for every application that does not opt out), configured through the
 * options its manifest declares, and uninstalled.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "client_commands.h"
#include "config.h"
#include "platform/common.h"

namespace plugins {
  /**
   * @brief Value type of a plugin option.
   */
  enum class option_type_e {
    boolean,  ///< `true` or `false`.
    number,  ///< A number, optionally bounded by `min` and `max`.
    string,  ///< Free-form text.
    choice  ///< One value from a fixed list.
  };

  /**
   * @brief One entry in the list of values a `choice` option accepts.
   */
  struct choice_t {
    std::string value;  ///< Value passed to the scripts.
    std::string label;  ///< Text shown in the web UI.
    std::string group;  ///< Heading the web UI lists the choice under, or empty for none.
  };

  /**
   * @brief Longest value, in bytes, a choices script may offer for a choice option.
   */
  constexpr std::size_t MAX_CHOICE_VALUE_LENGTH = 512;

  /**
   * @brief Most values a choices script may offer for a choice option.
   */
  constexpr std::size_t MAX_SCRIPT_CHOICES = 1000;

  /**
   * @brief Largest file, in bytes, a choices script may write.
   */
  constexpr std::size_t MAX_CHOICES_FILE_SIZE = 256 * 1024;

  /**
   * @brief How long a choices or status script may run before it is stopped.
   */
  constexpr std::chrono::milliseconds DEFAULT_CHOICES_TIMEOUT = std::chrono::seconds(15);

  /**
   * @brief Largest file, in bytes, a status script may write.
   */
  constexpr std::size_t MAX_STATUS_FILE_SIZE = 256 * 1024;

  /**
   * @brief Most notices a status script may report.
   */
  constexpr std::size_t MAX_NOTICES = 10;

  /**
   * @brief Longest title, in bytes, of a notice a status script reports.
   */
  constexpr std::size_t MAX_NOTICE_TITLE_LENGTH = 200;

  /**
   * @brief Longest text, in bytes, of a notice a status script reports.
   */
  constexpr std::size_t MAX_NOTICE_TEXT_LENGTH = 2000;

  /**
   * @brief Longest link, in bytes, of a notice a status script reports.
   */
  constexpr std::size_t MAX_NOTICE_HREF_LENGTH = 512;

  /**
   * @brief Longest button text, in bytes, of a notice a status script reports.
   */
  constexpr std::size_t MAX_NOTICE_ACTION_LENGTH = 64;

  /**
   * @brief A configuration option declared by a plugin manifest.
   */
  struct option_t {
    std::string id;  ///< Identifier, also used to build the environment variable name.
    option_type_e type;  ///< Value type.
    std::string label;  ///< Text shown in the web UI.
    std::string description;  ///< Help text shown in the web UI.
    nlohmann::json default_value;  ///< Value used when the user has not set one.
    std::optional<double> min;  ///< Smallest accepted value of a number option.
    std::optional<double> max;  ///< Largest accepted value of a number option.
    std::optional<double> step;  ///< Increment suggested to the web UI for a number option.
    std::vector<choice_t> choices;  ///< Accepted values of a choice option, or the values listed first when `choices_script` is set.
    std::string choices_script;  ///< Script listing more values of a choice option when the web UI asks, relative to the plugin directory, or empty for none.
    bool apply = false;  ///< Whether saving a new value runs the plugin's `apply` script.
    bool host = false;  ///< Whether the option is set for the whole host only, from `"scope": "host"`, so applications and clients can't override it.
  };

  /**
   * @brief Check whether applications and clients may override an option.
   *
   * Options marked `apply` change the host itself and options with `"scope": "host"` are host-wide, so neither can.
   *
   * @param option Option to check.
   * @return `true` when an override may set the option.
   */
  bool is_overridable(const option_t &option);

  /**
   * @brief Scripts a plugin runs, as file names relative to its directory. Empty when absent.
   */
  struct scripts_t {
    std::string install;  ///< Fetches dependencies. Always runs elevated.
    std::string uninstall;  ///< Removes dependencies. Always runs elevated.
    std::string do_script;  ///< Runs before the application starts.
    std::string undo_script;  ///< Runs after the application exits.
    std::string connect_script;  ///< Runs when a client resumes the running application.
    std::string disconnect_script;  ///< Runs when the last client leaves the application running.
    std::string apply;  ///< Puts changed options into effect when they are saved. Always runs elevated.
    std::string status;  ///< Reports the plugin's state and notices for the web UI. Runs as the signed-in user.
  };

  /**
   * @brief A button on the plugin's card that runs one of its scripts.
   */
  struct action_t {
    std::string id;  ///< Identifier, sent by the web UI to run the action.
    std::string label;  ///< Button text.
    std::string description;  ///< Tooltip shown in the web UI.
    std::string script;  ///< Script file relative to the plugin directory. Always runs elevated, like install.
  };

  /**
   * @brief A command the plugin adds to clients' in-stream overlay menus (see `client_commands`).
   *
   * It either runs one of the plugin's scripts, with the plugin's environment, types key chords, or, for a plugin that
   * ships with Sunshine, runs an action Sunshine handles itself (see `client_commands::register_action()`).
   */
  struct client_command_t {
    std::string id;  ///< Identifier, unique within the plugin.
    std::string label;  ///< Text shown on the client's button.
    std::string icon;  ///< Icon for the client's button, such as `material:<name>`, or empty for none.
    bool close_menu = true;  ///< Whether the client closes its menu after running the command.
    bool top_level = false;  ///< Whether the command is shown with the menu's own commands rather than in the plugin's group.
    std::string script;  ///< Script file relative to the plugin directory, or empty for a command that types keys or runs an action.
    std::string action;  ///< Action Sunshine runs, or empty; only plugins that ship with Sunshine may set it.
    std::vector<std::string> args;  ///< Arguments passed to the script or the action.
    std::vector<std::vector<std::uint16_t>> steps;  ///< Key chords a command without a script types.
    bool elevated = false;  ///< Whether the script runs elevated; defaults to the plugin's `elevated`.
    std::string status_file;  ///< JSON file in the plugin's data directory whose `label` and `icon` replace the command's while it exists, or empty for none.
    std::vector<std::string> programs;  ///< File names of the programs the command is limited to while they're in the foreground, or empty for no limit.
  };

  /**
   * @brief An HTML page from the plugin directory shown on the plugin's card.
   *
   * The page may read data from one local port through `/api/plugins/proxy`, so a plugin that starts a local
   * service can show its live state without the browser reaching the service directly.
   */
  struct panel_t {
    std::string file;  ///< HTML file relative to the plugin directory.
    int port = 0;  ///< Local port `/api/plugins/proxy` forwards GET requests to, or 0 for none.
    int height = 360;  ///< Height of the panel in pixels, or its height until its content is measured when `fit` is set.
    bool fit = false;  ///< Whether the panel grows and shrinks to fit its content, from `"height": "auto"`.
  };

  /**
   * @brief A parsed and validated `plugin.json`.
   */
  struct manifest_t {
    std::string id;  ///< Unique identifier.
    std::string name;  ///< Display name.
    std::string version;  ///< Plugin version.
    std::string description;  ///< Short description.
    std::string author;  ///< Author name.
    std::string homepage;  ///< Project URL.
    std::string notice;  ///< Something to know before using the plugin, shown on its page.
    std::string notice_url;  ///< An https:// page about `notice`, linked after it, or empty.
    int order;  ///< Do scripts run in ascending order, undo scripts in descending order.
    bool elevated;  ///< Whether the do and undo scripts run elevated.
    bool manages_display;  ///< Whether the plugin sets up the display itself, replacing Sunshine's display device configuration.
    bool idle_only;  ///< Whether the plugin can only be installed or uninstalled while no stream is running.
    bool restart_after_change;  ///< Whether Sunshine must restart after the plugin is installed or uninstalled for the change to take effect.
    bool default_active;  ///< Whether a bundled plugin is installed and activated the first time Sunshine starts with it.
    scripts_t scripts;  ///< Scripts the plugin provides.
    std::vector<option_t> options;  ///< Configuration options.
    std::vector<action_t> actions;  ///< Buttons that run scripts on demand.
    std::vector<client_command_t> client_commands;  ///< Commands added to clients' overlay menus.
    std::string icon;  ///< Icon of the plugin's group in clients' overlay menus, or empty for the clients' default.
    std::optional<panel_t> panel;  ///< Page shown on the plugin's card.
    std::filesystem::path dir;  ///< Directory containing the manifest.
    bool builtin;  ///< Whether the plugin ships with Sunshine.
  };

  /**
   * @brief Persisted state of one plugin.
   */
  struct plugin_state_t {
    bool installed = false;  ///< Whether the install script completed successfully.
    bool active = false;  ///< Whether the do and undo scripts run for applications.
    nlohmann::json options = nlohmann::json::object();  ///< Option values set by the user.
    std::string installed_version;  ///< Manifest version whose install script last succeeded, or empty when not installed.
    std::string origin;  ///< Folder or update feed URL a custom plugin was last imported from, or empty.
    nlohmann::json clients = nlohmann::json::object();  ///< Overrides for each client, keyed by its UUID, as `{"active": false, "options": {...}}` with either key optional.
    bool installed_before = false;  ///< Whether the plugin was ever installed, so a `default_active` one isn't installed again after the user uninstalls it.
  };

  /**
   * @brief A long-running plugin operation.
   */
  enum class action_e {
    install,  ///< Run the install script.
    uninstall,  ///< Run the uninstall script.
    run,  ///< Run the script of one of the plugin's actions.
    apply  ///< Run the apply script, then save the new options if it succeeds.
  };

  /**
   * @brief Progress of the last operation started for a plugin.
   */
  enum class task_status_e {
    running,  ///< Still running.
    succeeded,  ///< Finished with exit code 0.
    failed  ///< Could not start or finished with a non-zero exit code.
  };

  /**
   * @brief Outcome of a request to change a plugin.
   */
  enum class result_e {
    ok,  ///< The request was accepted.
    not_found,  ///< No plugin has the requested ID.
    invalid,  ///< The request contained an invalid value.
    not_installed,  ///< The plugin must be installed first.
    busy,  ///< Another install or uninstall is still running.
    unavailable  ///< The local service behind a plugin panel did not answer.
  };

  /**
   * @brief A response from the local service behind a plugin panel.
   */
  struct proxy_response_t {
    int status = 0;  ///< HTTP status code.
    std::string content_type;  ///< `Content-Type` of the response, or empty.
    std::string body;  ///< Response body.
  };

  /**
   * @brief Locations the plugin registry reads from and writes to.
   */
  struct paths_t {
    std::filesystem::path builtin_dir;  ///< Directory of bundled plugins.
    std::filesystem::path custom_dir;  ///< Directory of user-provided plugins.
    std::filesystem::path data_dir;  ///< Directory for downloaded dependencies and logs.
    std::filesystem::path state_file;  ///< File storing the install, activation and option state.
  };

  /**
   * @brief Runs one plugin script to completion.
   *
   * @param command Command line to execute.
   * @param elevated Whether to run with elevated privileges.
   * @param env Variables added to Sunshine's environment.
   * @param working_dir Directory the script starts in.
   * @param log_file File that receives the script output.
   * @param stop Requests the script be terminated early.
   * @return Exit code of the script, or -1 when it could not run.
   */
  using script_runner_t = std::function<int(const std::string &command, bool elevated, const std::map<std::string, std::string> &env, const std::filesystem::path &working_dir, const std::filesystem::path &log_file, std::stop_token stop)>;

  /**
   * @brief Function that downloads a file over HTTPS.
   *
   * @param url Address to download.
   * @param max_bytes Largest response accepted.
   * @param timeout Longest the whole download may take.
   * @param body Receives the response.
   * @param final_url Receives the address the response came from once redirects were followed.
   * @param error Receives the reason when the download fails.
   * @return `true` when the server answered 200 OK with no more than `max_bytes`.
   */
  using fetcher_t = std::function<bool(const std::string &url, std::size_t max_bytes, std::chrono::seconds timeout, std::string &body, std::string &final_url, std::string &error)>;

  /**
   * @brief Check whether a string can be used as a plugin ID.
   *
   * @param id Candidate ID.
   * @return `true` for 1 to 64 lowercase letters, digits, `.`, `_` or `-`, starting with a letter or digit.
   */
  bool is_valid_id(std::string_view id);

  /**
   * @brief Parse and validate a plugin manifest.
   *
   * @param json Contents of `plugin.json`.
   * @param dir Directory containing the manifest; referenced scripts must exist in it.
   * @param builtin Whether the plugin ships with Sunshine.
   * @param error Receives the reason when the manifest is rejected.
   * @return The manifest, or `std::nullopt` when it is invalid.
   */
  std::optional<manifest_t> parse_manifest(const nlohmann::json &json, const std::filesystem::path &dir, bool builtin, std::string &error);

  /**
   * @brief Find every valid plugin in the bundled and custom directories.
   *
   * A custom plugin whose ID matches a bundled plugin is ignored.
   *
   * @param builtin_dir Directory of bundled plugins.
   * @param custom_dir Directory of user-provided plugins.
   * @return Plugins sorted by order, then ID.
   */
  std::vector<manifest_t> discover(const std::filesystem::path &builtin_dir, const std::filesystem::path &custom_dir);

  /**
   * @brief Merge option values with the defaults declared by a manifest.
   *
   * Keys the manifest does not declare are dropped.
   *
   * @param manifest Plugin declaring the options.
   * @param input Object of option values.
   * @param strict When `true`, an invalid value rejects the input; otherwise it is replaced by the default.
   * @param error Receives the reason when the input is rejected.
   * @return An object holding a value for every declared option, or `std::nullopt` when rejected.
   */
  std::optional<nlohmann::json> normalize_options(const manifest_t &manifest, const nlohmann::json &input, bool strict, std::string &error);

  /**
   * @brief Validate the option values an application or client overrides for a plugin.
   *
   * Unlike `normalize_options()`, the result only holds the options the input sets.
   *
   * @param manifest Plugin declaring the options.
   * @param input Object of option values.
   * @param strict When `true`, an unknown option, an invalid value or an option that can't be overridden (see
   *               `is_overridable()`) rejects the input; otherwise each is dropped.
   * @param error Receives the reason when the input is rejected.
   * @return Object holding the accepted values, or `std::nullopt` when rejected.
   */
  std::optional<nlohmann::json> normalize_overrides(const manifest_t &manifest, const nlohmann::json &input, bool strict, std::string &error);

  /**
   * @brief Combine a plugin's option values with an application's and a client's overrides.
   *
   * Each layer replaces the values of the one before: the plugin's own values, then the application's, then the
   * client's. Invalid values and options that can't be overridden are ignored in the overrides.
   *
   * @param manifest Plugin declaring the options.
   * @param options The plugin's own option values.
   * @param app_overrides Values the application overrides, or `null`.
   * @param client_overrides Values the client overrides, or `null`.
   * @return An object holding a value for every declared option.
   */
  nlohmann::json resolve_options(const manifest_t &manifest, const nlohmann::json &options, const nlohmann::json &app_overrides, const nlohmann::json &client_overrides);

  /**
   * @brief Build the environment of a plugin's stream scripts for an application and a client.
   *
   * @param id Plugin ID.
   * @param app_overrides The application's `plugin-options`: override objects keyed by plugin ID.
   * @param client_uuid UUID of the client streaming, or empty for none.
   * @return The plugin's script environment with the options resolved by `resolve_options()`, or `std::nullopt`
   *         when the plugin no longer exists or the client turned it off.
   */
  std::optional<std::map<std::string, std::string>> stream_env(const std::string &id, const nlohmann::json &app_overrides, const std::string &client_uuid);

  /**
   * @brief Get a plugin's option values for an application and a client, as its stream scripts get them.
   *
   * For Sunshine's own code that acts on a plugin's options, such as the xgpad plugin's controller type.
   *
   * @param id Plugin ID.
   * @param app_overrides The application's `plugin-options`, keyed by plugin ID, or `null`.
   * @param client_uuid Client UUID, or empty for none.
   * @return An object holding a value for every declared option, or `std::nullopt` when the plugin doesn't exist, is
   *         not installed and active, needs its install script to run again, or the client turned it off.
   */
  std::optional<nlohmann::json> resolved_options(const std::string &id, const nlohmann::json &app_overrides, const std::string &client_uuid);

  /**
   * @brief Check whether a client turned a plugin off for its streams.
   *
   * @param id Plugin ID.
   * @param client_uuid Client UUID, or empty for none.
   * @return `true` when the client's overrides set `"active": false` for the plugin.
   */
  bool disabled_for_client(const std::string &id, const std::string &client_uuid);

  /**
   * @brief Validate the `plugin-options` of an application.
   *
   * @param input Object mapping plugin IDs to objects of option values.
   * @param error Receives the reason when the input is rejected.
   * @return The input with each plugin's values validated by `normalize_overrides()` in strict mode and empty entries
   *         removed, or `std::nullopt` when an entry names an unknown plugin or is rejected.
   */
  std::optional<nlohmann::json> normalize_app_overrides(const nlohmann::json &input, std::string &error);

  /**
   * @brief List every client's plugin overrides.
   *
   * @return Object mapping client UUIDs to objects mapping plugin IDs to `{"active": false, "options": {...}}`,
   *         with either key optional.
   */
  nlohmann::json client_overrides();

  /**
   * @brief Replace a client's plugin overrides.
   *
   * @param client_uuid Client UUID.
   * @param input Object mapping plugin IDs to `{"active": bool, "options": {...}}`, with either key optional.
   *              `"active": true` is the same as leaving it out. Options are validated by `normalize_overrides()`
   *              in strict mode. Plugins missing from the input lose the client's overrides.
   * @param error Receives the reason when the input is rejected.
   * @return `result_e::ok`, `result_e::not_found` for an unknown plugin, or `result_e::invalid`.
   */
  result_e set_client_overrides(const std::string &client_uuid, const nlohmann::json &input, std::string &error);

  /**
   * @brief Remove the plugin overrides of a client that was unpaired.
   *
   * @param client_uuid Client UUID, or empty to remove every client's overrides.
   */
  void forget_client(const std::string &client_uuid);

  /**
   * @brief Build the environment variable name that carries an option to the scripts.
   *
   * @param option_id Option ID.
   * @return `SUNSHINE_PLUGIN_OPT_` followed by the uppercased ID.
   */
  std::string option_env_name(std::string_view option_id);

  /**
   * @brief Format an option value for an environment variable.
   *
   * @param value Option value.
   * @return `true`/`false` for booleans, integers without a decimal point, other values as text.
   */
  std::string option_env_value(const nlohmann::json &value);

  /**
   * @brief Build the variables passed to every script of a plugin.
   *
   * @param manifest Plugin running the script.
   * @param options Normalized option values.
   * @param paths Plugin directories, which give the plugin's data directory and the shared helpers.
   * @return `SUNSHINE_PLUGIN_ID`, `SUNSHINE_PLUGIN_DIR`, `SUNSHINE_PLUGIN_DATA_DIR`, `SUNSHINE_PLUGIN_HELPERS` and one
   *         variable per option.
   */
  std::map<std::string, std::string> script_env(const manifest_t &manifest, const nlohmann::json &options, const paths_t &paths);

  /**
   * @brief Build the command line that runs a PowerShell script.
   *
   * @param script Path of the script.
   * @return Command line invoking PowerShell non-interactively on the script.
   */
  std::string script_command(const std::filesystem::path &script);

  /**
   * @brief Check whether a plugin's install script must run again before the plugin is used.
   *
   * A plugin needs a reinstall when its version changed since its install script last succeeded, for example after
   * Sunshine updated a bundled plugin or a newer copy of a custom plugin was imported. Until then it doesn't run for
   * streams or offer client commands.
   *
   * @param manifest Plugin as it is now.
   * @param state Persisted state of the plugin.
   * @return `true` when the plugin is installed and its installed version differs from the manifest's.
   */
  bool needs_reinstall(const manifest_t &manifest, const plugin_state_t &state);

  /**
   * @brief Read the persisted plugin state.
   *
   * @param state_file File to read.
   * @return State keyed by plugin ID; empty when the file is missing or unreadable.
   */
  std::unordered_map<std::string, plugin_state_t> load_state(const std::filesystem::path &state_file);

  /**
   * @brief Persist the plugin state.
   *
   * @param state_file File to write.
   * @param state State keyed by plugin ID.
   * @return `true` on success.
   */
  bool save_state(const std::filesystem::path &state_file, const std::unordered_map<std::string, plugin_state_t> &state);

  /**
   * @brief Return the default locations used by Sunshine.
   *
   * @return Bundled plugins in the assets directory and everything else in the appdata directory.
   */
  paths_t default_paths();

  /**
   * @brief Load the plugin registry from the given locations.
   *
   * Stops reinstalling updated plugins and waits for a running install or uninstall to finish first. An installed
   * plugin whose state has no installed version, as written before versions were recorded, is taken to have its
   * current version installed, so it isn't reinstalled.
   *
   * @param paths Locations to use.
   */
  void load(const paths_t &paths);

  /**
   * @brief Start reinstalling, in the background, the bundled plugins whose version changed since they were installed.
   *
   * Sunshine calls it once at startup, since bundled plugins only change when Sunshine is updated. It also installs
   * and activates each bundled plugin whose manifest sets `default_active` and that was never installed. The plugins
   * are installed one at a time, after any operation already running, and each is tried once; a failure is shown on
   * the plugin's page and the plugin waits to be reinstalled from the web UI. When install scripts can't run yet
   * because no user is signed in, it waits for one first. Custom plugins are never reinstalled automatically.
   * Returns immediately; `cancel_task()` and `load()` stop it.
   */
  void reinstall_updated_builtins();

  /**
   * @brief Load the plugin registry from the default locations.
   *
   * @return Object that stops a running install or uninstall when destroyed.
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Return the locations the registry was loaded from.
   *
   * @return Locations passed to the last `load()`.
   */
  paths_t current_paths();

  /**
   * @brief Return the file of the guide to writing plugins, which ships with the bundled plugins.
   *
   * @return `PLUGIN_GUIDE.md` in the directory of bundled plugins. It may not exist.
   */
  std::filesystem::path guide_file();

  /**
   * @brief Rescan the plugin directories, keeping the current state.
   */
  void rescan();

  /**
   * @brief Copy a plugin's folder into the custom plugins directory, adding the plugin or replacing it.
   *
   * The folder's manifest and scripts are checked first, and an existing copy is only replaced once the new one has
   * been copied in full. Entries whose names start with a dot, such as `.git`, are left out. The plugin keeps its
   * installed state and options when it's replaced, and the folder is recorded as its origin.
   *
   * @param source Absolute path of the folder holding the plugin's `plugin.json`.
   * @param id Receives the plugin's ID.
   * @param updated Receives whether a plugin with that ID was replaced.
   * @param error Receives the reason when the import is rejected.
   * @return Outcome of the request.
   */
  result_e import_folder(const std::filesystem::path &source, std::string &id, bool &updated, std::string &error);

  /**
   * @brief Return the update feed for a plugin's address.
   *
   * @param url An HTTPS address of a feed, or of a GitHub repository, whose latest release's `latest.json` asset is
   * then the feed.
   * @param error Receives the reason when the address can't be used.
   * @return Address of the feed, or `std::nullopt` when `url` isn't a valid HTTPS address.
   */
  std::optional<std::string> feed_url(const std::string &url, std::string &error);

  /**
   * @brief Download a plugin from its update feed and import it like `import_folder()`.
   *
   * The feed is a JSON object with the plugin's `version`, the `url` of a zip file holding it (absolute, or relative
   * to the feed), and that file's `sha256`. The zip holds `plugin.json` at its top level or in a single top-level
   * folder, and its version must match the feed. The feed is recorded as the plugin's origin.
   *
   * @param url Address given to `feed_url()`.
   * @param id Receives the plugin's ID.
   * @param updated Receives whether a plugin with that ID was replaced.
   * @param error Receives the reason when the import is rejected.
   * @return Outcome of the request: `result_e::unavailable` when a download failed.
   */
  result_e import_url(const std::string &url, std::string &id, bool &updated, std::string &error);

  /**
   * @brief How a check for updates went.
   */
  struct update_summary_t {
    int checked = 0;  ///< Custom plugins whose update feed was read, or tried.
    int available = 0;  ///< Plugins whose feed has a different version than the one imported.
    int failed = 0;  ///< Plugins whose feed couldn't be read.
  };

  /**
   * @brief Read the update feed of every custom plugin imported from a URL.
   *
   * The results show in `list()` until Sunshine restarts or the plugin is imported again. A plugin has an update when
   * its feed's version differs from its own. Bundled plugins update with Sunshine and aren't checked.
   *
   * @return Counts of plugins checked, with updates and that couldn't be checked.
   */
  update_summary_t check_for_updates();

  /**
   * @brief Import a custom plugin again from the update feed it was imported from, like `import_url()`.
   *
   * Like any import with a new version, an installed plugin then needs a reinstall.
   *
   * @param id Plugin ID.
   * @param error Receives the reason when the update fails.
   * @return Outcome of the request: `result_e::invalid` for a bundled plugin, one imported from a folder, or a feed
   * that now holds another plugin.
   */
  result_e update(const std::string &id, std::string &error);

  /**
   * @brief Describe every plugin for the web UI.
   *
   * @return Array of plugin objects with manifest fields, state and the last task. `display_exclusions` lists the
   *         applications the plugin was turned off for when it was last activated, because another plugin already
   *         sets up the display for them.
   */
  nlohmann::json list();

  /**
   * @brief Build the preparation commands of the active plugins.
   *
   * Each command names its plugin in `plugin`, and its environment holds the options with the application's
   * overrides. When a client starts or resumes the application, `stream_env()` adds the client's.
   *
   * @param excluded IDs of plugins the application opted out of.
   * @param app_overrides The application's `plugin-options`: override objects keyed by plugin ID.
   * @return In plugin order, an app command for each active plugin with a do or undo script that doesn't need a
   *         reinstall (see `needs_reinstall()`),
   *         followed by a connect command when the plugin also has a connect or disconnect script.
   */
  std::vector<config::prep_cmd_t> prep_cmds(const std::vector<std::string> &excluded, const nlohmann::json &app_overrides = nullptr);

  /**
   * @brief Find the active plugin that sets up the display for an application.
   *
   * While such a plugin runs for a stream, Sunshine skips its own display device configuration, which would
   * otherwise be applied before the plugin's do script and fight over the display layout.
   *
   * @param excluded IDs of plugins the application opted out of.
   * @param client_uuid UUID of the client streaming, whose turned-off plugins are skipped too, or empty for none.
   * @return Name of the first active, installed plugin (in plugin order) that doesn't need a reinstall and whose
   *         manifest sets `manages_display`, or an empty string when there is none.
   */
  std::string display_plugin(const std::vector<std::string> &excluded, const std::string &client_uuid = {});

  /**
   * @brief Check whether a plugin sets up the display itself.
   *
   * Only one such plugin runs for a stream, since two would each rearrange the displays the other set up.
   *
   * @param id Plugin ID.
   * @return true if the plugin exists and its manifest sets `manages_display`.
   */
  bool manages_display(const std::string &id);

  /**
   * @brief Return a plugin's name.
   *
   * @param id Plugin ID.
   * @return The name from its manifest, or the ID when the plugin isn't found.
   */
  std::string plugin_name(const std::string &id);

  /**
   * @brief Find the error a plugin's script reported in the last run its log records.
   *
   * The last run starts at the last `[YYYY-MM-DD HH:MM:SS] ...` heading, or after the log's first line, which names
   * the install or other task, when there is none. A PowerShell error is reported on the line before its
   * `At <script>:<line> char:<column>` line. Otherwise the run's last line is used, other than the line Sunshine
   * adds with the exit code.
   *
   * @param log Contents of the plugin's log.
   * @return The error, shortened to 200 characters, or empty when the run printed nothing.
   */
  std::string error_summary(std::string_view log);

  /**
   * @brief Find the error a plugin's script reported in the last run of its log file.
   *
   * @param log_file Plugin log.
   * @return The error as `error_summary()` finds it, or empty when the log can't be read.
   */
  std::string log_error_summary(const std::filesystem::path &log_file);

  /**
   * @brief Turn a plugin that sets up the display off for the applications another such plugin already serves.
   *
   * Only one plugin sets up the display for a stream, so a newly activated one would otherwise either never run
   * or replace the one an application already uses. An application keeps the plugin it has.
   *
   * @param id ID of the plugin that was just activated.
   * @param apps_tree Contents of apps.json. The plugin is added to `exclude-plugins` of each application in which
   *                  another active plugin that sets up the display isn't excluded.
   * @return Names of the applications changed, empty when the plugin doesn't set up the display.
   */
  std::vector<std::string> exclude_display_conflicts(const std::string &id, nlohmann::json &apps_tree);

  /**
   * @brief The client commands of one plugin, as offered to clients.
   */
  struct offered_commands_t {
    std::string id;  ///< Plugin ID.
    std::string name;  ///< Plugin name, the label of its group.
    std::string icon;  ///< Icon of the plugin's group.
    std::vector<client_commands::command_t> grouped;  ///< Commands shown in the plugin's group.
    std::vector<client_commands::command_t> top_level;  ///< Commands shown with the menu's own commands.
  };

  /**
   * @brief Build the client commands of the active plugins.
   *
   * A script command runs with the plugin's environment, in the plugin directory; its ID is
   * `plugin:<plugin id>/<command id>`. A command limited to programs is left out unless one of them is in the
   * foreground on the host.
   *
   * @param excluded IDs of plugins the application being streamed opted out of.
   * @param overrides The user's labels, icons and programs, which replace the plugin's. A status file still
   *                  replaces the label and icon while it exists, as it shows what the command will do next.
   * @param app_overrides The `plugin-options` of the application being streamed, used in the scripts' environment.
   * @param client_uuid UUID of the client asking: its overrides are used in the scripts' environment, and the
   *                    plugins it turned off are left out. Empty for none.
   * @return In plugin order, the commands of each active, installed plugin that has any and doesn't need a reinstall.
   */
  std::vector<offered_commands_t> offered_client_commands(const std::vector<std::string> &excluded, const client_commands::plugin_overrides_t &overrides = {}, const nlohmann::json &app_overrides = nullptr, const std::string &client_uuid = {});

  /**
   * @brief Change whether a plugin is active and the values of its options.
   *
   * When the plugin is installed and a new value changes an option marked `apply`, the plugin's apply script runs
   * in the background and all the new values are saved only if it succeeds. Otherwise they are saved immediately.
   *
   * @param id Plugin ID.
   * @param active Whether the plugin should be active, or `std::nullopt` to keep the current value.
   * @param options Option values, or `std::nullopt` to keep the current values.
   * @param error Receives the reason when the request is rejected.
   * @param applying Set to `true` when the apply script was started, so the new values aren't saved yet. May be null.
   * @return Outcome of the request.
   */
  result_e configure(const std::string &id, std::optional<bool> active, const std::optional<nlohmann::json> &options, std::string &error, bool *applying = nullptr);

  /**
   * @brief Start installing or uninstalling a plugin in the background.
   *
   * Uninstalling deactivates the plugin immediately.
   *
   * @param id Plugin ID.
   * @param action Operation to run.
   * @param error Receives the reason when the request is rejected.
   * @param activate Whether to activate the plugin once an install succeeds. Ignored for uninstall.
   * @param action_id Action to run when `action` is `action_e::run`; the plugin must be installed.
   * @return Outcome of the request: `result_e::busy` too when another operation is running, or when the plugin is
   *         `idle_only` and a stream is running.
   */
  result_e start_task(const std::string &id, action_e action, std::string &error, bool activate = false, const std::string &action_id = {});

  /**
   * @brief Check whether a plugin can only be installed or uninstalled while no stream is running.
   *
   * @param id Plugin ID.
   * @return true if the plugin exists and its manifest sets `idle_only`.
   */
  bool idle_only(const std::string &id);

  /**
   * @brief Check whether a plugin does anything for a stream, so applications and clients can turn it off.
   *
   * A plugin that only installs something, such as a driver, does the same for every stream.
   *
   * @param manifest Plugin to check.
   * @return `true` when the plugin has a do, undo, connect or disconnect script or client commands, or sets up
   *         the display.
   */
  bool per_stream(const manifest_t &manifest);

  /**
   * @brief Check whether a choices script may offer a value.
   *
   * @param value Candidate value.
   * @return `true` for 1 to `MAX_CHOICE_VALUE_LENGTH` bytes without control characters.
   */
  bool is_valid_script_choice(std::string_view value);

  /**
   * @brief Parse the list a choices script wrote.
   *
   * @param node Array of strings, or of objects with a string `value` and optional `label` and `group`.
   * @param out Receives the parsed choices.
   * @param error Receives the reason when the list is rejected.
   * @return `false` when the list is not an array, has more than `MAX_SCRIPT_CHOICES` entries, or has an invalid
   *         entry or value (see `is_valid_script_choice()`).
   */
  bool parse_script_choices(const nlohmann::json &node, std::vector<choice_t> &out, std::string &error);

  /**
   * @brief Describe a list of choices for the web UI.
   *
   * @param choices Choices to describe.
   * @return Array of objects with `value`, `label`, and `group` when the choice has one.
   */
  nlohmann::json choices_json(const std::vector<choice_t> &choices);

  /**
   * @brief List the values a choice option accepts, running its choices script.
   *
   * The script runs without elevation, as the signed-in user, in the plugin's data directory with the plugin's
   * environment and two more variables: `SUNSHINE_PLUGIN_CHOICE_OPTION` names the option and
   * `SUNSHINE_PLUGIN_CHOICES_FILE` names the file the script writes its list to, as JSON (see
   * `parse_script_choices()`). Its output goes to `.choices.log` in the data directory.
   *
   * @param id Plugin ID.
   * @param option_id ID of a choice option with a `choices_script`.
   * @param out Receives the manifest's choices followed by the script's, without repeating a value.
   * @param error Receives the reason when the list could not be made.
   * @return `ok`, `not_found` (no such plugin), `invalid` (no such option, or it has no choices script, or the
   *         script failed or wrote a bad list), `not_installed`, or `unavailable` (no user is signed in).
   */
  result_e list_choices(const std::string &id, const std::string &option_id, std::vector<choice_t> &out, std::string &error);

  /**
   * @brief Replace how long `list_choices()` and `plugin_status()` let a script run.
   *
   * @param timeout Longest run time.
   */
  void set_choices_timeout_for_testing(std::chrono::milliseconds timeout);

  /**
   * @brief Check the status a status script wrote, and its notices.
   *
   * The status is an object. Its optional `notices` is an array of at most `MAX_NOTICES` objects, each with a
   * `title`, and optionally a `text`, a `level` of `info` (the default) or `warning`, an `href` starting with `./`
   * (a page of the web UI) or `https://`, and an `action`, the text of the button that opens `href`. Other fields
   * are kept as they are, for the plugin's panel.
   *
   * @param node Parsed JSON the script wrote.
   * @param out Receives the status, with each notice holding exactly `level`, `title`, `text`, `href` and `action`.
   * @param error Receives the reason when the status is rejected.
   * @return `false` when the status isn't an object or a notice is invalid.
   */
  bool parse_status(const nlohmann::json &node, nlohmann::json &out, std::string &error);

  /**
   * @brief Get a plugin's status by running its status script.
   *
   * The script runs like a choices script, without elevation, as the signed-in user, in the plugin's data directory
   * with the plugin's environment and `SUNSHINE_PLUGIN_STATUS_FILE`, which names the file the script writes its
   * status to, as JSON (see `parse_status()`). Its output goes to `.status.log` in the data directory.
   *
   * @param id Plugin ID.
   * @param out Receives the status.
   * @param error Receives the reason when there is no status.
   * @return `ok`, `not_found` (no such plugin), `invalid` (the plugin has no status script, or the script failed or
   *         wrote a bad status), `not_installed`, or `unavailable` (no user is signed in).
   */
  result_e plugin_status(const std::string &id, nlohmann::json &out, std::string &error);

  /**
   * @brief Collect the notices of every installed, active plugin with a status script, for the home page.
   *
   * The status scripts run at the same time. A plugin whose script fails, or that needs a reinstall, adds nothing.
   *
   * @return Array of notices in plugin order, as `parse_status()` describes them, each with the `plugin` ID and its
   *         `plugin_name` added. Empty when no user is signed in.
   */
  nlohmann::json plugin_notices();

  /**
   * @brief Return the HTML file of a plugin's panel.
   *
   * @param id Plugin ID.
   * @return Path of the panel file, or `std::nullopt` when the plugin does not exist or has no panel.
   */
  std::optional<std::filesystem::path> panel_file(const std::string &id);

  /**
   * @brief Check whether a path may be forwarded to a plugin's local service.
   *
   * @param path Path and query, as sent by the panel.
   * @return `true` for an absolute path of at most 512 printable characters without a scheme or authority.
   */
  bool is_valid_proxy_path(std::string_view path);

  /**
   * @brief Forward a GET request from a plugin panel to the local port its manifest declares.
   *
   * The request goes to `http://127.0.0.1:<port><path>` with a short timeout, so the panel can show the
   * state of a service the plugin started without the browser leaving Sunshine's origin.
   *
   * @param id Plugin ID.
   * @param path Path and query to request.
   * @param out Receives the response.
   * @param error Receives the reason when the request is rejected or fails.
   * @return `ok`, `not_found`, `invalid` (no proxy port or a bad path) or `unavailable` (nothing answered).
   */
  result_e proxy_get(const std::string &id, const std::string &path, proxy_response_t &out, std::string &error);

  /**
   * @brief Receives the status and content type of a streamed panel response, before any of its body.
   */
  using proxy_head_t = std::function<void(int status, const std::string &content_type)>;

  /**
   * @brief Receives each piece of a streamed panel response as it arrives; returns `false` to stop.
   */
  using proxy_data_t = std::function<bool(std::string_view data)>;

  /**
   * @brief Forward a GET request from a plugin panel and pass the response on as it arrives.
   *
   * For server-sent events and other long-lived responses. Like `proxy_get()`, but the body is handed to
   * `on_data` piece by piece instead of being collected, and the request runs until the service ends the
   * response, `on_data` returns `false`, `max_duration` passes, or Sunshine shuts down.
   *
   * @param id Plugin ID.
   * @param path Path and query to request.
   * @param on_head Called once with the status and content type, before the first call to `on_data`.
   * @param on_data Called with each piece of the body.
   * @param max_duration Longest time to keep the request open.
   * @param error Receives the reason when the request is rejected or fails.
   * @return `ok` once a response was received (`on_head` was called), otherwise `not_found`, `invalid` or
   *         `unavailable` as for `proxy_get()`.
   */
  result_e proxy_stream(const std::string &id, const std::string &path, const proxy_head_t &on_head, const proxy_data_t &on_data, std::chrono::milliseconds max_duration, std::string &error);

  /**
   * @brief End every running `proxy_stream()`. Called when Sunshine shuts down.
   */
  void stop_proxy_streams();

  /**
   * @brief Set the function called after an install or uninstall changes plugin state in the background.
   *
   * Sunshine uses it to rebuild the applications' preparation commands.
   *
   * @param callback Function to call, or an empty function to call nothing.
   */
  void set_change_callback(std::function<void()> callback);

  /**
   * @brief Set the function called when a plugin is activated, whether directly or by an install.
   *
   * Sunshine uses it to turn a plugin that sets up the display off for applications another such plugin serves.
   * The applications it returns are listed as the plugin's `display_exclusions` until the plugin is deactivated
   * or activated again. It's called without the registry lock held, before an install task reports success.
   *
   * @param callback Function given the plugin ID that returns the names of the applications it changed, or an
   *                 empty function to call nothing.
   */
  void set_activation_callback(std::function<std::vector<std::string>(const std::string &id)> callback);

  /**
   * @brief Set the function that tells whether a stream is running.
   *
   * Installing or uninstalling a plugin whose manifest sets `idle_only` is refused while it returns true.
   *
   * @param check Function returning whether a stream is running, or an empty function for never.
   */
  void set_stream_check(std::function<bool()> check);

  /**
   * @brief Set the function that restarts Sunshine.
   *
   * `reinstall_updated_builtins()` calls it once its installs are done when one succeeded for a plugin whose
   * manifest sets `restart_after_change`, after waiting for any running stream to end.
   *
   * @param callback Function restarting Sunshine, or an empty function to restart nothing.
   */
  void set_restart_callback(std::function<void()> callback);

  /**
   * @brief Set the function called when an install Sunshine started by itself fails.
   *
   * Installs of plugins that are on by default or changed since they were installed run in the background, where
   * no one watches the web UI. Sunshine uses this to show a notification.
   *
   * @param callback Function given the plugin ID, its name and the error from its log, or an empty function.
   */
  void set_failure_callback(std::function<void(const std::string &id, const std::string &name, const std::string &detail)> callback);

  /**
   * @brief Block until the running install or uninstall finishes.
   */
  void wait_for_task();

  /**
   * @brief Stop reinstalling updated plugins, then terminate the running install or uninstall and wait for it to finish.
   */
  void cancel_task();

  /**
   * @brief Read the output of the last install or uninstall of a plugin.
   *
   * @param id Plugin ID.
   * @return Log contents, or `std::nullopt` when the plugin does not exist.
   */
  std::optional<std::string> read_log(const std::string &id);

  /**
   * @brief Replace the function that runs install and uninstall scripts.
   *
   * @param runner Replacement runner.
   */
  void set_script_runner_for_testing(script_runner_t runner);

  /**
   * @brief Restore the default script runner.
   */
  void reset_script_runner_for_testing();

  /**
   * @brief Replace the check `reinstall_updated_builtins()` uses to tell whether install scripts can run yet.
   *
   * @param ready Returns `true` once a user is signed in, or Sunshine doesn't need one to run scripts.
   * @param poll How long to wait between checks.
   */
  void set_session_check_for_testing(std::function<bool()> ready, std::chrono::milliseconds poll);

  /**
   * @brief Restore the default check for a signed-in user.
   */
  void reset_session_check_for_testing();

  /**
   * @brief Replace the function `import_url()` downloads with.
   *
   * @param fetcher Replacement fetcher, which `check_for_updates()` uses too.
   */
  void set_fetcher_for_testing(fetcher_t fetcher);

  /**
   * @brief Restore the default HTTPS fetcher.
   */
  void reset_fetcher_for_testing();
}  // namespace plugins
