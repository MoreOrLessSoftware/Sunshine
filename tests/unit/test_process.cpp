/**
 * @file tests/unit/test_process.cpp
 * @brief Test src/process.* functions.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>

// lib includes
#include <boost/algorithm/string/trim.hpp>

// local includes
#include <src/config.h>
#include <src/process.h>

namespace fs = std::filesystem;

class ProcessPNGTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    // Create test directory
    test_dir = fs::temp_directory_path() / "sunshine_process_png_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
    BaseTest::TearDown();
  }

  // Helper function to create a file with specific content
  void createTestFile(const fs::path &path, const std::vector<unsigned char> &content) const {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(content.data()), content.size());
    file.close();
  }

  fs::path test_dir;
};

// Tests for check_valid_png function
TEST_F(ProcessPNGTest, CheckValidPNG_ValidSignature) {
  // Valid PNG signature
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,  // PNG signature
    // Add some dummy data to make it more realistic
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  EXPECT_TRUE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_WrongSignature) {
  // Invalid PNG signature (wrong magic bytes)
  const std::vector<unsigned char> invalid_png_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_TooShort) {
  // File too short (less than 8 bytes)
  const std::vector<unsigned char> short_data = {
    0x89,
    0x50,
    0x4E,
    0x47
  };

  const fs::path test_file = test_dir / "short.png";
  createTestFile(test_file, short_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_EmptyFile) {
  // Empty file
  const std::vector<unsigned char> empty_data = {};

  const fs::path test_file = test_dir / "empty.png";
  createTestFile(test_file, empty_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_NonExistentFile) {
  // File doesn't exist
  const fs::path test_file = test_dir / "nonexistent.png";

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_RealFile) {
  // Test with the actual sunshine.png from the project root

  // Only run this test if the file exists
  if (const fs::path sunshine_png = fs::path(SUNSHINE_SOURCE_DIR) / "sunshine.png"; fs::exists(sunshine_png)) {
    EXPECT_TRUE(proc::check_valid_png(sunshine_png));
  } else {
    GTEST_SKIP() << "sunshine.png not found in project root";
  }
}

TEST_F(ProcessPNGTest, CheckValidPNG_JPEGFile) {
  // JPEG signature (not PNG)
  const std::vector<unsigned char> jpeg_data = {
    0xFF,
    0xD8,
    0xFF,
    0xE0,
    0x00,
    0x10,
    0x4A,
    0x46
  };

  const fs::path test_file = test_dir / "fake.png";
  createTestFile(test_file, jpeg_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_PartialSignature) {
  // Partial PNG signature (first 4 bytes correct, rest wrong)
  const std::vector<unsigned char> partial_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "partial.png";
  createTestFile(test_file, partial_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

// Tests for validate_app_image_path function
TEST_F(ProcessPNGTest, ValidateAppImagePath_EmptyPath) {
  // Empty path should return default
  const std::string result = proc::validate_app_image_path("");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonPNGExtension) {
  // Non-PNG extension should return default
  const std::string result = proc::validate_app_image_path("image.jpg");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_CaseInsensitiveExtension) {
  // Test that .PNG (uppercase) is recognized
  // Create a valid PNG file
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "test.PNG";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  // Should accept uppercase .PNG extension
  EXPECT_NE(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonExistentFile) {
  // Non-existent PNG file should return default
  const std::string result = proc::validate_app_image_path("/nonexistent/path/image.png");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_InvalidPNGSignature) {
  // File with .png extension but invalid signature should return default
  const std::vector<unsigned char> invalid_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_ValidPNG) {
  // Valid PNG file should return the path
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, test_file.string());
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_OldSteamDefault) {
  // Test the special case for old steam image path
  const std::string result = proc::validate_app_image_path("./assets/steam.png");
  EXPECT_EQ(result, SUNSHINE_ASSETS_DIR "/steam.png");
}

namespace {
#ifdef _WIN32
  /**
   * @brief Build a command that prints a line to the app's output.
   *
   * @param text The line to print.
   * @return The command line.
   */
  std::string echo_cmd(std::string_view text) {
    return std::format("cmd /c echo {}", text);
  }

  constexpr auto FAIL_CMD = "cmd /c exit 1";  ///< A command that exits with code 1.
  constexpr auto ECHO_CLIENT_CMD = "cmd /c echo client=%SUNSHINE_CLIENT_NAME%";  ///< Prints the client name variable.
