#pragma once
// User settings of the port (blub.ini in the per-user data directory), shared by the launcher and
// the command line.
#include <string>
#include <vector>

namespace blub {

struct Settings {
    // game
    std::string game_dir;                 // folder with the ISO images or the disc contents
    std::string save_dir;                 // DID.CFG and save games; empty = default
    // display
    bool fullscreen = false;
    int scale = 2;                        // window size in multiples of 640x480
    bool smooth = false;                  // linear filtering when scaling
    bool xbrz = false;                    // xBRZ upscaling (high quality, instead of the above)
    bool vsync = true;
    bool dualpage = true;                 // page flipping (/DUALPAGE) instead of drawing on screen
    // sound
    bool sound = true;
    int volume = 100;                     // master volume in percent
    // controls
    bool esc_skips = true;                // Esc behaves like the space bar (skip videos)
    bool gamepad = true;                  // game controllers move the mouse pointer
    // launcher
    bool show_launcher = true;            // open the launcher when blub is started without arguments
    std::string language = "auto";        // launcher language: auto (system language), de, en

    static std::string program_dir();     // folder of the executable (with a trailing slash)
    static bool portable();               // blub.ini or ISOs/ next to the program
    static std::string default_path();    // blub.ini (portable: next to the program, else pref path)
    static std::string default_save_dir();
    static std::string to_ini_path(const std::string &p);     // portable: relative to the program
    static std::string from_ini_path(const std::string &p);
    bool load(const std::string &path);
    void apply_language() const;          // sets the language of the launcher and of the messages
    bool save(const std::string &path) const;
    std::string effective_save_dir() const { return save_dir.empty() ? default_save_dir() : save_dir; }
};

// what a game data folder contains
struct GameCheck {
    std::vector<std::string> discs;       // found discs, e.g. "CD1 (Disc 1 of 3.iso)"
    std::string problems;                 // images that could not be used
    bool exe = false;                     // DID.EXE present
    bool original = false;                // the DID.EXE the port was recompiled from (sha1)
    std::string exe_sha1;
    size_t exe_size = 0;
    std::string language;                 // language of the game data: de, en, fr or empty
    bool itoon = false, setup = false;
    std::string chapters;                 // e.g. "1 2 3 4 6"
};
GameCheck check_game_dir(const std::string &dir);
class GameData;
std::string detect_game_language(GameData &g);   // de, en, fr or empty

}  // namespace blub
