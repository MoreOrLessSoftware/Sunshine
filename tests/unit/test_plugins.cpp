/**
 * @file tests/unit/test_plugins.cpp
 * @brief Test src/plugins.* and the plugin integration in src/process.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

// lib includes
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

// local includes
#include <src/config.h>
#include <src/plugins.h>
#include <src/process.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::literals;

namespace {
  /**
   * @brief Arguments of one call to the test script runner.
   */
  struct runner_call_t {
    std::string command;  ///< Command line.
    bool elevated;  ///< Whether elevation was requested.
    std::map<std::string, std::string> env;  ///< Variables passed to the script.
    fs::path working_dir;  ///< Directory the script starts in.
    fs::path log_file;  ///< File receiving the output.
  };

  /**
   * @brief Write text to a file, creating parent directories.
   *
   * @param path File to write.
   * @param content Text to write.
   */
  void write_text(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
  }

  /**
   * @brief Read a whole file.
   *
   * @param path File to read.
   * @return File contents, or an empty string when missing.
   */
  std::string read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  /**
   * @brief Find a plugin in the output of `plugins::list()`.
   *
   * @param list Plugin array.
   * @param id Plugin ID.
   * @return The plugin object, or `null` when absent.
   */
  json find_plugin(const json &list, const std::string &id) {
    for (const auto &plugin : list) {
      if (plugin["id"] == id) {
        return plugin;
      }
    }
    return nullptr;
  }

  /**
   * @brief A manifest with every script and an option of each type.
   *
   * @param id Plugin ID.
   * @param order Plugin order.
   * @return Manifest JSON.
   */
  json full_manifest(const std::string &id, int order = 100) {
    return {
      {"id", id},
      {"name", "Plugin " + id},
      {"version", "1.2.3"},
      {"description", "A test plugin"},
      {"author", "Tester"},
      {"homepage", "https://example.invalid"},
      {"order", order},
      {"elevated", true},
      {"manages_display", true},
      {"scripts", {{"install", "install.ps1"}, {"uninstall", "uninstall.ps1"}, {"do", "do.ps1"}, {"undo", "undo.ps1"}}},
      {"options", json::array({
                    {{"id", "enabled"}, {"type", "boolean"}, {"label", "Enabled"}, {"default", true}},
                    {{"id", "multiplier"}, {"type", "number"}, {"label", "Multiplier"}, {"description", "Scale"}, {"default", 2}, {"min", 1}, {"max", 8}, {"step", 1}},
                    {{"id", "name"}, {"type", "string"}, {"default", "Global"}},
                    {{"id", "mode"}, {"type", "choice"}, {"default", "off"}, {"choices", json::array({"off", {{"value", "on"}, {"label", "On"}}})}},
                  })},
    };
  }

  /**
   * @brief A manifest with an apply script that runs when the `mode` option changes.
   *
   * @param id Plugin ID.
   * @return Manifest JSON.
   */
  json apply_manifest(const std::string &id) {
    auto manifest = full_manifest(id);
    manifest["scripts"]["apply"] = "apply.ps1";
    manifest["options"][3]["apply"] = true;
    return manifest;
  }
}  // namespace

/**
 * @brief Fixture with temporary plugin directories and a recording script runner.
 */
class PluginsTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    root = fs::temp_directory_path() / "sunshine_plugins_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::remove_all(root);
    builtin_dir = root / "builtin";
    custom_dir = root / "custom";
    data_dir = root / "data";
    state_file = root / "plugins.json";
    fs::create_directories(builtin_dir);
    fs::create_directories(custom_dir);

    plugins::set_script_runner_for_testing([this](const std::string &command, bool elevated, const std::map<std::string, std::string> &env, const fs::path &working_dir, const fs::path &log_file, std::stop_token) {
      std::scoped_lock lock {calls_mutex};
      calls.push_back({command, elevated, env, working_dir, log_file});
      return runner_exit_code;
    });
  }

  void TearDown() override {
    plugins::cancel_task();
    plugins::reset_script_runner_for_testing();
    plugins::set_change_callback({});
    plugins::load({});
    client_commands::set_foreground_program_for_testing(std::nullopt);
    client_commands::set_file_for_testing({});
    fs::remove_all(root);
    BaseTest::TearDown();
  }

  /**
   * @brief Return the temporary plugin locations.
   *
   * @return Paths under the test root.
   */
  plugins::paths_t paths() const {
    return {builtin_dir, custom_dir, data_dir, state_file};
  }

  /**
   * @brief Create a plugin directory.
   *
   * @param parent Bundled or custom root.
   * @param dir_name Directory name.
   * @param manifest Manifest to write.
   * @return Plugin directory.
   */
  static fs::path write_plugin(const fs::path &parent, const std::string &dir_name, const json &manifest) {
    const auto dir = parent / dir_name;
    write_text(dir / "plugin.json", manifest.dump(2));
    if (manifest.contains("scripts")) {
      for (const auto &[_, script] : manifest["scripts"].items()) {
        write_text(dir / script.get<std::string>(), "exit 0\n");
      }
    }
    return dir;
  }

  fs::path root;  ///< Test root directory.
  fs::path builtin_dir;  ///< Bundled plugins.
  fs::path custom_dir;  ///< Custom plugins.
  fs::path data_dir;  ///< Plugin data.
  fs::path state_file;  ///< Persisted state.
  std::mutex calls_mutex;  ///< Guards `calls`.
  std::vector<runner_call_t> calls;  ///< Calls made to the script runner.
  int runner_exit_code = 0;  ///< Exit code the runner returns.
};

TEST(PluginsIdTest, AcceptsValidIds) {
  EXPECT_TRUE(plugins::is_valid_id("virtual-display"));
  EXPECT_TRUE(plugins::is_valid_id("a"));
  EXPECT_TRUE(plugins::is_valid_id("1plugin.v2_x"));
  EXPECT_TRUE(plugins::is_valid_id(std::string(64, 'a')));
}

TEST(PluginsIdTest, RejectsInvalidIds) {
  EXPECT_FALSE(plugins::is_valid_id(""));
  EXPECT_FALSE(plugins::is_valid_id(std::string(65, 'a')));
  EXPECT_FALSE(plugins::is_valid_id("-plugin"));
  EXPECT_FALSE(plugins::is_valid_id(".plugin"));
  EXPECT_FALSE(plugins::is_valid_id("Plugin"));
  EXPECT_FALSE(plugins::is_valid_id("my plugin"));
  EXPECT_FALSE(plugins::is_valid_id("../x"));
}

TEST_F(PluginsTest, ParseManifestAppliesDefaults) {
  std::string error;
  const auto manifest = plugins::parse_manifest({{"id", "minimal"}}, builtin_dir, true, error);
  ASSERT_TRUE(manifest) << error;
  EXPECT_EQ(manifest->id, "minimal");
  EXPECT_EQ(manifest->name, "minimal");
  EXPECT_EQ(manifest->order, 1000);
  EXPECT_FALSE(manifest->elevated);
  EXPECT_FALSE(manifest->manages_display);
  EXPECT_TRUE(manifest->builtin);
  EXPECT_EQ(manifest->dir, builtin_dir);
  EXPECT_TRUE(manifest->scripts.install.empty());
  EXPECT_TRUE(manifest->options.empty());
}

TEST_F(PluginsTest, ParseManifestReadsEveryField) {
  const auto dir = write_plugin(builtin_dir, "full", full_manifest("full", 42));
  std::string error;
  const auto manifest = plugins::parse_manifest(full_manifest("full", 42), dir, false, error);
  ASSERT_TRUE(manifest) << error;
  EXPECT_EQ(manifest->name, "Plugin full");
  EXPECT_EQ(manifest->version, "1.2.3");
  EXPECT_EQ(manifest->description, "A test plugin");
  EXPECT_EQ(manifest->author, "Tester");
  EXPECT_EQ(manifest->homepage, "https://example.invalid");
  EXPECT_EQ(manifest->order, 42);
  EXPECT_TRUE(manifest->elevated);
  EXPECT_TRUE(manifest->manages_display);
  EXPECT_FALSE(manifest->builtin);
  EXPECT_EQ(manifest->scripts.install, "install.ps1");
  EXPECT_EQ(manifest->scripts.uninstall, "uninstall.ps1");
  EXPECT_EQ(manifest->scripts.do_script, "do.ps1");
  EXPECT_EQ(manifest->scripts.undo_script, "undo.ps1");
  EXPECT_TRUE(manifest->scripts.connect_script.empty());
  EXPECT_TRUE(manifest->scripts.disconnect_script.empty());

  ASSERT_EQ(manifest->options.size(), 4);
  EXPECT_EQ(manifest->options[0].type, plugins::option_type_e::boolean);
  EXPECT_EQ(manifest->options[1].type, plugins::option_type_e::number);
  EXPECT_EQ(manifest->options[1].description, "Scale");
  EXPECT_EQ(manifest->options[1].min, 1.0);
  EXPECT_EQ(manifest->options[1].max, 8.0);
  EXPECT_EQ(manifest->options[1].step, 1.0);
  EXPECT_EQ(manifest->options[2].type, plugins::option_type_e::string);
  EXPECT_EQ(manifest->options[2].label, "name");
  EXPECT_EQ(manifest->options[3].type, plugins::option_type_e::choice);
  ASSERT_EQ(manifest->options[3].choices.size(), 2);
  EXPECT_EQ(manifest->options[3].choices[0].value, "off");
  EXPECT_EQ(manifest->options[3].choices[0].label, "off");
  EXPECT_EQ(manifest->options[3].choices[1].value, "on");
  EXPECT_EQ(manifest->options[3].choices[1].label, "On");
}

TEST_F(PluginsTest, ParseManifestDerivesOptionDefaults) {
  const json manifest_json = {
    {"id", "defaults"},
    {"options", json::array({
                  {{"id", "flag"}, {"type", "boolean"}},
                  {{"id", "bounded"}, {"type", "number"}, {"min", 3}},
                  {{"id", "unbounded"}, {"type", "number"}},
                  {{"id", "text"}, {"type", "string"}},
                  {{"id", "pick"}, {"type", "choice"}, {"choices", json::array({"b", "a"})}},
                })},
  };
  std::string error;
  const auto manifest = plugins::parse_manifest(manifest_json, builtin_dir, true, error);
  ASSERT_TRUE(manifest) << error;
  EXPECT_EQ(manifest->options[0].default_value, false);
  EXPECT_EQ(manifest->options[1].default_value, 3.0);
  EXPECT_EQ(manifest->options[2].default_value, 0.0);
  EXPECT_EQ(manifest->options[3].default_value, "");
  EXPECT_EQ(manifest->options[4].default_value, "b");
}

/**
 * @brief An invalid manifest and the error it should produce.
 */
struct invalid_manifest_t {
  const char *name;  ///< Stable parameter name.
  const char *manifest;  ///< Manifest JSON.
  const char *expected_error;  ///< Text expected in the error.
};

/**
 * @brief Parameterized fixture for rejected manifests.
 */
class InvalidManifestTest: public PluginsTest, public testing::WithParamInterface<invalid_manifest_t> {};

TEST_P(InvalidManifestTest, IsRejected) {
  const auto &param = GetParam();
  write_text(builtin_dir / "exists.ps1", "exit 0\n");
  write_text(builtin_dir / "readme.txt", "");
  std::string error;
  EXPECT_FALSE(plugins::parse_manifest(json::parse(param.manifest), builtin_dir, true, error));
  EXPECT_TRUE(error.contains(param.expected_error)) << error;
}