#else
  /**
   * @brief Build a command that prints a line to the app's output.
   *
   * @param text The line to print.
   * @return The command line.
   */
  std::string echo_cmd(std::string_view text) {
    return std::format("sh -c \"echo {}\"", text);
  }

  constexpr auto FAIL_CMD = "sh -c \"exit 1\"";  ///< A command that exits with code 1.
  constexpr auto ECHO_CLIENT_CMD = "sh -c \"echo client=$SUNSHINE_CLIENT_NAME\"";  ///< Prints the client name variable.
#endif

  /**
   * @brief Build a connect prep command.
   *
   * @param do_cmd Command run when a client resumes.
   * @param undo_cmd Command run when the last client leaves.
   * @return The prep command.
   */
  proc::cmd_t connect_cmd(std::string do_cmd, std::string undo_cmd) {
    return proc::cmd_t {std::move(do_cmd), std::move(undo_cmd), false, proc::cmd_t::type_e::connect};
  }

  /**
   * @brief Build a launch session for a named client.
   *
   * @param client_name The client's name.
   * @return The launch session.
   */
  std::shared_ptr<rtsp_stream::launch_session_t> make_session(const std::string &client_name) {
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->client_name = client_name;
    session->width = 1920;
    session->height = 1080;
    session->fps = 60;
    return session;
  }
}  // namespace

/**
 * @brief Runs a desktop app whose prep commands print to an output file.
 */
class ProcessConnectCmdTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    output_file = fs::temp_directory_path() / "sunshine_process_connect_test.log";  // NOSONAR(cpp:S5443): safe for tests
    fs::remove(output_file);
  }

  void TearDown() override {
    if (process) {
      process->terminate();
    }
    fs::remove(output_file);
    BaseTest::TearDown();
  }

  /**
   * @brief Launch a desktop app with the given prep commands.
   *
   * @param prep_cmds Commands run on app start/stop.
   * @param connect_cmds Commands run on client resume/leave.
   */
  void launch(std::vector<proc::cmd_t> prep_cmds, std::vector<proc::cmd_t> connect_cmds) {
    proc::ctx_t ctx {};
    ctx.name = "Desktop";
    ctx.id = "1";
    ctx.output = output_file.string();
    ctx.prep_cmds = std::move(prep_cmds);
    ctx.connect_cmds = std::move(connect_cmds);
    ctx.exit_timeout = std::chrono::seconds {1};

    std::vector<proc::ctx_t> apps;
    apps.emplace_back(std::move(ctx));
    process = std::make_unique<proc::proc_t>(boost::this_process::environment(), std::move(apps));
    ASSERT_EQ(0, process->execute(1, make_session("launcher")));
  }

  /**
   * @brief Read what the commands printed since the last call.
   *
   * The file stays open for the app's commands, so it is read from where the last call stopped
   * rather than truncated.
   *
   * @return One entry per printed line.
   */
  std::vector<std::string> take_output() {
    std::vector<std::string> lines;
    std::ifstream file(output_file, std::ios::binary);
    file.seekg(read_offset);
    for (std::string line; std::getline(file, line);) {
      boost::trim_right(line);
      if (!line.empty()) {
        lines.push_back(line);
      }
    }
    file.clear();
    file.seekg(0, std::ios::end);
    if (const auto end = file.tellg(); end >= 0) {
      read_offset = end;
    }
    return lines;
  }

  fs::path output_file;  ///< Where the app's commands print.
  std::streamoff read_offset {};  ///< How much of the output take_output() has returned.
  std::unique_ptr<proc::proc_t> process;  ///< The process manager under test.
};

