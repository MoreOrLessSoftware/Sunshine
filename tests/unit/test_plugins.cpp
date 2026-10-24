/**
 * @file tests/unit/test_plugins.cpp
 * @brief Test src/plugins.* and the plugin integration in src/process.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

// lib includes
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
      {"scripts", {{"install", "install.ps1"}, {"uninstall", "uninstall.ps1"}, {"do", "do.ps1"}, {"undo", "undo.ps1"}}},
      {"options", json::array({
                    {{"id", "enabled"}, {"type", "boolean"}, {"label", "Enabled"}, {"default", true}},
                    {{"id", "multiplier"}, {"type", "number"}, {"label", "Multiplier"}, {"description", "Scale"}, {"default", 2}, {"min", 1}, {"max", 8}, {"step", 1}},
                    {{"id", "name"}, {"type", "string"}, {"default", "Global"}},
                    {{"id", "mode"}, {"type", "choice"}, {"default", "off"}, {"choices", json::array({"off", {{"value", "on"}, {"label", "On"}}})}},
                  })},
    };
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
  EXPECT_FALSE(manifest->builtin);
  EXPECT_EQ(manifest->scripts.install, "install.ps1");
  EXPECT_EQ(manifest->scripts.uninstall, "uninstall.ps1");
  EXPECT_EQ(manifest->scripts.do_script, "do.ps1");
  EXPECT_EQ(manifest->scripts.undo_script, "undo.ps1");

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