INSTANTIATE_TEST_SUITE_P(
  Manifests,
  InvalidManifestTest,
  testing::Values(
    invalid_manifest_t {"NotObject", R"([])", "manifest must be an object"},
    invalid_manifest_t {"MissingId", R"({})", "invalid plugin id ''"},
    invalid_manifest_t {"BadId", R"({"id":"Bad Id"})", "invalid plugin id"},
    invalid_manifest_t {"NameNotString", R"({"id":"p","name":1})", "'name' must be a string"},
    invalid_manifest_t {"OrderNotInteger", R"({"id":"p","order":1.5})", "'order' must be an integer"},
    invalid_manifest_t {"ElevatedNotBoolean", R"({"id":"p","elevated":"yes"})", "'elevated' must be a boolean"},
    invalid_manifest_t {"ManagesDisplayNotBoolean", R"({"id":"p","manages_display":1})", "'manages_display' must be a boolean"},
    invalid_manifest_t {"ScriptsNotObject", R"({"id":"p","scripts":[]})", "'scripts' must be an object"},
    invalid_manifest_t {"ScriptNotString", R"({"id":"p","scripts":{"do":1}})", "'do' must be a string"},
    invalid_manifest_t {"ScriptTraversal", R"({"id":"p","scripts":{"do":"../exists.ps1"}})", "must be a .ps1 file"},
    invalid_manifest_t {"ScriptHidden", R"({"id":"p","scripts":{"do":".exists.ps1"}})", "must be a .ps1 file"},
    invalid_manifest_t {"ScriptDrive", R"({"id":"p","scripts":{"do":"C:exists.ps1"}})", "must be a .ps1 file"},
    invalid_manifest_t {"ScriptNotPs1", R"({"id":"p","scripts":{"do":"readme.txt"}})", "must be a .ps1 file"},
    invalid_manifest_t {"ScriptMissing", R"({"id":"p","scripts":{"undo":"missing.ps1"}})", "does not exist"},
    invalid_manifest_t {"OptionsNotArray", R"({"id":"p","options":{}})", "'options' must be an array"},
    invalid_manifest_t {"OptionNotObject", R"({"id":"p","options":[1]})", "each option must be an object"},
    invalid_manifest_t {"OptionIdNotString", R"({"id":"p","options":[{"id":1}]})", "'id' must be a string"},
    invalid_manifest_t {"OptionBadId", R"({"id":"p","options":[{"id":"Bad","type":"string"}]})", "invalid option id"},
    invalid_manifest_t {"OptionTypeNotString", R"({"id":"p","options":[{"id":"o","type":1}]})", "'type' must be a string"},
    invalid_manifest_t {"OptionUnknownType", R"({"id":"p","options":[{"id":"o","type":"color"}]})", "unknown type 'color'"},
    invalid_manifest_t {"OptionLabelNotString", R"({"id":"p","options":[{"id":"o","type":"string","label":1}]})", "'label' must be a string"},
    invalid_manifest_t {"OptionMinNotNumber", R"({"id":"p","options":[{"id":"o","type":"number","min":"1"}]})", "'min' must be a number"},
    invalid_manifest_t {"OptionDefaultOutOfRange", R"({"id":"p","options":[{"id":"o","type":"number","max":2,"default":3}]})", "invalid default"},
    invalid_manifest_t {"OptionDefaultBelowMin", R"({"id":"p","options":[{"id":"o","type":"number","min":2,"default":1}]})", "invalid default"},
    invalid_manifest_t {"OptionDefaultWrongType", R"({"id":"p","options":[{"id":"o","type":"boolean","default":"yes"}]})", "invalid default"},
    invalid_manifest_t {"OptionStringDefaultWrongType", R"({"id":"p","options":[{"id":"o","type":"string","default":1}]})", "invalid default"},
    invalid_manifest_t {"OptionChoiceDefaultUnknown", R"({"id":"p","options":[{"id":"o","type":"choice","choices":["a"],"default":"b"}]})", "invalid default"},
    invalid_manifest_t {"OptionDuplicate", R"({"id":"p","options":[{"id":"o","type":"string"},{"id":"o","type":"string"}]})", "duplicate option id 'o'"},
    invalid_manifest_t {"ApplyScriptNotString", R"({"id":"p","scripts":{"apply":1}})", "'apply' must be a string"},
    invalid_manifest_t {"ApplyScriptMissing", R"({"id":"p","scripts":{"apply":"missing.ps1"}})", "does not exist"},
    invalid_manifest_t {"OptionApplyNotBoolean", R"({"id":"p","scripts":{"apply":"exists.ps1"},"options":[{"id":"o","type":"string","apply":1}]})", "'apply' must be a boolean"},
    invalid_manifest_t {"OptionApplyWithoutScript", R"({"id":"p","options":[{"id":"o","type":"string","apply":true}]})", "has no apply script"},
    invalid_manifest_t {"ChoicesMissing", R"({"id":"p","options":[{"id":"o","type":"choice"}]})", "'choices' must be a non-empty array"},
    invalid_manifest_t {"ChoicesEmpty", R"({"id":"p","options":[{"id":"o","type":"choice","choices":[]}]})", "'choices' must be a non-empty array"},
    invalid_manifest_t {"ChoiceInvalid", R"({"id":"p","options":[{"id":"o","type":"choice","choices":[1]}]})", "each choice must be"},
    invalid_manifest_t {"ChoiceValueMissing", R"({"id":"p","options":[{"id":"o","type":"choice","choices":[{"label":"x"}]}]})", "each choice must be"},
    invalid_manifest_t {"ChoiceLabelNotString", R"({"id":"p","options":[{"id":"o","type":"choice","choices":[{"value":"a","label":1}]}]})", "'label' must be a string"}
  ),
  [](const testing::TestParamInfo<invalid_manifest_t> &info) {
    return std::string(info.param.name);
  }
);

TEST_F(PluginsTest, DiscoverFindsValidPluginsInOrder) {
  write_plugin(builtin_dir, "zeta", full_manifest("zeta", 300));
  write_plugin(builtin_dir, "alpha", full_manifest("alpha", 100));
  write_plugin(custom_dir, "beta", {{"id", "beta"}, {"order", 100}});
  write_plugin(custom_dir, "dup", {{"id", "alpha"}});
  write_plugin(custom_dir, "invalid", {{"id", "Not Valid"}});
  write_text(custom_dir / "broken" / "plugin.json", "{not json");
  fs::create_directories(custom_dir / "empty");
  write_text(custom_dir / "file.txt", "");

  const auto manifests = plugins::discover(builtin_dir, custom_dir);
  ASSERT_EQ(manifests.size(), 3);
  EXPECT_EQ(manifests[0].id, "alpha");
  EXPECT_TRUE(manifests[0].builtin);
  EXPECT_EQ(manifests[1].id, "beta");
  EXPECT_FALSE(manifests[1].builtin);
  EXPECT_EQ(manifests[2].id, "zeta");
  EXPECT_TRUE(manifests[0].dir.is_absolute());
}

TEST_F(PluginsTest, DiscoverIgnoresMissingDirectories) {
  EXPECT_TRUE(plugins::discover(root / "missing", "").empty());
}

TEST_F(PluginsTest, NormalizeOptionsMergesDefaults) {
  std::string error;
  const auto manifest = plugins::parse_manifest(full_manifest("p"), write_plugin(builtin_dir, "p", full_manifest("p")), true, error);
  ASSERT_TRUE(manifest);

  const auto result = plugins::normalize_options(*manifest, {{"multiplier", 4}, {"unknown", 1}}, true, error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ((*result)["multiplier"], 4);
  EXPECT_EQ((*result)["enabled"], true);
  EXPECT_EQ((*result)["name"], "Global");
  EXPECT_EQ((*result)["mode"], "off");
  EXPECT_FALSE(result->contains("unknown"));
}

TEST_F(PluginsTest, NormalizeOptionsStrictRejectsInvalidValues) {
  std::string error;
  const auto manifest = plugins::parse_manifest(full_manifest("p"), write_plugin(builtin_dir, "p", full_manifest("p")), true, error);
  ASSERT_TRUE(manifest);

  EXPECT_FALSE(plugins::normalize_options(*manifest, {{"multiplier", 9}}, true, error));
  EXPECT_EQ(error, "invalid value for option 'multiplier'");
  EXPECT_FALSE(plugins::normalize_options(*manifest, {{"mode", "fast"}}, true, error));
  EXPECT_FALSE(plugins::normalize_options(*manifest, {{"enabled", 1}}, true, error));
  EXPECT_FALSE(plugins::normalize_options(*manifest, {{"multiplier", "2"}}, true, error));
  EXPECT_FALSE(plugins::normalize_options(*manifest, json::array(), true, error));
  EXPECT_EQ(error, "options must be an object");
}

TEST_F(PluginsTest, NormalizeOptionsLenientUsesDefaults) {
  std::string error;
  const auto manifest = plugins::parse_manifest(full_manifest("p"), write_plugin(builtin_dir, "p", full_manifest("p")), true, error);
  ASSERT_TRUE(manifest);

  const auto result = plugins::normalize_options(*manifest, {{"multiplier", 0}, {"mode", "on"}}, false, error);
  ASSERT_TRUE(result);
  EXPECT_EQ((*result)["multiplier"], 2);
  EXPECT_EQ((*result)["mode"], "on");

  const auto from_array = plugins::normalize_options(*manifest, json::array(), false, error);
  ASSERT_TRUE(from_array);
  EXPECT_EQ((*from_array)["multiplier"], 2);
}

TEST(PluginsEnvTest, OptionEnvName) {
  EXPECT_EQ(plugins::option_env_name("fps_multiplier"), "SUNSHINE_PLUGIN_OPT_FPS_MULTIPLIER");
  EXPECT_EQ(plugins::option_env_name("a-b.c9"), "SUNSHINE_PLUGIN_OPT_A_B_C9");
}

TEST(PluginsEnvTest, OptionEnvValue) {
  EXPECT_EQ(plugins::option_env_value(true), "true");
  EXPECT_EQ(plugins::option_env_value(false), "false");
  EXPECT_EQ(plugins::option_env_value(4), "4");
  EXPECT_EQ(plugins::option_env_value(-3), "-3");
  EXPECT_EQ(plugins::option_env_value(4.0), "4");
  EXPECT_EQ(plugins::option_env_value(2.5), "2.5");
  EXPECT_EQ(plugins::option_env_value(1e20), "1e+20");
  EXPECT_EQ(plugins::option_env_value("text"), "text");
  EXPECT_EQ(plugins::option_env_value(nullptr), "null");
  EXPECT_EQ(plugins::option_env_value(json::array({1, 2})), "[1,2]");
}

TEST_F(PluginsTest, ScriptEnvIncludesPathsAndOptions) {
  std::string error;
  const auto dir = write_plugin(builtin_dir, "p", full_manifest("p"));
  const auto manifest = plugins::parse_manifest(full_manifest("p"), dir, true, error);
  ASSERT_TRUE(manifest);

  const auto env = plugins::script_env(*manifest, {{"multiplier", 3}}, data_dir / "p");
  EXPECT_EQ(env.at("SUNSHINE_PLUGIN_ID"), "p");
  EXPECT_EQ(fs::path(env.at("SUNSHINE_PLUGIN_DIR")), fs::path(dir).make_preferred());
  EXPECT_EQ(fs::path(env.at("SUNSHINE_PLUGIN_DATA_DIR")), fs::path(data_dir / "p").make_preferred());
  EXPECT_EQ(env.at("SUNSHINE_PLUGIN_OPT_MULTIPLIER"), "3");
  EXPECT_EQ(env.at("SUNSHINE_PLUGIN_OPT_ENABLED"), "true");
  EXPECT_EQ(env.at("SUNSHINE_PLUGIN_OPT_MODE"), "off");
}

TEST_F(PluginsTest, ScriptCommandRunsPowerShellFile) {
  const auto command = plugins::script_command(builtin_dir / "do.ps1");
#ifdef _WIN32
  EXPECT_TRUE(command.starts_with("powershell.exe "));
  EXPECT_TRUE(command.contains("-ExecutionPolicy Bypass"));
#else
  EXPECT_TRUE(command.starts_with("pwsh "));
#endif
  EXPECT_TRUE(command.contains("-NonInteractive"));
  EXPECT_TRUE(command.ends_with("-File \"" + fs::path(builtin_dir / "do.ps1").make_preferred().string() + "\""));
}

TEST_F(PluginsTest, StateRoundTrips) {
  std::unordered_map<std::string, plugins::plugin_state_t> state;
  state["a"] = {true, true, {{"x", 1}}};
  state["b"] = {false, false, json::object()};
  ASSERT_TRUE(plugins::save_state(state_file, state));
  EXPECT_FALSE(fs::exists(state_file.string() + ".tmp"));

  const auto loaded = plugins::load_state(state_file);
  ASSERT_EQ(loaded.size(), 2);
  EXPECT_TRUE(loaded.at("a").installed);
  EXPECT_TRUE(loaded.at("a").active);
  EXPECT_EQ(loaded.at("a").options["x"], 1);
  EXPECT_FALSE(loaded.at("b").installed);
}

TEST_F(PluginsTest, LoadStateHandlesBadInput) {
  EXPECT_TRUE(plugins::load_state(root / "missing.json").empty());

  write_text(state_file, "{not json");
  EXPECT_TRUE(plugins::load_state(state_file).empty());

  write_text(state_file, R"({"other":{}})");
  EXPECT_TRUE(plugins::load_state(state_file).empty());

  write_text(state_file, R"({"plugins":{"a":{"active":true},"b":1,"c":{"installed":true,"options":[1]}}})");
  const auto state = plugins::load_state(state_file);
  ASSERT_EQ(state.size(), 2);
  EXPECT_FALSE(state.at("a").active);
  EXPECT_TRUE(state.at("c").installed);
  EXPECT_TRUE(state.at("c").options.is_object());
  EXPECT_TRUE(state.at("c").options.empty());
}

TEST_F(PluginsTest, SaveStateReportsFailures) {
  // The final path is an existing directory, so the rename fails.
  fs::create_directories(state_file);
  write_text(state_file / "keep", "");
  EXPECT_FALSE(plugins::save_state(state_file, {}));

  // The parent is a file, so the temporary file cannot be created.
  write_text(root / "file", "");
  EXPECT_FALSE(plugins::save_state(root / "file" / "plugins.json", {}));
}

TEST_F(PluginsTest, DefaultPathsUseAssetsAndAppdata) {
  const auto defaults = plugins::default_paths();
  EXPECT_EQ(defaults.builtin_dir.filename(), "plugins");
  EXPECT_EQ(defaults.custom_dir, platf::appdata() / "plugins");
  EXPECT_EQ(defaults.data_dir, platf::appdata() / "plugin-data");
  EXPECT_EQ(defaults.state_file, platf::appdata() / "plugins.json");
}

TEST_F(PluginsTest, ListDescribesPlugins) {
  write_plugin(builtin_dir, "full", full_manifest("full"));
  plugins::load(paths());
  EXPECT_EQ(plugins::current_paths().custom_dir, custom_dir);

  const auto list = plugins::list();
  ASSERT_EQ(list.size(), 1);
  const auto &plugin = list[0];
  EXPECT_EQ(plugin["id"], "full");
  EXPECT_EQ(plugin["name"], "Plugin full");
  EXPECT_EQ(plugin["version"], "1.2.3");
  EXPECT_EQ(plugin["builtin"], true);
  EXPECT_EQ(plugin["elevated"], true);
  EXPECT_EQ(plugin["manages_display"], true);
  EXPECT_EQ(plugin["order"], 100);
  EXPECT_EQ(plugin["installed"], false);
  EXPECT_EQ(plugin["active"], false);
  EXPECT_TRUE(plugin["task"].is_null());
  EXPECT_EQ(plugin["config"]["multiplier"], 2);

  const auto &options = plugin["options"];
  ASSERT_EQ(options.size(), 4);
  EXPECT_EQ(options[0]["type"], "boolean");
  EXPECT_EQ(options[1]["type"], "number");
  EXPECT_EQ(options[1]["min"], 1.0);
  EXPECT_EQ(options[1]["max"], 8.0);
  EXPECT_EQ(options[1]["step"], 1.0);
  EXPECT_EQ(options[2]["type"], "string");
  EXPECT_FALSE(options[2].contains("min"));
  EXPECT_EQ(options[3]["type"], "choice");
  EXPECT_EQ(options[3]["choices"][1]["label"], "On");
}

TEST_F(PluginsTest, ConfigureValidatesRequests) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  plugins::load(paths());

  std::string error;
  EXPECT_EQ(plugins::configure("missing", true, std::nullopt, error), plugins::result_e::not_found);
  EXPECT_EQ(plugins::configure("p", std::nullopt, json {{"multiplier", 99}}, error), plugins::result_e::invalid);
  EXPECT_EQ(plugins::configure("p", true, std::nullopt, error), plugins::result_e::not_installed);
  EXPECT_TRUE(error.contains("must be installed"));

  // Options can be set before the plugin is installed.
  EXPECT_EQ(plugins::configure("p", false, json {{"multiplier", 5}}, error), plugins::result_e::ok);
  EXPECT_EQ(find_plugin(plugins::list(), "p")["config"]["multiplier"], 5);
  EXPECT_EQ(plugins::load_state(state_file).at("p").options["multiplier"], 5);
}

