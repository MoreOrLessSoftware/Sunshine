/**
 * @file src/plugins.cpp
 * @brief Definitions for installable plugins that run PowerShell scripts around a streaming session.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <ranges>
#include <set>
#include <sstream>
#include <thread>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/process/v1.hpp>
#include <curl/curl.h>
#include <zlib.h>

// local includes
#include "crypto.h"
#include "logging.h"
#include "plugins.h"
#include "utility.h"

#ifdef _WIN32
  // fopen() interprets the filename as an ANSI string on Windows, so we use _wfsopen() instead.
  #include <share.h>

  #include <boost/winapi/process.hpp>
  #include <boost/winapi/wait.hpp>

  #include "platform/windows/misc.h"
#endif

using namespace std::literals;

namespace plugins {
  namespace fs = std::filesystem;
  using json = nlohmann::json;  ///< JSON value type used by manifests and state.

  namespace {
    /**
     * @brief Name of the manifest file in each plugin directory.
     */
    constexpr auto MANIFEST_FILE = "plugin.json";

    /**
     * @brief Name of the guide to writing plugins in the directory of bundled plugins.
     */
    constexpr auto GUIDE_FILE = "PLUGIN_GUIDE.md";

    /**
     * @brief Order given to plugins whose manifest does not set one.
     */
    constexpr int DEFAULT_ORDER = 1000;

    /**
     * @brief Status of the last install or uninstall of a plugin.
     */
    struct task_info_t {
      action_e action;  ///< Operation that ran.
      task_status_e status;  ///< Progress of the operation.
      int exit_code;  ///< Exit code of the script once finished.
      std::string action_id;  ///< Action that ran when `action` is `action_e::run`.
    };

    /**
     * @brief What a check for updates found for one plugin.
     */
    struct update_check_t {
      std::string version;  ///< Version in the plugin's update feed, or empty when it couldn't be read.
      std::string error;  ///< Why the feed couldn't be read.
    };

    int run_script(const std::string &command, bool elevated, const std::map<std::string, std::string> &env, const fs::path &working_dir, const fs::path &log_file, std::stop_token stop);
    bool fetch_https(const std::string &url, std::size_t max_bytes, std::chrono::seconds timeout, std::string &body, std::string &final_url, std::string &error);

    /**
     * @brief Check whether install scripts can run yet: on Windows, whether a user is signed in when one is needed.
     *
     * @return `true` when scripts can start.
     */
    bool default_session_ready() {
#ifdef _WIN32
      return platf::user_session_available();
#else
      return true;
#endif
    }

    /**
     * @brief How long `reinstall_updated_builtins()` waits between checks for a signed-in user.
     */
    constexpr std::chrono::milliseconds DEFAULT_SESSION_POLL = 10s;

    /**
     * @brief Everything the plugin registry knows, guarded by `mutex`.
     */
    struct registry_t {
      std::mutex mutex;  ///< Guards every other member except `worker` and `reinstaller`.
      paths_t paths;  ///< Locations in use.
      std::vector<manifest_t> manifests;  ///< Discovered plugins, sorted by order.
      std::unordered_map<std::string, plugin_state_t> state;  ///< Persisted state keyed by plugin ID.
      std::unordered_map<std::string, task_info_t> tasks;  ///< Last operation keyed by plugin ID.
      bool task_running = false;  ///< Whether an install or uninstall is running.
      script_runner_t runner = run_script;  ///< Runs install and uninstall scripts.
      std::function<void()> on_change;  ///< Called after a background operation changes plugin state.
      std::function<std::vector<std::string>(const std::string &)> on_activate;  ///< Called when a plugin is activated.
      std::unordered_map<std::string, std::vector<std::string>> display_exclusions;  ///< Applications each plugin was turned off for when last activated.
      std::function<bool()> streaming;  ///< Whether a stream is running, for plugins that are `idle_only`.
      std::jthread worker;  ///< Thread running the current install or uninstall.
      std::function<bool()> session_ready = default_session_ready;  ///< Whether install scripts can run yet.
      std::chrono::milliseconds session_poll = DEFAULT_SESSION_POLL;  ///< Time between checks of `session_ready`.
      std::jthread reinstaller;  ///< Thread started by `reinstall_updated_builtins()`.
      fetcher_t fetcher = fetch_https;  ///< Downloads plugins imported from a URL.
      std::unordered_map<std::string, update_check_t> updates;  ///< Last check for updates keyed by plugin ID.
      std::chrono::milliseconds choices_timeout = DEFAULT_CHOICES_TIMEOUT;  ///< How long `list_choices()` lets a choices script run.
    };

    /**
     * @brief Return the process-wide plugin registry.
     *
     * @return Registry instance.
     */
    registry_t &registry() {
      static registry_t instance;
      return instance;
    }

    /**
     * @brief Set by `stop_proxy_streams()` so running panel streams end when Sunshine shuts down; cleared by `load()`.
     */
    std::atomic<bool> proxy_streams_stopping {false};

    /**
     * @brief Convert a path to UTF-8 text.
     *
     * @param path Path to convert.
     * @return UTF-8 encoded path.
     */
    std::string path_to_utf8(const fs::path &path) {
      const auto u8 = path.u8string();
      return {u8.begin(), u8.end()};
    }

    /**
     * @brief Read an optional string field.
     *
     * @param node Object to read from.
     * @param key Field name.
     * @param out Receives the value when present.
     * @param error Receives the reason when the field has the wrong type.
     * @return `false` when the field exists but is not a string.
     */
    bool read_string(const json &node, const char *key, std::string &out, std::string &error) {
      if (!node.contains(key)) {
        return true;
      }
      if (!node[key].is_string()) {
        error = std::format("'{}' must be a string", key);
        return false;
      }
      out = node[key].get<std::string>();
      return true;
    }

    /**
     * @brief Read an optional number field.
     *
     * @param node Object to read from.
     * @param key Field name.
     * @param out Receives the value when present.
     * @param error Receives the reason when the field has the wrong type.
     * @return `false` when the field exists but is not a number.
     */
    bool read_number(const json &node, const char *key, std::optional<double> &out, std::string &error) {
      if (!node.contains(key)) {
        return true;
      }
      if (!node[key].is_number()) {
        error = std::format("'{}' must be a number", key);
        return false;
      }
      out = node[key].get<double>();
      return true;
    }

    /**
     * @brief Check whether an option ID is acceptable.
     *
     * @param id Candidate ID.
     * @return `true` for 1 to 64 lowercase letters, digits or `_`, starting with a letter.
     */
    bool is_valid_option_id(std::string_view id) {
      if (id.empty() || id.size() > 64 || !std::islower(static_cast<unsigned char>(id.front()))) {
        return false;
      }
      return std::ranges::all_of(id, [](const char c) {
        return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_';
      });
    }

    /**
     * @brief Check whether a value is acceptable for an option.
     *
     * @param option Option to check against.
     * @param value Candidate value.
     * @return `true` when the value has the right type and lies within the allowed range or list. A choice option
     *         with a choices script also accepts any value the script could offer, since its list can change.
     */
    bool is_valid_value(const option_t &option, const json &value) {
      switch (option.type) {
        case option_type_e::boolean:
          return value.is_boolean();
        case option_type_e::number:
          {
            if (!value.is_number()) {
              return false;
            }
            const auto number = value.get<double>();
            return std::isfinite(number) && (!option.min || number >= *option.min) && (!option.max || number <= *option.max);
          }
        case option_type_e::string:
          return value.is_string();
        case option_type_e::choice:
          {
            if (!value.is_string()) {
              return false;
            }
            const auto &text = value.get_ref<const std::string &>();
            return std::ranges::any_of(option.choices, [&text](const choice_t &choice) {
                     return choice.value == text;
                   }) ||
                   (!option.choices_script.empty() && is_valid_script_choice(text));
          }
      }
      return false;
    }

    /**
     * @brief Parse the type of an option.
     *
     * @param type Type name from the manifest.
     * @return Parsed type, or `std::nullopt` when unknown.
     */
    std::optional<option_type_e> parse_option_type(std::string_view type) {
      if (type == "boolean"sv) {
        return option_type_e::boolean;
      }
      if (type == "number"sv) {
        return option_type_e::number;
      }
      if (type == "string"sv) {
        return option_type_e::string;
      }
      if (type == "choice"sv) {
        return option_type_e::choice;
      }
      return std::nullopt;
    }

    /**
     * @brief Return the manifest name of an option type.
     *
     * @param type Option type.
     * @return Type name.
     */
    std::string_view option_type_name(option_type_e type) {
      switch (type) {
        case option_type_e::boolean:
          return "boolean"sv;
        case option_type_e::number:
          return "number"sv;
        case option_type_e::choice:
          return "choice"sv;
        case option_type_e::string:
          break;
      }
      return "string"sv;
    }

    /**
     * @brief Return the name of a task status used by the web UI.
     *
     * @param status Task status.
     * @return Status name.
     */
    std::string_view task_status_name(task_status_e status) {
      switch (status) {
        case task_status_e::running:
          return "running"sv;
        case task_status_e::succeeded:
          return "succeeded"sv;
        case task_status_e::failed:
          break;
      }
      return "failed"sv;
    }

    /**
     * @brief Return the name of a plugin operation used by the web UI.
     *
     * @param action Operation.
     * @return Operation name.
     */
    std::string_view task_action_name(action_e action) {
      switch (action) {
        case action_e::install:
          return "install"sv;
        case action_e::uninstall:
          return "uninstall"sv;
        case action_e::apply:
          return "apply"sv;
        case action_e::run:
          break;
      }
      return "run"sv;
    }

    /**
     * @brief Parse one entry of a list of choices.
     *
     * @param entry A string, or an object with a string `value` and optional `label` and `group`.
     * @param out Receives the choice, labeled with its value when it has no label.
     * @param error Receives the reason when the entry is invalid.
     * @return `false` when the entry is invalid.
     */
    bool parse_choice(const json &entry, choice_t &out, std::string &error) {
      if (entry.is_string()) {
        out.value = entry.get<std::string>();
      } else if (entry.is_object() && entry.contains("value") && entry["value"].is_string()) {
        out.value = entry["value"].get<std::string>();
        if (!read_string(entry, "label", out.label, error) || !read_string(entry, "group", out.group, error)) {
          return false;
        }
      } else {
        error = "each choice must be a string or an object with a string 'value'";
        return false;
      }
      if (out.label.empty()) {
        out.label = out.value;
      }
      return true;
    }

    /**
     * @brief Parse the list of values a choice option accepts.
     *
     * @param node Array of entries accepted by `parse_choice()`.
     * @param out Receives the parsed choices.
     * @param error Receives the reason when the list is invalid.
     * @return `false` when the list is invalid.
     */
    bool parse_choices(const json &node, std::vector<choice_t> &out, std::string &error) {
      if (!node.is_array() || node.empty()) {
        error = "'choices' must be a non-empty array";
        return false;
      }
      for (const auto &entry : node) {
        choice_t choice;
        if (!parse_choice(entry, choice, error)) {
          return false;
        }
        out.emplace_back(std::move(choice));
      }
      return true;
    }

    /**
     * @brief Parse one option of a manifest.
     *
     * @param node Option object.
     * @param error Receives the reason when the option is invalid.
     * @return Parsed option, or `std::nullopt` when invalid.
     */
    std::optional<option_t> parse_option(const json &node, std::string &error) {
      if (!node.is_object()) {
        error = "each option must be an object";
        return std::nullopt;
      }

      option_t option {};
      if (!read_string(node, "id", option.id, error)) {
        return std::nullopt;
      }
      if (!is_valid_option_id(option.id)) {
        error = std::format("invalid option id '{}'", option.id);
        return std::nullopt;
      }

      std::string type;
      if (!read_string(node, "type", type, error)) {
        return std::nullopt;
      }
      const auto parsed_type = parse_option_type(type);
      if (!parsed_type) {
        error = std::format("option '{}' has unknown type '{}'", option.id, type);
        return std::nullopt;
      }
      option.type = *parsed_type;

      if (!read_string(node, "label", option.label, error) ||
          !read_string(node, "description", option.description, error) ||
          !read_number(node, "min", option.min, error) ||
          !read_number(node, "max", option.max, error) ||
          !read_number(node, "step", option.step, error)) {
        return std::nullopt;
      }
      if (node.contains("apply")) {
        if (!node["apply"].is_boolean()) {
          error = std::format("option '{}': 'apply' must be a boolean", option.id);
          return std::nullopt;
        }
        option.apply = node["apply"].get<bool>();
      }
      if (node.contains("scope")) {
        const auto &scope = node["scope"];
        if (!scope.is_string() || (scope != "host" && scope != "stream")) {
          error = std::format("option '{}': 'scope' must be \"host\" or \"stream\"", option.id);
          return std::nullopt;
        }
        option.host = scope == "host";
      }
      if (option.label.empty()) {
        option.label = option.id;
      }

      if (option.type == option_type_e::choice) {
        if (!parse_choices(node.value("choices", json::array()), option.choices, error) ||
            !read_string(node, "choices_script", option.choices_script, error)) {
          return std::nullopt;
        }
      } else if (node.contains("choices_script")) {
        error = std::format("option '{}': only a choice option can have a 'choices_script'", option.id);
        return std::nullopt;
      }

      if (node.contains("default")) {
        option.default_value = node["default"];
      } else {
        switch (option.type) {
          case option_type_e::boolean:
            option.default_value = false;
            break;
          case option_type_e::number:
            option.default_value = option.min.value_or(0.0);
            break;
          case option_type_e::string:
            option.default_value = "";
            break;
          case option_type_e::choice:
            option.default_value = option.choices.front().value;
            break;
        }
      }
      if (!is_valid_value(option, option.default_value)) {
        error = std::format("option '{}' has an invalid default", option.id);
        return std::nullopt;
      }

      return option;
    }

    /**
     * @brief Validate a script reference of a manifest.
     *
     * @param name File name from the manifest.
     * @param dir Plugin directory.
     * @param error Receives the reason when the reference is invalid.
     * @return `true` when the name is a `.ps1` file directly inside the plugin directory.
     */
    bool is_valid_script(const std::string &name, const fs::path &dir, std::string &error) {
      if (name.empty()) {
        return true;
      }
      const auto lower = boost::algorithm::to_lower_copy(name);
      if (name.find_first_of("/\\:") != std::string::npos || name.starts_with('.') || !lower.ends_with(".ps1")) {
        error = std::format("script '{}' must be a .ps1 file in the plugin directory", name);
        return false;
      }
      std::error_code ec;
      if (!fs::is_regular_file(dir / name, ec)) {
        error = std::format("script '{}' does not exist", name);
        return false;
      }
      return true;
    }

    /**
     * @brief Validate the panel file of a manifest.
     *
     * @param name File name from the manifest.
     * @param dir Plugin directory.
     * @param error Receives the reason when the reference is invalid.
     * @return `true` when the name is an existing `.html` file directly inside the plugin directory.
     */
    bool is_valid_panel_file(const std::string &name, const fs::path &dir, std::string &error) {
      const auto lower = boost::algorithm::to_lower_copy(name);
      if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name.starts_with('.') || !lower.ends_with(".html")) {
        error = std::format("panel '{}' must be an .html file in the plugin directory", name);
        return false;
      }
      std::error_code ec;
      if (!fs::is_regular_file(dir / name, ec)) {
        error = std::format("panel '{}' does not exist", name);
        return false;
      }
      return true;
    }

    /**
     * @brief Parse the actions of a manifest.
     *
     * @param node Array of action objects.
     * @param dir Plugin directory.
     * @param out Receives the parsed actions.
     * @param error Receives the reason when the list is invalid.
     * @return `false` when the list is invalid.
     */
    bool parse_actions(const json &node, const fs::path &dir, std::vector<action_t> &out, std::string &error) {
      if (!node.is_array()) {
        error = "'actions' must be an array";
        return false;
      }
      for (const auto &entry : node) {
        if (!entry.is_object()) {
          error = "each action must be an object";
          return false;
        }
        action_t action;
        if (!read_string(entry, "id", action.id, error) ||
            !read_string(entry, "label", action.label, error) ||
            !read_string(entry, "description", action.description, error) ||
            !read_string(entry, "script", action.script, error)) {
          return false;
        }
        if (!is_valid_option_id(action.id)) {
          error = std::format("invalid action id '{}'", action.id);
          return false;
        }
        if (action.script.empty()) {
          error = std::format("action '{}' needs a script", action.id);
          return false;
        }
        if (!is_valid_script(action.script, dir, error)) {
          return false;
        }
        if (std::ranges::any_of(out, [&action](const action_t &existing) {
              return existing.id == action.id;
            })) {
          error = std::format("duplicate action id '{}'", action.id);
          return false;
        }
        if (action.label.empty()) {
          action.label = action.id;
        }
        out.emplace_back(std::move(action));
      }
      return true;
    }

    /**
     * @brief Parse the client commands of a manifest.
     *
     * @param node Array of command objects.
     * @param dir Plugin directory.
     * @param elevated Whether a script runs elevated when its command doesn't say.
     * @param out Receives the parsed commands.
     * @param error Receives the reason when the list is invalid.
     * @return `false` when the list is invalid.
     */
    bool parse_client_commands(const json &node, const fs::path &dir, bool elevated, std::vector<client_command_t> &out, std::string &error) {
      if (!node.is_array()) {
        error = "'client_commands' must be an array";
        return false;
      }
      for (const auto &entry : node) {
        if (!entry.is_object()) {
          error = "each client command must be an object";
          return false;
        }
        client_command_t command;
        command.elevated = elevated;
        if (!read_string(entry, "id", command.id, error) ||
            !read_string(entry, "label", command.label, error) ||
            !read_string(entry, "icon", command.icon, error) ||
            !read_string(entry, "script", command.script, error) ||
            !read_string(entry, "status_file", command.status_file, error)) {
          return false;
        }
        if (!command.status_file.empty() &&
            (command.status_file.find_first_of("/\\:") != std::string::npos || command.status_file.starts_with('.'))) {
          error = std::format("client command '{}': 'status_file' must be a file name in the plugin's data directory", command.id);
          return false;
        }
        if (!is_valid_option_id(command.id)) {
          error = std::format("invalid client command id '{}'", command.id);
          return false;
        }
        if (std::ranges::any_of(out, [&command](const client_command_t &existing) {
              return existing.id == command.id;
            })) {
          error = std::format("duplicate client command id '{}'", command.id);
          return false;
        }
        for (const auto &[key, member] : {std::pair {"close_menu", &command.close_menu}, std::pair {"top_level", &command.top_level}, std::pair {"elevated", &command.elevated}}) {
          if (entry.contains(key)) {
            if (!entry[key].is_boolean()) {
              error = std::format("client command '{}': '{}' must be a boolean", command.id, key);
              return false;
            }
            *member = entry[key].get<bool>();
          }
        }
        if (command.label.empty()) {
          command.label = command.id;
        }

        if (entry.contains("args")) {
          if (!entry["args"].is_array() || !std::ranges::all_of(entry["args"], [](const json &arg) {
                return arg.is_string();
              })) {
            error = std::format("client command '{}': 'args' must be an array of strings", command.id);
            return false;
          }
          for (const auto &arg : entry["args"]) {
            command.args.push_back(arg.get<std::string>());
          }
        }

        if (entry.contains("programs")) {
          const auto &programs = entry["programs"];
          if (!programs.is_array() || programs.empty() || !std::ranges::all_of(programs, [](const json &program) {
                return program.is_string() && !program.get<std::string>().empty();
              })) {
            error = std::format("client command '{}': 'programs' must be a non-empty array of file names", command.id);
            return false;
          }
          command.programs = programs.get<std::vector<std::string>>();
        }

        const bool has_steps = entry.contains("steps");
        if (command.script.empty() == !has_steps) {
          error = std::format("client command '{}' needs either a script or steps", command.id);
          return false;
        }
        if (has_steps) {
          // Checked as a command of the host's own would be
          std::string reason;
          auto keys = client_commands::parse_command({{"id", command.id}, {"label", command.label}, {"type", "keys"}, {"steps", entry["steps"]}}, command.id, reason);
          if (!keys) {
            error = std::format("client command '{}': {}", command.id, reason);
            return false;
          }
          command.steps = std::move(keys->steps);
        } else if (!is_valid_script(command.script, dir, error)) {
          return false;
        }

        out.emplace_back(std::move(command));
      }
      return true;
    }

    /**
     * @brief Apply a client command's status file, which a script writes to change the command while it exists.
     *
     * @param file Status file.
     * @param command Command to change: its label and icon, when the file sets them.
     */
    void apply_status_file(const fs::path &file, client_commands::command_t &command) {
      std::error_code ec;
      if (!fs::is_regular_file(file, ec)) {
        return;
      }
      try {
        std::ifstream in {file};
        const auto status = json::parse(in);
        if (!status.is_object()) {
          return;
        }
        if (status.contains("label") && status["label"].is_string() && !status["label"].get<std::string>().empty()) {
          command.label = status["label"].get<std::string>();
        }
        if (status.contains("icon") && status["icon"].is_string()) {
          command.icon = status["icon"].get<std::string>();
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Ignoring the status file ["sv << path_to_utf8(file) << "]: "sv << e.what();
      }
    }

    /**
     * @brief Quote an argument for a command line.
     *
     * @param arg Argument.
     * @return The argument in double quotes, with embedded quotes escaped.
     */
    std::string quote_arg(const std::string &arg) {
      std::string quoted = "\"";
      for (const char c : arg) {
        if (c == '"') {
          quoted += "\\\"";
        } else {
          quoted += c;
        }
      }
      return quoted + "\"";
    }

    /**
     * @brief Parse the panel of a manifest.
     *
     * @param node Panel object.
     * @param dir Plugin directory.
     * @param error Receives the reason when the panel is invalid.
     * @return Parsed panel, or `std::nullopt` when invalid.
     */
    std::optional<panel_t> parse_panel(const json &node, const fs::path &dir, std::string &error) {
      if (!node.is_object()) {
        error = "'panel' must be an object";
        return std::nullopt;
      }
      panel_t panel;
      if (!read_string(node, "file", panel.file, error) || !is_valid_panel_file(panel.file, dir, error)) {
        return std::nullopt;
      }
      if (node.contains("port")) {
        if (!node["port"].is_number_integer() || node["port"].get<int>() < 1 || node["port"].get<int>() > 65535) {
          error = "panel 'port' must be an integer from 1 to 65535";
          return std::nullopt;
        }
        panel.port = node["port"].get<int>();
      }
      if (node.contains("height") && node["height"] == "auto") {
        panel.fit = true;
      } else if (node.contains("height")) {
        if (!node["height"].is_number_integer() || node["height"].get<int>() < 100 || node["height"].get<int>() > 2000) {
          error = "panel 'height' must be \"auto\" or an integer from 100 to 2000";
          return std::nullopt;
        }
        panel.height = node["height"].get<int>();
      }
      return panel;
    }

    /**
     * @brief Read and parse the manifest in a directory.
     *
     * @param dir Plugin directory.
     * @param builtin Whether the plugin ships with Sunshine.
     * @return Parsed manifest, or `std::nullopt` when missing or invalid.
     */
    std::optional<manifest_t> read_manifest(const fs::path &dir, bool builtin) {
      std::error_code ec;
      const auto file = dir / MANIFEST_FILE;
      if (!fs::is_regular_file(file, ec)) {
        return std::nullopt;
      }

      std::string error;
      try {
        std::ifstream in(file);
        const auto node = json::parse(in);
        if (auto manifest = parse_manifest(node, dir, builtin, error)) {
          return manifest;
        }
      } catch (const std::exception &e) {
        error = e.what();
      }
      BOOST_LOG(warning) << "Ignoring plugin ["sv << path_to_utf8(dir) << "]: "sv << error;
      return std::nullopt;
    }

    /**
     * @brief Find a discovered plugin. The caller must hold the registry mutex.
     *
     * @param reg Registry to search.
     * @param id Plugin ID.
     * @return Matching manifest, or `nullptr`.
     */
    const manifest_t *find_manifest(const registry_t &reg, const std::string &id) {
      const auto it = std::ranges::find_if(reg.manifests, [&id](const manifest_t &manifest) {
        return manifest.id == id;
      });
      return it == reg.manifests.end() ? nullptr : &*it;
    }

    /**
     * @brief Return the option values of a plugin merged with its defaults. The caller must hold the registry mutex.
     *
     * @param reg Registry holding the state.
     * @param manifest Plugin whose options to read.
     * @return Object holding a value for every declared option.
     */
    json effective_options(const registry_t &reg, const manifest_t &manifest) {
      std::string error;
      const auto it = reg.state.find(manifest.id);
      const auto &stored = it == reg.state.end() ? json::object() : it->second.options;
      return *normalize_options(manifest, stored, false, error);
    }

    /**
     * @brief Return one client's overrides of a plugin. The caller must hold the registry mutex.
     *
     * @param reg Registry holding the state.
     * @param id Plugin ID.
     * @param client_uuid Client UUID, or empty for none.
     * @return The client's `{"active": false, "options": {...}}` entry, or `null` when it has none.
     */
    json client_entry(const registry_t &reg, const std::string &id, const std::string &client_uuid) {
      const auto it = reg.state.find(id);
      if (client_uuid.empty() || it == reg.state.end()) {
        return nullptr;
      }
      const auto entry = it->second.clients.find(client_uuid);
      return entry == it->second.clients.end() || !entry->is_object() ? json(nullptr) : *entry;
    }

    /**
     * @brief Check whether a client turned a plugin off. The caller must hold the registry mutex.
     *
     * @param reg Registry holding the state.
     * @param id Plugin ID.
     * @param client_uuid Client UUID, or empty for none.
     * @return `true` when the client's entry sets `"active": false`.
     */
    bool off_for_client(const registry_t &reg, const std::string &id, const std::string &client_uuid) {
      const auto entry = client_entry(reg, id, client_uuid);
      return entry.is_object() && entry.contains("active") && entry["active"] == false;
    }

    /**
     * @brief Resolve a plugin's options for an application and a client. The caller must hold the registry mutex.
     *
     * @param reg Registry holding the state.
     * @param manifest Plugin whose options to resolve.
     * @param app_overrides The application's `plugin-options`, keyed by plugin ID, or `null`.
     * @param client_uuid Client UUID, or empty for none.
     * @return An object holding a value for every declared option.
     */
    json stream_options(const registry_t &reg, const manifest_t &manifest, const json &app_overrides, const std::string &client_uuid) {
      const json app = app_overrides.is_object() && app_overrides.contains(manifest.id) ? app_overrides[manifest.id] : json(nullptr);
      const auto entry = client_entry(reg, manifest.id, client_uuid);
      const json client = entry.is_object() && entry.contains("options") ? entry["options"] : json(nullptr);
      return resolve_options(manifest, effective_options(reg, manifest), app, client);
    }

    /**
     * @brief Check whether new values change an option marked `apply`.
     *
     * @param manifest Plugin declaring the options.
     * @param current Normalized current values.
     * @param updated Normalized new values.
     * @return `true` when at least one such option differs.
     */
    bool apply_option_changed(const manifest_t &manifest, const json &current, const json &updated) {
      return std::ranges::any_of(manifest.options, [&current, &updated](const option_t &option) {
        return option.apply && current.at(option.id) != updated.at(option.id);
      });
    }

    /**
     * @brief Check whether a plugin's apply script is running.
     *
     * @param reg Registry holding the tasks.
     * @param id Plugin ID.
     * @return `true` while new options are waiting for the script to finish.
     */
    bool is_applying(const registry_t &reg, const std::string &id) {
      const auto it = reg.tasks.find(id);
      return it != reg.tasks.end() && it->second.action == action_e::apply && it->second.status == task_status_e::running;
    }

    /**
     * @brief Return the directory holding a plugin's dependencies.
     *
     * @param paths Locations in use.
     * @param id Plugin ID.
     * @return Data directory of the plugin.
     */
    fs::path plugin_data_dir(const paths_t &paths, const std::string &id) {
      return paths.data_dir / id;
    }

    /**
     * @brief Return the file receiving install and uninstall output for a plugin.
     *
     * @param paths Locations in use.
     * @param id Plugin ID.
     * @return Log file of the plugin.
     */
    fs::path plugin_log_file(const paths_t &paths, const std::string &id) {
      return paths.data_dir / (id + ".log");
    }

    /**
     * @brief Open a file for writing so other readers can open it at the same time.
     *
     * @param path File to open.
     * @param mode `fopen()` mode.
     * @return File handle, or `nullptr` on failure.
     */
    FILE *open_shared(const fs::path &path, const wchar_t *mode) {
#ifdef _WIN32
      return _wfsopen(path.wstring().c_str(), mode, _SH_DENYNO);
#else
      std::string narrow_mode;
      for (const wchar_t *c = mode; *c; ++c) {
        narrow_mode += static_cast<char>(*c);
      }
      return std::fopen(path.c_str(), narrow_mode.c_str());
#endif
    }

    /**
     * @brief Run a script with `platf::run_command()` and wait for it.
     *
     * @param command Command line to execute.
     * @param elevated Whether to run with elevated privileges.
     * @param env Variables added to Sunshine's environment.
     * @param working_dir Directory the script starts in.
     * @param log_file File that receives the script output.
     * @param stop Requests the script be terminated early.
     * @return Exit code of the script, or -1 when it could not run or was stopped.
     */
    int run_script(const std::string &command, bool elevated, const std::map<std::string, std::string> &env, const fs::path &working_dir, const fs::path &log_file, std::stop_token stop) {
      auto proc_env = boost::this_process::environment();
      for (const auto &[name, value] : env) {
        proc_env[name] = value;
      }

      std::unique_ptr<FILE, decltype(&std::fclose)> file {open_shared(log_file, L"a"), &std::fclose};
      boost::filesystem::path dir {working_dir.native()};
      std::error_code ec;
      auto child = platf::run_command(elevated, false, command, dir, proc_env, file.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << command << "]: System: "sv << ec.message();
        return -1;
      }

      while (child.running(ec) && !ec) {
        if (stop.stop_requested()) {
          BOOST_LOG(warning) << "Stopping ["sv << command << ']';
#ifdef _WIN32
          // child.terminate() closes the process handle without waiting for the asynchronous exit, so
          // terminate and wait here: the script must release its log and working directory before returning.
          if (boost::winapi::TerminateProcess(child.native_handle(), 1)) {
            boost::winapi::WaitForSingleObject(child.native_handle(), 5000);
          }
#endif
          child.terminate(ec);
          return -1;
        }
        std::this_thread::sleep_for(100ms);
      }
      child.wait(ec);
      return child.exit_code();
    }

    /**
     * @brief Return the applications a plugin was turned off for when it was last activated.
     *
     * The caller must hold the registry mutex.
     *
     * @param reg Registry to read.
     * @param id Plugin ID.
     * @return Array of application names, empty when there are none.
     */
    json display_exclusions_of(const registry_t &reg, const std::string &id) {
      const auto it = reg.display_exclusions.find(id);
      return it == reg.display_exclusions.end() ? json::array() : json(it->second);
    }

    /**
     * @brief Call the activation callback for a plugin and remember the applications it changed.
     *
     * The caller must not hold the registry mutex, since the callback reads the registry.
     *
     * @param reg Registry to update.
     * @param id ID of the plugin that was just activated.
     */
    void notify_activation(registry_t &reg, const std::string &id) {
      std::function<std::vector<std::string>(const std::string &)> callback;
      {
        std::scoped_lock lock {reg.mutex};
        callback = reg.on_activate;
      }
      auto changed = callback ? callback(id) : std::vector<std::string> {};
      std::scoped_lock lock {reg.mutex};
      if (changed.empty()) {
        reg.display_exclusions.erase(id);
      } else {
        reg.display_exclusions[id] = std::move(changed);
      }
    }

    /**
     * @brief Run an install or uninstall script and record the outcome.
     *
     * @param id Plugin ID.
     * @param action Operation to run.
     * @param manifest Plugin running the script.
     * @param env Variables passed to the script.
     * @param paths Locations in use.
     * @param runner Function that runs the script.
     * @param activate Whether to activate the plugin once an install succeeds.
     * @param action_id Action to run when `action` is `action_e::run`.
     * @param pending_options Options to save once an apply succeeds.
     * @param stop Requests the script be terminated early.
     */
    void run_task(const std::string &id, action_e action, const manifest_t &manifest, const std::map<std::string, std::string> &env, const paths_t &paths, const script_runner_t &runner, bool activate, const std::string &action_id, const std::optional<json> &pending_options, std::stop_token stop) {
      std::string script;
      std::string heading;
      switch (action) {
        case action_e::install:
          script = manifest.scripts.install;
          heading = std::format("Installing {} {}", manifest.name, manifest.version);
          break;
        case action_e::uninstall:
          script = manifest.scripts.uninstall;
          heading = std::format("Uninstalling {} {}", manifest.name, manifest.version);
          break;
        case action_e::run:
          for (const auto &candidate : manifest.actions) {
            if (candidate.id == action_id) {
              script = candidate.script;
              heading = std::format("{}: {}", manifest.name, candidate.label);
            }
          }
          break;
        case action_e::apply:
          script = manifest.scripts.apply;
          heading = std::format("Applying options for {}", manifest.name);
          break;
      }
      const auto data_dir = plugin_data_dir(paths, id);
      const auto log_file = plugin_log_file(paths, id);

      std::error_code ec;
      fs::create_directories(data_dir, ec);

      if (std::unique_ptr<FILE, decltype(&std::fclose)> file {open_shared(log_file, L"w"), &std::fclose}; file) {
        std::fputs((heading + "\n").c_str(), file.get());
      }

      int exit_code = 0;
      if (!script.empty()) {
        const auto command = script_command(manifest.dir / script);
        BOOST_LOG(info) << "Running plugin script ["sv << command << ']';
        exit_code = runner(command, true, env, data_dir, log_file, stop);
      }
      const bool success = exit_code == 0;

      if (success && action == action_e::uninstall) {
        fs::remove_all(data_dir, ec);
      }

      if (std::unique_ptr<FILE, decltype(&std::fclose)> file {open_shared(log_file, L"a"), &std::fclose}; file) {
        std::fputs(std::format("\n{} (exit code {})\n", success ? "Completed"sv : "Failed"sv, exit_code).c_str(), file.get());
      }

      const auto what = action == action_e::run ? std::format("action '{}'", action_id) : std::string {task_action_name(action)};
      if (success) {
        BOOST_LOG(info) << "Plugin ["sv << id << "] "sv << what << " succeeded"sv;
      } else {
        BOOST_LOG(error) << "Plugin ["sv << id << "] "sv << what << " failed with exit code ["sv << exit_code << ']';
      }

      auto &reg = registry();
      std::function<void()> on_change;
      bool activated = false;
      {
        std::scoped_lock lock {reg.mutex};
        if (success && action != action_e::run) {
          auto &state = reg.state[id];
          if (action == action_e::apply) {
            state.options = *pending_options;
          } else {
            const bool was_active = state.active;
            state.installed = action == action_e::install;
            state.installed_before = state.installed_before || state.installed;
            state.active = state.installed && (state.active || activate);
            state.installed_version = state.installed ? manifest.version : std::string {};
            activated = state.active && !was_active;
          }
          save_state(paths.state_file, reg.state);
          on_change = reg.on_change;
        }
        if (!activated) {
          reg.tasks[id] = {action, success ? task_status_e::succeeded : task_status_e::failed, exit_code, action_id};
          reg.task_running = false;
        }
      }

      // The task is reported finished only after this, so the web UI sees the applications it changed.
      if (activated) {
        notify_activation(reg, id);
        std::scoped_lock lock {reg.mutex};
        reg.tasks[id] = {action, task_status_e::succeeded, exit_code, action_id};
        reg.task_running = false;
      }

      // Called without the lock since the callback reads the plugin registry.
      if (on_change) {
        on_change();
      }
    }

    /**
     * @brief Mark a plugin operation as running and start it on the worker thread.
     *
     * The caller holds the registry lock and has checked that no other operation is running.
     *
     * @param reg Registry to update.
     * @param id Plugin ID.
     * @param action Operation to run.
     * @param manifest Plugin running the script.
     * @param env Variables passed to the script.
     * @param activate Whether to activate the plugin once an install succeeds.
     * @param action_id Action to run when `action` is `action_e::run`.
     * @param pending_options Options to save once an apply succeeds.
     */
    void launch_task(registry_t &reg, const std::string &id, action_e action, const manifest_t &manifest, const std::map<std::string, std::string> &env, bool activate, const std::string &action_id, std::optional<json> pending_options) {
      reg.task_running = true;
      reg.tasks[id] = {action, task_status_e::running, 0, action == action_e::run ? action_id : std::string {}};

      // The previous worker has already finished since task_running was false.
      if (reg.worker.joinable()) {
        reg.worker.join();
      }

      reg.worker = std::jthread([id, action, manifest, env, paths = reg.paths, runner = reg.runner, activate, action_id, pending_options = std::move(pending_options)](std::stop_token stop) {
        run_task(id, action, manifest, env, paths, runner, activate && action == action_e::install, action_id, pending_options, stop);
      });
    }

    /**
     * @brief Find the persisted state of a plugin.
     *
     * @param reg Registry to search.
     * @param id Plugin ID.
     * @return The state, or `nullptr` when the plugin has none.
     */
    const plugin_state_t *find_state(const registry_t &reg, const std::string &id) {
      const auto it = reg.state.find(id);
      return it == reg.state.end() ? nullptr : &it->second;
    }

    /**
     * @brief Check whether a plugin runs for streams and offers client commands.
     *
     * @param reg Registry holding the plugin's state.
     * @param manifest Plugin to check.
     * @return `true` when the plugin is installed, active and doesn't need a reinstall.
     */
    bool runs_for_streams(const registry_t &reg, const manifest_t &manifest) {
      const auto *state = find_state(reg, manifest.id);
      return state && state->active && state->installed && !needs_reinstall(manifest, *state);
    }

    /**
     * @brief Wait for a time unless a stop is requested first.
     *
     * @param stop Requests the wait to end early.
     * @param duration Longest time to wait.
     * @return `false` when a stop was requested.
     */
    bool sleep_unless_stopped(std::stop_token stop, std::chrono::milliseconds duration) {
      std::mutex mutex;
      std::condition_variable_any cv;
      std::unique_lock lock {mutex};
      cv.wait_for(lock, stop, duration, [] {
        return false;
      });
      return !stop.stop_requested();
    }

    /**
     * @brief Find the first bundled plugin that needs a reinstall and hasn't been tried yet.
     *
     * @param reg Registry to search.
     * @param attempted IDs of the plugins already tried.
     * @return The plugin, or `nullptr` when there is none.
     */
    const manifest_t *next_builtin_reinstall(const registry_t &reg, const std::set<std::string> &attempted) {
      for (const auto &manifest : reg.manifests) {
        const auto *state = find_state(reg, manifest.id);
        if (manifest.builtin && state && needs_reinstall(manifest, *state) && !attempted.contains(manifest.id)) {
          return &manifest;
        }
      }
      return nullptr;
    }

    /**
     * @brief Find the first bundled plugin that is on by default, was never installed, and hasn't been tried yet.
     *
     * @param reg Registry to search.
     * @param attempted IDs of the plugins already tried.
     * @return The plugin, or `nullptr` when there is none.
     */
    const manifest_t *next_default_install(const registry_t &reg, const std::set<std::string> &attempted) {
      for (const auto &manifest : reg.manifests) {
        const auto *state = find_state(reg, manifest.id);
        if (manifest.builtin && manifest.default_active && (!state || (!state->installed && !state->installed_before)) && !attempted.contains(manifest.id)) {
          return &manifest;
        }
      }
      return nullptr;
    }

    /**
     * @brief Body of the thread started by `reinstall_updated_builtins()`.
     *
     * @param stop Requests the thread to end; a reinstall it already started keeps running.
     */
    void reinstall_builtins(std::stop_token stop) {
      auto &reg = registry();
      std::function<bool()> session_ready;
      std::chrono::milliseconds poll;
      {
        std::scoped_lock lock {reg.mutex};
        if (!next_builtin_reinstall(reg, {}) && !next_default_install(reg, {})) {
          return;
        }
        session_ready = reg.session_ready;
        poll = reg.session_poll;
      }

      if (!session_ready()) {
        BOOST_LOG(info) << "Waiting for a user to sign in before reinstalling updated plugins"sv;
        do {
          if (!sleep_unless_stopped(stop, poll)) {
            return;
          }
        } while (!session_ready());
      }

      std::set<std::string> attempted;
      while (!stop.stop_requested()) {
        {
          std::scoped_lock lock {reg.mutex};
          if (!reg.task_running) {
            const auto *manifest = next_builtin_reinstall(reg, attempted);
            bool first_install = false;
            if (!manifest) {
              manifest = next_default_install(reg, attempted);
              first_install = true;
            }
            if (!manifest) {
              return;
            }
            attempted.insert(manifest->id);
            if (first_install) {
              // Only once: a user who uninstalls it keeps it uninstalled, and a failed install waits for the web UI
              BOOST_LOG(info) << "Installing plugin ["sv << manifest->id << "] since it's on by default"sv;
              reg.state[manifest->id].installed_before = true;
              save_state(reg.paths.state_file, reg.state);
            } else {
              BOOST_LOG(info) << "Reinstalling plugin ["sv << manifest->id << "] since it changed from version ["sv
                              << find_state(reg, manifest->id)->installed_version << "] to ["sv << manifest->version << ']';
            }
            const auto env = script_env(*manifest, effective_options(reg, *manifest), plugin_data_dir(reg.paths, manifest->id));
            launch_task(reg, manifest->id, action_e::install, *manifest, env, first_install, {}, std::nullopt);
          }
        }
        sleep_unless_stopped(stop, 100ms);
      }
    }

    /**
     * @brief Stop the thread started by `reinstall_updated_builtins()` and wait for it to end.
     *
     * A reinstall it already started keeps running.
     */
    void stop_reinstaller() {
      auto &reg = registry();
      std::jthread reinstaller;
      {
        std::scoped_lock lock {reg.mutex};
        reinstaller = std::move(reg.reinstaller);
      }
      if (reinstaller.joinable()) {
        reinstaller.request_stop();
        reinstaller.join();
      }
    }
  }  // namespace

  bool needs_reinstall(const manifest_t &manifest, const plugin_state_t &state) {
    return state.installed && !state.installed_version.empty() && state.installed_version != manifest.version;
  }

  void reinstall_updated_builtins() {
    stop_reinstaller();
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.reinstaller = std::jthread(reinstall_builtins);
  }

  bool is_valid_id(std::string_view id) {
    if (id.empty() || id.size() > 64) {
      return false;
    }
    const auto first = static_cast<unsigned char>(id.front());
    if (!std::islower(first) && !std::isdigit(first)) {
      return false;
    }
    return std::ranges::all_of(id, [](const char c) {
      const auto u = static_cast<unsigned char>(c);
      return std::islower(u) || std::isdigit(u) || c == '.' || c == '_' || c == '-';
    });
  }

  std::optional<manifest_t> parse_manifest(const nlohmann::json &node, const std::filesystem::path &dir, bool builtin, std::string &error) {
    if (!node.is_object()) {
      error = "manifest must be an object";
      return std::nullopt;
    }

    manifest_t manifest {};
    manifest.dir = dir;
    manifest.builtin = builtin;
    manifest.order = DEFAULT_ORDER;

    if (!read_string(node, "id", manifest.id, error) ||
        !read_string(node, "name", manifest.name, error) ||
        !read_string(node, "version", manifest.version, error) ||
        !read_string(node, "description", manifest.description, error) ||
        !read_string(node, "author", manifest.author, error) ||
        !read_string(node, "homepage", manifest.homepage, error) ||
        !read_string(node, "notice", manifest.notice, error) ||
        !read_string(node, "notice_url", manifest.notice_url, error)) {
      return std::nullopt;
    }
    if (!manifest.notice_url.empty() && !manifest.notice_url.starts_with("https://")) {
      error = "'notice_url' must be an https:// address";
      return std::nullopt;
    }
    if (!is_valid_id(manifest.id)) {
      error = std::format("invalid plugin id '{}'", manifest.id);
      return std::nullopt;
    }
    if (manifest.name.empty()) {
      manifest.name = manifest.id;
    }

    if (node.contains("order")) {
      if (!node["order"].is_number_integer()) {
        error = "'order' must be an integer";
        return std::nullopt;
      }
      manifest.order = node["order"].get<int>();
    }
    if (node.contains("elevated")) {
      if (!node["elevated"].is_boolean()) {
        error = "'elevated' must be a boolean";
        return std::nullopt;
      }
      manifest.elevated = node["elevated"].get<bool>();
    }
    if (node.contains("manages_display")) {
      if (!node["manages_display"].is_boolean()) {
        error = "'manages_display' must be a boolean";
        return std::nullopt;
      }
      manifest.manages_display = node["manages_display"].get<bool>();
    }
    if (node.contains("idle_only")) {
      if (!node["idle_only"].is_boolean()) {
        error = "'idle_only' must be a boolean";
        return std::nullopt;
      }
      manifest.idle_only = node["idle_only"].get<bool>();
    }
    if (node.contains("default_active")) {
      if (!node["default_active"].is_boolean()) {
        error = "'default_active' must be a boolean";
        return std::nullopt;
      }
      manifest.default_active = node["default_active"].get<bool>();
    }

    if (node.contains("scripts")) {
      const auto &scripts = node["scripts"];
      if (!scripts.is_object()) {
        error = "'scripts' must be an object";
        return std::nullopt;
      }
      if (!read_string(scripts, "install", manifest.scripts.install, error) ||
          !read_string(scripts, "uninstall", manifest.scripts.uninstall, error) ||
          !read_string(scripts, "do", manifest.scripts.do_script, error) ||
          !read_string(scripts, "undo", manifest.scripts.undo_script, error) ||
          !read_string(scripts, "connect", manifest.scripts.connect_script, error) ||
          !read_string(scripts, "disconnect", manifest.scripts.disconnect_script, error) ||
          !read_string(scripts, "apply", manifest.scripts.apply, error) ||
          !read_string(scripts, "status", manifest.scripts.status, error)) {
        return std::nullopt;
      }
      for (const auto *script : {&manifest.scripts.install, &manifest.scripts.uninstall, &manifest.scripts.do_script, &manifest.scripts.undo_script, &manifest.scripts.connect_script, &manifest.scripts.disconnect_script, &manifest.scripts.apply, &manifest.scripts.status}) {
        if (!is_valid_script(*script, dir, error)) {
          return std::nullopt;
        }
      }
    }

    if (node.contains("options")) {
      const auto &options = node["options"];
      if (!options.is_array()) {
        error = "'options' must be an array";
        return std::nullopt;
      }
      for (const auto &entry : options) {
        auto option = parse_option(entry, error);
        if (!option) {
          return std::nullopt;
        }
        if (std::ranges::any_of(manifest.options, [&option](const option_t &existing) {
              return existing.id == option->id;
            })) {
          error = std::format("duplicate option id '{}'", option->id);
          return std::nullopt;
        }
        if (!is_valid_script(option->choices_script, dir, error)) {
          return std::nullopt;
        }
        if (option->apply && manifest.scripts.apply.empty()) {
          error = std::format("option '{}' sets 'apply' but the plugin has no apply script", option->id);
          return std::nullopt;
        }
        manifest.options.emplace_back(std::move(*option));
      }
    }

    if (node.contains("actions") && !parse_actions(node["actions"], dir, manifest.actions, error)) {
      return std::nullopt;
    }
    if (node.contains("client_commands") && !parse_client_commands(node["client_commands"], dir, manifest.elevated, manifest.client_commands, error)) {
      return std::nullopt;
    }
    if (!read_string(node, "icon", manifest.icon, error)) {
      return std::nullopt;
    }
    if (node.contains("panel")) {
      manifest.panel = parse_panel(node["panel"], dir, error);
      if (!manifest.panel) {
        return std::nullopt;
      }
    }

    return manifest;
  }

  std::vector<manifest_t> discover(const std::filesystem::path &builtin_dir, const std::filesystem::path &custom_dir) {
    std::vector<manifest_t> manifests;

    for (const auto &[root, builtin] : {std::pair {builtin_dir, true}, std::pair {custom_dir, false}}) {
      std::error_code ec;
      if (root.empty() || !fs::is_directory(root, ec)) {
        continue;
      }

      std::vector<fs::path> dirs;
      for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory(ec)) {
          dirs.emplace_back(fs::absolute(entry.path(), ec));
        }
      }
      std::ranges::sort(dirs);

      for (const auto &dir : dirs) {
        auto manifest = read_manifest(dir, builtin);
        if (!manifest) {
          continue;
        }
        if (std::ranges::any_of(manifests, [&manifest](const manifest_t &existing) {
              return existing.id == manifest->id;
            })) {
          BOOST_LOG(warning) << "Ignoring plugin ["sv << path_to_utf8(dir) << "]: id '"sv << manifest->id << "' is already in use"sv;
          continue;
        }
        manifests.emplace_back(std::move(*manifest));
      }
    }

    std::ranges::sort(manifests, [](const manifest_t &a, const manifest_t &b) {
      return std::tie(a.order, a.id) < std::tie(b.order, b.id);
    });
    return manifests;
  }

  std::optional<nlohmann::json> normalize_options(const manifest_t &manifest, const nlohmann::json &input, bool strict, std::string &error) {
    if (!input.is_object()) {
      if (strict) {
        error = "options must be an object";
        return std::nullopt;
      }
    }

    json result = json::object();
    for (const auto &option : manifest.options) {
      if (input.is_object() && input.contains(option.id)) {
        const auto &value = input[option.id];
        if (is_valid_value(option, value)) {
          result[option.id] = value;
          continue;
        }
        if (strict) {
          error = std::format("invalid value for option '{}'", option.id);
          return std::nullopt;
        }
      }
      result[option.id] = option.default_value;
    }
    return result;
  }

  bool is_overridable(const option_t &option) {
    return !option.apply && !option.host;
  }

  std::optional<nlohmann::json> normalize_overrides(const manifest_t &manifest, const nlohmann::json &input, bool strict, std::string &error) {
    json result = json::object();
    if (input.is_null()) {
      return result;
    }
    if (!input.is_object()) {
      if (strict) {
        error = "options must be an object";
        return std::nullopt;
      }
      return result;
    }

    for (const auto &[key, value] : input.items()) {
      const auto option = std::ranges::find_if(manifest.options, [&key](const option_t &candidate) {
        return candidate.id == key;
      });
      std::string problem;
      if (option == manifest.options.end()) {
        problem = std::format("plugin '{}' has no option '{}'", manifest.id, key);
      } else if (!is_overridable(*option)) {
        problem = std::format("option '{}' of plugin '{}' can't be overridden", key, manifest.id);
      } else if (!is_valid_value(*option, value)) {
        problem = std::format("invalid value for option '{}'", key);
      } else {
        result[key] = value;
        continue;
      }
      if (strict) {
        error = std::move(problem);
        return std::nullopt;
      }
    }
    return result;
  }

  nlohmann::json resolve_options(const manifest_t &manifest, const nlohmann::json &options, const nlohmann::json &app_overrides, const nlohmann::json &client_overrides) {
    std::string error;
    auto result = *normalize_options(manifest, options, false, error);
    for (const auto *layer : {&app_overrides, &client_overrides}) {
      for (const auto &[key, value] : normalize_overrides(manifest, *layer, false, error)->items()) {
        result[key] = value;
      }
    }
    return result;
  }

  std::string option_env_name(std::string_view option_id) {
    std::string name = "SUNSHINE_PLUGIN_OPT_";
    for (const char c : option_id) {
      const auto u = static_cast<unsigned char>(c);
      name += std::isalnum(u) ? static_cast<char>(std::toupper(u)) : '_';
    }
    return name;
  }

  std::string option_env_value(const nlohmann::json &value) {
    if (value.is_boolean()) {
      return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
      return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number()) {
      const auto number = value.get<double>();
      if (std::trunc(number) == number && std::abs(number) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(number));
      }
      return std::format("{}", number);
    }
    if (value.is_string()) {
      return value.get<std::string>();
    }
    return value.dump();
  }

  std::map<std::string, std::string> script_env(const manifest_t &manifest, const nlohmann::json &options, const std::filesystem::path &data_dir) {
    std::map<std::string, std::string> env;
    env["SUNSHINE_PLUGIN_ID"] = manifest.id;
    env["SUNSHINE_PLUGIN_DIR"] = path_to_utf8(fs::path(manifest.dir).make_preferred());
    env["SUNSHINE_PLUGIN_DATA_DIR"] = path_to_utf8(fs::path(data_dir).make_preferred());
    for (const auto &option : manifest.options) {
      env[option_env_name(option.id)] = option_env_value(options.contains(option.id) ? options[option.id] : option.default_value);
    }
    return env;
  }

  std::string script_command(const std::filesystem::path &script) {
#ifdef _WIN32
    constexpr auto shell = "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass"sv;
#else
    constexpr auto shell = "pwsh -NoLogo -NoProfile -NonInteractive"sv;
#endif
    return std::format("{} -File \"{}\"", shell, path_to_utf8(fs::path(script).make_preferred()));
  }

  std::unordered_map<std::string, plugin_state_t> load_state(const std::filesystem::path &state_file) {
    std::unordered_map<std::string, plugin_state_t> state;

    std::error_code ec;
    if (!fs::is_regular_file(state_file, ec)) {
      return state;
    }

    try {
      std::ifstream in(state_file);
      const auto node = json::parse(in);
      for (const auto &[id, entry] : node.at("plugins").items()) {
        if (!entry.is_object()) {
          continue;
        }
        plugin_state_t plugin;
        plugin.installed = entry.value("installed", false);
        plugin.active = entry.value("active", false) && plugin.installed;
        if (entry.contains("options") && entry["options"].is_object()) {
          plugin.options = entry["options"];
        }
        if (plugin.installed && entry.contains("installed_version") && entry["installed_version"].is_string()) {
          plugin.installed_version = entry["installed_version"].get<std::string>();
        }
        if (entry.contains("origin") && entry["origin"].is_string()) {
          plugin.origin = entry["origin"].get<std::string>();
        }
        if (entry.contains("clients") && entry["clients"].is_object()) {
          plugin.clients = entry["clients"];
        }
        plugin.installed_before = entry.value("installed_before", false) || plugin.installed;
        state[id] = std::move(plugin);
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Couldn't read plugin state ["sv << path_to_utf8(state_file) << "]: "sv << e.what();
      state.clear();
    }
    return state;
  }

  bool save_state(const std::filesystem::path &state_file, const std::unordered_map<std::string, plugin_state_t> &state) {
    json plugins = json::object();
    for (const auto &[id, plugin] : state) {
      plugins[id] = {
        {"installed", plugin.installed},
        {"active", plugin.active},
        {"options", plugin.options},
        {"installed_version", plugin.installed_version},
        {"origin", plugin.origin},
        {"clients", plugin.clients},
        {"installed_before", plugin.installed_before}
      };
    }

    std::error_code ec;
    if (state_file.has_parent_path()) {
      fs::create_directories(state_file.parent_path(), ec);
    }

    auto tmp = state_file;
    tmp += ".tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      out << json {{"plugins", plugins}}.dump(4);
      if (!out) {
        BOOST_LOG(error) << "Couldn't write plugin state ["sv << path_to_utf8(tmp) << ']';
        return false;
      }
    }
    fs::rename(tmp, state_file, ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't write plugin state ["sv << path_to_utf8(state_file) << "]: "sv << ec.message();
      return false;
    }
    return true;
  }

  paths_t default_paths() {
    const auto appdata = platf::appdata();
    return {
      fs::path(SUNSHINE_ASSETS_DIR) / "plugins",
      appdata / "plugins",
      appdata / "plugin-data",
      appdata / "plugins.json"
    };
  }

  void load(const paths_t &paths) {
    stop_reinstaller();
    wait_for_task();
    proxy_streams_stopping = false;

    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.paths = paths;
    // So there's somewhere to put custom plugins; it inherits the config directory's permissions
    if (std::error_code ec; !paths.custom_dir.empty() && !fs::create_directories(paths.custom_dir, ec) && ec) {
      BOOST_LOG(warning) << "Couldn't create the custom plugins directory: "sv << ec.message();
    }
    reg.manifests = discover(paths.builtin_dir, paths.custom_dir);
    reg.state = load_state(paths.state_file);
    reg.tasks.clear();
    reg.updates.clear();
    reg.display_exclusions.clear();

    // State written before installed versions were recorded: take the current version to be the installed one,
    // rather than run every install script again after an update.
    bool recorded = false;
    for (const auto &manifest : reg.manifests) {
      if (const auto it = reg.state.find(manifest.id); !manifest.version.empty() && it != reg.state.end() && it->second.installed && it->second.installed_version.empty()) {
        it->second.installed_version = manifest.version;
        recorded = true;
      }
    }
    if (recorded) {
      save_state(paths.state_file, reg.state);
    }

    for (const auto &manifest : reg.manifests) {
      const auto *state = find_state(reg, manifest.id);
      const bool active = state && state->active;
      const bool reinstall = state && needs_reinstall(manifest, *state);
      BOOST_LOG(info) << "Found plugin ["sv << manifest.id << ' ' << manifest.version << ']' << (active ? " (active)"sv : ""sv)
                      << (reinstall ? std::format(" (installed version {}, needs a reinstall)", state->installed_version) : std::string {});
    }
  }

  std::unique_ptr<platf::deinit_t> init() {
    /**
     * @brief Ends running panel streams and stops a running install or uninstall when Sunshine exits.
     */
    class deinit_t: public platf::deinit_t {
    public:
      ~deinit_t() override {
        stop_proxy_streams();
        cancel_task();
      }
    };

    load(default_paths());
    return std::make_unique<deinit_t>();
  }

  paths_t current_paths() {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    return reg.paths;
  }

  std::filesystem::path guide_file() {
    return current_paths().builtin_dir / GUIDE_FILE;
  }

  void rescan() {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.manifests = discover(reg.paths.builtin_dir, reg.paths.custom_dir);
  }

  namespace {
    /**
     * @brief Largest total size of the files a plugin import copies.
     */
    constexpr std::uintmax_t MAX_IMPORT_BYTES = 256 * 1024 * 1024;

    /**
     * @brief Most files a plugin import copies.
     */
    constexpr std::size_t MAX_IMPORT_FILES = 10000;

    /**
     * @brief Copy a plugin's folder, leaving out entries whose names start with a dot.
     *
     * @param source Folder to copy.
     * @param dest Folder to create.
     * @param error Receives the reason when the copy fails.
     * @return `false` when the copy failed or the folder can't be imported.
     */
    bool copy_plugin_folder(const fs::path &source, const fs::path &dest, std::string &error) {
      std::error_code ec;
      fs::create_directories(dest, ec);
      if (ec) {
        error = std::format("couldn't prepare the import: {}", ec.message());
        return false;
      }

      std::uintmax_t bytes = 0;
      std::size_t files = 0;
      fs::recursive_directory_iterator it {source, ec};
      for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        const auto &entry = *it;
        const auto relative = entry.path().lexically_relative(source);
        if (path_to_utf8(entry.path().filename()).starts_with('.')) {
          it.disable_recursion_pending();
          continue;
        }
        if (entry.is_symlink(ec)) {
          error = std::format("'{}' is a link, which can't be imported", path_to_utf8(relative));
          return false;
        }
        if (entry.is_directory(ec)) {
          fs::create_directories(dest / relative, ec);
        } else if (entry.is_regular_file(ec)) {
          bytes += entry.file_size(ec);
          if (++files > MAX_IMPORT_FILES || bytes > MAX_IMPORT_BYTES) {
            error = std::format("the folder is too large to import: the limit is {} files and {} MB", MAX_IMPORT_FILES, MAX_IMPORT_BYTES / (1024 * 1024));
            return false;
          }
          fs::copy_file(entry.path(), dest / relative, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
          error = std::format("couldn't copy '{}': {}", path_to_utf8(relative), ec.message());
          return false;
        }
      }
      if (ec) {
        error = std::format("couldn't read the folder: {}", ec.message());
        return false;
      }
      return true;
    }

    /**
     * @brief Put a fully copied plugin folder in place, restoring the previous one if that fails.
     *
     * @param staged Folder holding the new copy.
     * @param target Where the plugin belongs.
     * @param backup Unused path to keep the previous copy at while swapping.
     * @param error Receives the reason when the swap fails.
     * @return `false` when the previous copy, if any, is still in place.
     */
    bool swap_plugin_folder(const fs::path &staged, const fs::path &target, const fs::path &backup, std::string &error) {
      std::error_code ec;
      const bool replacing = fs::exists(target, ec);
      fs::create_directories(target.parent_path(), ec);
      if (replacing) {
        fs::rename(target, backup, ec);
        if (ec) {
          error = std::format("the plugin's current folder is in use: {}", ec.message());
          return false;
        }
      }

      fs::rename(staged, target, ec);
      if (ec) {
        error = std::format("couldn't put the plugin in place: {}", ec.message());
        if (replacing) {
          std::error_code restore;
          fs::rename(backup, target, restore);
        }
        return false;
      }
      fs::remove_all(backup, ec);
      return true;
    }

    /**
     * @brief Copy a plugin's folder into the custom plugins directory and record where it came from.
     *
     * @param source Absolute path of the folder holding the plugin's `plugin.json`.
     * @param origin Folder or feed to record as the plugin's origin.
     * @param expected_version Version the manifest must have, or empty to accept any.
     * @param expected_id ID the manifest must have, or empty to accept any.
     * @param id Receives the plugin's ID.
     * @param updated Receives whether a plugin with that ID was replaced.
     * @param error Receives the reason when the import is rejected.
     * @return Outcome of the request.
     */
    result_e import_from(const fs::path &source, const std::string &origin, const std::string &expected_version, const std::string &expected_id, std::string &id, bool &updated, std::string &error) {
      auto &reg = registry();
      std::scoped_lock lock {reg.mutex};

      std::error_code ec;
      json node;
      try {
        std::ifstream in {source / MANIFEST_FILE};
        if (!in.is_open()) {
          error = std::format("the folder has no {}", MANIFEST_FILE);
          return result_e::invalid;
        }
        node = json::parse(in);
      } catch (const std::exception &e) {
        error = std::format("couldn't read {}: {}", MANIFEST_FILE, e.what());
        return result_e::invalid;
      }

      auto manifest = parse_manifest(node, source, false, error);
      if (!manifest) {
        return result_e::invalid;
      }
      if (!expected_version.empty() && manifest->version != expected_version) {
        error = std::format("the feed says version {}, but the plugin's {} says version {}", expected_version, MANIFEST_FILE, manifest->version.empty() ? "nothing"s : manifest->version);
        return result_e::invalid;
      }
      if (!expected_id.empty() && manifest->id != expected_id) {
        error = std::format("the feed now holds the plugin '{}' rather than '{}'", manifest->id, expected_id);
        return result_e::invalid;
      }

      const auto *existing = find_manifest(reg, manifest->id);
      if (existing && existing->builtin) {
        error = std::format("a bundled plugin already uses the ID '{}'", manifest->id);
        return result_e::invalid;
      }
      if (reg.task_running) {
        error = "another plugin operation is still running";
        return result_e::busy;
      }

      const auto target = existing ? existing->dir : reg.paths.custom_dir / manifest->id;
      if (fs::equivalent(source, target, ec)) {
        error = "that folder is already where Sunshine loads the plugin from";
        return result_e::invalid;
      }
      if (!existing && std::ranges::any_of(reg.manifests, [&target](const manifest_t &other) {
            std::error_code same;
            return fs::equivalent(other.dir, target, same);
          })) {
        error = std::format("another plugin is in the folder '{}'", manifest->id);
        return result_e::invalid;
      }

      const auto staged = reg.paths.data_dir / (".import-" + manifest->id);
      const auto backup = reg.paths.data_dir / (".replaced-" + manifest->id);
      fs::remove_all(staged, ec);
      fs::remove_all(backup, ec);
      if (!copy_plugin_folder(source, staged, error) || !swap_plugin_folder(staged, target, backup, error)) {
        fs::remove_all(staged, ec);
        return result_e::invalid;
      }

      id = manifest->id;
      updated = existing != nullptr;
      reg.manifests = discover(reg.paths.builtin_dir, reg.paths.custom_dir);
      reg.updates.erase(id);  // what a check found no longer applies
      if (auto &state = reg.state[id]; state.origin != origin) {
        state.origin = origin;
        save_state(reg.paths.state_file, reg.state);
      }
      BOOST_LOG(info) << "Plugin ["sv << id << ' ' << manifest->version << "] imported from ["sv << origin << ']';
      return result_e::ok;
    }

    /**
     * @brief Largest update feed `import_url()` downloads.
     */
    constexpr std::size_t MAX_FEED_BYTES = 64 * 1024;

    /**
     * @brief Largest zip file `import_url()` downloads.
     */
    constexpr std::size_t MAX_PACKAGE_BYTES = 20 * 1024 * 1024;

    /**
     * @brief Longest a single download for `import_url()` may take.
     */
    constexpr std::chrono::seconds DOWNLOAD_TIMEOUT = 60s;

    /**
     * @brief Owns a parsed URL.
     */
    using curl_url_t = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;

    /**
     * @brief Parse a URL, or resolve one relative to another.
     *
     * @param url Absolute URL, or one relative to `base`.
     * @param base Absolute URL `url` is relative to, or empty.
     * @return Parsed URL, or an empty pointer when it isn't valid.
     */
    curl_url_t parse_url(const std::string &url, const std::string &base = {}) {
      curl_url_t parsed {curl_url(), &curl_url_cleanup};
      if (!parsed || (!base.empty() && curl_url_set(parsed.get(), CURLUPART_URL, base.c_str(), 0) != CURLUE_OK) ||
          curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
        parsed.reset();
      }
      return parsed;
    }

    /**
     * @brief Read one part of a parsed URL.
     *
     * @param url Parsed URL.
     * @param part Part to read.
     * @return The part, or an empty string when the URL doesn't have it.
     */
    std::string url_part(CURLU *url, CURLUPart part) {
      char *value = nullptr;
      if (curl_url_get(url, part, &value, 0) != CURLUE_OK || !value) {
        return {};
      }
      std::string result {value};
      curl_free(value);
      return result;
    }

    /**
     * @brief Check whether a parsed URL uses HTTPS.
     *
     * @param url Parsed URL.
     * @return `true` for an `https:` URL.
     */
    bool is_https(CURLU *url) {
      return boost::iequals(url_part(url, CURLUPART_SCHEME), "https");
    }

    /**
     * @brief Download a file over HTTPS with curl, following redirects to other HTTPS addresses only.
     *
     * @param url Address to download.
     * @param max_bytes Largest response accepted.
     * @param timeout Longest the whole download may take.
     * @param body Receives the response.
     * @param final_url Receives the address the response came from.
     * @param error Receives the reason when the download fails.
     * @return `true` when the server answered 200 OK with no more than `max_bytes`.
     */
    bool fetch_https(const std::string &url, std::size_t max_bytes, std::chrono::seconds timeout, std::string &body, std::string &final_url, std::string &error) {
      std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl {curl_easy_init(), &curl_easy_cleanup};
      if (!curl) {
        error = "couldn't start the download";
        return false;
      }

      /**
       * @brief Where the write callback puts the response.
       */
      struct sink_t {
        std::string *body;  ///< Response received so far.
        std::size_t max_bytes;  ///< Largest response accepted.
        bool too_large = false;  ///< Whether the response went over `max_bytes`.
      } sink {&body, max_bytes};
      body.clear();

      const auto write = +[](char *data, std::size_t size, std::size_t count, void *user) -> std::size_t {
        auto *s = static_cast<sink_t *>(user);
        if (s->body->size() + size * count > s->max_bytes) {
          s->too_large = true;
          return 0;  // aborts the transfer
        }
        s->body->append(data, size * count);
        return size * count;
      };
      curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
      curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
      curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
      curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 10L);
      curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 15L);
      curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout.count()));
      curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Sunshine");
