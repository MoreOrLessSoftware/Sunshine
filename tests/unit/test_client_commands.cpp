/**
 * @file tests/unit/test_client_commands.cpp
 * @brief Test src/client_commands.* and the client commands of applications in src/process.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include <src/client_commands.h>
#include <src/process.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::literals;

namespace {
  /**
   * @brief A key event received by the test sink.
   */
  struct sent_key_t {
    std::uint16_t key_code;  ///< Virtual-key code.
    bool release;  ///< Whether the key was released.

    /**
     * @brief Compare two key events.
     *
     * @param other Event to compare with.
     * @return `true` when both are equal.
     */
    bool operator==(const sent_key_t &other) const = default;
  };

  /**
   * @brief Write text to a file, creating its directory.
   *
   * @param path File to write.
   * @param text Contents.
   */
  void write_file(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out {path, std::ios::binary};
    out << text;
  }

  /**
   * @brief Fixture with a temporary directory and keys sent to a recorder.
   */
  class ClientCommandsTest: public ::testing::Test {
  protected:
    /**
     * @brief Create the directory and start recording keys.
     */
    void SetUp() override {
      dir_ = fs::temp_directory_path() / std::format("sunshine-client-commands-{}", std::chrono::steady_clock::now().time_since_epoch().count());
      fs::create_directories(dir_);
      client_commands::set_key_sink_for_testing([this](std::uint16_t key_code, bool release) {
        std::scoped_lock lock {mutex_};
        keys_.push_back({key_code, release});
      });
    }

    /**
     * @brief Stop recording keys and remove the directory.
     */
    void TearDown() override {
      client_commands::stop();
      client_commands::set_key_sink_for_testing({});
      std::error_code ec;
      fs::remove_all(dir_, ec);
    }

    /**
     * @brief Return the keys recorded so far.
     *
     * @return Recorded key events.
     */
    std::vector<sent_key_t> keys() {
      std::scoped_lock lock {mutex_};
      return keys_;
    }

    fs::path dir_;  ///< Temporary directory.
    std::mutex mutex_;  ///< Guards `keys_`.
    std::vector<sent_key_t> keys_;  ///< Keys sent to the sink.
  };

  /**
   * @brief Parse a command that is expected to be valid.
   *
   * @param text Command JSON.
   * @return The command.
   */
  client_commands::command_t parse_valid(const std::string &text) {
    std::string error;
    auto command = client_commands::parse_command(json::parse(text), "fallback", error);
    EXPECT_TRUE(command) << error;
    return command.value_or(client_commands::command_t {});
  }

  /**
   * @brief Parse a command that is expected to be rejected.
   *
   * @param value Command JSON.
   * @return The reason it was rejected.
   */
  std::string parse_error(const json &value) {
    std::string error;
    auto command = client_commands::parse_command(value, "fallback", error);
    EXPECT_FALSE(command);
    return error;
  }
}  // namespace

TEST(ClientCommandsParse, KeysCommand) {
  auto command = parse_valid(R"({"id": "win-r", "label": "Run", "icon": "material:terminal", "close-menu": false, "type": "keys", "steps": [[91, 82], [27]]})");
  EXPECT_EQ(command.id, "win-r");
  EXPECT_EQ(command.label, "Run");
  EXPECT_EQ(command.icon, "material:terminal");
  EXPECT_FALSE(command.close_menu);
  EXPECT_EQ(command.type, client_commands::type_e::keys);
  EXPECT_EQ(command.steps, (std::vector<std::vector<std::uint16_t>> {{91, 82}, {27}}));
}

TEST(ClientCommandsParse, ScriptCommandWithDefaults) {
  auto command = parse_valid(R"({"label": "Notepad", "type": "script", "cmd": "notepad.exe"})");
  EXPECT_EQ(command.id, "fallback");
  EXPECT_TRUE(command.icon.empty());
  EXPECT_TRUE(command.close_menu);
  EXPECT_EQ(command.type, client_commands::type_e::script);
  EXPECT_EQ(command.cmd, "notepad.exe");
  EXPECT_TRUE(command.working_dir.empty());
  EXPECT_FALSE(command.elevated);
}

