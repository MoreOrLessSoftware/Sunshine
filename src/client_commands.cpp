/**
 * @file src/client_commands.cpp
 * @brief Definitions for commands the host offers in a client's in-stream overlay menu.
 */
// standard includes
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <thread>

#ifdef _WIN32
  #include <share.h>
#endif

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/process/v1.hpp>

// local includes
#include "client_commands.h"
#include "input.h"
#include "logging.h"
#include "plugins.h"
#include "process.h"

using namespace std::literals;

namespace client_commands {
  namespace {
    /// Time each key is held or released before the next key event, as on the clients.
    constexpr auto KEY_DELAY = 50ms;

    /// Time after each chord before the next one starts, as on the clients.
    constexpr auto CHORD_DELAY = 100ms;

    /// Most chords one command may type.
    constexpr std::size_t MAX_STEPS = 64;

    /// Most keys one chord may hold.
    constexpr std::size_t MAX_KEYS_PER_STEP = 8;

    /**
     * @brief The key sequence being typed, if any.
     */
    struct typist_t {
      std::mutex mutex;  ///< Guards every member.
      std::condition_variable_any wake;  ///< Ends a wait between key events early when the sequence is stopped.
      std::jthread thread;  ///< Thread typing the sequence; joinable while one has been started.
      bool typing = false;  ///< Whether a sequence is still being typed.
      key_sink_t sink;  ///< Receives the key events in tests, which also skip the waits.
    };

    /**
     * @brief Return the key sequence state.
     *
     * @return State shared by every caller.
     */
    typist_t &typist() {
      static typist_t instance;
      return instance;
    }

    /**
     * @brief A registered action.
     */
    struct registered_action_t {
      action_handler_t handler;  ///< Runs the action.
      action_status_t status;  ///< Decides how its commands show, or empty.
    };

    /**
     * @brief The registered actions.
     */
    struct action_registry_t {
      std::mutex mutex;  ///< Guards `actions`.
      std::map<std::string, registered_action_t, std::less<>> actions;  ///< Actions by name.
    };

    /**
     * @brief Return the action registry.
     *
     * @return Registry shared by every caller.
     */
    action_registry_t &action_registry() {
      static action_registry_t instance;
      return instance;
    }

