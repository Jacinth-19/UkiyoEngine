// A game's text, per language.
//
// The engine's own UI is English and stays English; this is for the strings a *game* shows — the line under the title,
// a message a script writes, the name of a level. A project keeps one JSON file per language under assets/locales/
// (`en.json`, `ja.json`, ...), each a flat map of key to text. The player picks one at start (project setting, then
// `--locale`), and Lua asks for a key with `ukiyo.tr("key")`, falling back to the key's own text.
//
// The fallback chain is deliberate: a missing key falls back to the project's base language, then to the key itself.
// A game with a half-finished translation still runs, and the untranslated line is visible as the key rather than empty.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ukiyo {

struct LocaleCatalog {
    std::string code;                            // "en", "ja", "pt-BR"
    std::map<std::string, std::string> strings;  // key -> text

    std::string tr(const std::string& key) const;   // the text, or the key when it is missing
    bool has(const std::string& key) const;
};

// Reads assets/locales/<code>.json below `projectRoot`. False when the file is missing or is not a flat string map.
bool loadLocale(const std::string& projectRoot, const std::string& code, LocaleCatalog* out, std::string* error);
std::string localeJson(const LocaleCatalog& catalog);

// The language codes a project has, sorted. The base language first is the caller's business: `baseLocale` reads the
// project setting and falls back to "en".
std::vector<std::string> availableLocales(const std::string& projectRoot);
std::string baseLocale(const std::string& projectRoot);

// A catalogue with the keys of `base` and `base`'s text for each, ready to be translated: what "new language" writes.
LocaleCatalog newLocale(const LocaleCatalog& base, const std::string& code);

} // namespace ukiyo