TEST(ClientCommandsParse, ScriptCommandWithEverything) {
  auto command = parse_valid(R"({"label": "Tool", "type": "script", "cmd": "tool.exe", "working-dir": "C:\\Tools", "elevated": true})");
  EXPECT_EQ(command.working_dir, "C:\\Tools");
  EXPECT_TRUE(command.elevated);
}

TEST(ClientCommandsParse, RejectsInvalidCommands) {
  EXPECT_FALSE(parse_error(json::array()).empty());
  EXPECT_NE(parse_error(json::parse(R"({"type": "keys", "steps": [[27]]})")).find("label"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x"})")).find("type"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "launch"})")).find("type"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "script"})")).find("cmd"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys"})")).find("steps"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys", "steps": []})")).find("steps"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys", "steps": [[]]})")).find("chord"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys", "steps": [27]})")).find("chord"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys", "steps": [[0]]})")).find("key codes"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys", "steps": [[255]]})")).find("key codes"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys", "steps": [["A"]]})")).find("key codes"), std::string::npos);
  EXPECT_NE(parse_error(json::parse(R"({"label": "x", "type": "keys", "steps": [[1, 2, 3, 4, 5, 6, 7, 8, 9]]})")).find("chord"), std::string::npos);

  json too_many = {{"label", "x"}, {"type", "keys"}, {"steps", json::array()}};
  for (int i = 0; i < 65; i++) {
    too_many["steps"].push_back({27});
  }
  EXPECT_NE(parse_error(too_many).find("at most"), std::string::npos);
}

TEST(ClientCommandsParse, ListSkipsInvalidCommandsAndNumbersTheRest) {
  auto commands = client_commands::parse_commands(json::parse(R"([
    {"label": "A", "type": "keys", "steps": [[65]]},
    {"label": "Broken"},
    {"id": "named", "label": "C", "type": "keys", "steps": [[67]]}
  ])"),
                                                  "global");
  ASSERT_EQ(commands.size(), 2);
  EXPECT_EQ(commands[0].id, "global/0");
  EXPECT_EQ(commands[1].id, "named");
}

TEST(ClientCommandsParse, ListOfSomethingElseIsEmpty) {
  EXPECT_TRUE(client_commands::parse_commands(json {}, "global").empty());
  EXPECT_TRUE(client_commands::parse_commands(json::object(), "global").empty());
}

TEST_F(ClientCommandsTest, LoadsGlobalCommands) {
  auto file = dir_ / "client_commands.json";
  write_file(file, R"({"commands": [{"id": "esc", "label": "Escape", "type": "keys", "steps": [[27]]}]})");

  auto commands = client_commands::load_global(file);
  ASSERT_EQ(commands.size(), 1);
  EXPECT_EQ(commands[0].id, "esc");
}

TEST_F(ClientCommandsTest, MissingOrBrokenFileHasNoCommands) {
  EXPECT_TRUE(client_commands::load_global(dir_ / "missing.json").empty());

  auto broken = dir_ / "broken.json";
  write_file(broken, "{ not json");
  EXPECT_TRUE(client_commands::load_global(broken).empty());

  auto no_commands = dir_ / "empty.json";
  write_file(no_commands, "[]");
  EXPECT_TRUE(client_commands::load_global(no_commands).empty());
}

TEST(ClientCommandsFile, IsInTheConfigDirectory) {
  EXPECT_EQ(client_commands::default_file().filename(), "client_commands.json");
}