    /**
     * @brief Look up a registered action.
     *
     * @param name Action name.
     * @return A copy of the action, or `std::nullopt` when it has no handler. Copies let handlers run unlocked.
     */
    std::optional<registered_action_t> find_action(std::string_view name) {
      auto &registry = action_registry();
      std::scoped_lock lock {registry.mutex};
      const auto it = registry.actions.find(name);
      if (it == registry.actions.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    /**
     * @brief Read an optional string member.
     *
     * @param json Object to read.
     * @param key Member name.
     * @return The string, or empty when the member is missing or not a string.
     */
    std::string string_member(const nlohmann::json &json, const char *key) {
      auto it = json.find(key);
      return it != json.end() && it->is_string() ? it->get<std::string>() : std::string {};
    }

    /**
     * @brief Read an optional boolean member.
     *
     * @param json Object to read.
     * @param key Member name.
     * @param fallback Value when the member is missing or not a boolean.
     * @return The boolean.
     */
    bool bool_member(const nlohmann::json &json, const char *key, bool fallback) {
      auto it = json.find(key);
      return it != json.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    /**
     * @brief Parse the key chords of a `keys` command.
     *
     * @param json Array of chords, each an array of virtual-key codes from 1 to 254.
     * @param steps Receives the chords.
     * @param error Receives the reason when the chords are rejected.
     * @return `true` when the chords are valid.
     */
    bool parse_steps(const nlohmann::json &json, std::vector<std::vector<std::uint16_t>> &steps, std::string &error) {
      if (!json.is_array() || json.empty()) {
        error = "\"steps\" must be a non-empty array of key chords";
        return false;
      }
      if (json.size() > MAX_STEPS) {
        error = std::format("\"steps\" may hold at most {} chords", MAX_STEPS);
        return false;
      }

      for (const auto &step_json : json) {
        if (!step_json.is_array() || step_json.empty() || step_json.size() > MAX_KEYS_PER_STEP) {
          error = std::format("each chord must be an array of 1 to {} key codes", MAX_KEYS_PER_STEP);
          return false;
        }

        std::vector<std::uint16_t> step;
        for (const auto &key : step_json) {
          if (!key.is_number_integer() || key.get<int>() < 1 || key.get<int>() > 254) {
            error = "key codes must be integers from 1 to 254";
            return false;
          }
          step.push_back(static_cast<std::uint16_t>(key.get<int>()));
        }
        steps.push_back(std::move(step));
      }

      return true;
    }

    /**
     * @brief Describe one command for the client.
     *
     * @param command Command to describe.
     * @return Object with the fields the client shows.
     */
    nlohmann::json describe_command(const command_t &command) {
      return {
        {"id", command.id},
        {"label", command.label},
        {"icon", command.icon},
        {"close-menu", command.close_menu},
      };
    }

    /**
     * @brief Type a key sequence, waiting between events, until it ends or is stopped.
     *
     * Keys still held when the sequence is stopped are released.
     *
     * @param stop Requests the sequence end early.
     * @param events Key events to send.
     */
    void type_keys(std::stop_token stop, std::vector<key_event_t> events) {
      auto &state = typist();
      std::vector<std::uint16_t> held;

      auto send = [&](std::uint16_t key_code, bool release) {
        key_sink_t sink;
        {
          std::scoped_lock lock {state.mutex};
          sink = state.sink;
        }
        if (sink) {
          sink(key_code, release);
        } else {
          input::send_host_key(key_code, release);
        }
      };

      for (const auto &event : events) {
        if (stop.stop_requested()) {
          break;
        }

        send(event.key_code, event.release);
        if (event.release) {
          std::erase(held, event.key_code);
        } else {
          held.push_back(event.key_code);
        }

        std::unique_lock lock {state.mutex};
        if (!state.sink) {
          state.wake.wait_for(lock, stop, event.delay_after, [] {
            return false;
          });
        }
      }

      for (auto it = held.rbegin(); it != held.rend(); ++it) {
        send(*it, true);
      }

      std::scoped_lock lock {state.mutex};
      state.typing = false;
    }

    /**
     * @brief Start a `script` command and leave it running.
     *
     * @param command Command to start.
     * @param context Who the command runs for.
     * @param error Receives the reason when it does not start.
     * @param on_finished When set and the command has a `finish_wait`, called from another thread once the script
     *                    has ended or that long has passed; otherwise called before this returns, once it started.
     * @return `true` when it started.
     */
    bool start_script(const command_t &command, const run_context_t &context, std::string &error, const std::function<void()> &on_finished) {
      // A copy, so the variables are only for the command
      boost::process::v1::environment env = boost::this_process::environment();
      env["SUNSHINE_CLIENT_COMMAND_ID"] = command.id;
      env["SUNSHINE_CLIENT_COMMAND_LABEL"] = command.label;
      if (!context.client_name.empty()) {
        env["SUNSHINE_CLIENT_NAME"] = context.client_name;
      }
      if (!context.app_id.empty()) {
        env["SUNSHINE_APP_ID"] = context.app_id;
      }
      if (!context.app_name.empty()) {
        env["SUNSHINE_APP_NAME"] = context.app_name;
      }
      for (const auto &[name, value] : command.env) {
        env[name] = value;
      }

      boost::filesystem::path working_dir = command.working_dir.empty() ?
                                              proc::find_working_directory(command.cmd, env) :
                                              boost::filesystem::path(command.working_dir);

      BOOST_LOG(info) << "Running client command ["sv << command.label << "]: ["sv << command.cmd << "] in ["sv << working_dir << ']';

      // The output goes to the log file when there is one, with a heading for each run
      std::unique_ptr<FILE, decltype(&std::fclose)> log {nullptr, &std::fclose};
      if (!command.log_file.empty()) {
#ifdef _WIN32
        log.reset(_wfsopen(command.log_file.wstring().c_str(), L"a", _SH_DENYNO));
#else
        log.reset(std::fopen(command.log_file.c_str(), "a"));
#endif
        if (log) {
          std::fputs(std::format("\nRunning client command {}\n", command.label).c_str(), log.get());
          std::fflush(log.get());
        }
      }

      std::error_code ec;
      // Without a console window, which would show over the game and could take the foreground
      auto child = platf::run_command(command.elevated, false, command.cmd, working_dir, env, log.get(), ec, nullptr);
      if (ec) {
        error = std::format("Couldn't start [{}]: {}", command.cmd, ec.message());
        BOOST_LOG(warning) << error;
        return false;
      }

      if (!on_finished || command.finish_wait <= std::chrono::milliseconds::zero()) {
        child.detach();
        if (on_finished) {
          on_finished();
        }
        return true;
      }

      // Waited for on a thread of its own, so the caller (such as the client's request) isn't held up
      std::thread([child = std::move(child), wait = command.finish_wait, label = command.label, on_finished]() mutable {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        std::error_code running_ec;
        // child::wait_for() is unreliable in Boost.Process v1, so poll
        while (child.running(running_ec) && !running_ec && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(50ms);
        }
        if (child.running(running_ec) && !running_ec) {
          BOOST_LOG(debug) << "Client command ["sv << label << "] is still running; reporting it ran"sv;
        }
        child.detach();
        on_finished();
      }).detach();
      return true;
    }
  }  // namespace

  std::optional<command_t> parse_command(const nlohmann::json &json, const std::string &fallback_id, std::string &error) {
    if (!json.is_object()) {
      error = "a command must be an object";
      return std::nullopt;
    }

    command_t command;
    command.id = string_member(json, "id");
    if (command.id.empty()) {
      command.id = fallback_id;
    }
    command.label = string_member(json, "label");
    if (command.label.empty()) {
      error = "\"label\" is required";
      return std::nullopt;
    }
    command.icon = string_member(json, "icon");
    command.close_menu = bool_member(json, "close-menu", true);
    command.enabled = bool_member(json, "enabled", true);
    if (const auto programs = json.find("programs"); programs != json.end()) {
      if (!programs->is_array() || !std::ranges::all_of(*programs, [](const nlohmann::json &program) {
            return program.is_string() && !program.get<std::string>().empty();
          })) {
        error = "\"programs\" must be an array of file names";
        return std::nullopt;
      }
      command.programs = programs->get<std::vector<std::string>>();
    }

    const auto type = string_member(json, "type");
    if (type == "keys") {
      command.type = type_e::keys;
      if (!parse_steps(json.contains("steps") ? json["steps"] : nlohmann::json {}, command.steps, error)) {
        return std::nullopt;
      }
    } else if (type == "script") {
      command.type = type_e::script;
      command.cmd = string_member(json, "cmd");
      if (command.cmd.empty()) {
        error = "\"cmd\" is required for a script command";
        return std::nullopt;
      }
      command.working_dir = string_member(json, "working-dir");
      command.elevated = bool_member(json, "elevated", false);
    } else {
      error = "\"type\" must be \"keys\" or \"script\"";
      return std::nullopt;
    }

    return command;
  }

  std::vector<command_t> parse_commands(const nlohmann::json &json, std::string_view scope) {
    std::vector<command_t> commands;
    if (!json.is_array()) {
      if (!json.is_null()) {
        BOOST_LOG(warning) << "Ignoring the client commands of ["sv << scope << "]: not an array"sv;
      }
      return commands;
    }

    for (std::size_t i = 0; i < json.size(); i++) {
      std::string error;
      auto command = parse_command(json[i], std::format("{}/{}", scope, i), error);
      if (!command) {
        BOOST_LOG(warning) << "Ignoring client command "sv << i << " of ["sv << scope << "]: "sv << error;
        continue;
      }
      commands.push_back(std::move(*command));
    }

    return commands;
  }

  std::vector<command_t> load_global(const std::filesystem::path &file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
      return {};
    }

    try {
      std::ifstream in {file};
      auto json = nlohmann::json::parse(in);
      return parse_commands(json.is_object() && json.contains("commands") ? json["commands"] : nlohmann::json {}, "global");
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Couldn't read the client commands file: "sv << e.what();
      return {};
    }
  }

  std::optional<program_group_t> parse_program_group(const nlohmann::json &json, const std::string &fallback_id, bool strict, std::string &error) {
    if (!json.is_object()) {
      error = "a program group must be an object";
      return std::nullopt;
    }

    program_group_t group;
    group.id = string_member(json, "id");
    if (group.id.empty()) {
      if (strict) {
        error = "it has no id";
        return std::nullopt;
      }
      group.id = fallback_id;
    }
    group.label = string_member(json, "label");
    if (group.label.empty()) {
      error = "\"label\" is required";
      return std::nullopt;
    }
    group.icon = string_member(json, "icon");

    auto programs = json.find("programs");
    if (programs == json.end() || !programs->is_array() || programs->empty()) {
      error = "\"programs\" must name at least one program";
      return std::nullopt;
    }
    for (const auto &program : *programs) {
      if (!program.is_string() || program.get<std::string>().empty()) {
        error = "each program must be a non-empty file name";
        return std::nullopt;
      }
      group.programs.push_back(program.get<std::string>());
    }

    const auto commands = json.contains("commands") ? json["commands"] : nlohmann::json::array();
    if (strict) {
      auto parsed = parse_commands_strict(commands, error);
      if (!parsed) {
        return std::nullopt;
      }
      group.commands = std::move(*parsed);
    } else {
      group.commands = parse_commands(commands, std::format("program:{}", group.id));
    }

    return group;
  }

  std::vector<program_group_t> parse_program_groups(const nlohmann::json &json) {
    std::vector<program_group_t> groups;
    if (!json.is_array()) {
      return groups;
    }

    for (std::size_t i = 0; i < json.size(); i++) {
      std::string error;
      auto group = parse_program_group(json[i], std::format("program{}", i), false, error);
      if (!group) {
        BOOST_LOG(warning) << "Ignoring program group "sv << i << " of the client commands: "sv << error;
        continue;
      }
      groups.push_back(std::move(*group));
    }

    return groups;
  }

  std::optional<std::vector<program_group_t>> parse_program_groups_strict(const nlohmann::json &json, std::string &error) {
    if (!json.is_array()) {
      error = "the program groups must be an array";
      return std::nullopt;
    }

    std::vector<program_group_t> groups;
    for (std::size_t i = 0; i < json.size(); i++) {
      const auto label = json[i].is_object() ? string_member(json[i], "label") : std::string {};
      const auto name = label.empty() ? std::format("program group {}", i + 1) : std::format("\"{}\"", label);

      std::string reason;
      auto group = parse_program_group(json[i], {}, true, reason);
      if (!group) {
        error = std::format("{}: {}", name, reason);
        return std::nullopt;
      }
      if (std::ranges::any_of(groups, [&](const program_group_t &other) {
            return other.id == group->id;
          })) {
        error = std::format("{} has the same id as another group", name);
        return std::nullopt;
      }
      groups.push_back(std::move(*group));
    }

    return groups;
  }

  namespace {
    /**
     * @brief Parse one override of a plugin command.
     *
     * @param key `<plugin id>/<command id>`.
     * @param json Override object.
     * @param error Receives the reason when it is rejected.
     * @return The override, or `std::nullopt` when it is invalid.
     */
    std::optional<plugin_override_t> parse_plugin_override(const std::string &key, const nlohmann::json &json, std::string &error) {
      const auto slash = key.find('/');
      if (slash == std::string::npos || slash == 0 || slash == key.size() - 1) {
        error = std::format("\"{}\" must be <plugin id>/<command id>", key);
        return std::nullopt;
      }
      if (!json.is_object()) {
        error = std::format("\"{}\" must be an object", key);
        return std::nullopt;
      }

      plugin_override_t entry {string_member(json, "label"), string_member(json, "icon")};
      entry.enabled = bool_member(json, "enabled", true);
      if (!entry.icon.empty() && !entry.icon.starts_with("material:")) {
        error = std::format("\"{}\": the icon must be material:<name>", key);
        return std::nullopt;
      }
      if (const auto programs = json.find("programs"); programs != json.end()) {
        if (!programs->is_array() || !std::ranges::all_of(*programs, [](const nlohmann::json &program) {
              return program.is_string() && !program.get<std::string>().empty();
            })) {
          error = std::format("\"{}\": \"programs\" must be an array of file names", key);
          return std::nullopt;
        }
        entry.programs = programs->get<std::vector<std::string>>();
      }
      if (const auto close_menu = json.find("close-menu"); close_menu != json.end()) {
        if (!close_menu->is_boolean()) {
          error = std::format("\"{}\": \"close-menu\" must be true or false", key);
          return std::nullopt;
        }
        entry.close_menu = close_menu->get<bool>();
      }
      return entry;
    }

    /**
     * @brief Check whether an override of a plugin command changes anything.
     *
     * @param entry Override to check.
     * @return `true` when it sets a label, an icon, programs or whether the menu closes, or turns the command off.
     */
    bool changes_something(const plugin_override_t &entry) {
      return !entry.label.empty() || !entry.icon.empty() || entry.programs.has_value() || entry.close_menu.has_value() || !entry.enabled;
    }
  }  // namespace

  plugin_overrides_t parse_plugin_overrides(const nlohmann::json &json) {
    plugin_overrides_t overrides;
    if (!json.is_object()) {
      return overrides;
    }

    for (const auto &[key, value] : json.items()) {
      std::string error;
      auto entry = parse_plugin_override(key, value, error);
      if (!entry) {
        BOOST_LOG(warning) << "Ignoring a plugin command override: "sv << error;
      } else if (changes_something(*entry)) {
        overrides[key] = std::move(*entry);
      }
    }
    return overrides;
  }

  std::optional<plugin_overrides_t> parse_plugin_overrides_strict(const nlohmann::json &json, std::string &error) {
    if (!json.is_object()) {
      error = "the plugin command overrides must be an object";
      return std::nullopt;
    }

    plugin_overrides_t overrides;
    for (const auto &[key, value] : json.items()) {
      auto entry = parse_plugin_override(key, value, error);
      if (!entry) {
        return std::nullopt;
      }
      if (changes_something(*entry)) {
        overrides[key] = std::move(*entry);
      }
    }
    return overrides;
  }

  plugin_overrides_t load_plugin_overrides(const std::filesystem::path &file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
      return {};
    }

    try {
      std::ifstream in {file};
      auto json = nlohmann::json::parse(in);
      return parse_plugin_overrides(json.is_object() && json.contains("plugin-commands") ? json["plugin-commands"] : nlohmann::json {});
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Couldn't read the client commands file: "sv << e.what();
      return {};
    }
  }

