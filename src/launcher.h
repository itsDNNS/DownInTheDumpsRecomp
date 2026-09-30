#pragma once
// Settings window of blub: choose the game folder, display/sound/control options, start the game.
#include "host/settings.h"

namespace blub {

enum class LauncherResult { Quit, Play, Setup };

// shows the launcher (SDL must not be initialized); saves the settings when the game is started
LauncherResult run_launcher(Settings &s, const std::string &settings_path, const std::string &message);

}  // namespace blub
