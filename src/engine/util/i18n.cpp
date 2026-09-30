#include "util/i18n.h"

#include <SDL.h>

#include <cstring>

namespace blub {

namespace {
Lang g_lang = Lang::English;
}  // namespace

Lang detect_language() {
    Lang l = Lang::English;
    if (SDL_Locale *locales = SDL_GetPreferredLocales()) {
        if (locales[0].language && std::strncmp(locales[0].language, "de", 2) == 0) l = Lang::German;
        SDL_free(locales);
    }
    return l;
}

void set_language(Lang l) { g_lang = l; }

Lang language() { return g_lang; }

const char *tr(const char *german, const char *english) { return g_lang == Lang::German ? german : english; }

}  // namespace blub
