#include "launcher.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "codec/image.h"
#include "data/gamefs.h"
#include "data/gap.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "tinyfiledialogs.h"
#include "util/i18n.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace blub {

namespace {

const ImVec4 kGreen(0.45f, 0.85f, 0.35f, 1.0f), kYellow(1.0f, 0.82f, 0.25f, 1.0f), kRed(1.0f, 0.4f, 0.4f, 1.0f),
    kDim(0.65f, 0.65f, 0.7f, 1.0f);

struct Banner {
    SDL_Texture *tex = nullptr;
    int w = 0, h = 0;
    std::string for_dir;
    ~Banner() { reset(); }
    void reset() {
        if (tex) SDL_DestroyTexture(tex);
        tex = nullptr;
    }
};

bool load_pcx(GameData &g, const char *dir, const char *file, int id, Indexed &img, Palette &pal) {
    GapArchive gap;
    if (!gap.open(g.open_file({dir, file}), file) || !gap.valid(id)) return false;
    return decode_pcx(gap.payload(id), img, &pal);
}

// header picture from the user's own game files: the crashed spaceship of the main menu with the
// "Down in the Dumps" logo of the setup program on top
void build_banner(SDL_Renderer *ren, Banner &b, const std::string &game_dir) {
    b.reset();
    b.for_dir = game_dir;
    Indexed ship, logo;
    Palette ship_pal{}, logo_pal{};
    GameData g;
    if (!g.open(game_dir) || !load_pcx(g, "ITOON", "ITOON.GAP", 33, ship, ship_pal)) return;
    const bool has_logo = load_pcx(g, "SETUP", "SETUP.GAP", 11, logo, logo_pal);
    // a 640x150 strip around the ship
    const int W = ship.w, H = std::min(150, ship.h), y0 = std::max(0, ship.h - H - 70);
    std::vector<uint32_t> px(size_t(W) * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t c = ship.pix[size_t(y + y0) * ship.w + x];
            // darken towards the left so that the logo stands out
            const float k = std::min(1.0f, 0.15f + 0.85f * float(x) / (0.6f * float(W)));
            px[size_t(y) * W + x] = 0xFF000000u | uint32_t(ship_pal[3 * c] * k) << 16 |
                                    uint32_t(ship_pal[3 * c + 1] * k) << 8 | uint32_t(ship_pal[3 * c + 2] * k);
        }
    if (has_logo) {
        // bounding box of the logo (the colour of the top left corner is the background), scaled
        // into the left part of the banner
        const uint8_t bg = logo.pix[0];
        int x0 = logo.w, x1 = 0, ly0 = logo.h, ly1 = 0;
        for (int y = 0; y < logo.h; y++)
            for (int x = 0; x < logo.w; x++)
                if (logo.pix[size_t(y) * logo.w + x] != bg) {
                    x0 = std::min(x0, x); x1 = std::max(x1, x);
                    ly0 = std::min(ly0, y); ly1 = std::max(ly1, y);
                }
        if (x1 > x0 && ly1 > ly0) {
            const int lw = x1 - x0 + 1, lh = ly1 - ly0 + 1;
            const float s = std::min(1.0f, float(H - 10) / float(lh));
            const int ox = 24, oy = (H - int(lh * s)) / 2;
            for (int y = 0; y < int(lh * s); y++)
                for (int x = 0; x < int(lw * s); x++) {
                    const uint8_t c = logo.pix[size_t(ly0 + int(y / s)) * logo.w + x0 + int(x / s)];
                    if (c != bg && ox + x < W && oy + y < H)
                        px[size_t(oy + y) * W + ox + x] = 0xFF000000u | uint32_t(logo_pal[3 * c]) << 16 |
                                                          uint32_t(logo_pal[3 * c + 1]) << 8 | logo_pal[3 * c + 2];
                }
        }
    }
    b.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (!b.tex) return;
    SDL_UpdateTexture(b.tex, nullptr, px.data(), W * 4);
    SDL_SetTextureScaleMode(b.tex, SDL_ScaleModeLinear);
    b.w = W;
    b.h = H;
}

void style(float scale) {
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0;
    st.FrameRounding = 5;
    st.GrabRounding = 5;
    st.TabRounding = 5;
    st.FramePadding = ImVec2(9, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.WindowPadding = ImVec2(18, 14);
    ImVec4 *c = st.Colors;
    const ImVec4 accent(0.55f, 0.20f, 0.45f, 1.0f), accent_hi(0.70f, 0.28f, 0.58f, 1.0f), green(0.20f, 0.45f, 0.22f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.08f, 0.11f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.15f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.20f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.24f, 0.34f, 1.0f);
    c[ImGuiCol_Button] = accent;
    c[ImGuiCol_ButtonHovered] = accent_hi;
    c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.35f, 0.66f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.17f, 0.15f, 0.20f, 1.0f);
    c[ImGuiCol_TabHovered] = accent_hi;
    c[ImGuiCol_TabSelected] = accent;
    c[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.90f, 0.40f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.90f, 0.40f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.65f, 1.0f, 0.50f, 1.0f);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.55f, 0.28f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.26f, 0.34f, 1.0f);
    st.ScaleAllSizes(scale);
}

