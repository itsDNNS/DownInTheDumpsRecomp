#pragma once
// Settings window of blub: choose the game folder, display/sound/control options, start the game.
#include "host/settings.h"

namespace blub {

enum class LauncherResult { Quit, Play, Setup };

// what the settings window shows besides the settings
struct LauncherInfo {
    std::string message;                  // e.g. why the game could not start
    std::string report;                   // error report of the last game (empty: it ended normally)
    std::string log_path;                 // where the program's output goes (empty: the console)
};

// shows the launcher (SDL must not be initialized); saves the settings when the game is started
LauncherResult run_launcher(Settings &s, const std::string &settings_path, const LauncherInfo &info);

}  // namespace blub
