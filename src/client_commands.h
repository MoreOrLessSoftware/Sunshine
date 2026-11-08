/**
 * @file src/client_commands.h
 * @brief Declarations for commands the host offers in a client's in-stream overlay menu.
 *
 * A client command is defined on the host and run on the host when a streaming client picks it
 * from its overlay menu. It either types a sequence of key chords or starts a command line.
 *
 * Commands come in groups. Global commands, from `client_commands.json` in the config directory,
 * always appear. An application's own commands, from its `client-commands` entry in `apps.json`,
 * appear only while that application is the one being streamed.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "platform/common.h"

namespace client_commands {
  /**
   * @brief What a command does when it runs.
   */
  enum class type_e {
    keys,  ///< Types a sequence of key chords.
    script  ///< Starts a command line.
  };

  /**
   * @brief One command a client can run from its overlay menu.
   */
  struct command_t {
    std::string id;  ///< Identifier the client sends to run the command, unique among the commands offered.
    std::string label;  ///< Text shown on the client's button.
    std::string icon;  ///< Icon for the client's button, such as `material:<name>`, or empty for none.
    bool close_menu = true;  ///< Whether the client closes its menu after running the command.
    type_e type = type_e::keys;  ///< What the command does.
    std::vector<std::vector<std::uint16_t>> steps;  ///< Key chords of a `keys` command, as Windows virtual-key codes, typed in order.
    std::string cmd;  ///< Command line of a `script` command.
    std::string working_dir;  ///< Directory a `script` command starts in, or empty to derive it from the command.
    bool elevated = false;  ///< Whether a `script` command runs elevated.
  };

  /**
   * @brief Commands shown together on one page of the client's menu.
   */
  struct group_t {
    std::string kind;  ///< Where the group comes from: `app`.
    std::string label;  ///< Text shown on the client's button for the group.
    std::string icon;  ///< Icon for the client's button, or empty for the client's default.
    std::vector<command_t> commands;  ///< Commands in the group, in menu order.
  };

  /**
   * @brief Every command offered to a client at one moment.
   */
  struct offer_t {
    std::vector<command_t> commands;  ///< Global commands, shown at the top level of the menu.
    std::vector<group_t> groups;  ///< Groups shown as pages of the menu.
  };

  /**
   * @brief Outcome of a request to run a command.
   */
  enum class run_result_e {
    ok,  ///< The command started.
    not_found,  ///< No command offered has the requested ID.
    busy,  ///< Another key sequence is still being typed.
    failed  ///< The command could not start.
  };

  /**
   * @brief Parse one command definition.
   *
   * @param json Command object.
   * @param fallback_id ID used when the object has none.
   * @param error Receives the reason when the command is rejected.
   * @return The command, or `std::nullopt` when it is invalid.
   */
  std::optional<command_t> parse_command(const nlohmann::json &json, const std::string &fallback_id, std::string &error);

  /**
   * @brief Parse a list of command definitions, skipping and logging invalid ones.
   *
   * @param json Array of command objects; anything else yields no commands.
   * @param scope Prefix of the IDs given to commands that have none, followed by `/` and their index.
   * @return Valid commands in their original order.
   */
  std::vector<command_t> parse_commands(const nlohmann::json &json, std::string_view scope);

  /**
   * @brief Read the global commands.
   *
   * @param file `client_commands.json` to read.
   * @return Commands from its `commands` array; empty when the file is missing or unreadable.
   */
  std::vector<command_t> load_global(const std::filesystem::path &file);

  /**
   * @brief Return the default file of the global commands.
   *
   * @return `client_commands.json` in the config directory.
   */
  std::filesystem::path default_file();

  /**
   * @brief Return the file the global commands are read from and saved to.
   *
   * @return The file set by `set_file_for_testing()`, otherwise `default_file()`.
   */
  std::filesystem::path file();

  /**
   * @brief Use another file for the global commands.
   *
   * @param path File to use, or an empty path for `default_file()`.
   */
  void set_file_for_testing(const std::filesystem::path &path);

  /**
   * @brief Turn a command back into its definition, as it is saved.
   *
   * @param command Command to describe.
   * @return Command object with only the members its type uses.
   */
  nlohmann::json to_json(const command_t &command);

  /**
   * @brief Turn a list of commands into their definitions.
   *
   * @param commands Commands to describe.
   * @return Array of command objects.
   */
  nlohmann::json to_json(const std::vector<command_t> &commands);

  /**
   * @brief Parse a list of command definitions, rejecting the list when any of them is invalid.
   *
   * Used for lists being saved, where nothing invalid should be dropped silently.
   *
   * @param json Array of command objects.
   * @param error Receives the reason when the list is rejected, naming the command.
   * @return The commands, or `std::nullopt` when the list is rejected.
   */
  std::optional<std::vector<command_t>> parse_commands_strict(const nlohmann::json &json, std::string &error);

  /**
   * @brief Save the global commands, keeping anything else the file holds.
   *
   * @param path File to write.
   * @param commands Commands to save.
   * @param error Receives the reason when the file cannot be written.
   * @return `true` on success.
   */
  bool save_global(const std::filesystem::path &path, const std::vector<command_t> &commands, std::string &error);

  /**
   * @brief Build what is offered with the given commands available.
   *
   * @param global Global commands.
   * @param app_name Name of the application being streamed, or empty for none.
   * @param app_commands Commands of the application being streamed.
   * @return The global commands, then a group for the application when it has commands.
   */
  offer_t make_offer(std::vector<command_t> global, const std::string &app_name, std::vector<command_t> app_commands);

  /**
   * @brief Build what is offered to a client now.
   *
   * @return The global commands and those of the application being streamed.
   */
  offer_t current_offer();

  /**
   * @brief Describe an offer for the client.
   *
   * Leaves out how each command runs, which only the host needs.
   *
   * @param offer Commands offered.
   * @return Object with `version`, `commands` and `groups`.
   */
  nlohmann::json describe(const offer_t &offer);

  /**
   * @brief Find an offered command by ID.
   *
   * @param offer Commands offered.
   * @param id Command ID.
   * @return The command, or `std::nullopt` when none has the ID.
   */
  std::optional<command_t> find(const offer_t &offer, std::string_view id);

  /**
   * @brief One key event of a typed key sequence.
   */
  struct key_event_t {
    std::uint16_t key_code;  ///< Windows virtual-key code.
    bool release;  ///< Whether the event releases the key.
    std::chrono::milliseconds delay_after;  ///< Time to wait before the next event.
  };

  /**
   * @brief Turn key chords into the key events that type them.
   *
   * Each chord presses its keys in order and releases them in reverse, with the generic Shift, Ctrl
   * and Alt sent as the left-hand keys, matching the client's own custom commands.
   *
   * @param steps Key chords.
   * @return Key events in the order they are sent.
   */
  std::vector<key_event_t> key_events(const std::vector<std::vector<std::uint16_t>> &steps);

  /**
   * @brief Run a command.
   *
   * A `keys` command is typed in the background; only one sequence is typed at a time. A `script` command is
   * started and left to run.
   *
   * @param command Command to run.
   * @param error Receives the reason when the command does not start.
   * @return Outcome of the request.
   */
  run_result_e run(const command_t &command, std::string &error);

  /**
   * @brief Prepare client commands for use.
   *
   * @return Object that stops a key sequence being typed when destroyed.
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Stop a key sequence being typed, releasing its held keys, and wait for it to end.
   */
  void stop();

  /**
   * @brief Receives each key event instead of the host's keyboard, for tests.
   */
  using key_sink_t = std::function<void(std::uint16_t key_code, bool release)>;

  /**
   * @brief Send typed keys to a function instead of the host's keyboard, and skip the waits between them.
   *
   * @param sink Function receiving the key events, or an empty function to restore the host's keyboard.
   */
  void set_key_sink_for_testing(key_sink_t sink);

  /**
   * @brief Wait until a key sequence being typed has ended.
   */
  void wait_for_keys();
}  // namespace client_commands