TEST_F(ProcessConnectCmdTest, ConnectCommandsRunOnlyOnResumeAndLeave) {
  launch(
    {proc::cmd_t {echo_cmd("app-do"), echo_cmd("app-undo"), false}},
    {connect_cmd(echo_cmd("connect-do-1"), echo_cmd("connect-undo-1")), connect_cmd(echo_cmd("connect-do-2"), echo_cmd("connect-undo-2"))}
  );
  EXPECT_EQ(take_output(), (std::vector<std::string> {"app-do"}));

  process->disconnect();
  EXPECT_EQ(take_output(), (std::vector<std::string> {"connect-undo-2", "connect-undo-1"}));

  // No client is streaming any more, so another leave has nothing to undo
  process->disconnect();
  EXPECT_TRUE(take_output().empty());

  process->connect(*make_session("resumer"));
  EXPECT_EQ(take_output(), (std::vector<std::string> {"connect-do-1", "connect-do-2"}));

  // The Do commands are already in effect
  process->connect(*make_session("resumer"));
  EXPECT_TRUE(take_output().empty());

  process->terminate();
  EXPECT_EQ(take_output(), (std::vector<std::string> {"app-undo"}));
}

TEST_F(ProcessConnectCmdTest, LeaveAfterQuitRunsNoUndo) {
  launch({}, {connect_cmd(echo_cmd("connect-do"), echo_cmd("connect-undo"))});

  process->drop_connection();
  process->disconnect();
  EXPECT_TRUE(take_output().empty());
}

TEST_F(ProcessConnectCmdTest, FailedConnectSkipsTheRestAndUndoesWhatRan) {
  launch(
    {},
    {connect_cmd(echo_cmd("connect-do-1"), echo_cmd("connect-undo-1")),
     connect_cmd(FAIL_CMD, echo_cmd("connect-undo-2")),
     connect_cmd(echo_cmd("connect-do-3"), echo_cmd("connect-undo-3"))}
  );
  process->disconnect();
  take_output();

  process->connect(*make_session("resumer"));
  EXPECT_EQ(take_output(), (std::vector<std::string> {"connect-do-1"}));

  process->disconnect();
  EXPECT_EQ(take_output(), (std::vector<std::string> {"connect-undo-1"}));
}

TEST_F(ProcessConnectCmdTest, EmptyCommandsAreSkipped) {
  launch({}, {connect_cmd("", echo_cmd("connect-undo")), connect_cmd(echo_cmd("connect-do"), "")});

  process->disconnect();
  EXPECT_EQ(take_output(), (std::vector<std::string> {"connect-undo"}));

  process->connect(*make_session("resumer"));
  EXPECT_EQ(take_output(), (std::vector<std::string> {"connect-do"}));
}

TEST_F(ProcessConnectCmdTest, ResumeSeesTheResumingClient) {
  launch({}, {connect_cmd(ECHO_CLIENT_CMD, ECHO_CLIENT_CMD)});

  process->disconnect();
  EXPECT_EQ(take_output(), (std::vector<std::string> {"client=launcher"}));

  process->connect(*make_session("resumer"));
  EXPECT_EQ(take_output(), (std::vector<std::string> {"client=resumer"}));
}

TEST_F(ProcessConnectCmdTest, CommandThatCannotStartCountsAsFailed) {
  launch({}, {connect_cmd("sunshine-test-command-that-does-not-exist", ""), connect_cmd(echo_cmd("connect-do-2"), "")});
  process->disconnect();

  process->connect(*make_session("resumer"));
  EXPECT_TRUE(take_output().empty());
}

TEST(PrepCmdTypeTest, ParsesType) {
  EXPECT_EQ(proc::cmd_t::type_e::connect, proc::cmd_t::parse_type("connect"));
  EXPECT_EQ(proc::cmd_t::type_e::app, proc::cmd_t::parse_type("app"));
  EXPECT_EQ(proc::cmd_t::type_e::app, proc::cmd_t::parse_type(""));
  EXPECT_EQ(proc::cmd_t::type_e::app, proc::cmd_t::parse_type("unknown"));
}

/**
 * @brief Applies configuration text and restores the global configuration afterwards.
 */
class GlobalPrepCmdConfigTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    // Applying configuration needs an apps file to exist
    config::stream.file_apps = SUNSHINE_SOURCE_DIR "/tests/unit/test_process.cpp";
    config::sunshine.prep_cmds.clear();
  }

  void TearDown() override {
    config::video = original_video;
    config::audio = original_audio;
    config::stream = original_stream;
    config::nvhttp = original_nvhttp;
    config::input = original_input;
    config::sunshine = original_sunshine;
    config::modified_config_settings = original_modified_config_settings;
    BaseTest::TearDown();
  }

  config::video_t original_video {config::video};  ///< Video configuration restored after the test.
  config::audio_t original_audio {config::audio};  ///< Audio configuration restored after the test.
  config::stream_t original_stream {config::stream};  ///< Stream configuration restored after the test.
  config::nvhttp_t original_nvhttp {config::nvhttp};  ///< HTTP configuration restored after the test.
  config::input_t original_input {config::input};  ///< Input configuration restored after the test.
  config::sunshine_t original_sunshine {config::sunshine};  ///< Core configuration restored after the test.
  decltype(config::modified_config_settings) original_modified_config_settings {config::modified_config_settings};  ///< Modified settings restored after the test.
};

TEST_F(GlobalPrepCmdConfigTest, ReadsType) {
  config::apply_config_for_test(R"(global_prep_cmd = [{"do":"a","undo":"b"},{"do":"c","undo":"d","elevated":true,"type":"connect"}])");

  const auto &parsed = config::sunshine.prep_cmds;
  ASSERT_EQ(2U, parsed.size());
  EXPECT_EQ(proc::cmd_t::type_e::app, parsed[0].type);
  EXPECT_EQ("c", parsed[1].do_cmd);
  EXPECT_TRUE(parsed[1].elevated);
  EXPECT_EQ(proc::cmd_t::type_e::connect, parsed[1].type);
}

TEST(PrepCmdTypeTest, ParseSplitsAppAndConnectCommands) {
  const auto saved = config::sunshine.prep_cmds;
  config::sunshine.prep_cmds.clear();
  config::sunshine.prep_cmds.emplace_back("global-app-do", "global-app-undo", false);
  config::sunshine.prep_cmds.emplace_back("global-connect-do", "global-connect-undo", true, proc::cmd_t::type_e::connect);

  const auto apps_file = fs::temp_directory_path() / "sunshine_process_prep_type_apps.json";  // NOSONAR(cpp:S5443): safe for tests
  {
    std::ofstream file(apps_file);
    file << R"({
      "env": {},
      "apps": [
        {
          "name": "With global",
          "prep-cmd": [
            {"do": "app-do", "undo": "app-undo"},
            {"do": "connect-do", "undo": "connect-undo", "type": "connect", "elevated": true}
          ]
        },
        {
          "name": "Without global",
          "exclude-global-prep-cmd": true,
          "prep-cmd": [{"do": "connect-do", "type": "connect"}]
        }
      ]
    })";
  }

  auto parsed = proc::parse(apps_file.string());
  fs::remove(apps_file);
  config::sunshine.prep_cmds = saved;

  ASSERT_TRUE(parsed);
  auto &apps = parsed->get_apps();
  ASSERT_EQ(2U, apps.size());

  ASSERT_EQ(2U, apps[0].prep_cmds.size());
  EXPECT_EQ("global-app-do", apps[0].prep_cmds[0].do_cmd);
  EXPECT_EQ("app-do", apps[0].prep_cmds[1].do_cmd);
  ASSERT_EQ(2U, apps[0].connect_cmds.size());
  EXPECT_EQ("global-connect-do", apps[0].connect_cmds[0].do_cmd);
  EXPECT_TRUE(apps[0].connect_cmds[0].elevated);
  EXPECT_EQ("connect-do", apps[0].connect_cmds[1].do_cmd);
  EXPECT_EQ("connect-undo", apps[0].connect_cmds[1].undo_cmd);
  EXPECT_TRUE(apps[0].connect_cmds[1].elevated);

  EXPECT_TRUE(apps[1].prep_cmds.empty());
  ASSERT_EQ(1U, apps[1].connect_cmds.size());
  EXPECT_EQ("", apps[1].connect_cmds[0].undo_cmd);
  EXPECT_EQ(proc::cmd_t::type_e::connect, apps[1].connect_cmds[0].type);
}