  nlohmann::json to_json(const plugin_overrides_t &overrides) {
    nlohmann::json json = nlohmann::json::object();
    for (const auto &[key, entry] : overrides) {
      nlohmann::json item = nlohmann::json::object();
      if (!entry.label.empty()) {
        item["label"] = entry.label;
      }
      if (!entry.icon.empty()) {
        item["icon"] = entry.icon;
      }
      if (entry.programs) {
        item["programs"] = *entry.programs;
      }
      if (entry.close_menu) {
        item["close-menu"] = *entry.close_menu;
      }
      if (!entry.enabled) {
        item["enabled"] = false;
      }
      json[key] = std::move(item);
    }
    return json;
  }

  std::vector<program_group_t> load_program_groups(const std::filesystem::path &file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
      return {};
    }

    try {
      std::ifstream in {file};
      auto json = nlohmann::json::parse(in);
      return parse_program_groups(json.is_object() && json.contains("program-groups") ? json["program-groups"] : nlohmann::json {});
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Couldn't read the client commands file: "sv << e.what();
      return {};
    }
  }

  namespace {
    /**
     * @brief Check whether a name from a list of programs is a program's file name.
     *
     * @param name File name or path from the list, without a leading `~`; the `.exe` can be left out.
     * @param program File name of a program, such as `notepad.exe`.
     * @return `true` when they match, ignoring case.
     */
    bool same_program(std::string_view name, std::string_view program) {
      if (program.empty()) {
        return false;
      }
      // A path matches by its file name, and the .exe can be left out
      auto file_name = std::filesystem::path {std::string {name}}.filename().string();
      return !file_name.empty() && (boost::iequals(file_name, program) || (!boost::iends_with(file_name, ".exe") && boost::iequals(file_name + ".exe", program)));
    }
  }  // namespace