TEST_F(PluginsTest, ParseManifestReadsApplyScriptAndOption) {
  const auto dir = write_plugin(builtin_dir, "p", apply_manifest("p"));
  std::string error;
  const auto manifest = plugins::parse_manifest(apply_manifest("p"), dir, true, error);
  ASSERT_TRUE(manifest) << error;
  EXPECT_EQ(manifest->scripts.apply, "apply.ps1");
  EXPECT_FALSE(manifest->options[0].apply);
  EXPECT_TRUE(manifest->options[3].apply);
}

TEST_F(PluginsTest, SavingAnApplyOptionRunsTheScriptThenCommits) {
  const auto dir = write_plugin(builtin_dir, "p", apply_manifest("p"));
  plugins::load(paths());
  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();
  calls.clear();

  int changes = 0;
  plugins::set_change_callback([&changes]() {
    ++changes;
  });

  bool applying = false;
  ASSERT_EQ(plugins::configure("p", std::nullopt, json {{"mode", "on"}, {"multiplier", 5}}, error, &applying), plugins::result_e::ok) << error;
  EXPECT_TRUE(applying);
  plugins::wait_for_task();

  ASSERT_EQ(calls.size(), 1);
  EXPECT_EQ(calls[0].command, plugins::script_command(dir / "apply.ps1"));
  EXPECT_TRUE(calls[0].elevated);
  EXPECT_EQ(calls[0].working_dir, data_dir / "p");
  EXPECT_EQ(calls[0].env.at("SUNSHINE_PLUGIN_OPT_MODE"), "on");
  EXPECT_EQ(calls[0].env.at("SUNSHINE_PLUGIN_OPT_MULTIPLIER"), "5");

  const auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["config"]["mode"], "on");
  EXPECT_EQ(plugin["config"]["multiplier"], 5);
  EXPECT_EQ(plugin["installed"], true);
  EXPECT_EQ(plugin["task"], json({{"action", "apply"}, {"status", "succeeded"}, {"exit_code", 0}}));
  EXPECT_EQ(plugins::load_state(state_file).at("p").options["mode"], "on");
  EXPECT_EQ(changes, 1);
  const auto log = plugins::read_log("p");
  ASSERT_TRUE(log);
  EXPECT_TRUE(log->contains("Applying options for Plugin p")) << *log;
}

TEST_F(PluginsTest, FailedApplyKeepsTheOldOptionsUntilRetried) {
  write_plugin(builtin_dir, "p", apply_manifest("p"));
  plugins::load(paths());
  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();

  int changes = 0;
  plugins::set_change_callback([&changes]() {
    ++changes;
  });

  const json updated {{"mode", "on"}, {"multiplier", 5}};
  runner_exit_code = 3;
  bool applying = false;
  ASSERT_EQ(plugins::configure("p", std::nullopt, updated, error, &applying), plugins::result_e::ok) << error;
  EXPECT_TRUE(applying);
  plugins::wait_for_task();

  auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["config"]["mode"], "off");
  EXPECT_EQ(plugin["config"]["multiplier"], 2);  // an unrelated edit in the same save is held back too
  EXPECT_EQ(plugin["task"]["action"], "apply");
  EXPECT_EQ(plugin["task"]["status"], "failed");
  EXPECT_EQ(plugin["task"]["exit_code"], 3);
  EXPECT_EQ(plugin["installed"], true);
  EXPECT_EQ(changes, 0);

  runner_exit_code = 0;
  ASSERT_EQ(plugins::configure("p", std::nullopt, updated, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();
  plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["config"]["mode"], "on");
  EXPECT_EQ(plugin["config"]["multiplier"], 5);
  EXPECT_EQ(changes, 1);
}

TEST_F(PluginsTest, SavesThatNeedNoApplyAreImmediate) {
  write_plugin(builtin_dir, "p", apply_manifest("p"));
  plugins::load(paths());
  std::string error;
  bool applying = false;

  // Before the install, the install script is what picks up the value.
  ASSERT_EQ(plugins::configure("p", std::nullopt, json {{"mode", "on"}}, error, &applying), plugins::result_e::ok) << error;
  EXPECT_FALSE(applying);
  EXPECT_EQ(find_plugin(plugins::list(), "p")["config"]["mode"], "on");
  EXPECT_TRUE(calls.empty());

  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();
  calls.clear();

  // The marked option keeps its value.
  ASSERT_EQ(plugins::configure("p", std::nullopt, json {{"mode", "on"}, {"multiplier", 7}}, error, &applying), plugins::result_e::ok) << error;
  EXPECT_FALSE(applying);
  EXPECT_EQ(find_plugin(plugins::list(), "p")["config"]["multiplier"], 7);

  // No options at all.
  ASSERT_EQ(plugins::configure("p", true, std::nullopt, error, &applying), plugins::result_e::ok) << error;
  EXPECT_FALSE(applying);
  EXPECT_TRUE(calls.empty());
}

TEST_F(PluginsTest, ActiveChangeInAnApplyingSaveIsImmediate) {
  write_plugin(builtin_dir, "p", apply_manifest("p"));
  plugins::load(paths());
  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();

  runner_exit_code = 1;
  ASSERT_EQ(plugins::configure("p", true, json {{"mode", "on"}}, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();
  const auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["active"], true);
  EXPECT_EQ(plugin["config"]["mode"], "off");
}

TEST_F(PluginsTest, SavesAreRejectedWhileAnApplyCannotStartOrIsRunning) {
  write_plugin(builtin_dir, "p", apply_manifest("p"));
  write_plugin(builtin_dir, "other", full_manifest("other"));
  plugins::load(paths());
  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();

  std::promise<void> release;
  auto released = release.get_future().share();
  plugins::set_script_runner_for_testing([released](const std::string &, bool, const std::map<std::string, std::string> &, const fs::path &, const fs::path &, std::stop_token) {
    released.wait();
    return 0;
  });

  bool applying = false;
  ASSERT_EQ(plugins::configure("p", std::nullopt, json {{"mode", "on"}}, error, &applying), plugins::result_e::ok) << error;
  ASSERT_TRUE(applying);
  EXPECT_EQ(find_plugin(plugins::list(), "p")["config"]["mode"], "off");  // not committed yet
  EXPECT_EQ(find_plugin(plugins::list(), "p")["task"]["status"], "running");

  // Another change to the marked option needs the worker that is busy.
  EXPECT_EQ(plugins::configure("p", std::nullopt, json {{"mode", "on"}, {"multiplier", 3}}, error), plugins::result_e::busy);
  EXPECT_EQ(error, "another plugin operation is still running");
  // The running script's options would overwrite any other save of this plugin.
  EXPECT_EQ(plugins::configure("p", std::nullopt, json {{"mode", "off"}, {"multiplier", 3}}, error), plugins::result_e::busy);
  EXPECT_TRUE(error.contains("still being applied")) << error;

  // Neither limit applies to switching the plugin off or to saving another plugin.
  EXPECT_EQ(plugins::configure("p", false, std::nullopt, error), plugins::result_e::ok) << error;
  EXPECT_EQ(plugins::configure("other", std::nullopt, json {{"multiplier", 4}}, error), plugins::result_e::ok) << error;
  EXPECT_EQ(find_plugin(plugins::list(), "other")["config"]["multiplier"], 4);

  release.set_value();
  plugins::wait_for_task();
  EXPECT_EQ(find_plugin(plugins::list(), "p")["config"]["mode"], "on");
}

TEST_F(PluginsTest, InstallActivateAndUninstall) {
  const auto dir = write_plugin(builtin_dir, "p", full_manifest("p"));
  plugins::load(paths());

  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok) << error;
  plugins::wait_for_task();

  ASSERT_EQ(calls.size(), 1);
  EXPECT_EQ(calls[0].command, plugins::script_command(dir / "install.ps1"));
  EXPECT_TRUE(calls[0].elevated);
  EXPECT_EQ(calls[0].working_dir, data_dir / "p");
  EXPECT_EQ(calls[0].log_file, data_dir / "p.log");
  EXPECT_EQ(calls[0].env.at("SUNSHINE_PLUGIN_OPT_MULTIPLIER"), "2");
  EXPECT_TRUE(fs::is_directory(data_dir / "p"));

  auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], true);
  EXPECT_EQ(plugin["task"]["action"], "install");
  EXPECT_EQ(plugin["task"]["status"], "succeeded");
  EXPECT_EQ(plugin["task"]["exit_code"], 0);

  const auto log = plugins::read_log("p");
  ASSERT_TRUE(log);
  EXPECT_TRUE(log->contains("Installing Plugin p 1.2.3"));
  EXPECT_TRUE(log->contains("Completed (exit code 0)"));

  ASSERT_EQ(plugins::configure("p", true, std::nullopt, error), plugins::result_e::ok);
  EXPECT_EQ(find_plugin(plugins::list(), "p")["active"], true);

  ASSERT_EQ(plugins::start_task("p", plugins::action_e::uninstall, error), plugins::result_e::ok);
  plugins::wait_for_task();
  ASSERT_EQ(calls.size(), 2);
  EXPECT_EQ(calls[1].command, plugins::script_command(dir / "uninstall.ps1"));
  EXPECT_FALSE(fs::exists(data_dir / "p"));

  plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], false);
  EXPECT_EQ(plugin["active"], false);
  EXPECT_EQ(plugin["task"]["action"], "uninstall");
  EXPECT_EQ(plugin["task"]["status"], "succeeded");

  const auto state = plugins::load_state(state_file);
  EXPECT_FALSE(state.at("p").installed);
  EXPECT_FALSE(state.at("p").active);
}

