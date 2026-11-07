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
  };

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
    std::vector<choice_t> choices;  ///< Accepted values of a choice option.
  };

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
    int order;  ///< Do scripts run in ascending order, undo scripts in descending order.
    bool elevated;  ///< Whether the do and undo scripts run elevated.
    scripts_t scripts;  ///< Scripts the plugin provides.
    std::vector<option_t> options;  ///< Configuration options.
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
  };

  /**
   * @brief A long-running plugin operation.
   */
  enum class action_e {
    install,  ///< Run the install script.
    uninstall  ///< Run the uninstall script.
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
    busy  ///< Another install or uninstall is still running.
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
   * @param data_dir Directory for the plugin's dependencies.
   * @return `SUNSHINE_PLUGIN_ID`, `SUNSHINE_PLUGIN_DIR`, `SUNSHINE_PLUGIN_DATA_DIR` and one variable per option.
   */
  std::map<std::string, std::string> script_env(const manifest_t &manifest, const nlohmann::json &options, const std::filesystem::path &data_dir);

  /**
   * @brief Build the command line that runs a PowerShell script.
   *
   * @param script Path of the script.
   * @return Command line invoking PowerShell non-interactively on the script.
   */
  std::string script_command(const std::filesystem::path &script);

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
   * Waits for a running install or uninstall to finish first.
   *
   * @param paths Locations to use.
   */
  void load(const paths_t &paths);

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
   * @brief Rescan the plugin directories, keeping the current state.
   */
  void rescan();

  /**
   * @brief Describe every plugin for the web UI.
   *
   * @return Array of plugin objects with manifest fields, state and the last task.
   */
  nlohmann::json list();

  /**
   * @brief Build the preparation commands of the active plugins.
   *
   * @param excluded IDs of plugins the application opted out of.
   * @return In plugin order, an app command for each active plugin with a do or undo script,
   *         followed by a connect command when the plugin also has a connect or disconnect script.
   */
  std::vector<config::prep_cmd_t> prep_cmds(const std::vector<std::string> &excluded);

  /**
   * @brief Change whether a plugin is active and the values of its options.
   *
   * @param id Plugin ID.
   * @param active Whether the plugin should be active, or `std::nullopt` to keep the current value.
   * @param options Option values, or `std::nullopt` to keep the current values.
   * @param error Receives the reason when the request is rejected.
   * @return Outcome of the request.
   */
  result_e configure(const std::string &id, std::optional<bool> active, const std::optional<nlohmann::json> &options, std::string &error);

  /**
   * @brief Start installing or uninstalling a plugin in the background.
   *
   * Uninstalling deactivates the plugin immediately.
   *
   * @param id Plugin ID.
   * @param action Operation to run.
   * @param error Receives the reason when the request is rejected.
   * @param activate Whether to activate the plugin once an install succeeds. Ignored for uninstall.
   * @return Outcome of the request.
   */
  result_e start_task(const std::string &id, action_e action, std::string &error, bool activate = false);

  /**
   * @brief Set the function called after an install or uninstall changes plugin state in the background.
   *
   * Sunshine uses it to rebuild the applications' preparation commands.
   *
   * @param callback Function to call, or an empty function to call nothing.
   */
  void set_change_callback(std::function<void()> callback);

  /**
   * @brief Block until the running install or uninstall finishes.
   */
  void wait_for_task();

  /**
   * @brief Terminate the running install or uninstall and wait for it to finish.
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
}  // namespace plugins