  host_programs_t::host_programs_t(std::string foreground, std::vector<std::string> running):
      foreground_ {std::move(foreground)},
      running_ {std::move(running)} {
  }

  const std::string &host_programs_t::foreground() const {
    if (!foreground_) {
      foreground_ = foreground_program();
    }
    return *foreground_;
  }

  bool host_programs_t::running(std::string_view name) const {
    if (same_program(name, foreground())) {
      return true;
    }
    if (!running_) {
      running_ = running_programs();
    }
    return std::ranges::any_of(*running_, [name](const std::string &program) {
      return same_program(name, program);
    });
  }

  bool matches(const std::vector<std::string> &programs, const host_programs_t &host) {
    return std::ranges::any_of(programs, [&host](const std::string &name) {
      if (name.starts_with('~')) {
        return host.running(std::string_view {name}.substr(1));
      }
      return same_program(name, host.foreground());
    });
  }

  bool matches(const std::vector<std::string> &programs, std::string_view program) {
    return matches(programs, host_programs_t {std::string {program}, {}});
  }

  bool matches(const program_group_t &group, std::string_view program) {
    return matches(group.programs, program);
  }

  std::vector<command_t> offered_commands(std::vector<command_t> commands, const host_programs_t &host) {
    std::erase_if(commands, [&host](const command_t &command) {
      return !command.enabled || (!command.programs.empty() && !matches(command.programs, host));
    });
    return commands;
  }