TEST_F(PluginsTest, InstallCanActivateAndNotifies) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  plugins::load(paths());
  int changes = 0;
  plugins::set_change_callback([&changes]() {
    // The registry must be usable from the callback.
    EXPECT_EQ(plugins::list().size(), 1);
    ++changes;
  });

  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error, true), plugins::result_e::ok);
  plugins::wait_for_task();
  auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], true);
  EXPECT_EQ(plugin["active"], true);
  EXPECT_TRUE(plugins::load_state(state_file).at("p").active);
  EXPECT_EQ(changes, 1);

  // Activation only applies to installs.
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::uninstall, error, true), plugins::result_e::ok);
  plugins::wait_for_task();
  plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], false);
  EXPECT_EQ(plugin["active"], false);
  EXPECT_EQ(changes, 2);
}

TEST_F(PluginsTest, InstallWithoutActivateLeavesPluginInactive) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  plugins::load(paths());

  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();
  const auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], true);
  EXPECT_EQ(plugin["active"], false);
}

TEST_F(PluginsTest, FailedInstallDoesNotActivateOrNotify) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  plugins::load(paths());
  runner_exit_code = 2;
  int changes = 0;
  plugins::set_change_callback([&changes]() {
    ++changes;
  });

  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error, true), plugins::result_e::ok);
  plugins::wait_for_task();
  const auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], false);
  EXPECT_EQ(plugin["active"], false);
  EXPECT_EQ(changes, 0);
}

TEST_F(PluginsTest, FailedInstallLeavesPluginUninstalled) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  plugins::load(paths());
  runner_exit_code = 3;

  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();

  const auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], false);
  EXPECT_EQ(plugin["task"]["status"], "failed");
  EXPECT_EQ(plugin["task"]["exit_code"], 3);
  EXPECT_TRUE(plugins::read_log("p")->contains("Failed (exit code 3)"));
}

TEST_F(PluginsTest, FailedUninstallDeactivatesButKeepsInstalled) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());
  runner_exit_code = 1;

  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::uninstall, error), plugins::result_e::ok);
  // The plugin is deactivated as soon as the uninstall starts.
  EXPECT_FALSE(plugins::load_state(state_file).at("p").active);
  plugins::wait_for_task();

  const auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], true);
  EXPECT_EQ(plugin["active"], false);
  EXPECT_EQ(plugin["task"]["status"], "failed");
}

TEST_F(PluginsTest, PluginWithoutScriptsInstallsWithoutRunning) {
  write_plugin(custom_dir, "bare", {{"id", "bare"}});
  plugins::load(paths());

  std::string error;
  ASSERT_EQ(plugins::start_task("bare", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();
  EXPECT_TRUE(calls.empty());
  EXPECT_EQ(find_plugin(plugins::list(), "bare")["installed"], true);
}

TEST_F(PluginsTest, StartTaskRejectsUnknownAndConcurrentRequests) {
  write_plugin(builtin_dir, "a", full_manifest("a"));
  write_plugin(builtin_dir, "b", full_manifest("b"));
  plugins::load(paths());

  std::string error;
  EXPECT_EQ(plugins::start_task("missing", plugins::action_e::install, error), plugins::result_e::not_found);

  std::promise<void> release;
  auto released = release.get_future().share();
  plugins::set_script_runner_for_testing([released](const std::string &, bool, const std::map<std::string, std::string> &, const fs::path &, const fs::path &, std::stop_token) {
    released.wait();
    return 0;
  });

  ASSERT_EQ(plugins::start_task("a", plugins::action_e::install, error), plugins::result_e::ok);
  EXPECT_EQ(find_plugin(plugins::list(), "a")["task"]["status"], "running");
  EXPECT_EQ(plugins::start_task("b", plugins::action_e::install, error), plugins::result_e::busy);
  EXPECT_EQ(error, "another plugin operation is still running");

  release.set_value();
  // Wait for the task to report completion without joining it, so the next start joins the finished worker.
  for (int i = 0; i < 500 && find_plugin(plugins::list(), "a")["task"]["status"] == "running"; ++i) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(find_plugin(plugins::list(), "a")["task"]["status"], "succeeded");
  EXPECT_EQ(plugins::start_task("b", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();
  EXPECT_EQ(find_plugin(plugins::list(), "b")["installed"], true);
}

TEST_F(PluginsTest, PrepCmdsIncludeActivePluginsInOrder) {
  const auto second = write_plugin(builtin_dir, "second", full_manifest("second", 200));
  const auto first = write_plugin(custom_dir, "first", {
                                                         {"id", "first"},
                                                         {"order", 100},
                                                         {"scripts", {{"do", "do.ps1"}}},
                                                       });
  write_plugin(builtin_dir, "inactive", full_manifest("inactive", 50));
  write_plugin(builtin_dir, "noscripts", {{"id", "noscripts"}});
  write_plugin(builtin_dir, "excluded", full_manifest("excluded", 10));
  write_text(state_file, R"({"plugins":{
    "first":{"installed":true,"active":true},
    "second":{"installed":true,"active":true,"options":{"multiplier":4}},
    "inactive":{"installed":true,"active":false},
    "noscripts":{"installed":true,"active":true},
    "excluded":{"installed":true,"active":true}
  }})");
  plugins::load(paths());

  const auto cmds = plugins::prep_cmds({"excluded"});
  ASSERT_EQ(cmds.size(), 2);
  EXPECT_EQ(cmds[0].do_cmd, plugins::script_command(first / "do.ps1"));
  EXPECT_TRUE(cmds[0].undo_cmd.empty());
  EXPECT_FALSE(cmds[0].elevated);
  EXPECT_EQ(cmds[0].env.at("SUNSHINE_PLUGIN_ID"), "first");

  EXPECT_EQ(cmds[1].do_cmd, plugins::script_command(second / "do.ps1"));
  EXPECT_EQ(cmds[1].undo_cmd, plugins::script_command(second / "undo.ps1"));
  EXPECT_TRUE(cmds[1].elevated);
  EXPECT_EQ(cmds[1].env.at("SUNSHINE_PLUGIN_OPT_MULTIPLIER"), "4");
  EXPECT_EQ(fs::path(cmds[1].env.at("SUNSHINE_PLUGIN_DATA_DIR")), fs::path(data_dir / "second").make_preferred());

  EXPECT_EQ(plugins::prep_cmds({}).size(), 3);
}

TEST_F(PluginsTest, DisplayPluginIsFirstActiveManagingPlugin) {
  write_plugin(builtin_dir, "plain", {{"id", "plain"}, {"order", 5}});
  write_plugin(builtin_dir, "excluded", {{"id", "excluded"}, {"name", "Excluded"}, {"order", 10}, {"manages_display", true}});
  write_plugin(builtin_dir, "inactive", {{"id", "inactive"}, {"order", 20}, {"manages_display", true}});
  write_plugin(builtin_dir, "uninstalled", {{"id", "uninstalled"}, {"order", 30}, {"manages_display", true}});
  write_plugin(builtin_dir, "display", {{"id", "display"}, {"name", "Display"}, {"order", 40}, {"manages_display", true}});
  write_plugin(builtin_dir, "later", {{"id", "later"}, {"name", "Later"}, {"order", 50}, {"manages_display", true}});
  write_text(state_file, R"({"plugins":{
    "plain":{"installed":true,"active":true},
    "excluded":{"installed":true,"active":true},
    "inactive":{"installed":true,"active":false},
    "uninstalled":{"installed":false,"active":true},
    "display":{"installed":true,"active":true},
    "later":{"installed":true,"active":true}
  }})");
  plugins::load(paths());

  EXPECT_EQ(plugins::display_plugin({}), "Excluded");
  EXPECT_EQ(plugins::display_plugin({"excluded"}), "Display");
  EXPECT_EQ(plugins::display_plugin({"excluded", "display"}), "Later");
  EXPECT_EQ(plugins::display_plugin({"excluded", "display", "later"}), "");
}

TEST_F(PluginsTest, ParsesConnectAndDisconnectScripts) {
  const json node = {{"id", "c"}, {"scripts", {{"connect", "resume.ps1"}, {"disconnect", "leave.ps1"}}}};
  const auto dir = write_plugin(builtin_dir, "c", node);
  std::string error;
  const auto manifest = plugins::parse_manifest(node, dir, false, error);
  ASSERT_TRUE(manifest) << error;
  EXPECT_EQ(manifest->scripts.connect_script, "resume.ps1");
  EXPECT_EQ(manifest->scripts.disconnect_script, "leave.ps1");
}

TEST_F(PluginsTest, RejectsMissingConnectScript) {
  const auto dir = write_plugin(builtin_dir, "c", {{"id", "c"}});
  std::string error;
  EXPECT_FALSE(plugins::parse_manifest({{"id", "c"}, {"scripts", {{"connect", "missing.ps1"}}}}, dir, false, error));
  EXPECT_EQ(error, "script 'missing.ps1' does not exist");
}

TEST_F(PluginsTest, PrepCmdsAddConnectCommandsAfterAppCommands) {
  auto both_manifest = full_manifest("both");
  both_manifest["scripts"]["connect"] = "resume.ps1";
  const auto both = write_plugin(builtin_dir, "both", both_manifest);
  const auto resume_only = write_plugin(builtin_dir, "resume-only", {
                                                                       {"id", "resume-only"},
                                                                       {"order", 300},
                                                                       {"scripts", {{"disconnect", "leave.ps1"}}},
                                                                     });
  write_text(state_file, R"({"plugins":{
    "both":{"installed":true,"active":true,"options":{"multiplier":3}},
    "resume-only":{"installed":true,"active":true}
  }})");
  plugins::load(paths());

  const auto cmds = plugins::prep_cmds({});
  ASSERT_EQ(cmds.size(), 3);
  EXPECT_EQ(cmds[0].type, config::prep_cmd_t::type_e::app);
  EXPECT_EQ(cmds[0].do_cmd, plugins::script_command(both / "do.ps1"));

  EXPECT_EQ(cmds[1].type, config::prep_cmd_t::type_e::connect);
  EXPECT_EQ(cmds[1].do_cmd, plugins::script_command(both / "resume.ps1"));
  EXPECT_TRUE(cmds[1].undo_cmd.empty());
  EXPECT_TRUE(cmds[1].elevated);
  EXPECT_EQ(cmds[1].env.at("SUNSHINE_PLUGIN_OPT_MULTIPLIER"), "3");

  EXPECT_EQ(cmds[2].type, config::prep_cmd_t::type_e::connect);
  EXPECT_TRUE(cmds[2].do_cmd.empty());
  EXPECT_EQ(cmds[2].undo_cmd, plugins::script_command(resume_only / "leave.ps1"));
  EXPECT_EQ(cmds[2].env.at("SUNSHINE_PLUGIN_ID"), "resume-only");
}

TEST_F(PluginsTest, InstallSucceedsWhenLogCannotBeWritten) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  fs::create_directories(data_dir / "p.log");
  plugins::load(paths());

  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();
  EXPECT_EQ(find_plugin(plugins::list(), "p")["installed"], true);
  EXPECT_EQ(plugins::read_log("p"), "");
}

