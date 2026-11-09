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
#include <map>
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
    std::map<std::string, std::string> env;  ///< Variables added to a `script` command's environment, such as a plugin's.
    std::filesystem::path log_file;  ///< File a `script` command's output is added to, or empty to discard it.
  };

  /**
   * @brief Commands shown together on one page of the client's menu.
   */
  struct group_t {
    std::string kind;  ///< Where the group comes from: `app`, `program` or `plugin`.
    std::string label;  ///< Text shown on the client's button for the group.
    std::string icon;  ///< Icon for the client's button, or empty for the client's default.
    std::vector<command_t> commands;  ///< Commands in the group, in menu order.
  };

  /**
   * @brief Commands offered while a program is in the foreground on the host.
   */
  struct program_group_t {
    std::string id;  ///< Identifier of the group.
    std::string label;  ///< Text shown on the client's button for the group.
    std::string icon;  ///< Icon for the client's button, or empty for the client's default.
    std::vector<std::string> programs;  ///< File names of the programs, such as `photoshop.exe`, matched ignoring case.
    std::vector<command_t> commands;  ///< Commands in the group, in menu order.
  };

  /**
   * @brief The user's label and icon for a command that comes with a plugin.
   */
  struct plugin_override_t {
    std::string label;  ///< Label to show instead of the plugin's, or empty to keep it.
    std::string icon;  ///< Icon to show instead of the plugin's, such as `material:<name>`, or empty to keep it.
  };

  /**
   * @brief The user's overrides of plugin commands, by `<plugin id>/<command id>`.
   */
  using plugin_overrides_t = std::map<std::string, plugin_override_t>;

  /**
   * @brief Every command offered to a client at one moment.
   */
  struct offer_t {
    std::vector<command_t> commands;  ///< Global commands, shown at the top level of the menu.
    std::vector<group_t> groups;  ///< Groups shown as pages of the menu.
    std::string app_id;  ///< ID of the application being streamed, or empty for none.
    std::string app_name;  ///< Name of the application being streamed, or empty for none.
  };

  /**
   * @brief Who a command runs for, passed to a `script` command in its environment.
   */
  struct run_context_t {
    std::string client_name;  ///< Name of the client that picked the command, as `SUNSHINE_CLIENT_NAME`.
    std::string app_id;  ///< ID of the application being streamed, as `SUNSHINE_APP_ID`.
    std::string app_name;  ///< Name of the application being streamed, as `SUNSHINE_APP_NAME`.
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
   * @brief Parse one program group.
   *
   * @param json Group object.
   * @param fallback_id ID used when the object has none and `strict` is `false`.
   * @param strict Whether an ID is required and any invalid command rejects the group, as for saving; otherwise
   *               invalid commands are skipped and logged.
   * @param error Receives the reason when the group is rejected.
   * @return The group, or `std::nullopt` when it is invalid.
   */
  std::optional<program_group_t> parse_program_group(const nlohmann::json &json, const std::string &fallback_id, bool strict, std::string &error);

  /**
   * @brief Parse a list of program groups, skipping and logging invalid ones.
   *
   * @param json Array of group objects; anything else yields no groups.
   * @return Valid groups in their original order.
   */
  std::vector<program_group_t> parse_program_groups(const nlohmann::json &json);

  /**
   * @brief Parse a list of program groups, rejecting the list when any of them is invalid.
   *
   * @param json Array of group objects.
   * @param error Receives the reason when the list is rejected, naming the group.
   * @return The groups, or `std::nullopt` when the list is rejected.
   */
  std::optional<std::vector<program_group_t>> parse_program_groups_strict(const nlohmann::json &json, std::string &error);

  /**
   * @brief Read the program groups.
   *
   * @param file `client_commands.json` to read.
   * @return Groups from its `program-groups` array; empty when the file is missing or unreadable.
   */
  std::vector<program_group_t> load_program_groups(const std::filesystem::path &file);

  /**
   * @brief Check whether a program group is for a program.
   *
   * @param group Group to check.
   * @param program File name of the program, such as `notepad.exe`.
   * @return `true` when one of the group's programs has the same file name, ignoring case.
   */
  bool matches(const program_group_t &group, std::string_view program);

  /**
   * @brief Build the groups offered while a program is in the foreground.
   *
   * @param groups Every program group.
   * @param program File name of the program in the foreground, or empty for none.
   * @return A `program` group for each group of the program that has commands.
   */
  std::vector<group_t> program_offer(const std::vector<program_group_t> &groups, std::string_view program);

  /**
   * @brief Return the program in the foreground on the host.
   *
   * @return The program set by `set_foreground_program_for_testing()`, otherwise `platf::foreground_program()`.
   */
  std::string foreground_program();

  /**
   * @brief Pretend a program is in the foreground.
   *
   * @param program File name of the program, or `std::nullopt` to ask the platform again.
   */
  void set_foreground_program_for_testing(std::optional<std::string> program);

  /**
   * @brief Parse the overrides of plugin commands, skipping and logging invalid ones.
   *
   * @param json Object of overrides by `<plugin id>/<command id>`; anything else yields none.
   * @return Valid overrides that change something.
   */
  plugin_overrides_t parse_plugin_overrides(const nlohmann::json &json);

  /**
   * @brief Parse the overrides of plugin commands, rejecting them when any is invalid.
   *
   * @param json Object of overrides by `<plugin id>/<command id>`.
   * @param error Receives the reason when they are rejected, naming the command.
   * @return The overrides that change something, or `std::nullopt` when rejected.
   */
  std::optional<plugin_overrides_t> parse_plugin_overrides_strict(const nlohmann::json &json, std::string &error);

  /**
   * @brief Read the overrides of plugin commands.
   *
   * @param file `client_commands.json` to read.
   * @return Overrides from its `plugin-commands` object; empty when the file is missing or unreadable.
   */
  plugin_overrides_t load_plugin_overrides(const std::filesystem::path &file);

  /**
   * @brief Turn the overrides of plugin commands back into their definition, as it is saved.
   *
   * @param overrides Overrides to describe.
   * @return Object of overrides by `<plugin id>/<command id>`, with only the members that are set.
   */
  nlohmann::json to_json(const plugin_overrides_t &overrides);

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
   * @brief Turn a program group back into its definition, as it is saved.
   *
   * @param group Group to describe.
   * @return Group object.
   */
  nlohmann::json to_json(const program_group_t &group);

  /**
   * @brief Turn a list of program groups into their definitions.
   *
   * @param groups Groups to describe.
   * @return Array of group objects.
   */
  nlohmann::json to_json(const std::vector<program_group_t> &groups);

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
   * @brief Save the global commands, the program groups and the overrides of plugin commands, keeping anything else the file holds.
   *
   * @param path File to write.
   * @param commands Commands to save, or `std::nullopt` to keep those in the file.
   * @param groups Program groups to save, or `std::nullopt` to keep those in the file.
   * @param error Receives the reason when the file cannot be written.
   * @param overrides Overrides of plugin commands to save, or `std::nullopt` to keep those in the file.
   * @return `true` on success.
   */
  bool save(const std::filesystem::path &path, const std::optional<std::vector<command_t>> &commands, const std::optional<std::vector<program_group_t>> &groups, std::string &error, const std::optional<plugin_overrides_t> &overrides = std::nullopt);

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
   * @return The global commands followed by the active plugins' top-level commands, then the groups of the
   *         application being streamed, of the program in the foreground, and of the active plugins.
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
   * @param context Who the command runs for; a `script` command gets each non-empty value in its environment.
   * @return Outcome of the request.
   */
  run_result_e run(const command_t &command, std::string &error, const run_context_t &context = {});

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