TEST(ClientCommandsOffer, AppCommandsFormAGroup) {
  client_commands::command_t global {.id = "g", .label = "Global"};
  client_commands::command_t app {.id = "a", .label = "App", .icon = "material:star", .close_menu = false};

  auto offer = client_commands::make_offer({global}, "Desktop", {app});
  ASSERT_EQ(offer.commands.size(), 1);
  ASSERT_EQ(offer.groups.size(), 1);
  EXPECT_EQ(offer.groups[0].kind, "app");
  EXPECT_EQ(offer.groups[0].label, "Desktop");

  auto described = client_commands::describe(offer);
  EXPECT_EQ(described["version"], 1);
  EXPECT_EQ(described["commands"][0], (json {{"id", "g"}, {"label", "Global"}, {"icon", ""}, {"close-menu", true}}));
  EXPECT_EQ(described["groups"][0]["kind"], "app");
  EXPECT_EQ(described["groups"][0]["label"], "Desktop");
  EXPECT_EQ(described["groups"][0]["commands"][0], (json {{"id", "a"}, {"label", "App"}, {"icon", "material:star"}, {"close-menu", false}}));

  // How a command runs stays on the host
  EXPECT_FALSE(described["commands"][0].contains("type"));

  EXPECT_EQ(client_commands::find(offer, "g")->label, "Global");
  EXPECT_EQ(client_commands::find(offer, "a")->label, "App");
  EXPECT_FALSE(client_commands::find(offer, "missing"));
}

TEST(ClientCommandsOffer, NoGroupWithoutAnAppOrItsCommands) {
  client_commands::command_t app {.id = "a", .label = "App"};
  EXPECT_TRUE(client_commands::make_offer({}, "", {app}).groups.empty());
  EXPECT_TRUE(client_commands::make_offer({}, "Desktop", {}).groups.empty());
  EXPECT_TRUE(client_commands::describe(client_commands::make_offer({}, "", {}))["groups"].empty());
}

TEST(ClientCommandsOffer, NothingRunningOffersOnlyGlobalCommands) {
  EXPECT_TRUE(client_commands::current_offer().groups.empty());
}

TEST(ClientCommandsKeys, ChordsPressInOrderAndReleaseInReverse) {
  auto events = client_commands::key_events({{0x11, 0x10, 0x1B}, {0x41}});
  ASSERT_EQ(events.size(), 8);

  // Generic modifiers become the left ones
  EXPECT_EQ(events[0].key_code, 0xA2);
  EXPECT_EQ(events[1].key_code, 0xA0);
  EXPECT_EQ(events[2].key_code, 0x1B);
  EXPECT_FALSE(events[2].release);
  EXPECT_EQ(events[3].key_code, 0x1B);
  EXPECT_TRUE(events[3].release);
  EXPECT_EQ(events[5].key_code, 0xA2);
  EXPECT_TRUE(events[5].release);

  EXPECT_EQ(events[0].delay_after, 50ms);
  EXPECT_EQ(events[5].delay_after, 100ms);
  EXPECT_EQ(events[7].delay_after, 100ms);

  EXPECT_EQ(client_commands::key_events({{0x12}})[0].key_code, 0xA4);
  EXPECT_TRUE(client_commands::key_events({}).empty());
}

TEST_F(ClientCommandsTest, RunTypesTheKeys) {
  client_commands::command_t command {.id = "x", .label = "Win+R", .type = client_commands::type_e::keys, .steps = {{0x5B, 0x52}}};

  std::string error;
  EXPECT_EQ(client_commands::run(command, error), client_commands::run_result_e::ok);
  client_commands::wait_for_keys();

  EXPECT_EQ(keys(), (std::vector<sent_key_t> {{0x5B, false}, {0x52, false}, {0x52, true}, {0x5B, true}}));

  // The next one may run once it has finished
  EXPECT_EQ(client_commands::run(command, error), client_commands::run_result_e::ok);
  client_commands::wait_for_keys();
  EXPECT_EQ(keys().size(), 8);
}

TEST_F(ClientCommandsTest, OneKeySequenceAtATimeAndStopReleasesHeldKeys) {
  std::mutex gate_mutex;
  std::condition_variable gate;
  bool first_key = false;
  bool open = false;

  client_commands::set_key_sink_for_testing([&](std::uint16_t key_code, bool release) {
    {
      std::scoped_lock lock {mutex_};
      keys_.push_back({key_code, release});
    }
    std::unique_lock lock {gate_mutex};
    first_key = true;
    gate.notify_all();
    gate.wait(lock, [&] {
      return open;
    });
  });

  client_commands::command_t command {.id = "x", .label = "Ctrl+Esc", .type = client_commands::type_e::keys, .steps = {{0x11, 0x1B}}};
  std::string error;
  ASSERT_EQ(client_commands::run(command, error), client_commands::run_result_e::ok);
  {
    std::unique_lock lock {gate_mutex};
    gate.wait(lock, [&] {
      return first_key;
    });
  }

  EXPECT_EQ(client_commands::run(command, error), client_commands::run_result_e::busy);
  EXPECT_FALSE(error.empty());

  // Stopped while Ctrl is down: Ctrl is released and Esc is never pressed
  std::thread stopper {[] {
    client_commands::stop();
  }};
  std::this_thread::sleep_for(50ms);
  {
    std::scoped_lock lock {gate_mutex};
    open = true;
  }
  gate.notify_all();
  stopper.join();

  EXPECT_EQ(keys(), (std::vector<sent_key_t> {{0xA2, false}, {0xA2, true}}));
}

#ifdef _WIN32
TEST_F(ClientCommandsTest, ScriptsGetTheClientAndApp) {
  auto out = dir_ / "env.txt";
  client_commands::command_t command {
    .id = "env",
    .label = "Env",
    .type = client_commands::type_e::script,
    .cmd = std::format("cmd.exe /c echo %SUNSHINE_CLIENT_NAME%^|%SUNSHINE_APP_ID%^|%SUNSHINE_APP_NAME%^|%SUNSHINE_CLIENT_COMMAND_ID%> \"{}\"", out.string()),
    .working_dir = dir_.string(),
  };
  std::string error;
  ASSERT_EQ(client_commands::run(command, error, {"Living Room", "1234", "Desktop"}), client_commands::run_result_e::ok) << error;

  // It's left running, so wait for what it writes
  std::string text;
  for (int i = 0; i < 100 && text.find('\n') == std::string::npos; i++) {
    std::this_thread::sleep_for(50ms);
    std::ifstream in {out};
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  EXPECT_EQ(text.substr(0, text.find_first_of("\r\n")), "Living Room|1234|Desktop|env");
}

TEST_F(ClientCommandsTest, ScriptOutputGoesToTheLogFile) {
  auto log = dir_ / "logs" / "plugin.log";
  fs::create_directories(log.parent_path());
  client_commands::command_t command {
    .id = "log",
    .label = "Logged",
    .type = client_commands::type_e::script,
    .cmd = "cmd.exe /c echo hello from the command",
    .working_dir = dir_.string(),
    .log_file = log,
  };
  std::string error;
  ASSERT_EQ(client_commands::run(command, error), client_commands::run_result_e::ok) << error;

  std::string text;
  for (int i = 0; i < 100 && text.find("hello from the command") == std::string::npos; i++) {
    std::this_thread::sleep_for(50ms);
    std::ifstream in {log};
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  EXPECT_NE(text.find("Running client command Logged"), std::string::npos) << text;
  EXPECT_NE(text.find("hello from the command"), std::string::npos) << text;
}

TEST_F(ClientCommandsTest, RunStartsAScript) {
  client_commands::command_t command {.id = "x", .label = "Exit", .type = client_commands::type_e::script, .cmd = "cmd.exe /c exit 0", .working_dir = dir_.string()};
  std::string error;
  EXPECT_EQ(client_commands::run(command, error), client_commands::run_result_e::ok) << error;
}
#endif

TEST_F(ClientCommandsTest, RunReportsAScriptThatDoesNotStart) {
  client_commands::command_t command {.id = "x", .label = "Missing", .type = client_commands::type_e::script, .cmd = "sunshine-no-such-program-for-client-commands"};
  std::string error;
  EXPECT_EQ(client_commands::run(command, error), client_commands::run_result_e::failed);
  EXPECT_NE(error.find("sunshine-no-such-program-for-client-commands"), std::string::npos);
}

TEST_F(ClientCommandsTest, InitStopsTypingWhenDestroyed) {
  auto guard = client_commands::init();
  EXPECT_NE(guard, nullptr);
  guard.reset();
}

TEST_F(ClientCommandsTest, AppsCarryTheirOwnCommands) {
  auto file = dir_ / "apps.json";
  write_file(file, R"({
    "env": {},
    "apps": [
      {"name": "Desktop", "client-commands": [{"label": "Esc", "type": "keys", "steps": [[27]]}, {"label": "Broken"}]},
      {"name": "Steam"},
      {"name": "Game", "client-commands": {"not": "an array"}}
    ]
  })");

  auto proc = proc::parse(file.string());
  ASSERT_TRUE(proc);
  const auto &apps = proc->get_apps();
  ASSERT_EQ(apps.size(), 3);
  ASSERT_EQ(apps[0].client_commands.size(), 1);
  EXPECT_EQ(apps[0].client_commands[0].id, "app0/0");
  EXPECT_EQ(apps[0].client_commands[0].steps, (std::vector<std::vector<std::uint16_t>> {{27}}));
  EXPECT_TRUE(apps[1].client_commands.empty());
  EXPECT_TRUE(apps[2].client_commands.empty());
}


TEST(ClientCommandsSave, DefinitionsRoundTrip) {
  auto keys = parse_valid(R"({"id": "k", "label": "Keys", "icon": "material:star", "close-menu": false, "type": "keys", "steps": [[17, 27]]})");
  auto json = client_commands::to_json(keys);
  EXPECT_EQ(json, (json::parse(R"({"id": "k", "label": "Keys", "icon": "material:star", "close-menu": false, "type": "keys", "steps": [[17, 27]]})")));

  auto script = parse_valid(R"({"id": "s", "label": "Script", "type": "script", "cmd": "x.exe", "working-dir": "C:\\", "elevated": true})");
  json = client_commands::to_json(script);
  EXPECT_EQ(json["type"], "script");
  EXPECT_EQ(json["cmd"], "x.exe");
  EXPECT_EQ(json["working-dir"], "C:\\");
  EXPECT_EQ(json["elevated"], true);
  EXPECT_FALSE(json.contains("steps"));

  EXPECT_EQ(client_commands::to_json(std::vector<client_commands::command_t> {keys, script}).size(), 2);
}

TEST(ClientCommandsSave, StrictListsRejectAnythingInvalid) {
  std::string error;
  auto commands = client_commands::parse_commands_strict(json::parse(R"([{"id": "a", "label": "A", "type": "keys", "steps": [[65]]}])"), error);
  ASSERT_TRUE(commands);
  EXPECT_EQ(commands->size(), 1);

  EXPECT_FALSE(client_commands::parse_commands_strict(json::object(), error));
  EXPECT_EQ(error, "the commands must be an array");

  EXPECT_FALSE(client_commands::parse_commands_strict(json::parse(R"([{"label": "A", "type": "keys", "steps": [[65]]}])"), error));
  EXPECT_EQ(error, "\"A\" has no id");

  EXPECT_FALSE(client_commands::parse_commands_strict(json::parse(R"([5])"), error));
  EXPECT_EQ(error, "command 1 has no id");

  EXPECT_FALSE(client_commands::parse_commands_strict(json::parse(R"([{"id": "a", "label": "A", "type": "keys"}])"), error));
  EXPECT_NE(error.find("\"A\": "), std::string::npos);

  EXPECT_FALSE(client_commands::parse_commands_strict(json::parse(R"([{"id": "a", "type": "keys", "steps": [[65]]}])"), error));
  EXPECT_NE(error.find("command 1: "), std::string::npos);

  EXPECT_FALSE(client_commands::parse_commands_strict(json::parse(R"([
    {"id": "a", "label": "A", "type": "keys", "steps": [[65]]},
    {"id": "a", "label": "B", "type": "keys", "steps": [[66]]}
  ])"),
                                                      error));
  EXPECT_EQ(error, "\"B\" has the same id as another command");
}

TEST_F(ClientCommandsTest, SavingKeepsTheRestOfTheFile) {
  auto file = dir_ / "client_commands.json";
  write_file(file, R"({"commands": [], "exe-groups": [{"label": "Kept"}]})");

  std::string error;
  auto command = parse_valid(R"({"id": "esc", "label": "Escape", "type": "keys", "steps": [[27]]})");
  ASSERT_TRUE(client_commands::save_global(file, {command}, error)) << error;

  std::ifstream in {file};
  auto saved = json::parse(in);
  EXPECT_EQ(saved["exe-groups"][0]["label"], "Kept");
  EXPECT_EQ(client_commands::load_global(file).size(), 1);
}

TEST_F(ClientCommandsTest, SavingReplacesAnUnreadableFileOrMakesANewOne) {
  std::string error;
  auto command = parse_valid(R"({"id": "esc", "label": "Escape", "type": "keys", "steps": [[27]]})");

  auto broken = dir_ / "broken.json";
  write_file(broken, "{ nope");
  ASSERT_TRUE(client_commands::save_global(broken, {command}, error)) << error;
  EXPECT_EQ(client_commands::load_global(broken).size(), 1);

  auto not_object = dir_ / "array.json";
  write_file(not_object, "[]");
  ASSERT_TRUE(client_commands::save_global(not_object, {command}, error)) << error;
  EXPECT_EQ(client_commands::load_global(not_object).size(), 1);

  auto fresh = dir_ / "new" / "client_commands.json";
  ASSERT_TRUE(client_commands::save_global(fresh, {}, error)) << error;
  EXPECT_TRUE(fs::exists(fresh));
}

TEST_F(ClientCommandsTest, SavingReportsAFileThatCannotBeWritten) {
  std::string error;
  EXPECT_FALSE(client_commands::save_global(dir_, {}, error));
  EXPECT_FALSE(error.empty());
}

TEST_F(ClientCommandsTest, TheFileCanBeMovedForTests) {
  EXPECT_EQ(client_commands::file(), client_commands::default_file());
  client_commands::set_file_for_testing(dir_ / "other.json");
  EXPECT_EQ(client_commands::file(), dir_ / "other.json");
  client_commands::set_file_for_testing({});
  EXPECT_EQ(client_commands::file(), client_commands::default_file());
}


namespace {
  /**
   * @brief A program group that should parse.
   *
   * @param text Group JSON.
   * @param strict Whether to parse it as for saving.
   * @return The group.
   */
  client_commands::program_group_t parse_group(const std::string &text, bool strict = false) {
    std::string error;
    auto group = client_commands::parse_program_group(json::parse(text), "fallback", strict, error);
    EXPECT_TRUE(group) << error;
    return group.value_or(client_commands::program_group_t {});
  }

  /**
   * @brief Why a program group is rejected.
   *
   * @param text Group JSON.
   * @param strict Whether to parse it as for saving.
   * @return The reason.
   */
  std::string group_error(const std::string &text, bool strict = false) {
    std::string error;
    EXPECT_FALSE(client_commands::parse_program_group(json::parse(text), "fallback", strict, error));
    return error;
  }
}  // namespace

TEST(ClientCommandsPrograms, ParsesAGroup) {
  auto group = parse_group(R"({"id": "ps", "label": "Photoshop", "icon": "material:brush", "programs": ["Photoshop.exe", "C:\\Apps\\ps2.exe"],
                               "commands": [{"label": "Undo", "type": "keys", "steps": [[17, 90]]}, {"label": "Broken"}]})");
  EXPECT_EQ(group.id, "ps");
  EXPECT_EQ(group.label, "Photoshop");
  EXPECT_EQ(group.icon, "material:brush");
  EXPECT_EQ(group.programs, (std::vector<std::string> {"Photoshop.exe", "C:\\Apps\\ps2.exe"}));
  ASSERT_EQ(group.commands.size(), 1);
  EXPECT_EQ(group.commands[0].id, "program:ps/0");

  EXPECT_EQ(parse_group(R"({"label": "X", "programs": ["x.exe"]})").id, "fallback");
  EXPECT_TRUE(parse_group(R"({"label": "X", "programs": ["x.exe"]})").commands.empty());
}

TEST(ClientCommandsPrograms, RejectsInvalidGroups) {
  EXPECT_FALSE(group_error("[]").empty());
  EXPECT_NE(group_error(R"({"programs": ["x.exe"]})").find("label"), std::string::npos);
  EXPECT_NE(group_error(R"({"label": "X"})").find("programs"), std::string::npos);
  EXPECT_NE(group_error(R"({"label": "X", "programs": []})").find("programs"), std::string::npos);
  EXPECT_NE(group_error(R"({"label": "X", "programs": [""]})").find("file name"), std::string::npos);
  EXPECT_NE(group_error(R"({"label": "X", "programs": [5]})").find("file name"), std::string::npos);

  // Saving needs an id and valid commands
  EXPECT_EQ(group_error(R"({"label": "X", "programs": ["x.exe"]})", true), "it has no id");
  EXPECT_NE(group_error(R"({"id": "x", "label": "X", "programs": ["x.exe"], "commands": [{"id": "c", "label": "Broken"}]})", true).find("\"Broken\""), std::string::npos);
  EXPECT_EQ(parse_group(R"({"id": "x", "label": "X", "programs": ["x.exe"], "commands": [{"id": "c", "label": "C", "type": "keys", "steps": [[27]]}]})", true).commands.size(), 1);
}

TEST(ClientCommandsPrograms, ListsSkipOrRejectInvalidGroups) {
  auto groups = client_commands::parse_program_groups(json::parse(R"([{"label": "A", "programs": ["a.exe"]}, {"label": "Broken"}])"));
  ASSERT_EQ(groups.size(), 1);
  EXPECT_EQ(groups[0].id, "program0");
  EXPECT_TRUE(client_commands::parse_program_groups(json::object()).empty());

  std::string error;
  EXPECT_FALSE(client_commands::parse_program_groups_strict(json::object(), error));
  EXPECT_EQ(error, "the program groups must be an array");
  EXPECT_FALSE(client_commands::parse_program_groups_strict(json::parse(R"([{"id": "a", "label": "A"}])"), error));
  EXPECT_EQ(error, "\"A\": \"programs\" must name at least one program");
  EXPECT_FALSE(client_commands::parse_program_groups_strict(json::parse(R"([5])"), error));
  EXPECT_EQ(error.rfind("program group 1: ", 0), 0);
  EXPECT_FALSE(client_commands::parse_program_groups_strict(json::parse(R"([{"id": "a", "label": "A", "programs": ["a.exe"]}, {"id": "a", "label": "B", "programs": ["b.exe"]}])"), error));
  EXPECT_EQ(error, "\"B\" has the same id as another group");
  auto valid = client_commands::parse_program_groups_strict(json::parse(R"([{"id": "a", "label": "A", "programs": ["a.exe"]}])"), error);
  ASSERT_TRUE(valid);
  EXPECT_EQ(valid->size(), 1);
}

TEST(ClientCommandsPrograms, MatchesFileNamesIgnoringCase) {
  auto group = parse_group(R"({"label": "PS", "programs": ["Photoshop.exe", "D:\\Tools\\other.EXE"]})");
  EXPECT_TRUE(client_commands::matches(group, "photoshop.exe"));
  EXPECT_TRUE(client_commands::matches(group, "PHOTOSHOP.EXE"));
  EXPECT_TRUE(client_commands::matches(group, "other.exe"));
  EXPECT_FALSE(client_commands::matches(group, "photoshop"));
  EXPECT_FALSE(client_commands::matches(group, ""));
}

TEST(ClientCommandsPrograms, OffersTheForegroundProgramsGroups) {
  auto with_commands = parse_group(R"({"label": "Notepad", "icon": "material:edit", "programs": ["notepad.exe"], "commands": [{"label": "Esc", "type": "keys", "steps": [[27]]}]})");
  auto without_commands = parse_group(R"({"label": "Empty", "programs": ["notepad.exe"]})");
  auto other = parse_group(R"({"label": "Other", "programs": ["other.exe"], "commands": [{"label": "Esc", "type": "keys", "steps": [[27]]}]})");

  auto offered = client_commands::program_offer({with_commands, without_commands, other}, "Notepad.exe");
  ASSERT_EQ(offered.size(), 1);
  EXPECT_EQ(offered[0].kind, "program");
  EXPECT_EQ(offered[0].label, "Notepad");
  EXPECT_EQ(offered[0].icon, "material:edit");
  EXPECT_EQ(offered[0].commands.size(), 1);

  EXPECT_TRUE(client_commands::program_offer({with_commands}, "").empty());
}