TEST_F(PluginsTest, InitLoadsDefaultPaths) {
  auto guard = plugins::init();
  ASSERT_TRUE(guard);
  const auto loaded = plugins::current_paths();
  const auto defaults = plugins::default_paths();
  EXPECT_EQ(loaded.builtin_dir, defaults.builtin_dir);
  EXPECT_EQ(loaded.custom_dir, defaults.custom_dir);
  EXPECT_EQ(loaded.data_dir, defaults.data_dir);
  EXPECT_EQ(loaded.state_file, defaults.state_file);
  guard.reset();
}

TEST_F(PluginsTest, ReadLogHandlesMissingPluginsAndLogs) {
  write_plugin(builtin_dir, "p", full_manifest("p"));
  plugins::load(paths());
  EXPECT_FALSE(plugins::read_log("missing"));
  EXPECT_EQ(plugins::read_log("p"), "");
}

TEST_F(PluginsTest, RescanFindsNewPluginsAndKeepsState) {
  write_plugin(builtin_dir, "a", full_manifest("a"));
  write_text(state_file, R"({"plugins":{"a":{"installed":true,"active":true}}})");
  plugins::load(paths());
  EXPECT_EQ(plugins::list().size(), 1);

  write_plugin(custom_dir, "b", {{"id", "b"}});
  plugins::rescan();
  const auto list = plugins::list();
  ASSERT_EQ(list.size(), 2);
  EXPECT_EQ(find_plugin(list, "a")["active"], true);
  EXPECT_FALSE(find_plugin(list, "b").is_null());
}

TEST_F(PluginsTest, ParseAddsPluginCommandsBeforeGlobalCommands) {
  const auto dir = write_plugin(builtin_dir, "p", full_manifest("p"));
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  const auto saved_prep_cmds = config::sunshine.prep_cmds;
  config::sunshine.prep_cmds.clear();
  config::sunshine.prep_cmds.emplace_back("global-do", "global-undo", false);

  const auto apps_file = root / "apps.json";
  write_text(apps_file, R"({"env":{},"apps":[
    {"name":"Default"},
    {"name":"Excluded","exclude-plugins":["p"]},
    {"name":"NoGlobal","exclude-global-prep-cmd":true,"prep-cmd":[{"do":"app-do"}]}
  ]})");
  auto proc = proc::parse(apps_file.string());
  config::sunshine.prep_cmds = saved_prep_cmds;
  ASSERT_TRUE(proc);

  const auto &apps = proc->get_apps();
  ASSERT_EQ(apps.size(), 3);

  ASSERT_EQ(apps[0].prep_cmds.size(), 2);
  EXPECT_EQ(apps[0].prep_cmds[0].do_cmd, plugins::script_command(dir / "do.ps1"));
  EXPECT_EQ(apps[0].prep_cmds[0].env.at("SUNSHINE_PLUGIN_ID"), "p");
  EXPECT_EQ(apps[0].prep_cmds[1].do_cmd, "global-do");
  EXPECT_TRUE(apps[0].prep_cmds[1].env.empty());

  ASSERT_EQ(apps[1].prep_cmds.size(), 1);
  EXPECT_EQ(apps[1].prep_cmds[0].do_cmd, "global-do");

  ASSERT_EQ(apps[2].prep_cmds.size(), 2);
  EXPECT_EQ(apps[2].prep_cmds[0].env.at("SUNSHINE_PLUGIN_ID"), "p");
  EXPECT_EQ(apps[2].prep_cmds[1].do_cmd, "app-do");

  // The plugin manages the display for every application that does not exclude it
  EXPECT_EQ(apps[0].display_plugin, "Plugin p");
  EXPECT_EQ(apps[1].display_plugin, "");
  EXPECT_EQ(proc->get_display_plugin(std::stoi(apps[0].id)), "Plugin p");
  EXPECT_EQ(proc->get_display_plugin(std::stoi(apps[1].id)), "");
  EXPECT_EQ(proc->get_display_plugin(-1), "");
}

TEST_F(PluginsTest, ParseRoutesPluginConnectCommandsToConnectCommands) {
  auto manifest = full_manifest("p");
  manifest["scripts"]["connect"] = "resume.ps1";
  const auto dir = write_plugin(builtin_dir, "p", manifest);
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  const auto saved_prep_cmds = config::sunshine.prep_cmds;
  config::sunshine.prep_cmds.clear();
  config::sunshine.prep_cmds.emplace_back("global-connect", "", false, config::prep_cmd_t::type_e::connect);

  const auto apps_file = root / "apps.json";
  write_text(apps_file, R"({"env":{},"apps":[{"name":"Default"}]})");
  auto proc = proc::parse(apps_file.string());
  config::sunshine.prep_cmds = saved_prep_cmds;
  ASSERT_TRUE(proc);

  const auto &app = proc->get_apps().at(0);
  ASSERT_EQ(app.prep_cmds.size(), 1);
  EXPECT_EQ(app.prep_cmds[0].do_cmd, plugins::script_command(dir / "do.ps1"));

  ASSERT_EQ(app.connect_cmds.size(), 2);
  EXPECT_EQ(app.connect_cmds[0].do_cmd, plugins::script_command(dir / "resume.ps1"));
  EXPECT_EQ(app.connect_cmds[0].env.at("SUNSHINE_PLUGIN_ID"), "p");
  EXPECT_EQ(app.connect_cmds[1].do_cmd, "global-connect");
}

TEST(PrepCmdEnvTest, AddsCommandVariables) {
  boost::process::v1::environment base;
  base["SHARED"] = "1";
  base["OVERRIDDEN"] = "base";

  proc::cmd_t cmd {"do", "undo", false};
  cmd.env = {{"OVERRIDDEN", "cmd"}, {"ADDED", "2"}};

  const auto env = proc::prep_cmd_env(base, cmd);
  EXPECT_EQ(env.at("SHARED").to_string(), "1");
  EXPECT_EQ(env.at("OVERRIDDEN").to_string(), "cmd");
  EXPECT_EQ(env.at("ADDED").to_string(), "2");
  EXPECT_EQ(base.at("OVERRIDDEN").to_string(), "base");
  EXPECT_EQ(base.count("ADDED"), 0);
}

namespace {
  /**
   * @brief A manifest with two actions and a panel that reads from a local port.
   *
   * @param id Plugin ID.
   * @param port Port the panel may read from, or 0 for none.
   * @return Manifest JSON.
   */
  json manifest_with_panel(const std::string &id, int port) {
    auto manifest = full_manifest(id);
    manifest["actions"] = json::array({
      {{"id", "test_start"}, {"label", "Start test"}, {"description", "Starts the service"}, {"script", "test-start.ps1"}},
      {{"id", "test_stop"}, {"script", "test-stop.ps1"}},
    });
    manifest["panel"] = {{"file", "panel.html"}};
    if (port != 0) {
      manifest["panel"]["port"] = port;
    }
    return manifest;
  }

  /**
   * @brief Write a plugin with actions and a panel, including the files its manifest references.
   *
   * @param parent Bundled or custom root.
   * @param id Plugin ID, also its directory name.
   * @param port Port the panel may read from, or 0 for none.
   * @return Plugin directory.
   */
  fs::path write_panel_plugin(const fs::path &parent, const std::string &id, int port) {
    const auto manifest = manifest_with_panel(id, port);
    const auto dir = parent / id;
    write_text(dir / "plugin.json", manifest.dump(2));
    for (const auto &[_, script] : manifest["scripts"].items()) {
      write_text(dir / script.get<std::string>(), "exit 0\n");
    }
    write_text(dir / "test-start.ps1", "exit 0\n");
    write_text(dir / "test-stop.ps1", "exit 0\n");
    write_text(dir / "panel.html", "<!doctype html><title>panel</title>");
    return dir;
  }
}  // namespace

TEST_F(PluginsTest, ParseManifestReadsActionsAndPanel) {
  const auto dir = write_panel_plugin(builtin_dir, "p", 8008);
  std::string error;
  const auto manifest = plugins::parse_manifest(manifest_with_panel("p", 8008), dir, true, error);
  ASSERT_TRUE(manifest) << error;
  ASSERT_EQ(manifest->actions.size(), 2);
  EXPECT_EQ(manifest->actions[0].id, "test_start");
  EXPECT_EQ(manifest->actions[0].label, "Start test");
  EXPECT_EQ(manifest->actions[0].description, "Starts the service");
  EXPECT_EQ(manifest->actions[0].script, "test-start.ps1");
  EXPECT_EQ(manifest->actions[1].label, "test_stop");  // the id when no label is given
  ASSERT_TRUE(manifest->panel);
  EXPECT_EQ(manifest->panel->file, "panel.html");
  EXPECT_EQ(manifest->panel->port, 8008);
  EXPECT_EQ(manifest->panel->height, 360);
}

TEST_F(PluginsTest, InvalidActionsAndPanelsAreRejected) {
  write_text(builtin_dir / "exists.ps1", "exit 0\n");
  write_text(builtin_dir / "panel.html", "");
  write_text(builtin_dir / "readme.txt", "");
  const std::vector<std::pair<std::string, std::string>> cases {
    {R"({"id":"p","actions":{}})", "'actions' must be an array"},
    {R"({"id":"p","actions":[1]})", "each action must be an object"},
    {R"({"id":"p","actions":[{"id":"Bad","script":"exists.ps1"}]})", "invalid action id 'Bad'"},
    {R"({"id":"p","actions":[{"id":"a"}]})", "action 'a' needs a script"},
    {R"({"id":"p","actions":[{"id":"a","script":"missing.ps1"}]})", "does not exist"},
    {R"({"id":"p","actions":[{"id":"a","script":"../exists.ps1"}]})", "must be a .ps1 file"},
    {R"({"id":"p","actions":[{"id":"a","script":"exists.ps1"},{"id":"a","script":"exists.ps1"}]})", "duplicate action id 'a'"},
    {R"({"id":"p","panel":"panel.html"})", "'panel' must be an object"},
    {R"({"id":"p","panel":{}})", "must be an .html file"},
    {R"({"id":"p","panel":{"file":"readme.txt"}})", "must be an .html file"},
    {R"({"id":"p","panel":{"file":"../panel.html"}})", "must be an .html file"},
    {R"({"id":"p","panel":{"file":"missing.html"}})", "does not exist"},
    {R"({"id":"p","panel":{"file":"panel.html","port":0}})", "'port' must be an integer from 1 to 65535"},
    {R"({"id":"p","panel":{"file":"panel.html","port":"8008"}})", "'port' must be an integer from 1 to 65535"},
    {R"({"id":"p","panel":{"file":"panel.html","height":50}})", "'height' must be an integer from 100 to 2000"},
  };
  for (const auto &[manifest, expected] : cases) {
    std::string error;
    EXPECT_FALSE(plugins::parse_manifest(json::parse(manifest), builtin_dir, true, error)) << manifest;
    EXPECT_TRUE(error.contains(expected)) << manifest << ": " << error;
  }
}

TEST_F(PluginsTest, ListDescribesActionsAndPanel) {
  write_panel_plugin(builtin_dir, "p", 8008);
  write_plugin(builtin_dir, "plain", full_manifest("plain", 200));
  plugins::load(paths());

  const auto list = plugins::list();
  const auto plugin = find_plugin(list, "p");
  ASSERT_EQ(plugin["actions"].size(), 2);
  EXPECT_EQ(plugin["actions"][0], json({{"id", "test_start"}, {"label", "Start test"}, {"description", "Starts the service"}}));
  EXPECT_EQ(plugin["panel"], json({{"height", 360}, {"proxy", true}}));

  const auto plain = find_plugin(list, "plain");
  EXPECT_TRUE(plain["actions"].empty());
  EXPECT_TRUE(plain["panel"].is_null());
}

TEST_F(PluginsTest, RunActionRunsItsScriptElevatedAndKeepsState) {
  const auto dir = write_panel_plugin(builtin_dir, "p", 0);
  plugins::load(paths());
  std::string error;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::install, error, true), plugins::result_e::ok) << error;
  plugins::wait_for_task();

  ASSERT_EQ(plugins::start_task("p", plugins::action_e::run, error, false, "test_start"), plugins::result_e::ok) << error;
  plugins::wait_for_task();

  ASSERT_EQ(calls.size(), 2);
  EXPECT_EQ(calls[1].command, plugins::script_command(dir / "test-start.ps1"));
  EXPECT_TRUE(calls[1].elevated);
  EXPECT_EQ(calls[1].working_dir, data_dir / "p");
  EXPECT_EQ(calls[1].env.at("SUNSHINE_PLUGIN_OPT_MULTIPLIER"), "2");

  const auto plugin = find_plugin(plugins::list(), "p");
  EXPECT_EQ(plugin["installed"], true);
  EXPECT_EQ(plugin["active"], true);
  EXPECT_EQ(plugin["task"], json({{"action", "run"}, {"status", "succeeded"}, {"exit_code", 0}, {"action_id", "test_start"}}));
  const auto log = plugins::read_log("p");
  ASSERT_TRUE(log);
  EXPECT_TRUE(log->contains("Plugin p: Start test")) << *log;

  runner_exit_code = 3;
  ASSERT_EQ(plugins::start_task("p", plugins::action_e::run, error, false, "test_stop"), plugins::result_e::ok) << error;
  plugins::wait_for_task();
  const auto failed = find_plugin(plugins::list(), "p");
  EXPECT_EQ(failed["task"]["status"], "failed");
  EXPECT_EQ(failed["task"]["exit_code"], 3);
  EXPECT_EQ(failed["installed"], true);  // a failed action changes nothing
  EXPECT_EQ(failed["active"], true);
}

