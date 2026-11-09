/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  /**
   * @def __kernel_entry
   * @brief Macro for kernel entry.
   */
  #define __kernel_entry
#endif

// standard includes
#include <optional>
#include <unordered_map>

// lib includes
#include <boost/process/v1.hpp>

// local includes
#include "client_commands.h"
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

/**
 * @def DEFAULT_APP_IMAGE_PATH
 * @brief Macro for DEFAULT APP IMAGE PATH.
 */
#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  /**
   * @brief Boost.Process pipe stream used for child-process I/O.
   */
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

  /**
   * @brief Parsed command arguments used when launching a child process.
   */
  typedef config::prep_cmd_t cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;  ///< Prep cmds run when the app starts and stops.
    std::vector<cmd_t> connect_cmds;  ///< Prep cmds run when a client resumes the running app and when the last client leaves it running.

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string name;  ///< Human-readable name for this item.
    std::string cmd;  ///< Command line used to launch the application.
    std::string working_dir;  ///< Working dir.
    std::string output;  ///< Captured output from the launched process.
    std::string image_path;  ///< Image path.
    std::string id;  ///< Stable identifier for the configured application.
    std::string display_plugin;  ///< Name of an active plugin that sets up the display for this app; empty when Sunshine's display device configuration applies.
    std::vector<client_commands::command_t> client_commands;  ///< Commands a client can run from its overlay menu while this app is streamed.
    std::vector<std::string> exclude_plugins;  ///< IDs of plugins this app opted out of.
    bool elevated;  ///< Whether the process should be launched elevated.
    bool auto_detach;  ///< Whether the process should detach automatically.
    bool wait_all;  ///< Whether Sunshine waits for all child processes.
    std::chrono::seconds exit_timeout;  ///< Exit timeout.
  };

  /**
   * @brief Tracks launched child processes and terminates them during shutdown.
   */
  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    /**
     * @brief Construct a process manager.
     *
     * @param env Environment used when launching processes.
     * @param apps Application launch contexts.
     */
    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _app_id(0),
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    /**
     * @brief Launch the configured application process.
     *
     * @param app_id App ID.
     * @param launch_session Launch session.
     * @return Process exit code or launch error status.
     */
    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @brief A client is resuming the running app with no other client streaming it.
     *
     * Points the client environment variables at the new client and runs the Do command of each
     * connect prep command. A failing Do command stops the rest but does not stop the resume.
     * Does nothing while the connect commands are already in effect.
     *
     * @param launch_session The resuming client's launch session.
     */
    void connect(const rtsp_stream::launch_session_t &launch_session);

    /**
     * @brief The last client left and the app keeps running.
     *
     * Runs the Undo command of each connect prep command in effect, in reverse order.
     * The commands are in effect from the app's launch, or from the Do commands that succeeded
     * on the last connect().
     */
    void disconnect();

    /**
     * @brief The app is about to be quit: let its clients leave without running the connect Undo commands.
     */
    void drop_connection();

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    /**
     * @brief Return the configured applications.
     *
     * @return Immutable application list owned by the process manager.
     */
    const std::vector<ctx_t> &get_apps() const;
    /**
     * @brief Return the configured applications.
     *
     * @return Mutable application list owned by the process manager.
     */
    std::vector<ctx_t> &get_apps();
    /**
     * @brief Get app image.
     *
     * @param app_id App ID.
     * @return Validated image path for the requested application.
     */
    std::string get_app_image(int app_id);

    /**
     * @brief Find the plugin that sets up the display for an application.
     *
     * @param app_id Application ID.
     * @return Name of the plugin, or an empty string when Sunshine's display device configuration applies
     *         (also for unknown application IDs).
     */
    std::string get_display_plugin(int app_id) const;
    /**
     * @brief Get last run app name.
     *
     * @return Name of the most recently launched application.
     */
    std::string get_last_run_app_name();
    /**
     * @brief Terminate the launched application process.
     */
    void terminate();

  private:
    int _app_id;

    boost::process::v1::environment _env;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;

    // How many of _app.connect_cmds, from the front, have their Do in effect; their Undo runs on disconnect()
    std::size_t _connect_cmds_in_effect {};

    /**
     * @brief Point the client environment variables at a launch session's client.
     *
     * @param launch_session The client's launch session.
     */
    void set_client_env(const rtsp_stream::launch_session_t &launch_session);

    /**
     * @brief Run one side of a prep command and wait for it to exit.
     *
     * @param prep_cmd The prep command, whose elevation and extra variables apply.
     * @param cmd The command line to run: the Do or Undo command of `prep_cmd`.
     * @param label What kind of command it is, for the log.
     * @return `true` when it ran and exited with code 0.
     */
    bool run_prep(const cmd_t &prep_cmd, const std::string &cmd, std::string_view label);
  };

  /**
   * @brief Build the environment for a preparation command.
   *
   * @param base Environment shared by every command of the application.
   * @param cmd Preparation command that may add its own variables.
   * @return `base` with the command's variables added.
   */
  boost::process::v1::environment prep_cmd_env(const boost::process::v1::environment &base, const cmd_t &cmd);

  /**
   * @brief Determine the working directory to use for a command.
   *
   * @param cmd Command line to inspect.
   * @param env Environment variables used to search PATH.
   * @return Directory used to launch the command, falling back to PATH lookup when needed.
   */
  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env);

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   *
   * @param app_name App name.
   * @param app_image_path App image path.
   * @param index Zero-based index of the item being addressed.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  bool check_valid_png(const std::filesystem::path &path);
  /**
   * @brief Validate app image path.
   *
   * @param app_image_path Candidate image path from the application configuration.
   * @return Existing PNG path, or the default application image when validation fails.
   */
  std::string validate_app_image_path(std::string app_image_path);
  /**
   * @brief Refresh cached platform state from the operating system.
   *
   * @param file_name File name.
   */
  void refresh(const std::string &file_name);
  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   *
   * @param file_name File name.
   * @return Parsed value or parse status.
   */
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc
