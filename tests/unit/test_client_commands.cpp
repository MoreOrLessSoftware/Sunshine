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