TEST_F(PluginsTest, RunActionRequiresAnInstalledPluginAndAKnownAction) {
  write_panel_plugin(builtin_dir, "p", 0);
  plugins::load(paths());
  std::string error;
  EXPECT_EQ(plugins::start_task("p", plugins::action_e::run, error, false, "test_start"), plugins::result_e::not_installed);
  EXPECT_TRUE(error.contains("must be installed")) << error;
  EXPECT_EQ(plugins::start_task("p", plugins::action_e::run, error, false, "nope"), plugins::result_e::invalid);
  EXPECT_TRUE(error.contains("has no action 'nope'")) << error;
  EXPECT_EQ(plugins::start_task("missing", plugins::action_e::run, error, false, "test_start"), plugins::result_e::not_found);
  EXPECT_TRUE(calls.empty());
}

TEST(PluginsProxyTest, AcceptsOnlyLocalAbsolutePaths) {
  EXPECT_TRUE(plugins::is_valid_proxy_path("/_input"));
  EXPECT_TRUE(plugins::is_valid_proxy_path("/_status?controller=c1&x=%20y"));
  EXPECT_FALSE(plugins::is_valid_proxy_path(""));
  EXPECT_FALSE(plugins::is_valid_proxy_path("_input"));
  EXPECT_FALSE(plugins::is_valid_proxy_path("http://evil.invalid/"));
  EXPECT_FALSE(plugins::is_valid_proxy_path("//evil.invalid/"));
  EXPECT_FALSE(plugins::is_valid_proxy_path("/a b"));
  EXPECT_FALSE(plugins::is_valid_proxy_path("/a\r\nHost: x"));
  EXPECT_FALSE(plugins::is_valid_proxy_path("/a\\b"));
  EXPECT_FALSE(plugins::is_valid_proxy_path("/a#b"));
  EXPECT_FALSE(plugins::is_valid_proxy_path("/" + std::string(512, 'a')));
}

TEST_F(PluginsTest, PanelFileFindsThePanel) {
  const auto dir = write_panel_plugin(builtin_dir, "p", 0);
  write_plugin(builtin_dir, "plain", full_manifest("plain", 200));
  plugins::load(paths());
  EXPECT_EQ(plugins::panel_file("p"), dir / "panel.html");
  EXPECT_FALSE(plugins::panel_file("plain"));
  EXPECT_FALSE(plugins::panel_file("missing"));
}

TEST_F(PluginsTest, ProxyGetForwardsToTheDeclaredPort) {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor {io, {boost::asio::ip::make_address("127.0.0.1"), 0}};
  const auto port = acceptor.local_endpoint().port();
  std::promise<std::string> request_line;
  std::jthread server([&acceptor, &request_line]() {
    boost::asio::ip::tcp::socket socket {acceptor.get_executor()};
    acceptor.accept(socket);
    boost::asio::streambuf buffer;
    boost::asio::read_until(socket, buffer, "\r\n\r\n");
    std::istream in(&buffer);
    std::string line;
    std::getline(in, line);
    request_line.set_value(line);
    const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}";
    boost::asio::write(socket, boost::asio::buffer(reply));
  });

  write_panel_plugin(builtin_dir, "p", port);
  plugins::load(paths());
  plugins::proxy_response_t response;
  std::string error;
  ASSERT_EQ(plugins::proxy_get("p", "/_input?x=1", response, error), plugins::result_e::ok) << error;
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.content_type, "application/json");
  EXPECT_EQ(response.body, R"({"ok":true})");
  EXPECT_TRUE(request_line.get_future().get().starts_with("GET /_input?x=1 HTTP/1.1"));
}

