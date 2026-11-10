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
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/process/v1.hpp>
#include <curl/curl.h>

// local includes
#include "logging.h"
#include "plugins.h"

#ifdef _WIN32
  // fopen() interprets the filename as an ANSI string on Windows, so we use _wfsopen() instead.
  #include <share.h>

  #include <boost/winapi/process.hpp>
  #include <boost/winapi/wait.hpp>
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

    int run_script(const std::string &command, bool elevated, const std::map<std::string, std::string> &env, const fs::path &working_dir, const fs::path &log_file, std::stop_token stop);

    /**
     * @brief Everything the plugin registry knows, guarded by `mutex`.
     */
    struct registry_t {
      std::mutex mutex;  ///< Guards every other member except `worker`.
      paths_t paths;  ///< Locations in use.
      std::vector<manifest_t> manifests;  ///< Discovered plugins, sorted by order.
      std::unordered_map<std::string, plugin_state_t> state;  ///< Persisted state keyed by plugin ID.
      std::unordered_map<std::string, task_info_t> tasks;  ///< Last operation keyed by plugin ID.
      bool task_running = false;  ///< Whether an install or uninstall is running.
      script_runner_t runner = run_script;  ///< Runs install and uninstall scripts.
      std::function<void()> on_change;  ///< Called after a background operation changes plugin state.
      std::jthread worker;  ///< Thread running the current install or uninstall.
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
     * @return `true` when the value has the right type and lies within the allowed range or list.
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
          return value.is_string() && std::ranges::any_of(option.choices, [&value](const choice_t &choice) {
                   return choice.value == value.get<std::string>();
                 });
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
     * @brief Parse the list of values a choice option accepts.
     *
     * @param node Array of strings or of objects with `value` and optional `label`.
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
        if (entry.is_string()) {
          choice.value = entry.get<std::string>();
        } else if (entry.is_object() && entry.contains("value") && entry["value"].is_string()) {
          choice.value = entry["value"].get<std::string>();
          if (!read_string(entry, "label", choice.label, error)) {
            return false;
          }
        } else {
          error = "each choice must be a string or an object with a string 'value'";
          return false;
        }
        if (choice.label.empty()) {
          choice.label = choice.value;
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
      if (option.label.empty()) {
        option.label = option.id;
      }

      if (option.type == option_type_e::choice && !parse_choices(node.value("choices", json::array()), option.choices, error)) {
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
      if (node.contains("height")) {
        if (!node["height"].is_number_integer() || node["height"].get<int>() < 100 || node["height"].get<int>() > 2000) {
          error = "panel 'height' must be an integer from 100 to 2000";
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
      {
        std::scoped_lock lock {reg.mutex};
        if (success && action != action_e::run) {
          auto &state = reg.state[id];
          if (action == action_e::apply) {
            state.options = *pending_options;
          } else {
            state.installed = action == action_e::install;
            state.active = state.installed && (state.active || activate);
          }
          save_state(paths.state_file, reg.state);
          on_change = reg.on_change;
        }
        reg.tasks[id] = {action, success ? task_status_e::succeeded : task_status_e::failed, exit_code, action_id};
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
  }  // namespace

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
        !read_string(node, "homepage", manifest.homepage, error)) {
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
          !read_string(scripts, "apply", manifest.scripts.apply, error)) {
        return std::nullopt;
      }
      for (const auto *script : {&manifest.scripts.install, &manifest.scripts.uninstall, &manifest.scripts.do_script, &manifest.scripts.undo_script, &manifest.scripts.connect_script, &manifest.scripts.disconnect_script, &manifest.scripts.apply}) {
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
        {"options", plugin.options}
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

    for (const auto &manifest : reg.manifests) {
      const auto it = reg.state.find(manifest.id);
      const bool active = it != reg.state.end() && it->second.active;
      BOOST_LOG(info) << "Found plugin ["sv << manifest.id << ' ' << manifest.version << ']' << (active ? " (active)"sv : ""sv);
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
          {"default", option.default_value}
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
          entry["choices"] = json::array();
          for (const auto &choice : option.choices) {
            entry["choices"].push_back({{"value", choice.value}, {"label", choice.label}});
          }
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
      json panel = nullptr;
      if (manifest.panel) {
        panel = {{"height", manifest.panel->height}, {"proxy", manifest.panel->port != 0}};
      }

      result.push_back({
        {"id", manifest.id},
        {"name", manifest.name},
        {"version", manifest.version},
        {"description", manifest.description},
        {"author", manifest.author},
        {"homepage", manifest.homepage},
        {"builtin", manifest.builtin},
        {"elevated", manifest.elevated},
        {"manages_display", manifest.manages_display},
        {"order", manifest.order},
        {"options", std::move(options)},
        {"actions", std::move(actions)},
        {"client_commands", std::move(client_commands)},
        {"icon", manifest.icon},
        {"panel", std::move(panel)},
        {"installed", state.installed},
        {"active", state.active},
        {"config", effective_options(reg, manifest)},
        {"task", std::move(task)}
      });
    }
    return result;
  }

  std::vector<config::prep_cmd_t> prep_cmds(const std::vector<std::string> &excluded) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    std::vector<config::prep_cmd_t> cmds;
    for (const auto &manifest : reg.manifests) {
      const auto it = reg.state.find(manifest.id);
      if (it == reg.state.end() || !it->second.active || !it->second.installed) {
        continue;
      }
      if (std::ranges::find(excluded, manifest.id) != excluded.end()) {
        continue;
      }
      const auto env = script_env(manifest, effective_options(reg, manifest), plugin_data_dir(reg.paths, manifest.id));
      auto add = [&](const std::string &do_script, const std::string &undo_script, config::prep_cmd_t::type_e type) {
        if (do_script.empty() && undo_script.empty()) {
          return;
        }
        auto do_cmd = do_script.empty() ? std::string {} : script_command(manifest.dir / do_script);
        auto undo_cmd = undo_script.empty() ? std::string {} : script_command(manifest.dir / undo_script);
        auto elevated = manifest.elevated;
        auto &cmd = cmds.emplace_back(std::move(do_cmd), std::move(undo_cmd), std::move(elevated), type);
        cmd.env = env;
      };
      add(manifest.scripts.do_script, manifest.scripts.undo_script, config::prep_cmd_t::type_e::app);
      add(manifest.scripts.connect_script, manifest.scripts.disconnect_script, config::prep_cmd_t::type_e::connect);
    }
    return cmds;
  }

  std::vector<offered_commands_t> offered_client_commands(const std::vector<std::string> &excluded, const client_commands::plugin_overrides_t &overrides) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    std::vector<offered_commands_t> offered;
    std::optional<std::string> foreground;  // Only looked up when a command is limited to programs
    for (const auto &manifest : reg.manifests) {
      if (manifest.client_commands.empty() || std::ranges::find(excluded, manifest.id) != excluded.end()) {
        continue;
      }
      const auto it = reg.state.find(manifest.id);
      if (it == reg.state.end() || !it->second.active || !it->second.installed) {
        continue;
      }

      offered_commands_t plugin {manifest.id, manifest.name, manifest.icon, {}, {}};
      const auto data_dir = plugin_data_dir(reg.paths, manifest.id);
      const auto env = script_env(manifest, effective_options(reg, manifest), plugin_data_dir(reg.paths, manifest.id));
      for (const auto &source : manifest.client_commands) {
        const auto entry = overrides.find(std::format("{}/{}", manifest.id, source.id));
        const bool has_override = entry != overrides.end();
        const auto &programs = has_override && entry->second.programs ? *entry->second.programs : source.programs;
        if (!programs.empty()) {
          if (!foreground) {
            foreground = client_commands::foreground_program();
          }
          if (!client_commands::matches(programs, *foreground)) {
            continue;
          }
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

  std::string display_plugin(const std::vector<std::string> &excluded) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

    for (const auto &manifest : reg.manifests) {
      if (!manifest.manages_display || std::ranges::find(excluded, manifest.id) != excluded.end()) {
        continue;
      }
      const auto it = reg.state.find(manifest.id);
      if (it != reg.state.end() && it->second.active && it->second.installed) {
        return manifest.name;
      }
    }
    return {};
  }

  result_e configure(const std::string &id, std::optional<bool> active, const std::optional<nlohmann::json> &options, std::string &error, bool *applying) {
    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};

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

    if (active) {
      state.active = *active;
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

    const auto manifest = *found;
    const auto env = script_env(manifest, effective_options(reg, manifest), plugin_data_dir(reg.paths, id));

    if (action == action_e::uninstall) {
      reg.state[id].active = false;
      save_state(reg.paths.state_file, reg.state);
    }

    launch_task(reg, id, action, manifest, env, activate, action_id, std::nullopt);

    return result_e::ok;
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
}  // namespace plugins
