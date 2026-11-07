/**
 * @file src/plugins.cpp
 * @brief Definitions for installable plugins that run PowerShell scripts around a streaming session.
 */
// standard includes
#include <algorithm>
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

// local includes
#include "logging.h"
#include "plugins.h"

#ifdef _WIN32
  // fopen() interprets the filename as an ANSI string on Windows, so we use _wfsopen() instead.
  #include <share.h>
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
     * @param stop Requests the script be terminated early.
     */
    void run_task(const std::string &id, action_e action, const manifest_t &manifest, const std::map<std::string, std::string> &env, const paths_t &paths, const script_runner_t &runner, bool activate, std::stop_token stop) {
      const auto &script = action == action_e::install ? manifest.scripts.install : manifest.scripts.uninstall;
      const auto data_dir = plugin_data_dir(paths, id);
      const auto log_file = plugin_log_file(paths, id);

      std::error_code ec;
      fs::create_directories(data_dir, ec);

      if (std::unique_ptr<FILE, decltype(&std::fclose)> file {open_shared(log_file, L"w"), &std::fclose}; file) {
        std::fputs(std::format("{} {} {}\n", action == action_e::install ? "Installing"sv : "Uninstalling"sv, manifest.name, manifest.version).c_str(), file.get());
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

      if (success) {
        BOOST_LOG(info) << "Plugin ["sv << id << "] "sv << (action == action_e::install ? "installed"sv : "uninstalled"sv);
      } else {
        BOOST_LOG(error) << "Plugin ["sv << id << "] "sv << (action == action_e::install ? "install"sv : "uninstall"sv) << " failed with exit code ["sv << exit_code << ']';
      }

      auto &reg = registry();
      std::function<void()> on_change;
      {
        std::scoped_lock lock {reg.mutex};
        if (success) {
          auto &state = reg.state[id];
          state.installed = action == action_e::install;
          state.active = state.installed && (state.active || activate);
          save_state(paths.state_file, reg.state);
          on_change = reg.on_change;
        }
        reg.tasks[id] = {action, success ? task_status_e::succeeded : task_status_e::failed, exit_code};
        reg.task_running = false;
      }

      // Called without the lock since the callback reads the plugin registry.
      if (on_change) {
        on_change();
      }
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
          !read_string(scripts, "disconnect", manifest.scripts.disconnect_script, error)) {
        return std::nullopt;
      }
      for (const auto *script : {&manifest.scripts.install, &manifest.scripts.uninstall, &manifest.scripts.do_script, &manifest.scripts.undo_script, &manifest.scripts.connect_script, &manifest.scripts.disconnect_script}) {
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
        manifest.options.emplace_back(std::move(*option));
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

    auto &reg = registry();
    std::scoped_lock lock {reg.mutex};
    reg.paths = paths;
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
     * @brief Stops a running install or uninstall when Sunshine exits.
     */
    class deinit_t: public platf::deinit_t {
    public:
      ~deinit_t() override {
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
          {"action", info.action == action_e::install ? "install" : "uninstall"},
          {"status", task_status_name(info.status)},
          {"exit_code", info.exit_code}
        };
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
        {"order", manifest.order},
        {"options", std::move(options)},
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

  result_e configure(const std::string &id, std::optional<bool> active, const std::optional<nlohmann::json> &options, std::string &error) {
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

    if (active) {
      state.active = *active;
    }
    if (normalized) {
      state.options = std::move(*normalized);
    }
    save_state(reg.paths.state_file, reg.state);
    return result_e::ok;
  }

  result_e start_task(const std::string &id, action_e action, std::string &error, bool activate) {
    auto &reg = registry();
    std::unique_lock lock {reg.mutex};

    const auto *found = find_manifest(reg, id);
    if (!found) {
      error = std::format("plugin '{}' not found", id);
      return result_e::not_found;
    }
    if (reg.task_running) {
      error = "another plugin operation is still running";
      return result_e::busy;
    }

    const auto manifest = *found;
    const auto env = script_env(manifest, effective_options(reg, manifest), plugin_data_dir(reg.paths, id));
    auto runner = reg.runner;

    if (action == action_e::uninstall) {
      reg.state[id].active = false;
      save_state(reg.paths.state_file, reg.state);
    }

    reg.task_running = true;
    reg.tasks[id] = {action, task_status_e::running, 0};

    // The previous worker has already finished since task_running was false.
    if (reg.worker.joinable()) {
      reg.worker.join();
    }

    reg.worker = std::jthread([id, action, manifest, env, paths = reg.paths, runner, activate](std::stop_token stop) {
      run_task(id, action, manifest, env, paths, runner, activate && action == action_e::install, stop);
    });

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