TEST_F(PluginsTest, ProxyGetRejectsBadRequestsAndReportsAMissingService) {
  // A port nothing listens on: bind one, then close it.
  int closed_port = 0;
  {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor {io, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    closed_port = acceptor.local_endpoint().port();
  }
  write_panel_plugin(builtin_dir, "p", closed_port);
  write_panel_plugin(custom_dir, "noport", 0);
  plugins::load(paths());

  plugins::proxy_response_t response;
  std::string error;
  EXPECT_EQ(plugins::proxy_get("missing", "/_input", response, error), plugins::result_e::not_found);
  EXPECT_EQ(plugins::proxy_get("noport", "/_input", response, error), plugins::result_e::invalid);
  EXPECT_TRUE(error.contains("has no panel port")) << error;
  EXPECT_EQ(plugins::proxy_get("p", "http://evil.invalid/", response, error), plugins::result_e::invalid);
  EXPECT_EQ(error, "invalid proxy path");
  EXPECT_EQ(plugins::proxy_get("p", "/_input", response, error), plugins::result_e::unavailable);
  EXPECT_TRUE(error.contains(std::format("nothing answered on local port {}", closed_port))) << error;
}

namespace {
  /**
   * @brief A local HTTP service that accepts one connection and lets the test script what it sends.
   */
  class one_shot_service_t {
  public:
    /**
     * @brief Listen on an ephemeral loopback port and serve one connection with `serve`.
     *
     * @param serve Called with the connected socket once the request head has been read.
     */
    explicit one_shot_service_t(std::function<void(boost::asio::ip::tcp::socket &)> serve):
        acceptor {io, {boost::asio::ip::make_address("127.0.0.1"), 0}},
        thread {[this, serve = std::move(serve)]() {
          boost::asio::ip::tcp::socket socket {acceptor.get_executor()};
          boost::system::error_code ec;
          acceptor.accept(socket, ec);
          if (ec) {
            return;
          }
          boost::asio::streambuf buffer;
          boost::asio::read_until(socket, buffer, "\r\n\r\n", ec);
          if (!ec) {
            serve(socket);
          }
        }} {
    }

    /**
     * @brief The port the service listens on.
     *
     * @return Port number.
     */
    int port() const {
      return acceptor.local_endpoint().port();
    }

  private:
    boost::asio::io_context io;  ///< I/O context of the acceptor.
    boost::asio::ip::tcp::acceptor acceptor;  ///< Listening socket.
    std::jthread thread;  ///< Serves the one connection.
  };

  /**
   * @brief Head of a server-sent event stream that ends when the connection closes.
   */
  constexpr auto event_stream_head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";

  /**
   * @brief A head callback that ignores the head.
   */
  const plugins::proxy_head_t ignore_head = [](int, const std::string &) {
  };
}  // namespace

TEST_F(PluginsTest, ProxyStreamPassesEachPieceOnAsItArrives) {
  std::promise<void> first_seen;
  auto first_seen_future = first_seen.get_future();
  one_shot_service_t service([&first_seen_future](boost::asio::ip::tcp::socket &socket) {
    boost::asio::write(socket, boost::asio::buffer(std::string(event_stream_head) + "data: 1\n\n"));
    // A proxy that collected the body would never pass "data: 1" on, and this would time out.
    if (first_seen_future.wait_for(5s) == std::future_status::ready) {
      boost::asio::write(socket, boost::asio::buffer(std::string("data: 2\n\n")));
    }
  });
  write_panel_plugin(builtin_dir, "p", service.port());
  plugins::load(paths());

  int status = 0;
  std::string content_type;
  std::string received;
  bool signalled = false;
  const plugins::proxy_head_t on_head = [&](int s, const std::string &type) {
    status = s;
    content_type = type;
  };
  const plugins::proxy_data_t on_data = [&](std::string_view data) {
    received.append(data);
    if (!signalled && received.contains("data: 1")) {
      signalled = true;
      first_seen.set_value();
    }
    return true;
  };
  std::string error;
  ASSERT_EQ(plugins::proxy_stream("p", "/_events", on_head, on_data, 10s, error), plugins::result_e::ok) << error;
  EXPECT_EQ(status, 200);
  EXPECT_EQ(content_type, "text/event-stream");
  EXPECT_EQ(received, "data: 1\n\ndata: 2\n\n");
}

TEST_F(PluginsTest, ProxyStreamEndsAtItsDeadline) {
  std::promise<void> release;
  auto released = release.get_future();
  one_shot_service_t service([&released](boost::asio::ip::tcp::socket &socket) {
    boost::asio::write(socket, boost::asio::buffer(std::string(event_stream_head) + "data: 1\n\n"));
    released.wait_for(10s);  // keeps the stream open
  });
  write_panel_plugin(builtin_dir, "p", service.port());
  plugins::load(paths());

  const plugins::proxy_data_t keep_reading = [](std::string_view) {
    return true;
  };
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  const auto result = plugins::proxy_stream("p", "/_events", ignore_head, keep_reading, 1500ms, error);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  release.set_value();
  EXPECT_EQ(result, plugins::result_e::ok) << error;
  EXPECT_GE(elapsed, 1500ms);
  EXPECT_LT(elapsed, 5s);
}

TEST_F(PluginsTest, ProxyStreamStopsWhenTheReaderDoes) {
  std::promise<void> release;
  auto released = release.get_future();
  one_shot_service_t service([&released](boost::asio::ip::tcp::socket &socket) {
    boost::asio::write(socket, boost::asio::buffer(std::string(event_stream_head) + "data: 1\n\n"));
    released.wait_for(10s);
  });
  write_panel_plugin(builtin_dir, "p", service.port());
  plugins::load(paths());

  int pieces = 0;
  const plugins::proxy_data_t stop_reading = [&pieces](std::string_view) {
    ++pieces;
    return false;
  };
  std::string error;
  const auto started = std::chrono::steady_clock::now();
  const auto result = plugins::proxy_stream("p", "/_events", ignore_head, stop_reading, 10s, error);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  release.set_value();
  EXPECT_EQ(result, plugins::result_e::ok) << error;
  EXPECT_EQ(pieces, 1);
  EXPECT_LT(elapsed, 3s);
}

TEST_F(PluginsTest, ProxyStreamEndsAtShutdownUntilTheNextLoad) {
  std::promise<void> release;
  auto released = release.get_future().share();
  one_shot_service_t service([released](boost::asio::ip::tcp::socket &socket) {
    boost::asio::write(socket, boost::asio::buffer(std::string(event_stream_head) + "data: 1\n\n"));
    released.wait_for(10s);
  });
  write_panel_plugin(builtin_dir, "p", service.port());
  plugins::load(paths());

  std::promise<void> streaming;
  bool signalled = false;
  const plugins::proxy_data_t on_data = [&](std::string_view) {
    if (!signalled) {
      signalled = true;
      streaming.set_value();
    }
    return true;
  };
  std::string error;
  auto running = std::async(std::launch::async, [&]() {
    return plugins::proxy_stream("p", "/_events", ignore_head, on_data, 10s, error);
  });
  ASSERT_EQ(streaming.get_future().wait_for(5s), std::future_status::ready);
  const auto stopped_at = std::chrono::steady_clock::now();
  plugins::stop_proxy_streams();
  ASSERT_EQ(running.wait_for(5s), std::future_status::ready);
  EXPECT_LT(std::chrono::steady_clock::now() - stopped_at, 3s);
  EXPECT_EQ(running.get(), plugins::result_e::ok) << error;
  release.set_value();

  // Loading the registry again (as Sunshine does on start) lets streams run again: this one waits on the
  // service's listening socket until its deadline instead of being aborted at once.
  plugins::load(paths());
  const auto restarted_at = std::chrono::steady_clock::now();
  plugins::proxy_stream("p", "/_events", ignore_head, on_data, 1s, error);
  EXPECT_GE(std::chrono::steady_clock::now() - restarted_at, 900ms) << error;
}

TEST_F(PluginsTest, ProxyStreamReportsAResponseWithoutABody) {
  one_shot_service_t service([](boost::asio::ip::tcp::socket &socket) {
    boost::asio::write(socket, boost::asio::buffer(std::string("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")));
  });
  write_panel_plugin(builtin_dir, "p", service.port());
  plugins::load(paths());

  int status = 0;
  int pieces = 0;
  const plugins::proxy_head_t on_head = [&status](int s, const std::string &) {
    status = s;
  };
  const plugins::proxy_data_t on_data = [&pieces](std::string_view) {
    ++pieces;
    return true;
  };
  std::string error;
  EXPECT_EQ(plugins::proxy_stream("p", "/_events", on_head, on_data, 5s, error), plugins::result_e::ok) << error;
  EXPECT_EQ(status, 404);
  EXPECT_EQ(pieces, 0);
}

TEST_F(PluginsTest, ProxyStreamRejectsBadRequestsAndReportsAMissingService) {
  int closed_port = 0;
  {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor {io, {boost::asio::ip::make_address("127.0.0.1"), 0}};
    closed_port = acceptor.local_endpoint().port();
  }
  write_panel_plugin(builtin_dir, "p", closed_port);
  write_panel_plugin(custom_dir, "noport", 0);
  plugins::load(paths());

  bool head_called = false;
  const plugins::proxy_head_t on_head = [&head_called](int, const std::string &) {
    head_called = true;
  };
  const plugins::proxy_data_t on_data = [](std::string_view) {
    return true;
  };
  std::string error;
  EXPECT_EQ(plugins::proxy_stream("missing", "/_events", on_head, on_data, 1s, error), plugins::result_e::not_found);
  EXPECT_EQ(plugins::proxy_stream("noport", "/_events", on_head, on_data, 1s, error), plugins::result_e::invalid);
  EXPECT_EQ(plugins::proxy_stream("p", "//evil.invalid/", on_head, on_data, 1s, error), plugins::result_e::invalid);
  EXPECT_EQ(plugins::proxy_stream("p", "/_events", on_head, on_data, 1s, error), plugins::result_e::unavailable);
  EXPECT_TRUE(error.contains(std::format("nothing answered on local port {}", closed_port))) << error;
  EXPECT_FALSE(head_called);
}

#ifdef _WIN32
/**
 * @brief Fixture that runs plugin scripts with the real PowerShell runner.
 */
class PluginsPowerShellTest: public PluginsTest {
protected:
  void SetUp() override {
    PluginsTest::SetUp();
    plugins::reset_script_runner_for_testing();
  }

  /**
   * @brief Create a plugin whose install script has the given body.
   *
   * @param body PowerShell code.
   */
  void write_install_plugin(const std::string &body) {
    const json manifest = {
      {"id", "ps"},
      {"name", "PowerShell"},
      {"scripts", {{"install", "install.ps1"}}},
      {"options", json::array({{{"id", "greeting"}, {"type", "string"}, {"default", "world"}}})},
    };
    write_text(builtin_dir / "ps" / "plugin.json", manifest.dump());
    write_text(builtin_dir / "ps" / "install.ps1", body);
    plugins::load(paths());
  }
};

TEST_F(PluginsPowerShellTest, RunsScriptAndCapturesOutput) {
  write_install_plugin("Write-Output \"hello $env:SUNSHINE_PLUGIN_OPT_GREETING from $((Get-Location).Path)\"\nexit 0\n");

  std::string error;
  ASSERT_EQ(plugins::start_task("ps", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();

  const auto plugin = find_plugin(plugins::list(), "ps");
  const auto log = plugins::read_log("ps").value_or("");
  EXPECT_EQ(plugin["task"]["status"], "succeeded") << log;
  EXPECT_TRUE(log.contains("hello world from")) << log;
}

TEST_F(PluginsPowerShellTest, ReportsScriptExitCode) {
  write_install_plugin("exit 7\n");

  std::string error;
  ASSERT_EQ(plugins::start_task("ps", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();

  const auto plugin = find_plugin(plugins::list(), "ps");
  EXPECT_EQ(plugin["task"]["status"], "failed");
  EXPECT_EQ(plugin["task"]["exit_code"], 7);
}

TEST_F(PluginsPowerShellTest, FailsWhenScriptCannotStart) {
  write_install_plugin("exit 0\n");
  // A file where the data directory belongs leaves the script without a valid working directory.
  write_text(data_dir / "ps", "");

  std::string error;
  ASSERT_EQ(plugins::start_task("ps", plugins::action_e::install, error), plugins::result_e::ok);
  plugins::wait_for_task();

  const auto plugin = find_plugin(plugins::list(), "ps");
  EXPECT_EQ(plugin["task"]["status"], "failed");
  EXPECT_EQ(plugin["task"]["exit_code"], -1);
}

TEST_F(PluginsPowerShellTest, CancelStopsRunningScript) {
  write_install_plugin("Start-Sleep -Seconds 60\n");

  std::string error;
  ASSERT_EQ(plugins::start_task("ps", plugins::action_e::install, error), plugins::result_e::ok);
  std::this_thread::sleep_for(500ms);

  const auto start = std::chrono::steady_clock::now();
  plugins::cancel_task();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);

  const auto plugin = find_plugin(plugins::list(), "ps");
  EXPECT_EQ(plugin["task"]["status"], "failed");
  EXPECT_EQ(plugin["task"]["exit_code"], -1);
}
#endif


TEST_F(PluginsTest, ParseManifestReadsClientCommands) {
  write_text(builtin_dir / "toggle.ps1", "exit 0\n");
  std::string error;
  auto manifest = plugins::parse_manifest(json::parse(R"({
    "id": "p", "elevated": true, "icon": "material:pause",
    "client_commands": [
      {"id": "toggle", "label": "Suspend", "icon": "material:pause_circle", "top_level": true, "script": "toggle.ps1", "args": ["on", "two words"]},
      {"id": "keys", "steps": [[17, 27]], "close_menu": false, "elevated": false}
    ]
  })"),
                                          builtin_dir, true, error);
  ASSERT_TRUE(manifest) << error;
  EXPECT_EQ(manifest->icon, "material:pause");
  ASSERT_EQ(manifest->client_commands.size(), 2);

  const auto &toggle = manifest->client_commands[0];
  EXPECT_EQ(toggle.label, "Suspend");
  EXPECT_EQ(toggle.icon, "material:pause_circle");
  EXPECT_TRUE(toggle.top_level);
  EXPECT_TRUE(toggle.close_menu);
  EXPECT_TRUE(toggle.elevated);
  EXPECT_EQ(toggle.script, "toggle.ps1");
  EXPECT_EQ(toggle.args, (std::vector<std::string> {"on", "two words"}));

  const auto &keys = manifest->client_commands[1];
  EXPECT_EQ(keys.label, "keys");
  EXPECT_FALSE(keys.top_level);
  EXPECT_FALSE(keys.close_menu);
  EXPECT_FALSE(keys.elevated);
  EXPECT_EQ(keys.steps, (std::vector<std::vector<std::uint16_t>> {{17, 27}}));
}

TEST_F(PluginsTest, InvalidClientCommandsAreRejected) {
  write_text(builtin_dir / "exists.ps1", "exit 0\n");
  const std::vector<std::pair<std::string, std::string>> cases {
    {R"({"id":"p","client_commands":{}})", "'client_commands' must be an array"},
    {R"({"id":"p","client_commands":[{"id":"a","steps":[[27]],"programs":"a.exe"}]})", "'programs' must be a non-empty array"},
    {R"({"id":"p","client_commands":[{"id":"a","steps":[[27]],"programs":[]}]})", "'programs' must be a non-empty array"},
    {R"({"id":"p","client_commands":[{"id":"a","steps":[[27]],"programs":[1]}]})", "'programs' must be a non-empty array"},
    {R"({"id":"p","client_commands":[{"id":"a","steps":[[27]],"programs":[""]}]})", "'programs' must be a non-empty array"},
    {R"({"id":"p","client_commands":[1]})", "each client command must be an object"},
    {R"({"id":"p","client_commands":[{"id":"Bad","script":"exists.ps1"}]})", "invalid client command id 'Bad'"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"exists.ps1"},{"id":"a","script":"exists.ps1"}]})", "duplicate client command id 'a'"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"exists.ps1","top_level":"yes"}]})", "'top_level' must be a boolean"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"exists.ps1","close_menu":1}]})", "'close_menu' must be a boolean"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"exists.ps1","elevated":"no"}]})", "'elevated' must be a boolean"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"exists.ps1","args":"x"}]})", "'args' must be an array of strings"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"exists.ps1","args":[1]}]})", "'args' must be an array of strings"},
    {R"({"id":"p","client_commands":[{"id":"a"}]})", "needs either a script or steps"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"exists.ps1","steps":[[27]]}]})", "needs either a script or steps"},
    {R"({"id":"p","client_commands":[{"id":"a","steps":[[0]]}]})", "key codes"},
    {R"({"id":"p","client_commands":[{"id":"a","script":"missing.ps1"}]})", "does not exist"},
    {R"({"id":"p","client_commands":[{"id":"a","label":5,"script":"exists.ps1"}]})", "'label' must be a string"},
    {R"({"id":"p","icon":5})", "'icon' must be a string"},
  };
  for (const auto &[manifest, expected] : cases) {
    std::string error;
    EXPECT_FALSE(plugins::parse_manifest(json::parse(manifest), builtin_dir, true, error)) << manifest;
    EXPECT_TRUE(error.contains(expected)) << manifest << ": " << error;
  }
}

TEST_F(PluginsTest, CommandsLimitedToProgramsFollowTheForegroundProgram) {
  write_plugin(builtin_dir, "p", {
                                   {"id", "p"},
                                   {"name", "P"},
                                   {"client_commands", json::parse(R"([
                                     {"id": "always", "steps": [[27]]},
                                     {"id": "paint", "steps": [[27]], "programs": ["C:\\Apps\\Photoshop.exe", "gimp.exe"]},
                                     {"id": "top", "top_level": true, "steps": [[27]], "programs": ["gimp.exe"]}
                                   ])")},
                                 });
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  const auto ids = [](const std::vector<client_commands::command_t> &commands) {
    std::vector<std::string> out;
    for (const auto &command : commands) {
      out.push_back(command.id);
    }
    return out;
  };
  using ids_t = std::vector<std::string>;

  client_commands::set_foreground_program_for_testing("GIMP.EXE");
  auto offered = plugins::offered_client_commands({}).at(0);
  EXPECT_EQ(ids(offered.grouped), (ids_t {"plugin:p/always", "plugin:p/paint"}));
  EXPECT_EQ(ids(offered.top_level), (ids_t {"plugin:p/top"}));

  // A path in the manifest matches by its file name
  client_commands::set_foreground_program_for_testing("photoshop.exe");
  offered = plugins::offered_client_commands({}).at(0);
  EXPECT_EQ(ids(offered.grouped), (ids_t {"plugin:p/always", "plugin:p/paint"}));
  EXPECT_TRUE(offered.top_level.empty());

  for (const auto *program : {"notepad.exe", ""}) {
    client_commands::set_foreground_program_for_testing(program);
    offered = plugins::offered_client_commands({}).at(0);
    EXPECT_EQ(ids(offered.grouped), (ids_t {"plugin:p/always"})) << program;
    EXPECT_TRUE(offered.top_level.empty()) << program;
  }

  const auto listed = find_plugin(plugins::list(), "p")["client_commands"];
  EXPECT_EQ(listed[0]["programs"], json::array());
  EXPECT_EQ(listed[2]["programs"], json::array({"gimp.exe"}));
}

