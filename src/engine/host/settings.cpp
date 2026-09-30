#include "host/settings.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "data/exe_symbols.h"
#include "data/gamefs.h"
#include "util/bytes.h"
#include "util/i18n.h"
#include "util/sha1.h"

namespace fs = std::filesystem;

namespace blub {

namespace {

std::string pref_dir() {
    char *p = SDL_GetPrefPath("blub", "Down in the Dumps");
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(uint8_t(s[i]))) i++;
    return s.substr(i);
}

bool to_bool(const std::string &v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

}  // namespace

std::string Settings::program_dir() {
    char *p = SDL_GetBasePath();
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}

// portable installation: blub.ini or an ISOs folder next to the program keeps everything there
bool Settings::portable() {
    std::error_code ec;
    const fs::path base = fs::u8path(program_dir());
    return fs::exists(base / "blub.ini", ec) || fs::is_directory(base / "ISOs", ec);
}

// portable installation: paths inside the program folder are stored relative to it, so that the
// folder can be moved or copied to another computer
std::string Settings::to_ini_path(const std::string &p) {
    if (p.empty() || !portable()) return p;
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(fs::u8path(program_dir()), ec);
    const fs::path abs = fs::weakly_canonical(fs::u8path(p), ec);
    const std::string rel = abs.lexically_relative(base).generic_u8string();
    if (rel.empty() || rel.rfind("..", 0) == 0) return p;
    return rel;
}

std::string Settings::from_ini_path(const std::string &p) {
    if (p.empty() || fs::u8path(p).is_absolute()) return p;
    return (fs::u8path(program_dir()) / fs::u8path(p)).lexically_normal().u8string();
}

std::string Settings::default_path() { return (portable() ? program_dir() : pref_dir()) + "blub.ini"; }

std::string Settings::default_save_dir() {
    return (portable() ? program_dir() + "Saves" : pref_dir() + "save");
}

void Settings::apply_language() const {
    set_language(language == "de" ? Lang::German : language == "en" ? Lang::English : detect_language());
}

bool Settings::load(const std::string &path) {
    std::ifstream in(fs::u8path(path));
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "game_dir") game_dir = from_ini_path(v);
        else if (k == "save_dir") save_dir = from_ini_path(v);
        else if (k == "fullscreen") fullscreen = to_bool(v);
        else if (k == "scale") scale = std::clamp(std::atoi(v.c_str()), 1, 6);
        else if (k == "smooth") smooth = to_bool(v);
        else if (k == "vsync") vsync = to_bool(v);
        else if (k == "dualpage") dualpage = to_bool(v);
        else if (k == "sound") sound = to_bool(v);
        else if (k == "volume") volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "esc_skips") esc_skips = to_bool(v);
        else if (k == "show_launcher") show_launcher = to_bool(v);
        else if (k == "language") language = v;
    }
    return true;
}

bool Settings::save(const std::string &path) const {
    std::ofstream out(fs::u8path(path));
    if (!out) return false;
    auto b = [](bool v) { return v ? "1" : "0"; };
    out << "# Down in the Dumps (blub) - settings, written by the launcher\n"
        << "[game]\n"
        << "game_dir = " << to_ini_path(game_dir) << "\n"
        << "save_dir = " << to_ini_path(save_dir) << "\n"
        << "[display]\n"
        << "fullscreen = " << b(fullscreen) << "\n"
        << "scale = " << scale << "\n"
        << "smooth = " << b(smooth) << "\n"
        << "vsync = " << b(vsync) << "\n"
        << "dualpage = " << b(dualpage) << "\n"
        << "[sound]\n"
        << "sound = " << b(sound) << "\n"
        << "volume = " << volume << "\n"
        << "[controls]\n"
        << "esc_skips = " << b(esc_skips) << "\n"
        << "[launcher]\n"
        << "show_launcher = " << b(show_launcher) << "\n"
        << "language = " << language << "\n";
    return bool(out);
}

// language of the game data from typical words in the texts of the main menu (ITOON.SPD, plain text)
std::string detect_game_language(GameData &g) {
    const Bytes spd = g.read_all("\\ITOON\\ITOON.SPD");
    const std::string t(spd.begin(), spd.end());
    auto count = [&](std::initializer_list<const char *> words) {
        int n = 0;
        for (const char *w : words)
            for (size_t p = t.find(w); p != std::string::npos; p = t.find(w, p + 1)) n++;
        return n;
    };
    const int de = count({" der ", " die ", " und ", "Klicke", " das ", " mit "});
    const int en = count({" the ", " and ", "Click", " to ", " of ", " with "});
    const int fr = count({" le ", " la ", " et ", "Cliquez", " les ", " des "});
    if (de == 0 && en == 0 && fr == 0) return "";
    if (de >= en && de >= fr) return "de";
    return en >= fr ? "en" : "fr";
}

GameCheck check_game_dir(const std::string &dir) {
    GameCheck r;
    if (dir.empty()) return r;
    GameData g;
    g.open(dir, &r.problems);
    for (auto &d : g.discs()) r.discs.push_back(d->description());
    Bytes exe = g.read_all("\\DID.EXE");
    if (exe.empty()) return r;
    r.exe = true;
    r.exe_sha1 = sha1_hex(exe.data(), exe.size());
    r.exe_size = exe.size();
    // the recompiled code (port/src/engine/recomp) was generated from this DID.EXE; other builds of the
    // program would need their own recompilation
    r.original = r.exe_size == exesym::EXE_SIZE && r.exe_sha1 == exesym::EXE_SHA1;
    r.language = detect_game_language(g);
    r.itoon = g.find({"ITOON", "ITOON.EXP"}) != nullptr;
    r.setup = g.find({"SETUP", "SETUP.EXP"}) != nullptr;
    for (int n = 1; n <= 6; n++)
        if (g.find({"TOON" + std::to_string(n), "CARTOON" + std::to_string(n) + ".GAP"}))
            r.chapters += (r.chapters.empty() ? "" : " ") + std::to_string(n);
    return r;
}

}  // namespace blub
