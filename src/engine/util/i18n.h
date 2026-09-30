#pragma once
// Language of the launcher and of the port's messages (the game itself is the German release).
// Texts are written in place as tr("Deutsch", "English").
namespace blub {

enum class Lang { German, English };

Lang detect_language();                 // from the system's preferred locales: German or else English
void set_language(Lang l);
Lang language();
const char *tr(const char *german, const char *english);

}  // namespace blub