TEST(ClientCommandsPrograms, GroupsRoundTrip) {
  auto group = parse_group(R"({"id": "g", "label": "G", "icon": "", "programs": ["g.exe"], "commands": [{"id": "c", "label": "C", "type": "keys", "steps": [[27]]}]})");
  auto json = client_commands::to_json(group);
  EXPECT_EQ(json["programs"], json::parse(R"(["g.exe"])"));
  EXPECT_EQ(json["commands"][0]["id"], "c");
  EXPECT_EQ(client_commands::to_json(std::vector<client_commands::program_group_t> {group}).size(), 1);

  std::string error;
  auto back = client_commands::parse_program_group(json, {}, true, error);
  ASSERT_TRUE(back) << error;
  EXPECT_EQ(client_commands::to_json(*back), json);
}

TEST_F(ClientCommandsTest, LoadsAndSavesProgramGroups) {
  auto file = dir_ / "client_commands.json";
  EXPECT_TRUE(client_commands::load_program_groups(file).empty());

  write_file(file, "{ broken");
  EXPECT_TRUE(client_commands::load_program_groups(file).empty());

  write_file(file, R"({"commands": [{"id": "g", "label": "G", "type": "keys", "steps": [[27]]}], "program-groups": [{"id": "n", "label": "Notepad", "programs": ["notepad.exe"]}]})");
  ASSERT_EQ(client_commands::load_program_groups(file).size(), 1);

  // Saving only the groups keeps the commands, and the other way round
  std::string error;
  auto group = parse_group(R"({"id": "p", "label": "Paint", "programs": ["mspaint.exe"]})");
  ASSERT_TRUE(client_commands::save(file, std::nullopt, std::vector {group}, error)) << error;
  EXPECT_EQ(client_commands::load_global(file).size(), 1);
  ASSERT_EQ(client_commands::load_program_groups(file).size(), 1);
  EXPECT_EQ(client_commands::load_program_groups(file)[0].label, "Paint");

  ASSERT_TRUE(client_commands::save(file, std::vector<client_commands::command_t> {}, std::nullopt, error)) << error;
  EXPECT_TRUE(client_commands::load_global(file).empty());
  EXPECT_EQ(client_commands::load_program_groups(file).size(), 1);
}

TEST_F(ClientCommandsTest, TheCurrentOfferIncludesTheForegroundProgramsGroup) {
  auto file = dir_ / "client_commands.json";
  write_file(file, R"({"program-groups": [{"id": "n", "label": "Notepad", "programs": ["notepad.exe"],
                                           "commands": [{"id": "esc", "label": "Esc", "type": "keys", "steps": [[27]]}]}]})");
  client_commands::set_file_for_testing(file);

  client_commands::set_foreground_program_for_testing("NOTEPAD.EXE");
  auto offer = client_commands::current_offer();
  ASSERT_EQ(offer.groups.size(), 1);
  EXPECT_EQ(offer.groups[0].kind, "program");
  EXPECT_EQ(client_commands::find(offer, "esc")->label, "Esc");

  client_commands::set_foreground_program_for_testing("explorer.exe");
  EXPECT_TRUE(client_commands::current_offer().groups.empty());

  client_commands::set_foreground_program_for_testing(std::nullopt);
  client_commands::set_file_for_testing({});
}

TEST(ClientCommandsPrograms, ThePlatformCanBeAsked) {
  client_commands::set_foreground_program_for_testing(std::nullopt);
  // Whatever is in front of the test (possibly nothing), it's a file name rather than a path
  auto program = client_commands::foreground_program();
  EXPECT_EQ(program.find_first_of("\\/"), std::string::npos) << program;
}