  std::vector<group_t> program_offer(const std::vector<program_group_t> &groups, const host_programs_t &host) {
    std::vector<group_t> offered;
    for (const auto &group : groups) {
      auto commands = offered_commands(group.commands, host);
      if (!commands.empty() && matches(group.programs, host)) {
        offered.push_back(group_t {"program", group.label, group.icon, std::move(commands)});
      }
    }
    return offered;
  }

  std::vector<group_t> program_offer(const std::vector<program_group_t> &groups, std::string_view program) {
    return program_offer(groups, host_programs_t {std::string {program}, {}});
  }

  namespace {
    /**
     * @brief Return the foreground program set for tests.
     *
     * @return The program, or `std::nullopt` to ask the platform.
     */
    std::optional<std::string> &foreground_override() {
      static std::optional<std::string> program;
      return program;
    }

    /**
     * @brief Return the running programs set for tests.
     *
     * @return The programs, or `std::nullopt` to ask the platform.
     */
    std::optional<std::vector<std::string>> &running_override() {
      static std::optional<std::vector<std::string>> programs;
      return programs;
    }
  }  // namespace

  std::string foreground_program() {
    if (foreground_override()) {
      return *foreground_override();
    }
    return platf::foreground_program();
  }

  void set_foreground_program_for_testing(std::optional<std::string> program) {
    foreground_override() = std::move(program);
  }

  std::vector<std::string> running_programs() {
    if (running_override()) {
      return *running_override();
    }
    return platf::running_programs();
  }

  void set_running_programs_for_testing(std::optional<std::vector<std::string>> programs) {
    running_override() = std::move(programs);
  }

  std::filesystem::path default_file() {
    return platf::appdata() / "client_commands.json";
  }

  namespace {
    /**
     * @brief Return the file override set for tests.
     *
     * @return The override, empty when none is set.
     */
    std::filesystem::path &file_override() {
      static std::filesystem::path path;
      return path;
    }
  }  // namespace

  std::filesystem::path file() {
    return file_override().empty() ? default_file() : file_override();
  }

  void set_file_for_testing(const std::filesystem::path &path) {
    file_override() = path;
  }

  nlohmann::json to_json(const command_t &command) {
    nlohmann::json json = {
      {"id", command.id},
      {"label", command.label},
      {"icon", command.icon},
      {"close-menu", command.close_menu},
    };
    // Only written when off, so commands that are on read the same as before the setting existed
    if (!command.enabled) {
      json["enabled"] = false;
    }
    if (!command.programs.empty()) {
      json["programs"] = command.programs;
    }

    if (command.type == type_e::keys) {
      json["type"] = "keys";
      json["steps"] = command.steps;
    } else {
      json["type"] = "script";
      json["cmd"] = command.cmd;
      json["working-dir"] = command.working_dir;
      json["elevated"] = command.elevated;
    }

    return json;
  }

  nlohmann::json to_json(const program_group_t &group) {
    return {
      {"id", group.id},
      {"label", group.label},
      {"icon", group.icon},
      {"programs", group.programs},
      {"commands", to_json(group.commands)},
    };
  }

  nlohmann::json to_json(const std::vector<program_group_t> &groups) {
    nlohmann::json json = nlohmann::json::array();
    for (const auto &group : groups) {
      json.push_back(to_json(group));
    }
    return json;
  }

  nlohmann::json to_json(const std::vector<command_t> &commands) {
    nlohmann::json json = nlohmann::json::array();
    for (const auto &command : commands) {
      json.push_back(to_json(command));
    }
    return json;
  }

  std::optional<std::vector<command_t>> parse_commands_strict(const nlohmann::json &json, std::string &error) {
    if (!json.is_array()) {
      error = "the commands must be an array";
      return std::nullopt;
    }

    std::vector<command_t> commands;
    for (std::size_t i = 0; i < json.size(); i++) {
      const auto &item = json[i];
      const auto label = item.is_object() ? string_member(item, "label") : std::string {};
      const auto name = label.empty() ? std::format("command {}", i + 1) : std::format("\"{}\"", label);

      if (!item.is_object() || string_member(item, "id").empty()) {
        error = std::format("{} has no id", name);
        return std::nullopt;
      }

      std::string reason;
      auto command = parse_command(item, {}, reason);
      if (!command) {
        error = std::format("{}: {}", name, reason);
        return std::nullopt;
      }

      if (std::ranges::any_of(commands, [&](const command_t &other) {
            return other.id == command->id;
          })) {
        error = std::format("{} has the same id as another command", name);
        return std::nullopt;
      }

      commands.push_back(std::move(*command));
    }

    return commands;
  }

  bool save_global(const std::filesystem::path &path, const std::vector<command_t> &commands, std::string &error) {
    return save(path, commands, std::nullopt, error);
  }

  bool save(const std::filesystem::path &path, const std::optional<std::vector<command_t>> &commands, const std::optional<std::vector<program_group_t>> &groups, std::string &error, const std::optional<plugin_overrides_t> &overrides) {
    nlohmann::json json = nlohmann::json::object();

    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
      try {
        std::ifstream in {path};
        json = nlohmann::json::parse(in);
        if (!json.is_object()) {
          json = nlohmann::json::object();
        }
      } catch (const std::exception &) {
        // An unreadable file is replaced
        json = nlohmann::json::object();
      }
    }