#ifdef _WIN32
// the font data of an installed font through GDI: works on Windows and under Wine/Proton, where the
// font files are not at fixed paths and missing fonts are substituted
bool load_gdi_font(const wchar_t *face, float px) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = CreateFontW(-64, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    bool ok = false;
    if (font) {
        HGDIOBJ old = SelectObject(dc, font);
        const DWORD ttcf = 0x66637474;            // 'ttcf': the font is part of a collection
        const bool collection = GetFontData(dc, ttcf, 0, nullptr, 0) != GDI_ERROR;
        const DWORD size = collection ? GDI_ERROR : GetFontData(dc, 0, 0, nullptr, 0);
        if (size != GDI_ERROR && size > 1024) {
            void *data = IM_ALLOC(size);           // owned by the font atlas
            if (GetFontData(dc, 0, 0, data, size) == size &&
                ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, int(size), px)) {
                ok = true;
            } else {
                IM_FREE(data);
            }
        }
        SelectObject(dc, old);
        DeleteObject(font);
    }
    DeleteDC(dc);
    return ok;
}
#endif

void load_font(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    const float px = 17.0f * scale;
#ifdef _WIN32
    for (const wchar_t *face : {L"Segoe UI", L"Tahoma", L"Arial", L"Verdana"})
        if (load_gdi_font(face, px)) return;
#else
    const char *candidates[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                "/System/Library/Fonts/Supplemental/Arial.ttf"};
    for (const char *f : candidates) {
        std::error_code ec;
        if (fs::exists(f, ec) && io.Fonts->AddFontFromFileTTF(f, px)) return;
    }
#endif
    ImFontConfig cfg;                              // built-in font as the last resort
    cfg.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
}

