/**
 * @file src/client_commands.cpp
 * @brief Definitions for commands the host offers in a client's in-stream overlay menu.
 */
// standard includes
#include <condition_variable>
#include <format>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <thread>

// lib includes
#include <boost/filesystem.hpp>
#include <boost/process/v1.hpp>

// local includes
#include "client_commands.h"
#include "input.h"
#include "logging.h"
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
     * @param error Receives the reason when it does not start.
     * @return `true` when it started.
     */
    bool start_script(const command_t &command, std::string &error) {
      // A copy, so the variables are only for the command
      boost::process::v1::environment env = boost::this_process::environment();
      env["SUNSHINE_CLIENT_COMMAND_ID"] = command.id;
      env["SUNSHINE_CLIENT_COMMAND_LABEL"] = command.label;

      boost::filesystem::path working_dir = command.working_dir.empty() ?
                                              proc::find_working_directory(command.cmd, env) :
                                              boost::filesystem::path(command.working_dir);

      BOOST_LOG(info) << "Running client command ["sv << command.label << "]: ["sv << command.cmd << "] in ["sv << working_dir << ']';

      std::error_code ec;
      auto child = platf::run_command(command.elevated, true, command.cmd, working_dir, env, nullptr, ec, nullptr);
      if (ec) {
        error = std::format("Couldn't start [{}]: {}", command.cmd, ec.message());
        BOOST_LOG(warning) << error;
        return false;
      }

      child.detach();
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

  std::filesystem::path default_file() {
    return platf::appdata() / "client_commands.json";
  }

  offer_t make_offer(std::vector<command_t> global, const std::string &app_name, std::vector<command_t> app_commands) {
    offer_t offer;
    offer.commands = std::move(global);

    if (!app_name.empty() && !app_commands.empty()) {
      offer.groups.push_back(group_t {"app", app_name, {}, std::move(app_commands)});
    }

    return offer;
  }

  offer_t current_offer() {
    std::string app_name;
    std::vector<command_t> app_commands;

    if (const int app_id = proc::proc.running(); app_id > 0) {
      for (const auto &app : proc::proc.get_apps()) {
        if (app.id == std::to_string(app_id)) {
          app_name = app.name;
          app_commands = app.client_commands;
          break;
        }
      }
    }

    return make_offer(load_global(default_file()), app_name, std::move(app_commands));
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

  run_result_e run(const command_t &command, std::string &error) {
    if (command.type == type_e::script) {
      return start_script(command, error) ? run_result_e::ok : run_result_e::failed;
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