#ifdef _WIN32
      curl_easy_setopt(curl.get(), CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));  // trust what Windows trusts
#endif
      curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write);
      curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);

      const auto code = curl_easy_perform(curl.get());
      if (sink.too_large) {
        error = std::format("{} is larger than {} KB", url, max_bytes / 1024);
        return false;
      }
      if (code == CURLE_UNSUPPORTED_PROTOCOL) {
        error = std::format("couldn't download {}: it redirects to an address that isn't https://", url);
        return false;
      }
      if (code != CURLE_OK) {
        error = std::format("couldn't download {}: {}", url, curl_easy_strerror(code));
        return false;
      }
      long status = 0;
      curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
      if (status != 200) {
        error = std::format("couldn't download {}: the server answered {}", url, status);
        return false;
      }
      if (char *effective = nullptr; curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK && effective) {
        final_url = effective;
      } else {
        final_url = url;
      }
      return true;
    }

    /**
     * @brief Read a little-endian 16-bit number.
     *
     * @param data Bytes to read from.
     * @param at Offset of the number, which the caller has checked is in range.
     * @return The number.
     */
    std::uint32_t read_le16(std::string_view data, std::size_t at) {
      return static_cast<std::uint8_t>(data[at]) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[at + 1])) << 8);
    }

    /**
     * @brief Read a little-endian 32-bit number.
     *
     * @param data Bytes to read from.
     * @param at Offset of the number, which the caller has checked is in range.
     * @return The number.
     */
    std::uint32_t read_le32(std::string_view data, std::size_t at) {
      return read_le16(data, at) | (read_le16(data, at + 2) << 16);
    }

    /**
     * @brief Turn the name of a zip entry into a path that stays inside the folder it's extracted to.
     *
     * @param name Name stored in the zip, with `/` or `\` between folders.
     * @return Relative path, empty for the top-level folder, or `std::nullopt` when the name leaves the folder or
     * can't be a file name, such as one with `..` or a drive.
     */
    std::optional<fs::path> zip_entry_path(std::string_view name) {
      if (name.starts_with('/') || name.starts_with('\\')) {
        return std::nullopt;
      }
      fs::path path;
      std::vector<std::string> parts;
      boost::split(parts, name, boost::is_any_of("/\\"));
      for (const auto &part : parts) {
        if (part == ".." || part.contains(':')) {
          return std::nullopt;
        }
        if (!part.empty() && part != ".") {
          path /= fs::path {std::u8string {part.begin(), part.end()}};
        }
      }
      return path;
    }

    /**
     * @brief Decompress a deflated zip entry.
     *
     * @param compressed Entry data.
     * @param size Size the zip says the entry has.
     * @param out Receives the data.
     * @return `false` when the data is damaged or doesn't have that size.
     */
    bool inflate_entry(std::string_view compressed, std::size_t size, std::string &out) {
      // One byte more than expected, so a larger entry is caught rather than cut off
      out.assign(size + 1, '\0');
      z_stream stream {};
      if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return false;
      }
      stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
      stream.avail_in = static_cast<uInt>(compressed.size());
      stream.next_out = reinterpret_cast<Bytef *>(out.data());
      stream.avail_out = static_cast<uInt>(out.size());
      const auto result = inflate(&stream, Z_FINISH);
      const bool ok = result == Z_STREAM_END && stream.total_out == size;
      inflateEnd(&stream);
      out.resize(size);
      return ok;
    }

    /**
     * @brief Extract a zip file held in memory.
     *
     * Only stored and deflated entries are supported, without encryption or ZIP64. Every entry is checked against its
     * CRC, and the zip may hold no more files or bytes than a folder import.
     *
     * @param zip Contents of the zip file.
     * @param dest Folder to extract into.
     * @param error Receives the reason when the zip can't be extracted.
     * @return `false` when the zip is damaged, too large or holds an entry that can't be extracted.
     */
    bool extract_zip(std::string_view zip, const fs::path &dest, std::string &error) {
      constexpr std::uint32_t end_signature = 0x06054b50;
      constexpr std::uint32_t central_signature = 0x02014b50;
      constexpr std::uint32_t local_signature = 0x04034b50;
      constexpr std::size_t end_size = 22;
      constexpr std::size_t central_size = 46;
      constexpr std::size_t local_size = 30;
      const auto damaged = [&error]() {
        error = "the download isn't a zip file, or it's damaged";
        return false;
      };

      // The end of central directory record is followed by a comment of up to 64 KB
      if (zip.size() < end_size) {
        return damaged();
      }
      std::size_t end = std::string_view::npos;
      const auto lowest = zip.size() > end_size + 0xFFFF ? zip.size() - end_size - 0xFFFF : 0;
      for (auto at = zip.size() - end_size; end == std::string_view::npos; --at) {
        if (read_le32(zip, at) == end_signature) {
          end = at;
        }
        if (at == lowest) {
          break;
        }
      }
      if (end == std::string_view::npos) {
        return damaged();
      }

      const std::size_t count = read_le16(zip, end + 10);
      const std::size_t directory_size = read_le32(zip, end + 12);
      const std::size_t directory = read_le32(zip, end + 16);
      if (count == 0xFFFF || directory + directory_size > end) {
        return damaged();
      }
      if (count > MAX_IMPORT_FILES) {
        error = std::format("the zip file is too large to import: the limit is {} files", MAX_IMPORT_FILES);
        return false;
      }

      std::error_code ec;
      std::uintmax_t total = 0;
      std::size_t at = directory;
      for (std::size_t i = 0; i < count; ++i) {
        if (at + central_size > end || read_le32(zip, at) != central_signature) {
          return damaged();
        }
        const auto made_by = read_le16(zip, at + 4);
        const auto flags = read_le16(zip, at + 8);
        const auto method = read_le16(zip, at + 10);
        const auto crc = read_le32(zip, at + 16);
        const std::size_t compressed_size = read_le32(zip, at + 20);
        const std::size_t size = read_le32(zip, at + 24);
        const std::size_t name_length = read_le16(zip, at + 28);
        const auto attributes = read_le32(zip, at + 38);
        const std::size_t local = read_le32(zip, at + 42);
        if (at + central_size + name_length > end) {
          return damaged();
        }
        const auto name = zip.substr(at + central_size, name_length);
        at += central_size + name_length + read_le16(zip, at + 30) + read_le16(zip, at + 32);

        const auto relative = zip_entry_path(name);
        if (!relative) {
          error = std::format("the zip file holds '{}', which would be extracted outside the plugin's folder", name);
          return false;
        }
        if (flags & 1) {
          error = std::format("'{}' in the zip file is encrypted", name);
          return false;
        }
        constexpr std::uint32_t unix_host = 3;
        constexpr std::uint32_t type_mask = 0170000;
        constexpr std::uint32_t link_type = 0120000;
        if ((made_by >> 8) == unix_host && ((attributes >> 16) & type_mask) == link_type) {
          error = std::format("'{}' in the zip file is a link, which can't be imported", name);
          return false;
        }
        if (name.ends_with('/') || name.ends_with('\\')) {
          fs::create_directories(dest / *relative, ec);
          if (ec) {
            error = std::format("couldn't extract '{}': {}", name, ec.message());
            return false;
          }
          continue;
        }

        total += size;
        if (total > MAX_IMPORT_BYTES) {
          error = std::format("the zip file is too large to import: the limit is {} MB", MAX_IMPORT_BYTES / (1024 * 1024));
          return false;
        }
        if (method != 0 && method != Z_DEFLATED) {
          error = std::format("'{}' in the zip file uses a compression method other than deflate", name);
          return false;
        }
        if (local + local_size > zip.size() || read_le32(zip, local) != local_signature) {
          return damaged();
        }
        const auto data = local + local_size + read_le16(zip, local + 26) + read_le16(zip, local + 28);
        if (data + compressed_size > zip.size()) {
          return damaged();
        }
        std::string contents;
        if (method == 0) {
          if (compressed_size != size) {
            return damaged();
          }
          contents = zip.substr(data, size);
        } else if (!inflate_entry(zip.substr(data, compressed_size), size, contents)) {
          return damaged();
        }
        if (crc32(0, reinterpret_cast<const Bytef *>(contents.data()), static_cast<uInt>(contents.size())) != crc) {
          return damaged();
        }

        const auto path = dest / *relative;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out {path, std::ios::binary | std::ios::trunc};
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.close();
        if (ec || !out) {
          error = std::format("couldn't extract '{}'", name);
          return false;
        }
      }
      return true;
    }

    /**
     * @brief Find the folder holding `plugin.json` in an extracted zip file.
     *
     * @param root Folder the zip was extracted into.
     * @return `root`, or its only entry when that is a folder holding `plugin.json`, or `std::nullopt`.
     */
    std::optional<fs::path> find_plugin_root(const fs::path &root) {
      std::error_code ec;
      if (fs::is_regular_file(root / MANIFEST_FILE, ec)) {
        return root;
      }
      fs::directory_iterator it {root, ec};
      if (ec || it == fs::directory_iterator {}) {
        return std::nullopt;
      }
      const auto only = it->path();
      if (it.increment(ec); ec || it != fs::directory_iterator {} || !fs::is_regular_file(only / MANIFEST_FILE, ec)) {
        return std::nullopt;
      }
      return only;
    }

    /**
     * @brief Format a SHA-256 hash as lowercase hex.
     *
     * @param data Data to hash.
     * @return 64 hex digits.
     */
    std::string sha256_hex(std::string_view data) {
      std::string hex;
      for (const auto byte : crypto::hash(data)) {
        hex += std::format("{:02x}", byte);
      }
      return hex;
    }

    /**
     * @brief Longest a check for updates waits for each feed.
     */
    constexpr std::chrono::seconds CHECK_TIMEOUT = 20s;

    /**
     * @brief What a plugin's update feed says.
     */
    struct feed_t {
      std::string version;  ///< Latest version of the plugin.
      std::string package_url;  ///< Absolute address of its zip file.
      std::string sha256;  ///< SHA-256 of the zip file, as lowercase hex.
    };

    /**
     * @brief Download and check an update feed.
     *
     * @param fetch Function to download with.
     * @param feed Address of the feed.
     * @param timeout Longest the download may take.
     * @param out Receives what the feed says.
     * @param error Receives the reason when the feed can't be used.
     * @return `result_e::unavailable` when the download failed, or `result_e::invalid` when the feed is wrong.
     */
    result_e read_feed(const fetcher_t &fetch, const std::string &feed, std::chrono::seconds timeout, feed_t &out, std::string &error) {
      std::string body;
      std::string final_url;
      if (!fetch(feed, MAX_FEED_BYTES, timeout, body, final_url, error)) {
        return result_e::unavailable;
      }

      std::string package_url;
      try {
        const auto node = json::parse(body);
        out.version = node.at("version").get<std::string>();
        package_url = node.at("url").get<std::string>();
        out.sha256 = boost::to_lower_copy(node.at("sha256").get<std::string>());
      } catch (const std::exception &) {
        error = std::format("{} isn't a plugin update feed: it needs \"version\", \"url\" and \"sha256\"", feed);
        return result_e::invalid;
      }
      if (out.version.empty() || out.sha256.size() != 64 || !std::ranges::all_of(out.sha256, [](const char c) {
            return std::isxdigit(static_cast<unsigned char>(c));
          })) {
        error = std::format("{} needs a \"version\" and the zip file's SHA-256 as 64 hex digits in \"sha256\"", feed);
        return result_e::invalid;
      }
      const auto package = parse_url(package_url, final_url);
      if (!package || !is_https(package.get())) {
        error = std::format("the feed's \"url\" '{}' isn't an https:// address", package_url);
        return result_e::invalid;
      }
      out.package_url = url_part(package.get(), CURLUPART_URL);
      return result_e::ok;
    }

    /**
     * @brief Download a plugin from its update feed and import it.
     *
     * @param feed Address of the feed, as `feed_url()` returns it.
     * @param expected_id ID the plugin must have, or empty to accept any.
     * @param id Receives the plugin's ID.
     * @param updated Receives whether a plugin with that ID was replaced.
     * @param error Receives the reason when the import is rejected.
     * @return Outcome of the request: `result_e::unavailable` when a download failed.
     */
    result_e import_feed(const std::string &feed, const std::string &expected_id, std::string &id, bool &updated, std::string &error) {
      fetcher_t fetch;
      fs::path data_dir;
      {
        auto &reg = registry();
        std::scoped_lock lock {reg.mutex};
        fetch = reg.fetcher;
        data_dir = reg.paths.data_dir;
      }

      feed_t latest;
      if (const auto result = read_feed(fetch, feed, DOWNLOAD_TIMEOUT, latest, error); result != result_e::ok) {
        return result;
      }

      std::string zip;
      std::string final_url;
      if (!fetch(latest.package_url, MAX_PACKAGE_BYTES, DOWNLOAD_TIMEOUT, zip, final_url, error)) {
        return result_e::unavailable;
      }
      if (sha256_hex(zip) != latest.sha256) {
        error = std::format("{} doesn't match the SHA-256 in the feed", latest.package_url);
        return result_e::invalid;
      }

      static std::atomic<unsigned> downloads {0};
      const auto staged = data_dir / std::format(".download-{}", ++downloads);
      std::error_code ec;
      fs::remove_all(staged, ec);
      auto result = result_e::invalid;
      if (!extract_zip(zip, staged, error)) {
        error = std::format("{}: {}", latest.package_url, error);
      } else if (const auto root = find_plugin_root(staged); !root) {
        error = std::format("{} has no {} at its top level or in a single top-level folder", latest.package_url, MANIFEST_FILE);
      } else {
        result = import_from(*root, feed, latest.version, expected_id, id, updated, error);
      }
      fs::remove_all(staged, ec);
      return result;
    }
  }  // namespace

  std::optional<std::string> feed_url(const std::string &url, std::string &error) {
    const auto parsed = parse_url(url);
    if (!parsed || !is_https(parsed.get())) {
      error = std::format("'{}' isn't an https:// address", url);
      return std::nullopt;
    }

    // A GitHub repository: its latest release's latest.json
    if (boost::iequals(url_part(parsed.get(), CURLUPART_HOST), "github.com") && url_part(parsed.get(), CURLUPART_QUERY).empty()) {
      std::vector<std::string> parts;
      const auto path = url_part(parsed.get(), CURLUPART_PATH);
      boost::split(parts, path, boost::is_any_of("/"));
      std::erase(parts, std::string {});
      if (parts.size() == 2) {
        auto repo = parts[1];
        if (boost::iends_with(repo, ".git")) {
          repo.resize(repo.size() - 4);
        }
        return std::format("https://github.com/{}/{}/releases/latest/download/latest.json", parts[0], repo);
      }
    }
    return url_part(parsed.get(), CURLUPART_URL);
  }

  result_e import_url(const std::string &url, std::string &id, bool &updated, std::string &error) {
    const auto feed = feed_url(url, error);
    if (!feed) {
      return result_e::invalid;
    }
    return import_feed(*feed, {}, id, updated, error);
  }

  update_summary_t check_for_updates() {
    fetcher_t fetch;
    std::vector<std::pair<std::string, std::string>> feeds;  // plugin ID and feed
    {
      auto &reg = registry();
      std::scoped_lock lock {reg.mutex};
      fetch = reg.fetcher;
      for (const auto &manifest : reg.manifests) {
        if (const auto *state = find_state(reg, manifest.id); !manifest.builtin && state && state->origin.starts_with("https://")) {
          feeds.emplace_back(manifest.id, state->origin);
        }
      }
    }

    // Read every feed before taking the lock again, so streams don't wait on the network
    std::vector<std::pair<std::string, update_check_t>> results;
    for (const auto &[id, feed] : feeds) {
      feed_t latest;
      update_check_t check;
      if (read_feed(fetch, feed, CHECK_TIMEOUT, latest, check.error) == result_e::ok) {
        check.version = latest.version;
      } else {
        BOOST_LOG(warning) << "Couldn't check plugin ["sv << id << "] for updates: "sv << check.error;
      }
      results.emplace_back(id, std::move(check));
    }

    update_summary_t summary;
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    for (auto &[id, check] : results) {
      const auto *manifest = find_manifest(reg, id);
      if (!manifest) {
        continue;  // removed while the feeds were read
      }
      ++summary.checked;
      if (check.version.empty()) {
        ++summary.failed;
      } else if (check.version != manifest->version) {
        ++summary.available;
        BOOST_LOG(info) << "Plugin ["sv << id << "] has an update from version ["sv << manifest->version << "] to ["sv << check.version << ']';
      }
      reg.updates[id] = std::move(check);
    }
    return summary;
  }

  result_e update(const std::string &id, std::string &error) {
    std::string feed;
    {
      auto &reg = registry();
      std::scoped_lock lock {reg.mutex};
      const auto *manifest = find_manifest(reg, id);
      if (!manifest) {
        error = std::format("plugin '{}' not found", id);
        return result_e::not_found;
      }
      if (manifest->builtin) {
        error = std::format("'{}' is bundled with Sunshine, and updates with it", manifest->name);
        return result_e::invalid;
      }
      const auto *state = find_state(reg, id);
      if (!state || !state->origin.starts_with("https://")) {
        error = std::format("'{}' wasn't imported from a URL; import it again from its folder to update it", manifest->name);
        return result_e::invalid;
      }
      feed = state->origin;
    }

    std::string imported;
    bool updated = false;
    return import_feed(feed, id, imported, updated, error);
  }

  result_e import_folder(const std::filesystem::path &source, std::string &id, bool &updated, std::string &error) {
    std::error_code ec;
    if (!source.is_absolute() || !fs::is_directory(source, ec)) {
      error = std::format("the folder '{}' was not found; enter its full path", path_to_utf8(source));
      return result_e::invalid;
    }
    return import_from(source, path_to_utf8(source), {}, {}, id, updated, error);
  }

  nlohmann::json choices_json(const std::vector<choice_t> &choices) {
    json result = json::array();
    for (const auto &choice : choices) {
      json entry = {{"value", choice.value}, {"label", choice.label}};
      if (!choice.group.empty()) {
        entry["group"] = choice.group;
      }
      result.push_back(std::move(entry));
    }
    return result;
  }

  nlohmann::json list() {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    json result = json::array();
    for (const auto &manifest : reg.manifests) {
      const auto state_it = reg.state.find(manifest.id);
      const auto state = state_it == reg.state.end() ? plugin_state_t {} : state_it->second;

      json options = json::array();
      for (const auto &option : manifest.options) {
        json entry = {
          {"id", option.id},
          {"type", option_type_name(option.type)},
          {"label", option.label},
          {"description", option.description},
          {"default", option.default_value},
          {"overridable", is_overridable(option)}
        };
        if (option.min) {
          entry["min"] = *option.min;
        }
        if (option.max) {
          entry["max"] = *option.max;
        }
        if (option.step) {
          entry["step"] = *option.step;
        }
        if (option.type == option_type_e::choice) {
          entry["choices"] = choices_json(option.choices);
          entry["dynamic"] = !option.choices_script.empty();
        }
        options.push_back(std::move(entry));
      }

      json task = nullptr;
      if (const auto task_it = reg.tasks.find(manifest.id); task_it != reg.tasks.end()) {
        const auto &info = task_it->second;
        task = {
          {"action", task_action_name(info.action)},
          {"status", task_status_name(info.status)},
          {"exit_code", info.exit_code}
        };
        if (info.action == action_e::run) {
          task["action_id"] = info.action_id;
        }
      }

      json actions = json::array();
      for (const auto &action : manifest.actions) {
        actions.push_back({{"id", action.id}, {"label", action.label}, {"description", action.description}});
      }
      json client_commands = json::array();
      for (const auto &command : manifest.client_commands) {
        client_commands.push_back({
          {"id", command.id},
          {"label", command.label},
          {"icon", command.icon},
          {"close_menu", command.close_menu},
          {"top_level", command.top_level},
          {"script", command.script},
          {"args", command.args},
          {"steps", command.steps},
          {"elevated", command.elevated},
          {"status_file", command.status_file},
          {"programs", command.programs},
        });
      }
      json update = nullptr;
      bool update_available = false;
      if (const auto update_it = reg.updates.find(manifest.id); update_it != reg.updates.end()) {
        update = {{"version", update_it->second.version}, {"error", update_it->second.error}};
        update_available = !update_it->second.version.empty() && update_it->second.version != manifest.version;
      }
      json panel = nullptr;
      if (manifest.panel) {
        panel = {{"height", manifest.panel->height}, {"fit", manifest.panel->fit}, {"proxy", manifest.panel->port != 0}};
      }

      result.push_back({
        {"id", manifest.id},
        {"name", manifest.name},
        {"version", manifest.version},
        {"description", manifest.description},
        {"author", manifest.author},
        {"homepage", manifest.homepage},
        {"notice", manifest.notice},
        {"notice_url", manifest.notice_url},
        {"builtin", manifest.builtin},
        {"elevated", manifest.elevated},
        {"manages_display", manifest.manages_display},
        {"idle_only", manifest.idle_only},
        {"default_active", manifest.default_active},
        {"has_status", !manifest.scripts.status.empty()},
        {"per_stream", per_stream(manifest)},
        {"order", manifest.order},
        {"options", std::move(options)},
        {"actions", std::move(actions)},
        {"client_commands", std::move(client_commands)},
        {"icon", manifest.icon},
        {"panel", std::move(panel)},
        {"installed", state.installed},
        {"installed_version", state.installed_version},
        {"needs_reinstall", needs_reinstall(manifest, state)},
        {"origin", state.origin},
        {"update", std::move(update)},
        {"update_available", update_available},
        {"active", state.active},
        {"config", effective_options(reg, manifest)},
        {"task", std::move(task)},
        {"display_exclusions", display_exclusions_of(reg, manifest.id)}
      });
    }
    return result;
  }

  std::vector<config::prep_cmd_t> prep_cmds(const std::vector<std::string> &excluded, const nlohmann::json &app_overrides) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    std::vector<config::prep_cmd_t> cmds;
    for (const auto &manifest : reg.manifests) {
      if (!runs_for_streams(reg, manifest)) {
        continue;
      }
      if (std::ranges::find(excluded, manifest.id) != excluded.end()) {
        continue;
      }
      const auto env = script_env(manifest, stream_options(reg, manifest, app_overrides, {}), plugin_data_dir(reg.paths, manifest.id));
      auto add = [&](const std::string &do_script, const std::string &undo_script, config::prep_cmd_t::type_e type) {
        if (do_script.empty() && undo_script.empty()) {
          return;
        }
        auto do_cmd = do_script.empty() ? std::string {} : script_command(manifest.dir / do_script);
        auto undo_cmd = undo_script.empty() ? std::string {} : script_command(manifest.dir / undo_script);
        auto elevated = manifest.elevated;
        auto &cmd = cmds.emplace_back(std::move(do_cmd), std::move(undo_cmd), std::move(elevated), type);
        cmd.env = env;
        cmd.plugin = manifest.id;
        cmd.log_file = path_to_utf8(plugin_log_file(reg.paths, manifest.id));
      };
      add(manifest.scripts.do_script, manifest.scripts.undo_script, config::prep_cmd_t::type_e::app);
      add(manifest.scripts.connect_script, manifest.scripts.disconnect_script, config::prep_cmd_t::type_e::connect);
    }
    return cmds;
  }

  std::vector<offered_commands_t> offered_client_commands(const std::vector<std::string> &excluded, const client_commands::plugin_overrides_t &overrides, const nlohmann::json &app_overrides, const std::string &client_uuid) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    std::vector<offered_commands_t> offered;
    const client_commands::host_programs_t host;  // looked up only when a command is limited to programs
    for (const auto &manifest : reg.manifests) {
      if (manifest.client_commands.empty() || std::ranges::find(excluded, manifest.id) != excluded.end() || !runs_for_streams(reg, manifest) || off_for_client(reg, manifest.id, client_uuid)) {
        continue;
      }

      offered_commands_t plugin {manifest.id, manifest.name, manifest.icon, {}, {}};
      const auto data_dir = plugin_data_dir(reg.paths, manifest.id);
      const auto env = script_env(manifest, stream_options(reg, manifest, app_overrides, client_uuid), data_dir);
      for (const auto &source : manifest.client_commands) {
        const auto entry = overrides.find(std::format("{}/{}", manifest.id, source.id));
        const bool has_override = entry != overrides.end();
        if (has_override && !entry->second.enabled) {
          continue;
        }
        const auto &programs = has_override && entry->second.programs ? *entry->second.programs : source.programs;
        if (!programs.empty() && !client_commands::matches(programs, host)) {
          continue;
        }

        client_commands::command_t command;
        command.id = std::format("plugin:{}/{}", manifest.id, source.id);
        command.label = source.label;
        command.icon = source.icon;
        command.close_menu = source.close_menu;
        if (has_override) {
          if (!entry->second.label.empty()) {
            command.label = entry->second.label;
          }
          if (!entry->second.icon.empty()) {
            command.icon = entry->second.icon;
          }
        }
        if (!source.status_file.empty()) {
          apply_status_file(data_dir / source.status_file, command);
        }
        if (source.script.empty()) {
          command.type = client_commands::type_e::keys;
          command.steps = source.steps;
        } else {
          command.type = client_commands::type_e::script;
          command.cmd = script_command(manifest.dir / source.script);
          for (const auto &arg : source.args) {
            command.cmd += ' ' + quote_arg(arg);
          }
          command.working_dir = path_to_utf8(manifest.dir);
          command.elevated = source.elevated;
          command.env = env;
          command.log_file = plugin_log_file(reg.paths, manifest.id);
        }
        (source.top_level ? plugin.top_level : plugin.grouped).push_back(std::move(command));
      }
      offered.push_back(std::move(plugin));
    }
    return offered;
  }

  std::string display_plugin(const std::vector<std::string> &excluded, const std::string &client_uuid) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    for (const auto &manifest : reg.manifests) {
      if (manifest.manages_display && std::ranges::find(excluded, manifest.id) == excluded.end() && runs_for_streams(reg, manifest) && !off_for_client(reg, manifest.id, client_uuid)) {
        return manifest.name;
      }
    }
    return {};
  }

  bool per_stream(const manifest_t &manifest) {
    const auto &scripts = manifest.scripts;
    return !scripts.do_script.empty() || !scripts.undo_script.empty() || !scripts.connect_script.empty() ||
           !scripts.disconnect_script.empty() || !manifest.client_commands.empty() || manifest.manages_display;
  }

  bool idle_only(const std::string &id) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    const auto *manifest = find_manifest(reg, id);
    return manifest && manifest->idle_only;
  }

  bool manages_display(const std::string &id) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    const auto *manifest = find_manifest(reg, id);
    return manifest && manifest->manages_display;
  }

  std::vector<std::string> exclude_display_conflicts(const std::string &id, nlohmann::json &apps_tree) {
    std::vector<std::string> others;
    {
      auto &reg = registry();
      std::scoped_lock lock {reg.mutex};
      const auto *manifest = find_manifest(reg, id);
      if (!manifest || !manifest->manages_display) {
        return {};
      }
      for (const auto &other : reg.manifests) {
        if (other.id != id && other.manages_display && runs_for_streams(reg, other)) {
          others.push_back(other.id);
        }
      }
    }
    if (others.empty() || !apps_tree.is_object() || !apps_tree.contains("apps") || !apps_tree["apps"].is_array()) {
      return {};
    }

    std::vector<std::string> changed;
    for (auto &app : apps_tree["apps"]) {
      if (!app.is_object()) {
        continue;
      }
      std::vector<std::string> excluded;
      if (const auto it = app.find("exclude-plugins"); it != app.end() && it->is_array()) {
        for (const auto &entry : *it) {
          if (entry.is_string()) {
            excluded.push_back(entry.get<std::string>());
          }
        }
      }
      const auto is_excluded = [&excluded](const std::string &plugin) {
        return std::ranges::find(excluded, plugin) != excluded.end();
      };
      if (is_excluded(id) || std::ranges::all_of(others, is_excluded)) {
        continue;
      }
      if (!app.contains("exclude-plugins") || !app["exclude-plugins"].is_array()) {
        app["exclude-plugins"] = json::array();
      }
      app["exclude-plugins"].push_back(id);
      changed.push_back(app.value("name", ""));
    }
    return changed;
  }

  std::optional<std::map<std::string, std::string>> stream_env(const std::string &id, const nlohmann::json &app_overrides, const std::string &client_uuid) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    const auto *manifest = find_manifest(reg, id);
    if (!manifest || off_for_client(reg, id, client_uuid)) {
      return std::nullopt;
    }
    return script_env(*manifest, stream_options(reg, *manifest, app_overrides, client_uuid), plugin_data_dir(reg.paths, id));
  }

  bool disabled_for_client(const std::string &id, const std::string &client_uuid) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    return off_for_client(reg, id, client_uuid);
  }

  std::optional<nlohmann::json> normalize_app_overrides(const nlohmann::json &input, std::string &error) {
    if (!input.is_object()) {
      error = "'plugin-options' must be an object";
      return std::nullopt;
    }

    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    json result = json::object();
    for (const auto &[id, options] : input.items()) {
      const auto *manifest = find_manifest(reg, id);
      if (!manifest) {
        error = std::format("plugin '{}' not found", id);
        return std::nullopt;
      }
      auto normalized = normalize_overrides(*manifest, options, true, error);
      if (!normalized) {
        return std::nullopt;
      }
      if (!normalized->empty()) {
        result[id] = std::move(*normalized);
      }
    }
    return result;
  }

  nlohmann::json client_overrides() {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    json result = json::object();
    for (const auto &[id, state] : reg.state) {
      for (const auto &[uuid, entry] : state.clients.items()) {
        result[uuid][id] = entry;
      }
    }
    return result;
  }

  result_e set_client_overrides(const std::string &client_uuid, const nlohmann::json &input, std::string &error) {
    if (client_uuid.empty()) {
      error = "a client UUID is required";
      return result_e::invalid;
    }
    if (!input.is_object()) {
      error = "'plugins' must be an object";
      return result_e::invalid;
    }

    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    std::map<std::string, json> entries;
    for (const auto &[id, node] : input.items()) {
      const auto *manifest = find_manifest(reg, id);
      if (!manifest) {
        error = std::format("plugin '{}' not found", id);
        return result_e::not_found;
      }
      if (!node.is_object()) {
        error = std::format("the overrides of plugin '{}' must be an object", id);
        return result_e::invalid;
      }
      json entry = json::object();
      if (node.contains("active")) {
        if (!node["active"].is_boolean()) {
          error = std::format("'active' of plugin '{}' must be a boolean", id);
          return result_e::invalid;
        }
        if (!node["active"].get<bool>()) {
          entry["active"] = false;
        }
      }
      if (node.contains("options")) {
        auto options = normalize_overrides(*manifest, node["options"], true, error);
        if (!options) {
          return result_e::invalid;
        }
        if (!options->empty()) {
          entry["options"] = std::move(*options);
        }
      }
      if (!entry.empty()) {
        entries[id] = std::move(entry);
      }
    }

    for (auto &[id, state] : reg.state) {
      state.clients.erase(client_uuid);
    }
    for (auto &[id, entry] : entries) {
      reg.state[id].clients[client_uuid] = std::move(entry);
    }
    save_state(reg.paths.state_file, reg.state);
    return result_e::ok;
  }

  void forget_client(const std::string &client_uuid) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    bool changed = false;
    for (auto &[id, state] : reg.state) {
      if (state.clients.empty()) {
        continue;
      }
      if (client_uuid.empty()) {
        state.clients = json::object();
        changed = true;
      } else if (state.clients.erase(client_uuid) > 0) {
        changed = true;
      }
    }
    if (changed) {
      save_state(reg.paths.state_file, reg.state);
    }
  }

  result_e configure(const std::string &id, std::optional<bool> active, const std::optional<nlohmann::json> &options, std::string &error, bool *applying) {
    auto &reg = registry();
    std::unique_lock lock {reg.mutex};

    const auto *manifest = find_manifest(reg, id);
    if (!manifest) {
      error = std::format("plugin '{}' not found", id);
      return result_e::not_found;
    }

    std::optional<json> normalized;
    if (options) {
      normalized = normalize_options(*manifest, *options, true, error);
      if (!normalized) {
        return result_e::invalid;
      }
    }

    auto &state = reg.state[id];
    if (active.value_or(false) && !state.installed) {
      error = std::format("plugin '{}' must be installed before it can be activated", id);
      return result_e::not_installed;
    }

    const bool needs_apply = normalized && state.installed && !manifest->scripts.apply.empty() && apply_option_changed(*manifest, effective_options(reg, *manifest), *normalized);
    if (needs_apply && reg.task_running) {
      error = "another plugin operation is still running";
      return result_e::busy;
    }
    // The running script's options would overwrite these when it finishes.
    if (normalized && is_applying(reg, id)) {
      error = std::format("the options of plugin '{}' are still being applied", id);
      return result_e::busy;
    }

    const bool activated = active.value_or(false) && !state.active;
    if (active) {
      state.active = *active;
      if (!*active) {
        reg.display_exclusions.erase(id);
      }
    }
    if (needs_apply) {
      const auto env = script_env(*manifest, *normalized, plugin_data_dir(reg.paths, id));
      launch_task(reg, id, action_e::apply, *manifest, env, false, {}, std::move(normalized));
      if (applying) {
        *applying = true;
      }
    } else if (normalized) {
      state.options = std::move(*normalized);
    }
    save_state(reg.paths.state_file, reg.state);
    lock.unlock();

    if (activated) {
      notify_activation(reg, id);
    }
    return result_e::ok;
  }

  result_e start_task(const std::string &id, action_e action, std::string &error, bool activate, const std::string &action_id) {
    auto &reg = registry();
    std::unique_lock lock {reg.mutex};

    const auto *found = find_manifest(reg, id);
    if (!found) {
      error = std::format("plugin '{}' not found", id);
      return result_e::not_found;
    }
    if (action == action_e::run) {
      if (std::ranges::none_of(found->actions, [&action_id](const action_t &candidate) {
            return candidate.id == action_id;
          })) {
        error = std::format("plugin '{}' has no action '{}'", id, action_id);
        return result_e::invalid;
      }
      if (const auto it = reg.state.find(id); it == reg.state.end() || !it->second.installed) {
        error = std::format("plugin '{}' must be installed before running its actions", id);
        return result_e::not_installed;
      }
    }
    if (reg.task_running) {
      error = "another plugin operation is still running";
      return result_e::busy;
    }
    if (found->idle_only && action != action_e::run && reg.streaming && reg.streaming()) {
      error = std::format("plugin '{}' can't be installed or uninstalled while a stream is running", id);
      return result_e::busy;
    }

    const auto manifest = *found;
    const auto env = script_env(manifest, effective_options(reg, manifest), plugin_data_dir(reg.paths, id));

    if (action == action_e::uninstall) {
      reg.state[id].active = false;
      reg.display_exclusions.erase(id);
      save_state(reg.paths.state_file, reg.state);
    }

    launch_task(reg, id, action, manifest, env, activate, action_id, std::nullopt);

    return result_e::ok;
  }

  bool is_valid_script_choice(std::string_view value) {
    return !value.empty() && value.size() <= MAX_CHOICE_VALUE_LENGTH && std::ranges::none_of(value, [](const char c) {
      return std::iscntrl(static_cast<unsigned char>(c));
    });
  }

  bool parse_script_choices(const nlohmann::json &node, std::vector<choice_t> &out, std::string &error) {
    if (!node.is_array()) {
      error = "the list must be an array";
      return false;
    }
    if (node.size() > MAX_SCRIPT_CHOICES) {
      error = std::format("the list has more than {} choices", MAX_SCRIPT_CHOICES);
      return false;
    }
    for (const auto &entry : node) {
      choice_t choice;
      if (!parse_choice(entry, choice, error)) {
        return false;
      }
      if (!is_valid_script_choice(choice.value)) {
        error = std::format("each value must be 1 to {} bytes without control characters", MAX_CHOICE_VALUE_LENGTH);
        return false;
      }
      out.emplace_back(std::move(choice));
    }
    return true;
  }

  namespace {
    /**
     * @brief Numbers the files choices and status scripts write, so scripts running at the same time don't share one.
     */
    std::atomic<std::uint64_t> result_file_counter {0};

    /**
     * @brief A script that runs as the signed-in user and writes its result to a file, such as a choices script.
     */
    struct user_script_t {
      std::string command;  ///< Command line that runs the script.
      std::map<std::string, std::string> env;  ///< Environment, without the variable naming the result file.
      fs::path data_dir;  ///< Plugin data directory, where the script runs and writes its result and log.
      script_runner_t runner;  ///< Runs the script.
      std::chrono::milliseconds timeout {};  ///< Longest the script may run.
    };

    /**
     * @brief Describe what a user script writes and where.
     */
    struct user_script_output_t {
      std::string_view kind;  ///< What the script is, such as `choices`, used in the file names and messages.
      std::string_view result;  ///< Noun for what the script writes, such as `list`, used when it writes nothing.
      std::string_view file_variable;  ///< Environment variable naming the file the script writes.
      std::string log_header;  ///< First line of the log.
      std::size_t max_size;  ///< Largest file, in bytes, the script may write.
    };

    /**
     * @brief Run a user script, then read and delete the file it wrote.
     *
     * @param script Script to run.
     * @param output What the script writes and where.
     * @param content Receives the file's content, without a UTF-8 byte order mark.
     * @param reason Receives the reason when there is no content.
     * @return `true` when the script exited with 0 in time and wrote a file of at most `output.max_size` bytes.
     */
    bool run_user_script(user_script_t script, const user_script_output_t &output, std::string &content, std::string &reason) {
      std::error_code ec;
      fs::create_directories(script.data_dir, ec);
      const auto result_file = script.data_dir / std::format(".{}-{}.json", output.kind, result_file_counter++);
      const auto log_file = script.data_dir / std::format(".{}.log", output.kind);
      fs::remove(result_file, ec);
      script.env[std::string {output.file_variable}] = path_to_utf8(fs::path(result_file).make_preferred());

      if (std::unique_ptr<FILE, decltype(&std::fclose)> file {open_shared(log_file, L"w"), &std::fclose}; file) {
        std::fputs(std::format("{}\n", output.log_header).c_str(), file.get());
      }

      BOOST_LOG(info) << "Running plugin script ["sv << script.command << ']';
      std::stop_source stop;
      int exit_code;
      {
        // Stops the script once the timeout passes, unless the script finished first and stopped this thread.
        std::jthread watchdog([&stop, timeout = script.timeout](std::stop_token finished) {
          std::mutex mutex;
          std::condition_variable_any cv;
          std::unique_lock lock {mutex};
          cv.wait_for(lock, finished, timeout, [] {
            return false;
          });
          if (!finished.stop_requested()) {
            stop.request_stop();
          }
        });
        exit_code = script.runner(script.command, false, script.env, script.data_dir, log_file, stop.get_token());
      }

      auto remove_result = util::fail_guard([&result_file]() {
        std::error_code ignored;
        fs::remove(result_file, ignored);
      });
      if (stop.stop_requested()) {
        reason = std::format("the {} script did not finish within {} seconds", output.kind, std::chrono::ceil<std::chrono::seconds>(script.timeout).count());
        return false;
      }
      if (exit_code != 0) {
        reason = std::format("the {} script failed with exit code {}", output.kind, exit_code);
        return false;
      }
      const auto size = fs::file_size(result_file, ec);
      if (ec) {
        reason = std::format("the {} script wrote no {}", output.kind, output.result);
        return false;
      }
      if (size > output.max_size) {
        reason = std::format("the {} script wrote more than {} bytes", output.kind, output.max_size);
        return false;
      }

      {
        std::ifstream in(result_file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        content = ss.str();
      }
      // PowerShell 5.1 writes a byte order mark with -Encoding UTF8
      if (content.starts_with("\xEF\xBB\xBF"sv)) {
        content.erase(0, 3);
      }
      return true;
    }

    /**
     * @brief Check a notice a status script reported.
     *
     * @param entry Notice as the script wrote it.
     * @param out Receives the notice with every field filled in.
     * @param error Receives the reason when the notice is rejected.
     * @return `false` when a field has the wrong type, is empty or too long, or the level or link is not allowed.
     */
    bool parse_notice(const json &entry, json &out, std::string &error) {
      if (!entry.is_object()) {
        error = "each notice must be an object";
        return false;
      }
      std::string level = "info";
      std::string title;
      std::string text;
      std::string href;
      std::string action;
      if (!read_string(entry, "level", level, error) || !read_string(entry, "title", title, error) ||
          !read_string(entry, "text", text, error) || !read_string(entry, "href", href, error) ||
          !read_string(entry, "action", action, error)) {
        return false;
      }
      if (level != "info" && level != "warning") {
        error = "a notice's 'level' must be 'info' or 'warning'";
        return false;
      }
      if (title.empty() || title.size() > MAX_NOTICE_TITLE_LENGTH) {
        error = std::format("a notice's 'title' must be 1 to {} bytes", MAX_NOTICE_TITLE_LENGTH);
        return false;
      }
      if (text.size() > MAX_NOTICE_TEXT_LENGTH) {
        error = std::format("a notice's 'text' must be at most {} bytes", MAX_NOTICE_TEXT_LENGTH);
        return false;
      }
      // Only pages of the web UI and https:// pages, so a notice can't run script through a javascript: link
      if (href.size() > MAX_NOTICE_HREF_LENGTH || (!href.empty() && !href.starts_with("./") && !href.starts_with("https://"))) {
        error = std::format("a notice's 'href' must start with ./ or https:// and be at most {} bytes", MAX_NOTICE_HREF_LENGTH);
        return false;
      }
      if (action.size() > MAX_NOTICE_ACTION_LENGTH) {
        error = std::format("a notice's 'action' must be at most {} bytes", MAX_NOTICE_ACTION_LENGTH);
        return false;
      }
      out = {{"level", level}, {"title", title}, {"text", text}, {"href", href}, {"action", action}};
      return true;
    }
  }  // namespace

  result_e list_choices(const std::string &id, const std::string &option_id, std::vector<choice_t> &out, std::string &reason) {
    auto &reg = registry();
    option_t option;
    user_script_t script;
    {
      std::scoped_lock lock {reg.mutex};
      const auto *manifest = find_manifest(reg, id);
      if (!manifest) {
        reason = std::format("plugin '{}' not found", id);
        return result_e::not_found;
      }
      const auto found = std::ranges::find_if(manifest->options, [&option_id](const option_t &candidate) {
        return candidate.id == option_id && !candidate.choices_script.empty();
      });
      if (found == manifest->options.end()) {
        reason = std::format("plugin '{}' has no option '{}' with a choices script", id, option_id);
        return result_e::invalid;
      }
      if (const auto *state = find_state(reg, id); !state || !state->installed) {
        reason = std::format("plugin '{}' must be installed before listing its choices", id);
        return result_e::not_installed;
      }
      if (!reg.session_ready()) {
        reason = "a user must be signed in to list the choices";
        return result_e::unavailable;
      }
      option = *found;
      script.command = script_command(manifest->dir / option.choices_script);
      script.data_dir = plugin_data_dir(reg.paths, id);
      script.env = script_env(*manifest, effective_options(reg, *manifest), script.data_dir);
      script.runner = reg.runner;
      script.timeout = reg.choices_timeout;
    }
    script.env["SUNSHINE_PLUGIN_CHOICE_OPTION"] = option_id;

    std::string content;
    const user_script_output_t output {"choices", "list", "SUNSHINE_PLUGIN_CHOICES_FILE", std::format("Listing choices for option '{}'", option_id), MAX_CHOICES_FILE_SIZE};
    if (run_user_script(std::move(script), output, content, reason)) {
      std::vector<choice_t> listed;
      if (const auto node = json::parse(content, nullptr, false); node.is_discarded()) {
        reason = "the choices script wrote invalid JSON";
      } else if (!parse_script_choices(node, listed, reason)) {
        reason = "the choices script wrote a bad list: " + reason;
      } else {
        out = option.choices;
        std::set<std::string> seen;
        for (const auto &choice : out) {
          seen.insert(choice.value);
        }
        for (auto &choice : listed) {
          if (seen.insert(choice.value).second) {
            out.emplace_back(std::move(choice));
          }
        }
        return result_e::ok;
      }
    }

    BOOST_LOG(error) << "Plugin ["sv << id << "] couldn't list the choices of option ["sv << option_id << "]: "sv << reason;
    return result_e::invalid;
  }

  bool parse_status(const nlohmann::json &node, nlohmann::json &out, std::string &error) {
    if (!node.is_object()) {
      error = "the status must be an object";
      return false;
    }
    json notices = json::array();
    if (node.contains("notices")) {
      const auto &entries = node["notices"];
      if (!entries.is_array()) {
        error = "'notices' must be an array";
        return false;
      }
      if (entries.size() > MAX_NOTICES) {
        error = std::format("there can be at most {} notices", MAX_NOTICES);
        return false;
      }
      for (const auto &entry : entries) {
        json notice;
        if (!parse_notice(entry, notice, error)) {
          return false;
        }
        notices.push_back(std::move(notice));
      }
    }
    out = node;
    out["notices"] = std::move(notices);
    return true;
  }

  result_e plugin_status(const std::string &id, nlohmann::json &out, std::string &reason) {
    auto &reg = registry();
    user_script_t script;
    {
      std::scoped_lock lock {reg.mutex};
      const auto *manifest = find_manifest(reg, id);
      if (!manifest) {
        reason = std::format("plugin '{}' not found", id);
        return result_e::not_found;
      }
      if (manifest->scripts.status.empty()) {
        reason = std::format("plugin '{}' has no status script", id);
        return result_e::invalid;
      }
      if (const auto *state = find_state(reg, id); !state || !state->installed) {
        reason = std::format("plugin '{}' must be installed before reporting its status", id);
        return result_e::not_installed;
      }
      if (!reg.session_ready()) {
        reason = "a user must be signed in to report the status";
        return result_e::unavailable;
      }
      script.command = script_command(manifest->dir / manifest->scripts.status);
      script.data_dir = plugin_data_dir(reg.paths, id);
      script.env = script_env(*manifest, effective_options(reg, *manifest), script.data_dir);
      script.runner = reg.runner;
      script.timeout = reg.choices_timeout;
    }

    std::string content;
    const user_script_output_t output {"status", "status", "SUNSHINE_PLUGIN_STATUS_FILE", "Reporting the status", MAX_STATUS_FILE_SIZE};
    if (run_user_script(std::move(script), output, content, reason)) {
      if (const auto node = json::parse(content, nullptr, false); node.is_discarded()) {
        reason = "the status script wrote invalid JSON";
      } else if (!parse_status(node, out, reason)) {
        reason = "the status script wrote a bad status: " + reason;
      } else {
        return result_e::ok;
      }
    }

    BOOST_LOG(error) << "Plugin ["sv << id << "] couldn't report its status: "sv << reason;
    return result_e::invalid;
  }

  nlohmann::json plugin_notices() {
    auto &reg = registry();
    std::vector<std::pair<std::string, std::string>> reporting;  // ID and name of each plugin to ask
    {
      std::scoped_lock lock {reg.mutex};
      if (!reg.session_ready()) {
        return json::array();
      }
      for (const auto &manifest : reg.manifests) {
        const auto *state = find_state(reg, manifest.id);
        if (!manifest.scripts.status.empty() && state && state->installed && state->active && !needs_reinstall(manifest, *state)) {
          reporting.emplace_back(manifest.id, manifest.name);
        }
      }
    }

    std::vector<std::future<json>> statuses;
    statuses.reserve(reporting.size());
    for (const auto &id : reporting | std::views::keys) {
      statuses.push_back(std::async(std::launch::async, [id]() {
        json status;
        std::string error;
        return plugin_status(id, status, error) == result_e::ok ? status["notices"] : json::array();
      }));
    }

    json notices = json::array();
    for (std::size_t i = 0; i < reporting.size(); ++i) {
      for (auto notice : statuses[i].get()) {
        notice["plugin"] = reporting[i].first;
        notice["plugin_name"] = reporting[i].second;
        notices.push_back(std::move(notice));
      }
    }
    return notices;
  }

  void set_choices_timeout_for_testing(std::chrono::milliseconds timeout) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.choices_timeout = timeout;
  }

  void wait_for_task() {
    auto &reg = registry();
    std::jthread worker;
    {
      std::scoped_lock lock {reg.mutex};
      worker = std::move(reg.worker);
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  void cancel_task() {
    stop_reinstaller();
    auto &reg = registry();
    {
      std::scoped_lock lock {reg.mutex};
      reg.worker.request_stop();
    }
    wait_for_task();
  }

  std::optional<std::string> read_log(const std::string &id) {
    auto &reg = registry();
    fs::path log_file;
    {
      std::scoped_lock lock {reg.mutex};
      if (!find_manifest(reg, id)) {
        return std::nullopt;
      }
      log_file = plugin_log_file(reg.paths, id);
    }

    std::ifstream in(log_file, std::ios::binary);
    if (!in) {
      return std::string {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  std::optional<std::filesystem::path> panel_file(const std::string &id) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    const auto *manifest = find_manifest(reg, id);
    if (!manifest || !manifest->panel) {
      return std::nullopt;
    }
    return manifest->dir / manifest->panel->file;
  }

  bool is_valid_proxy_path(std::string_view path) {
    if (path.empty() || path.size() > 512 || path.front() != '/' || path.starts_with("//"sv)) {
      return false;
    }
    return std::ranges::all_of(path, [](const char c) {
      const auto u = static_cast<unsigned char>(c);
      return u > 0x20 && u < 0x7f && c != '\\' && c != '#';
    });
  }

  namespace {
    /**
     * @brief Find the port a plugin panel may read from and check the requested path.
     *
     * @param id Plugin ID.
     * @param path Path and query to request.
     * @param port Receives the port.
     * @param error Receives the reason when the request is rejected.
     * @return `ok`, `not_found` or `invalid`.
     */
    result_e resolve_proxy(const std::string &id, const std::string &path, int &port, std::string &error) {
      {
        auto &reg = registry();
        std::scoped_lock lock {reg.mutex};
        const auto *manifest = find_manifest(reg, id);
        if (!manifest) {
          error = std::format("plugin '{}' not found", id);
          return result_e::not_found;
        }
        if (!manifest->panel || manifest->panel->port == 0) {
          error = std::format("plugin '{}' has no panel port", id);
          return result_e::invalid;
        }
        port = manifest->panel->port;
      }
      if (!is_valid_proxy_path(path)) {
        error = "invalid proxy path";
        return result_e::invalid;
      }
      return result_e::ok;
    }

    /**
     * @brief Create a curl handle for a request to a plugin's local service, with the options every proxy request uses.
     *
     * @param url Address to request.
     * @return Handle, or an empty pointer when curl could not start.
     */
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> local_request(const std::string &url) {
      std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl {curl_easy_init(), &curl_easy_cleanup};
      if (curl) {
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");  // never through a configured proxy
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
      }
      return curl;
    }

    /**
     * @brief Read the status and content type of a finished or running curl transfer.
     *
     * @param curl Transfer to read.
     * @param status Receives the HTTP status.
     * @param content_type Receives the content type, or an empty string.
     */
    void response_head(CURL *curl, int &status, std::string &content_type) {
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      status = static_cast<int>(code);
      content_type.clear();
      if (char *type = nullptr; curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type) == CURLE_OK && type) {
        content_type = type;
      }
    }
  }  // namespace

  result_e proxy_stream(const std::string &id, const std::string &path, const proxy_head_t &on_head, const proxy_data_t &on_data, std::chrono::milliseconds max_duration, std::string &error) {
    int port = 0;
    if (const auto result = resolve_proxy(id, path, port, error); result != result_e::ok) {
      return result;
    }
    auto curl = local_request(std::format("http://127.0.0.1:{}{}", port, path));
    if (!curl) {
      error = "could not start the request";
      return result_e::unavailable;
    }

    /**
     * @brief What the curl callbacks share with this call.
     */
    struct stream_t {
      CURL *curl;  ///< The transfer.
      const proxy_head_t *on_head;  ///< Head callback.
      const proxy_data_t *on_data;  ///< Body callback.
      std::chrono::steady_clock::time_point deadline;  ///< When to end the request.
      bool head_sent = false;  ///< Whether `on_head` was called.
    } stream {curl.get(), &on_head, &on_data, std::chrono::steady_clock::now() + max_duration};

    const auto write = +[](char *data, std::size_t size, std::size_t count, void *user) -> std::size_t {
      auto *s = static_cast<stream_t *>(user);
      if (!s->head_sent) {
        int status = 0;
        std::string content_type;
        response_head(s->curl, status, content_type);
        (*s->on_head)(status, content_type);
        s->head_sent = true;
      }
      return (*s->on_data)(std::string_view {data, size * count}) ? size * count : 0;  // 0 aborts the transfer
    };
    // Called at least once a second while the transfer runs, also while no data arrives.
    const auto progress = +[](void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
      const auto *s = static_cast<stream_t *>(user);
      return proxy_streams_stopping || std::chrono::steady_clock::now() >= s->deadline ? 1 : 0;  // non-zero aborts
    };
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &stream);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &stream);
    // A service that stops sending altogether (server-sent events send a heartbeat) is given up on.
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 15L);

    const auto code = curl_easy_perform(curl.get());
    if (stream.head_sent) {
      return result_e::ok;  // ended by the service, by on_data, by the deadline or at shutdown
    }
    if (code != CURLE_OK) {
      error = std::format("nothing answered on local port {}: {}", port, curl_easy_strerror(code));
      return result_e::unavailable;
    }
    int status = 0;
    std::string content_type;
    response_head(curl.get(), status, content_type);
    on_head(status, content_type);  // a response without a body
    return result_e::ok;
  }

  void stop_proxy_streams() {
    proxy_streams_stopping = true;
  }

  result_e proxy_get(const std::string &id, const std::string &path, proxy_response_t &out, std::string &error) {
    int port = 0;
    if (const auto result = resolve_proxy(id, path, port, error); result != result_e::ok) {
      return result;
    }
    auto curl = local_request(std::format("http://127.0.0.1:{}{}", port, path));
    if (!curl) {
      error = "could not start the request";
      return result_e::unavailable;
    }

    constexpr std::size_t max_body = 4 * 1024 * 1024;
    out = {};
    const auto write = +[](char *data, std::size_t size, std::size_t count, void *user) -> std::size_t {
      auto *body = static_cast<std::string *>(user);
      if (body->size() + size * count > max_body) {
        return 0;  // aborts the transfer
      }
      body->append(data, size * count);
      return size * count;
    };
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &out.body);

    if (const auto code = curl_easy_perform(curl.get()); code != CURLE_OK) {
      error = std::format("nothing answered on local port {}: {}", port, curl_easy_strerror(code));
      return result_e::unavailable;
    }
    response_head(curl.get(), out.status, out.content_type);
    return result_e::ok;
  }

  void set_change_callback(std::function<void()> callback) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.on_change = std::move(callback);
  }

  void set_activation_callback(std::function<std::vector<std::string>(const std::string &id)> callback) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.on_activate = std::move(callback);
  }

  void set_stream_check(std::function<bool()> check) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.streaming = std::move(check);
  }

  void set_script_runner_for_testing(script_runner_t runner) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.runner = std::move(runner);
  }

  void reset_script_runner_for_testing() {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.runner = run_script;
  }

  void set_session_check_for_testing(std::function<bool()> ready, std::chrono::milliseconds poll) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.session_ready = std::move(ready);
    reg.session_poll = poll;
  }

  void reset_session_check_for_testing() {
    set_session_check_for_testing(default_session_ready, DEFAULT_SESSION_POLL);
  }

  void set_fetcher_for_testing(fetcher_t fetcher) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.fetcher = std::move(fetcher);
  }

  void reset_fetcher_for_testing() {
    set_fetcher_for_testing(fetch_https);
  }
}  // namespace plugins