// text field + "Durchsuchen..." for a folder
bool folder_field(const char *id, const char *title, std::string &value) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    bool changed = false;
    ImGui::PushID(id);
    const ImGuiStyle &st = ImGui::GetStyle();
    const float button = ImGui::CalcTextSize(tr("Durchsuchen...", "Browse...")).x + 2 * st.FramePadding.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button - st.ItemSpacing.x);
    if (ImGui::InputText("##path", buf, sizeof buf)) {
        value = buf;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Durchsuchen...", "Browse..."))) {
        const char *sel = tinyfd_selectFolderDialog(title, value.empty() ? nullptr : value.c_str());
        if (sel) {
            value = sel;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

// visible text with an ImGui id that does not change with the language
std::string label(const char *text, const char *id) { return std::string(text) + "###" + id; }

void open_folder(const std::string &dir) {
    std::error_code ec;
    fs::create_directories(fs::u8path(dir), ec);
    std::string url = "file:///" + dir;
    for (auto &ch : url)
        if (ch == '\\') ch = '/';
    SDL_OpenURL(url.c_str());
}

void help_marker(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace

LauncherResult run_launcher(Settings &s, const std::string &settings_path, const std::string &message) {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return LauncherResult::Quit;
    }
    float dpi = 96.0f;
    if (SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) != 0 || dpi < 72.0f) dpi = 96.0f;
    const float scale = std::clamp(dpi / 96.0f, 1.0f, 3.0f);
    SDL_Window *win = SDL_CreateWindow("Down in the Dumps", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       int(780 * scale), int(740 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED) : nullptr;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return LauncherResult::Quit;
    }
    SDL_SetWindowMinimumSize(win, int(600 * scale), int(520 * scale));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;              // no imgui.ini next to the program
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    style(scale);
    load_font(scale);
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    Banner banner;
    GameCheck check = check_game_dir(s.game_dir);
    std::string status = message;
    bool confirm_reset = false;
    LauncherResult result = LauncherResult::Quit;
    bool running = true;
    // testing: BLUB_LAUNCHER_SHOT=<file.bmp>[,tab] saves a screenshot after a few frames and quits
    const char *shot_env = std::getenv("BLUB_LAUNCHER_SHOT");
    std::string shot_file = shot_env ? shot_env : "";
    // (",play": press "Spielen" on the first call, quit on later ones)
    static int calls = 0;
    int shot_tab = -1, frame_no = 0;
    bool autoplay = false;
    if (size_t comma = shot_file.find(','); comma != std::string::npos) {
        const std::string arg = shot_file.substr(comma + 1);
        autoplay = arg == "play" && calls == 0;
        shot_tab = std::atoi(arg.c_str());
        shot_file.resize(comma);
        if (calls > 0) shot_file.insert(shot_file.size() - 4, "_" + std::to_string(calls));
    }
    calls++;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
        }
        if (banner.for_dir != s.game_dir) build_banner(ren, banner, s.game_dir);

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---- header
        const float full_w = ImGui::GetContentRegionAvail().x;
        if (banner.tex) {
            // full width; in wide windows show the middle band of the picture
            const float h = std::min(full_w * banner.h / banner.w, 190.0f * scale);
            const float frac = std::min(1.0f, (h / full_w) * float(banner.w) / float(banner.h));
            ImGui::Image((ImTextureID)(intptr_t)banner.tex, ImVec2(full_w, h), ImVec2(0, 0.5f - frac / 2),
                         ImVec2(1, 0.5f + frac / 2));
        } else {
            ImGui::Dummy(ImVec2(0, 10 * scale));
            ImGui::SetWindowFontScale(1.8f);
            ImGui::TextColored(kGreen, "Down in the Dumps");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextColored(kDim, "Philips Media / Haiku Studios, 1996");
        }
        ImGui::Spacing();

        // ---- settings
        const float bottom = ImGui::GetFrameHeightWithSpacing() * 2.6f;
        ImGui::BeginChild("tabs", ImVec2(0, -bottom));
        if (ImGui::BeginTabBar("tabbar")) {
            if (ImGui::BeginTabItem(label(tr("Spiel", "Game"), "game").c_str(), nullptr, frame_no < 3 && shot_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::Spacing();
                ImGui::TextUnformatted(tr("Spieldaten: Ordner mit den Original-ISOs (oder dem Inhalt der CDs)",
                                           "Game data: folder with the original ISO images (or the contents of the CDs)"));
                if (folder_field("game", tr("Ordner mit den ISO-Dateien wählen", "Choose the folder with the ISO images"), s.game_dir)) check = check_game_dir(s.game_dir);
                for (auto &d : check.discs) ImGui::TextColored(kDim, "  %s", d.c_str());
                if (!check.problems.empty()) ImGui::TextColored(kYellow, tr("Nicht verwendbar: %s", "Not usable: %s"), check.problems.c_str());
                if (s.game_dir.empty()) {
                    ImGui::TextColored(kYellow, tr("Bitte den Ordner mit den ISO-Dateien der drei Original-CDs wählen.",
                                                   "Please choose the folder with the ISO images of the three original CDs."));
                } else if (!check.exe) {
                    ImGui::TextColored(kRed, check.discs.empty() ? tr("In diesem Ordner liegen keine ISOs und keine Spieldateien.",
                                                                    "This folder contains no ISO images and no game files.")
                                                                 : tr("CD 1 fehlt (sie enthält DID.EXE).",
                                                                      "CD 1 is missing (it contains DID.EXE)."));
                } else {
                    const char *lang = check.language == "de" ? tr("deutsch", "German")
                                       : check.language == "en" ? tr("englisch", "English")
                                       : check.language == "fr" ? tr("französisch", "French")
                                                                : tr("unbekannt", "unknown");
                    if (check.original) {
                        ImGui::TextColored(kGreen, tr("DID.EXE erkannt: Originalprogramm von 1996",
                                                      "DID.EXE recognized: original 1996 program"));
                    } else {
                        ImGui::TextColored(kRed, tr("Diese DID.EXE ist eine andere Programmversion und wird noch "
                                                    "nicht unterstützt.",
                                                    "This DID.EXE is a different program version and is not "
                                                    "supported yet."));
                        ImGui::TextColored(kDim, "%zu %s, SHA-1 %s", check.exe_size, tr("Byte", "bytes"),
                                           check.exe_sha1.c_str());
                        help_marker(tr("Der Port ist eine Übersetzung genau einer Fassung von DID.EXE. Bitte "
                                       "Größe und SHA-1 melden, dann kann diese Fassung ergänzt werden.",
                                       "The port is a translation of exactly one build of DID.EXE. Please "
                                       "report the size and SHA-1 so that this build can be added."));
                    }
                    ImGui::TextColored(check.language.empty() ? kYellow : kGreen, tr("Sprache der Spieldaten: %s",
                                                                                   "Language of the game data: %s"),
                                       lang);
                    ImGui::TextColored(check.itoon ? kGreen : kRed, check.itoon ? tr("Hauptmenü (ITOON) vorhanden", "Main menu (ITOON) present")
                                                                                : tr("ITOON fehlt - das Spiel kann nicht starten",
                                                                                     "ITOON is missing - the game cannot start"));
                    // the chapters are spread over the discs: CD 1 = 3, CD 2 = 1 and 2, CD 3 = 4 and 6
                    // chapters 1, 2, 3, 4 and 6 (TOON1..TOON6 without 5), spread over the three CDs
                    std::string missing;
                    for (char n : std::string("12346"))
                        if (check.chapters.find(n) == std::string::npos)
                            missing += std::string(missing.empty() ? tr(" Kapitel ", " chapter ") : ", ") + n;
                    if (missing.empty())
                        ImGui::TextColored(kGreen, tr("Alle Kapitel vorhanden - bereit zum Spielen", "All chapters present - ready to play"));
                    else
                        ImGui::TextColored(kYellow, tr("Es fehlt:%s (auf einer der anderen CDs). Das Spiel startet, aber ohne diese Kapitel.",
                                                       "Missing:%s (on one of the other CDs). The game starts, but without them."),
                                           missing.c_str());
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextUnformatted(tr("Spielstände und Spielkonfiguration", "Saved games and game configuration"));
                std::string save = s.save_dir;
                if (folder_field("save", tr("Ordner für Spielstände wählen", "Choose the folder for saved games"), save)) s.save_dir = save;
                ImGui::TextColored(kDim, "%s", s.save_dir.empty() ? (tr("Standard: ", "Default: ") + s.effective_save_dir()).c_str()
                                                                  : tr("eigener Ordner", "custom folder"));
                if (Settings::portable())
                    ImGui::TextColored(kDim, tr("Portabel: Einstellungen und Spielstände liegen im Programmordner.",
                                                "Portable: settings and saved games are kept in the program folder."));
                if (ImGui::Button(tr("Ordner öffnen", "Open folder"))) open_folder(s.effective_save_dir());
                ImGui::SameLine();
                if (!s.save_dir.empty() && ImGui::Button(tr("Standardordner verwenden", "Use default folder"))) s.save_dir.clear();
                ImGui::SameLine();
                if (ImGui::Button(tr("Intro wieder zeigen...", "Show intro again...")))
                    confirm_reset = true;
                help_marker(tr("Das Spiel merkt sich in DID.CFG, dass das Intro schon lief. Löscht nur diese "
                               "Datei; Spielstände bleiben erhalten.",
                               "The game remembers in DID.CFG that the intro has already been shown. Only this "
                               "file is deleted; saved games are kept."));
                if (confirm_reset) {
                    ImGui::TextColored(kYellow, tr("DID.CFG löschen?", "Delete DID.CFG?"));
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Ja", "Yes"))) {
                        std::error_code ec;
                        bool removed = fs::remove(fs::u8path(s.effective_save_dir()) / "DID.CFG", ec);
                        status = removed ? tr("Das Intro wird beim nächsten Start wieder gezeigt.",
                                              "The intro will be shown at the next start.")
                                         : tr("Es gab keine DID.CFG - das Intro wird ohnehin gezeigt.",
                                              "There was no DID.CFG - the intro will be shown anyway.");
                        confirm_reset = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Nein", "No"))) confirm_reset = false;
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Checkbox(tr("Dieses Fenster beim Start zeigen", "Show this window at startup"), &s.show_launcher);
                help_marker(tr("Aus: blub startet direkt das Spiel. Das Fenster erscheint dann nur noch mit "
                               "\"blub --launcher\" oder wenn die Spieldaten nicht mehr gefunden werden.",
                               "Off: blub starts the game directly. The window then only appears with "
                               "\"blub --launcher\" or when the game data can no longer be found."));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(label(tr("Grafik", "Display"), "display").c_str(), nullptr, frame_no < 3 && shot_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::Spacing();
                ImGui::Checkbox(tr("Vollbild", "Fullscreen"), &s.fullscreen);
                help_marker(tr("Im Spiel jederzeit mit F11 oder Alt+Enter umschaltbar.", "Can be toggled any time in the game with F11 or Alt+Enter."));
                const char *sizes[] = {"640 x 480 (1x)", "1280 x 960 (2x)", "1920 x 1440 (3x)", "2560 x 1920 (4x)"};
                int idx = std::clamp(s.scale, 1, 4) - 1;
                ImGui::SetNextItemWidth(260 * scale);
                if (ImGui::Combo(label(tr("Fenstergröße", "Window size"), "size").c_str(), &idx, sizes, 4)) s.scale = idx + 1;
                ImGui::Checkbox(tr("Weiche Skalierung", "Smooth scaling"), &s.smooth);
                help_marker(tr("Aus: scharfe Pixel wie im Original. An: lineare Filterung.",
                                "Off: sharp pixels like the original. On: linear filtering."));
                ImGui::Checkbox(tr("Vertikale Synchronisation (VSync)", "Vertical sync (VSync)"), &s.vsync);
                ImGui::Checkbox(tr("Seitenumschaltung (flimmerfrei)", "Page flipping (flicker-free)"), &s.dualpage);
                help_marker(tr("Entspricht der Option /DUALPAGE des Originals: das Spiel zeichnet unsichtbar "
                               "und schaltet dann um. Empfohlen.",
                               "Same as the original's /DUALPAGE option: the game draws off-screen and then "
                               "flips the page. Recommended."));
                ImGui::Spacing();
                ImGui::TextColored(kDim, tr("Das Bild bleibt immer im Seitenverhältnis 4:3.", "The picture always keeps its 4:3 aspect ratio."));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(label(tr("Ton & Steuerung", "Sound & controls"), "sound").c_str(), nullptr, frame_no < 3 && shot_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::Spacing();
                ImGui::Checkbox(tr("Ton", "Sound"), &s.sound);
                ImGui::BeginDisabled(!s.sound);
                ImGui::SetNextItemWidth(300 * scale);
                ImGui::SliderInt(label(tr("Lautstärke", "Volume"), "volume").c_str(), &s.volume, 0, 100, "%d %%");
                ImGui::EndDisabled();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Checkbox(tr("Esc überspringt Videos (wie die Leertaste)", "Esc skips videos (like the space bar)"), &s.esc_skips);
                ImGui::Spacing();
                if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn(tr("Taste", "Key"), ImGuiTableColumnFlags_WidthFixed, 170 * scale);
                    ImGui::TableSetupColumn(tr("Funktion", "Function"));
                    const char *rows[][2] = {{tr("Maus", "Mouse"), tr("Alles im Spiel: zeigen, klicken, Gegenstände benutzen",
                                                                         "Everything in the game: look, click, use objects")},
                                             {tr("Leertaste", "Space bar"), tr("Video / Sequenz überspringen", "Skip video / sequence")},
                                             {"P", "Pause"},
                                             {"F11, Alt+Enter", tr("Vollbild ein/aus", "Fullscreen on/off")},
                                             {"Alt+F4", tr("Spiel beenden", "Quit the game")}};
                    for (auto &r : rows) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r[0]);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(r[1]);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(label("Info", "info").c_str(), nullptr, frame_no < 3 && shot_tab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", tr("Down in the Dumps (Philips Media / Haiku Studios, 1996) auf modernen Systemen.",
                                             "Down in the Dumps (Philips Media / Haiku Studios, 1996) on modern systems."));
                ImGui::Spacing();
                ImGui::TextWrapped("%s", tr("Das Programm DID.EXE der Original-CD ist statisch nach C++ rekompiliert; DOS, "
                                             "DOS/4GW, VESA-Grafik, Maus, CD-Laufwerk und die SOS-Soundbibliothek werden "
                                             "von diesem Port nachgebildet. Grafik, Ton, Videos und Skripte werden zur "
                                             "Laufzeit aus deinen Original-CDs (ISO-Dateien) geladen; das Spiel läuft in "
                                             "der Sprache deiner CDs.",
                                             "The program DID.EXE of the original CD is statically recompiled to C++; "
                                             "DOS, DOS/4GW, VESA graphics, mouse, CD drive and the SOS sound library are "
                                             "emulated by this port. Graphics, sound, videos and scripts are loaded from "
                                             "your original CDs (ISO images) at runtime; the game runs in the language of "
                                             "your CDs."));
                ImGui::Spacing();
                ImGui::Spacing();
                {
                    const char *langs[] = {tr("Automatisch (Systemsprache)", "Automatic (system language)"), "Deutsch",
                                           "English"};
                    int li = s.language == "de" ? 1 : s.language == "en" ? 2 : 0;
                    ImGui::SetNextItemWidth(260 * scale);
                    if (ImGui::Combo(label(tr("Sprache", "Language"), "language").c_str(), &li, langs, 3)) {
                        s.language = li == 1 ? "de" : li == 2 ? "en" : "auto";
                        s.apply_language();
                    }
                }
                ImGui::Spacing();
                ImGui::TextColored(kDim, tr("Einstellungen: %s", "Settings: %s"), settings_path.c_str());
                ImGui::TextColored(kDim, "SDL %d.%d.%d, Dear ImGui %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                                   SDL_PATCHLEVEL, IMGUI_VERSION);
                ImGui::TextColored(kDim, "%s", tr("Lizenz: GNU GPL v3.0 oder später - ohne Gewährleistung",
                                                  "License: GNU GPL v3.0 or later - without any warranty"));
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        // ---- bottom bar
        ImGui::Separator();
        if (!status.empty())
            ImGui::TextColored(kYellow, "%s", status.c_str());
        else
            ImGui::TextColored(kDim, " ");
        const bool can_play = check.exe && check.original && check.itoon;
        const float bw = 150 * scale;
        ImGui::BeginDisabled(!can_play);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.55f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.68f, 0.32f, 1.0f));
        if (ImGui::Button(label(tr("Spielen", "Play"), "play").c_str(), ImVec2(bw, 0)) || (can_play && ImGui::IsKeyPressed(ImGuiKey_Enter, false) &&
                                                        !ImGui::GetIO().WantTextInput)) {
            result = LauncherResult::Play;
            running = false;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::BeginDisabled(!check.setup);
        if (ImGui::Button(label(tr("Original-Setup", "Original setup"), "setup").c_str(), ImVec2(bw, 0))) {
            result = LauncherResult::Setup;
            running = false;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button(label(tr("Beenden", "Quit"), "quit").c_str(), ImVec2(bw, 0))) running = false;
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 20, 18, 26, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        if (!shot_file.empty() && ++frame_no == 8) {
            int w, h;
            SDL_GetRendererOutputSize(ren, &w, &h);
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                SDL_SaveBMP(surf, shot_file.c_str());
                SDL_FreeSurface(surf);
            }
            running = false;
            if (autoplay) result = LauncherResult::Play;
        }
        SDL_RenderPresent(ren);
    }

    s.save(settings_path);
    banner.reset();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return result;
}

}  // namespace blub