TEST_F(PluginsTest, OverridesReplaceTheProgramsACommandIsLimitedTo) {
  write_plugin(builtin_dir, "p", {
                                   {"id", "p"},
                                   {"name", "P"},
                                   {"client_commands", json::parse(R"([
                                     {"id": "limited", "steps": [[27]], "programs": ["gimp.exe"]},
                                     {"id": "open", "steps": [[27]]}
                                   ])")},
                                 });
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  const auto offered_ids = [](const client_commands::plugin_overrides_t &overrides) {
    std::vector<std::string> out;
    for (const auto &command : plugins::offered_client_commands({}, overrides).at(0).grouped) {
      out.push_back(command.id);
    }
    return out;
  };
  using ids_t = std::vector<std::string>;
  using programs_t = std::vector<std::string>;
  const ids_t both {"plugin:p/limited", "plugin:p/open"};
  const ids_t only_open {"plugin:p/open"};
  const ids_t none {};

  client_commands::plugin_override_t replaced;
  replaced.programs = programs_t {"game.exe"};
  client_commands::plugin_override_t any;
  any.programs = programs_t {};
  client_commands::plugin_override_t label_only;
  label_only.label = "Renamed";

  // An override that doesn't set programs keeps the plugin's
  client_commands::set_foreground_program_for_testing("notepad.exe");
  EXPECT_EQ(offered_ids({{"p/limited", label_only}}), only_open);

  // A list replaces the plugin's rather than adding to it
  EXPECT_EQ(offered_ids({{"p/limited", replaced}}), only_open);
  client_commands::set_foreground_program_for_testing("game.exe");
  EXPECT_EQ(offered_ids({{"p/limited", replaced}}), both);
  client_commands::set_foreground_program_for_testing("gimp.exe");
  EXPECT_EQ(offered_ids({{"p/limited", replaced}}), only_open);

  // An empty list removes the limit
  client_commands::set_foreground_program_for_testing("notepad.exe");
  EXPECT_EQ(offered_ids({{"p/limited", any}}), both);

  // A command the plugin didn't limit can be limited
  EXPECT_EQ(offered_ids({{"p/open", replaced}}), none);
  client_commands::set_foreground_program_for_testing("game.exe");
  EXPECT_EQ(offered_ids({{"p/open", replaced}}), only_open);
}

TEST_F(PluginsTest, ALimitedCommandOutOfScopeIsNotOfferedOrRunnable) {
  write_plugin(builtin_dir, "p", {
                                   {"id", "p"},
                                   {"name", "P"},
                                   {"client_commands", json::parse(R"([{"id": "paint", "steps": [[27]], "programs": ["gimp.exe"]}])")},
                                 });
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());
  client_commands::set_file_for_testing(root / "client_commands.json");

  client_commands::set_foreground_program_for_testing("gimp.exe");
  auto offer = client_commands::current_offer();
  ASSERT_EQ(offer.groups.size(), 1);
  EXPECT_EQ(offer.groups[0].kind, "plugin");
  EXPECT_TRUE(client_commands::find(offer, "plugin:p/paint"));

  // A plugin left with nothing to offer gets no empty page
  client_commands::set_foreground_program_for_testing("notepad.exe");
  offer = client_commands::current_offer();
  EXPECT_TRUE(offer.groups.empty());
  EXPECT_FALSE(client_commands::find(offer, "plugin:p/paint"));
}

TEST_F(PluginsTest, ActivePluginsOfferTheirClientCommands) {
  const auto dir = write_plugin(builtin_dir, "nyrna", {
                                                       {"id", "nyrna"},
                                                       {"name", "Nyrna"},
                                                       {"icon", "material:pause"},
                                                       {"elevated", true},
                                                       {"order", 10},
                                                       {"options", json::array({{{"id", "minimize"}, {"type", "boolean"}, {"default", true}}})},
                                                     });
  write_text(dir / "toggle.ps1", "exit 0\n");
  auto manifest = json::parse(read_text(dir / "plugin.json"));
  manifest["client_commands"] = json::parse(R"([
    {"id": "toggle", "label": "Nyrna Suspend", "top_level": true, "script": "toggle.ps1", "args": ["say \"hi\""]},
    {"id": "esc", "label": "Escape", "steps": [[27]]}
  ])");
  write_text(dir / "plugin.json", manifest.dump());
  write_plugin(builtin_dir, "inactive", {{"id", "inactive"}, {"client_commands", json::parse(R"([{"id": "esc", "steps": [[27]]}])")}});
  write_plugin(builtin_dir, "none", {{"id", "none"}});
  write_text(state_file, R"({"plugins":{
    "nyrna":{"installed":true,"active":true},
    "inactive":{"installed":true,"active":false},
    "none":{"installed":true,"active":true}
  }})");
  plugins::load(paths());

  const auto offered = plugins::offered_client_commands({});
  ASSERT_EQ(offered.size(), 1);
  EXPECT_EQ(offered[0].id, "nyrna");
  EXPECT_EQ(offered[0].name, "Nyrna");
  EXPECT_EQ(offered[0].icon, "material:pause");

  ASSERT_EQ(offered[0].top_level.size(), 1);
  const auto &toggle = offered[0].top_level[0];
  EXPECT_EQ(toggle.id, "plugin:nyrna/toggle");
  EXPECT_EQ(toggle.type, client_commands::type_e::script);
  EXPECT_EQ(toggle.cmd, plugins::script_command(dir / "toggle.ps1") + R"( "say \"hi\"")");
  EXPECT_EQ(fs::path(toggle.working_dir), dir);
  EXPECT_TRUE(toggle.elevated);
  EXPECT_EQ(toggle.env.at("SUNSHINE_PLUGIN_ID"), "nyrna");
  EXPECT_EQ(toggle.env.at("SUNSHINE_PLUGIN_OPT_MINIMIZE"), "true");
  EXPECT_EQ(toggle.log_file.filename(), "nyrna.log");

  ASSERT_EQ(offered[0].grouped.size(), 1);
  EXPECT_EQ(offered[0].grouped[0].id, "plugin:nyrna/esc");
  EXPECT_EQ(offered[0].grouped[0].type, client_commands::type_e::keys);
  EXPECT_TRUE(offered[0].grouped[0].env.empty());

  // An app that opts out of the plugin doesn't get them
  EXPECT_TRUE(plugins::offered_client_commands({"nyrna"}).empty());

  const auto listed = find_plugin(plugins::list(), "nyrna");
  EXPECT_EQ(listed["icon"], "material:pause");
  ASSERT_EQ(listed["client_commands"].size(), 2);
  EXPECT_EQ(listed["client_commands"][0]["top_level"], true);
  EXPECT_EQ(listed["client_commands"][0]["script"], "toggle.ps1");
  EXPECT_EQ(listed["client_commands"][1]["steps"], json::parse("[[27]]"));
}

#ifdef _WIN32
TEST(PluginsBundledTest, NyrnaManifestIsValid) {
  // The bundled plugins sit next to the shaders in the source tree
  const fs::path dir = fs::path(SUNSHINE_SHADERS_DIR).parent_path().parent_path() / "plugins" / "nyrna";
  std::string error;
  auto manifest = plugins::parse_manifest(json::parse(read_text(dir / "plugin.json")), dir, true, error);
  ASSERT_TRUE(manifest) << error;
  ASSERT_EQ(manifest->client_commands.size(), 1);
  EXPECT_EQ(manifest->client_commands[0].label, "Suspend");
  EXPECT_TRUE(manifest->client_commands[0].top_level);
  EXPECT_TRUE(manifest->client_commands[0].elevated);
}
#endif

TEST_F(PluginsTest, TheClientsOfferShowsTopLevelCommandsWithTheMenusOwn) {
  write_plugin(builtin_dir, "p", {
                                   {"id", "p"},
                                   {"name", "Plugin"},
                                   {"client_commands", json::parse(R"([{"id": "top", "label": "Top", "top_level": true, "steps": [[27]]}, {"id": "grouped", "label": "Grouped", "steps": [[27]]}])")},
                                 });
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  write_text(root / "client_commands.json", R"({"commands": [{"id": "own", "label": "Own", "type": "keys", "steps": [[27]]}]})");
  client_commands::set_file_for_testing(root / "client_commands.json");
  const auto offer = client_commands::current_offer();
  client_commands::set_file_for_testing({});

  ASSERT_EQ(offer.commands.size(), 2);
  EXPECT_EQ(offer.commands[0].id, "own");
  EXPECT_EQ(offer.commands[1].id, "plugin:p/top");
  ASSERT_EQ(offer.groups.size(), 1);
  EXPECT_EQ(offer.groups[0].kind, "plugin");
  EXPECT_EQ(offer.groups[0].label, "Plugin");
  ASSERT_EQ(offer.groups[0].commands.size(), 1);
  EXPECT_EQ(offer.groups[0].commands[0].id, "plugin:p/grouped");
}


TEST_F(PluginsTest, AStatusFileChangesACommandWhileItExists) {
  write_plugin(builtin_dir, "p", {
                                   {"id", "p"},
                                   {"client_commands", json::parse(R"([{"id": "toggle", "label": "Suspend", "icon": "material:pause", "steps": [[27]], "status_file": "menu.json"}])")},
                                 });
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  auto command = [] {
    return plugins::offered_client_commands({}).at(0).grouped.at(0);
  };
  EXPECT_EQ(command().label, "Suspend");

  write_text(data_dir / "p" / "menu.json", R"j({"label": "Resume (Notepad)", "icon": "material:play_arrow"})j");
  EXPECT_EQ(command().label, "Resume (Notepad)");
  EXPECT_EQ(command().icon, "material:play_arrow");

  // Only what it sets, and only when it's readable
  write_text(data_dir / "p" / "menu.json", R"({"label": ""})");
  EXPECT_EQ(command().label, "Suspend");
  EXPECT_EQ(command().icon, "material:pause");
  write_text(data_dir / "p" / "menu.json", "[]");
  EXPECT_EQ(command().label, "Suspend");
  write_text(data_dir / "p" / "menu.json", "{ broken");
  EXPECT_EQ(command().label, "Suspend");

  fs::remove(data_dir / "p" / "menu.json");
  EXPECT_EQ(command().label, "Suspend");
  EXPECT_EQ(find_plugin(plugins::list(), "p")["client_commands"][0]["status_file"], "menu.json");
}

TEST_F(PluginsTest, StatusFilesMustBeInTheDataDirectory) {
  for (const auto *name : {"../menu.json", "sub/menu.json", "C:menu.json", ".hidden"}) {
    std::string error;
    json manifest = {{"id", "p"}, {"client_commands", json::array({{{"id", "a"}, {"steps", json::parse("[[27]]")}, {"status_file", name}}})}};
    EXPECT_FALSE(plugins::parse_manifest(manifest, builtin_dir, true, error)) << name;
    EXPECT_TRUE(error.contains("'status_file' must be a file name")) << name << ": " << error;
  }
}


TEST_F(PluginsTest, TheUsersLabelAndIconReplaceThePluginsUntilAStatusFileDoes) {
  write_plugin(builtin_dir, "p", {
                                   {"id", "p"},
                                   {"client_commands", json::parse(R"([
                                     {"id": "toggle", "label": "Suspend", "icon": "material:pause", "steps": [[27]], "status_file": "menu.json"},
                                     {"id": "other", "label": "Other", "icon": "material:star", "steps": [[27]]}
                                   ])")},
                                 });
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  const client_commands::plugin_overrides_t overrides {{"p/toggle", {"Pause Game", "material:bedtime"}}, {"p/other", {"", "material:bolt"}}, {"q/toggle", {"Not This", ""}}};
  auto commands = [&] {
    return plugins::offered_client_commands({}, overrides).at(0).grouped;
  };
  EXPECT_EQ(commands()[0].label, "Pause Game");
  EXPECT_EQ(commands()[0].icon, "material:bedtime");

  // Only what's set
  EXPECT_EQ(commands()[1].label, "Other");
  EXPECT_EQ(commands()[1].icon, "material:bolt");

  // The status file shows what the command will do next
  write_text(data_dir / "p" / "menu.json", R"({"label": "Resume", "icon": "material:play_arrow"})");
  EXPECT_EQ(commands()[0].label, "Resume");
  EXPECT_EQ(commands()[0].icon, "material:play_arrow");
  fs::remove(data_dir / "p" / "menu.json");

  // Without any, the plugin's own
  EXPECT_EQ(plugins::offered_client_commands({}).at(0).grouped[0].label, "Suspend");
}

TEST_F(PluginsTest, TheClientsOfferUsesTheSavedOverrides) {
  write_plugin(builtin_dir, "p", {{"id", "p"}, {"client_commands", json::parse(R"([{"id": "top", "label": "Top", "top_level": true, "steps": [[27]]}])")}});
  write_text(state_file, R"({"plugins":{"p":{"installed":true,"active":true}}})");
  plugins::load(paths());

  write_text(root / "client_commands.json", R"({"plugin-commands": {"p/top": {"label": "Renamed"}}})");
  client_commands::set_file_for_testing(root / "client_commands.json");
  const auto offer = client_commands::current_offer();
  client_commands::set_file_for_testing({});

  ASSERT_EQ(offer.commands.size(), 1);
  EXPECT_EQ(offer.commands[0].id, "plugin:p/top");
  EXPECT_EQ(offer.commands[0].label, "Renamed");
}