    if (commands) {
      json["commands"] = to_json(*commands);
    }
    if (groups) {
      json["program-groups"] = to_json(*groups);
    }
    if (overrides) {
      json["plugin-commands"] = to_json(*overrides);
    }

    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    out << json.dump(4);
    out.close();
    if (!out) {
      error = "Couldn't write the client commands file";
      return false;
    }
    return true;
  }

  offer_t make_offer(std::vector<command_t> global, const std::string &app_name, std::vector<command_t> app_commands, const host_programs_t &host) {
    offer_t offer;
    offer.commands = offered_commands(std::move(global), host);
    app_commands = offered_commands(std::move(app_commands), host);

    if (!app_name.empty() && !app_commands.empty()) {
      offer.groups.push_back(group_t {"app", app_name, {}, std::move(app_commands)});
    }

    return offer;
  }

  offer_t current_offer(const std::string &client_uuid) {
    std::string app_id;
    std::string app_name;
    std::vector<command_t> app_commands;
    std::vector<std::string> excluded_plugins;
    nlohmann::json plugin_options;

    if (const int running_id = proc::proc.running(); running_id > 0) {
      for (const auto &app : proc::proc.get_apps()) {
        if (app.id == std::to_string(running_id)) {
          app_id = app.id;
          app_name = app.name;
          excluded_plugins = app.exclude_plugins;
          plugin_options = app.plugin_options;
          app_commands = app.client_commands;
          break;
        }
      }
    }

    // The programs on the host are only looked up once, and only when a command or group lists programs
    const host_programs_t host;
    auto offer = make_offer(load_global(file()), app_name, std::move(app_commands), host);

    if (auto groups = load_program_groups(file()); !groups.empty()) {
      for (auto &group : program_offer(groups, host)) {
        offer.groups.push_back(std::move(group));
      }
    }

    // The active plugins' commands: some with the menu's own, the rest in a group for each plugin. Actions decide
    // here, outside the plugin registry's lock, whether and how their commands show.
    run_context_t action_context;
    action_context.client_uuid = client_uuid;
    for (auto &plugin : plugins::offered_client_commands(excluded_plugins, load_plugin_overrides(file()), plugin_options, client_uuid)) {
      apply_action_status(plugin.top_level, action_context);
      apply_action_status(plugin.grouped, action_context);
      for (auto &command : plugin.top_level) {
        offer.commands.push_back(std::move(command));
      }
      if (!plugin.grouped.empty()) {
        offer.groups.push_back(group_t {"plugin", plugin.name, plugin.icon, std::move(plugin.grouped)});
      }
    }

    offer.app_id = std::move(app_id);
    offer.app_name = std::move(app_name);
    return offer;
  }

  nlohmann::json describe(const offer_t &offer) {
    nlohmann::json commands = nlohmann::json::array();
    for (const auto &command : offer.commands) {
      commands.push_back(describe_command(command));
    }

    nlohmann::json groups = nlohmann::json::array();
    for (const auto &group : offer.groups) {
      nlohmann::json group_commands = nlohmann::json::array();
      for (const auto &command : group.commands) {
        group_commands.push_back(describe_command(command));
      }
      groups.push_back({
        {"kind", group.kind},
        {"label", group.label},
        {"icon", group.icon},
        {"commands", std::move(group_commands)},
      });
    }

    return {
      {"version", 1},
      {"commands", std::move(commands)},
      {"groups", std::move(groups)},
    };
  }

  std::optional<command_t> find(const offer_t &offer, std::string_view id) {
    for (const auto &command : offer.commands) {
      if (command.id == id) {
        return command;
      }
    }
    for (const auto &group : offer.groups) {
      for (const auto &command : group.commands) {
        if (command.id == id) {
          return command;
        }
      }
    }
    return std::nullopt;
  }

  std::vector<key_event_t> key_events(const std::vector<std::vector<std::uint16_t>> &steps) {
    std::vector<key_event_t> events;

    for (const auto &step : steps) {
      std::vector<std::uint16_t> keys;
      for (auto key : step) {
        // The generic modifiers are sent as the left ones, as from a keyboard
        switch (key) {
          case 0x10:
            key = 0xA0;
            break;
          case 0x11:
            key = 0xA2;
            break;
          case 0x12:
            key = 0xA4;
            break;
          default:
            break;
        }
        keys.push_back(key);
      }

      for (auto key : keys) {
        events.push_back({key, false, KEY_DELAY});
      }
      for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        events.push_back({*it, true, KEY_DELAY});
      }
      if (!events.empty()) {
        events.back().delay_after = CHORD_DELAY;
      }
    }

    return events;
  }

  void register_action(const std::string &name, action_handler_t handler, action_status_t status) {
    auto &registry = action_registry();
    std::scoped_lock lock {registry.mutex};
    registry.actions[name] = {std::move(handler), std::move(status)};
  }

  void unregister_action(const std::string &name) {
    auto &registry = action_registry();
    std::scoped_lock lock {registry.mutex};
    registry.actions.erase(name);
  }

  bool has_action(const std::string &name) {
    return find_action(name).has_value();
  }

  bool is_valid_action_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() < 'a' || name.front() > 'z') {
      return false;
    }
    return std::ranges::all_of(name, [](const char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
  }

  void apply_action_status(std::vector<command_t> &commands, const run_context_t &context) {
    std::erase_if(commands, [&context](command_t &command) {
      if (command.type != type_e::action) {
        return false;
      }
      const auto action = find_action(command.action);
      if (!action) {
        return true;  // nothing handles it here, for example while its driver is not in use
      }
      if (!action->status) {
        return false;
      }
      const auto display = action->status(context, command.args);
      if (display.hidden) {
        return true;
      }
      if (!display.label.empty()) {
        command.label = display.label;
      }
      if (!display.icon.empty()) {
        command.icon = display.icon;
      }
      return false;
    });
  }

  /**
   * @brief Run a `keys` or `action` command (see `run()`).
   *
   * @param command Command to run.
   * @param error Receives the reason when the command does not start or its action fails.
   * @param context Who the command runs for.
   * @return Outcome of the request.
   */
  static run_result_e run_other(const command_t &command, std::string &error, const run_context_t &context);

  run_result_e run(const command_t &command, std::string &error, const run_context_t &context, const std::function<void()> &on_finished) {
    if (command.type == type_e::script) {
      return start_script(command, context, error, on_finished) ? run_result_e::ok : run_result_e::failed;
    }
    const auto result = run_other(command, error, context);
    if (result == run_result_e::ok && on_finished) {
      on_finished();
    }
    return result;
  }

  static run_result_e run_other(const command_t &command, std::string &error, const run_context_t &context) {
    if (command.type == type_e::action) {
      const auto action = find_action(command.action);
      if (!action) {
        error = "This command isn't available now";
        return run_result_e::failed;
      }
      BOOST_LOG(info) << "Running client command ["sv << command.label << "] for ["sv << context.client_name << ']';
      const auto result = action->handler(context, command.args);
      if (!result.ok) {
        error = result.message.empty() ? "The command failed" : result.message;
        BOOST_LOG(warning) << "Client command ["sv << command.label << "] failed: "sv << error;
        return run_result_e::failed;
      }
      return run_result_e::ok;
    }

    auto &state = typist();
    std::unique_lock lock {state.mutex};
    if (state.typing) {
      error = "Another command is still typing its keys";
      return run_result_e::busy;
    }

    // The thread of the last sequence has ended; joining it frees it for the next one
    auto finished = std::move(state.thread);
    state.typing = true;
    lock.unlock();
    finished = {};

    BOOST_LOG(info) << "Running client command ["sv << command.label << ']';
    lock.lock();
    state.thread = std::jthread(type_keys, key_events(command.steps));
    return run_result_e::ok;
  }

  std::unique_ptr<platf::deinit_t> init() {
    /**
     * @brief Stops a key sequence being typed when Sunshine exits.
     */
    class deinit_t: public platf::deinit_t {
    public:
      ~deinit_t() override {
        stop();
      }
    };

    return std::make_unique<deinit_t>();
  }

  void stop() {
    auto &state = typist();
    std::jthread thread;
    {
      std::scoped_lock lock {state.mutex};
      thread = std::move(state.thread);
    }
    if (thread.joinable()) {
      thread.request_stop();
      thread.join();
    }
  }

  void set_key_sink_for_testing(key_sink_t sink) {
    auto &state = typist();
    std::scoped_lock lock {state.mutex};
    state.sink = std::move(sink);
  }

  void wait_for_keys() {
    auto &state = typist();
    std::jthread thread;
    {
      std::scoped_lock lock {state.mutex};
      thread = std::move(state.thread);
    }
    if (thread.joinable()) {
      thread.join();
    }
  }
}  // namespace client_commands
