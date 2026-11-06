// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each new screen shows a translated line when the language changes, and no
// line of a shipped language is wider than the control that draws it.
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/font_atlases.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/matchmaking.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/boneyards/utf8.hpp"

#include "oa/boneyards_test/installs.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/netgame_installs.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr const char* k_screens[] = {
    "gui/locator.gui",
    "gui/matchmaking.gui",
    "gui/quickplay.gui",
    "gui/registration.gui",
    "gui/registration_form.gui",
    "gui/register_unavailable.gui",
};

constexpr const char* k_tags[] = {"de", "fr", "it", "es", "zh-Hans"};

struct InstalledAssets {
    oa::AssetStore* assets = nullptr;
    oa::Palette palette{};
};

uint8_t* read_asset(void* user, const char* path, size_t* size) {
    auto* installed = static_cast<InstalledAssets*>(user);
    std::vector<uint8_t> bytes;
    try {
        bytes = installed->assets->read(path).bytes;
    } catch (...) {
        return nullptr;
    }
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr)
        return nullptr;
    std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

bool read_gui(void*, const char* path, std::vector<uint8_t>* out) {
    if (path == nullptr || out == nullptr)
        return false;
    const std::filesystem::path file = std::filesystem::path(OA_BONEYARDS_GUI_DIR) / path;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0)
        return false;
    in.seekg(0);
    out->resize(static_cast<std::size_t>(length));
    if (length > 0)
        in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(length));
    return static_cast<bool>(in);
}

std::string read_disk(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const char* sentinel_translate(void*, const char* text) {
    if (text == nullptr)
        return nullptr;
    if (std::strcmp(text, "Duel") == 0)
        return "31C";
    if (std::strcmp(text, "Ok") == 0)
        return "OK!";
    if (std::strcmp(text, "Yes") == 0)
        return "JA";
    return nullptr;
}

struct GameWords {
    const oa::formats::tdf::Block* root = nullptr;
    const char* word = nullptr;
};

const char* game_translate(void* context, const char* text) {
    auto* words = static_cast<GameWords*>(context);
    if (words == nullptr || words->root == nullptr || words->word == nullptr || text == nullptr)
        return nullptr;
    const oa::formats::tdf::Block* root = words->root;
    for (uint32_t i = 0; i < root->child_count; ++i) {
        const oa::formats::tdf::Block* child = root->children[i];
        if (child == nullptr || child->name == nullptr || std::strcmp(child->name, text) != 0)
            continue;
        return oa::formats::tdf::find_value(child, words->word);
    }
    return nullptr;
}

const char* game_word(const char* tag) {
    if (std::strcmp(tag, "de") == 0)
        return "german";
    if (std::strcmp(tag, "fr") == 0)
        return "french";
    if (std::strcmp(tag, "it") == 0)
        return "italian";
    if (std::strcmp(tag, "es") == 0)
        return "spanish";
    return nullptr;
}

void use_language(const oa::data::languages::InterfaceText& catalogue, const char* tag) {
    const oa::data::languages::Language* language = oa::data::languages::find_by_tag(tag);
    CHECK(language != nullptr);
    if (language != nullptr)
        oa::data::languages::set_interface_language(&catalogue, *language);
}

GuiScreen* load_screen(const char* path) {
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load(path, &error);
    if (screen == nullptr)
        std::fprintf(stderr, "load %s: %s\n", path, error.message);
    CHECK(screen != nullptr);
    return screen;
}

const char* caption_of(const GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    CHECK(control != nullptr);
    return control != nullptr ? control->caption : "";
}

void expect_caption(const GuiScreen* screen, const char* name, const char* want) {
    const char* got = caption_of(screen, name);
    if (std::strcmp(got, want) != 0) {
        std::fprintf(stderr, "caption %s is \"%s\", wanted \"%s\"\n", name, got, want);
        ++g_failures;
    }
}

void expect_contains(const GuiScreen* screen, const char* name, const char* piece) {
    const char* got = caption_of(screen, name);
    if (std::strstr(got, piece) == nullptr) {
        std::fprintf(stderr, "caption %s is \"%s\", missing \"%s\"\n", name, got, piece);
        ++g_failures;
    }
}

int32_t measure_width(Handle font, const char* text) {
    int32_t width = 0;
    int32_t height = 0;
    text_measure_string(font, text, &width, &height);
    return width;
}

bool missing_glyph(Handle font, const char* text) {
    for (int32_t index = 0; text[index] != '\0';) {
        const utf8::Unit unit = utf8::read(text + index);
        const int32_t bytes = unit.bytes > 0 ? unit.bytes : 1;
        index += bytes;
        if (unit.code == ' ' || unit.code == '\n' || unit.code == '\r')
            continue;
        if (text_char_width(font, static_cast<int32_t>(unit.code)) <= 0)
            return true;
    }
    return false;
}

// How the caption breaks in the box. A word wider than the box does not fit.
struct Fit {
    int32_t lines = 0;
    int32_t widest = 0;
    bool word_wide = false;
};

Fit fit_caption(Handle font, const char* text, int32_t room, int32_t style) {
    Fit fit;
    const bool wrap = (style & text_style::wrap) != 0;
    const int32_t gap = measure_width(font, " ");
    const char* cursor = text;
    while (cursor[0] != '\0') {
        const char* line_end = cursor;
        while (line_end[0] != '\0' && line_end[0] != '\n' && line_end[0] != '\r')
            ++line_end;
        if (!wrap) {
            char buffer[160];
            const std::size_t bytes = static_cast<std::size_t>(line_end - cursor);
            const std::size_t count = bytes < sizeof buffer ? bytes : sizeof buffer - 1;
            if (count > 0)
                std::memcpy(buffer, cursor, count);
            buffer[count] = '\0';
            const int32_t width = measure_width(font, buffer);
            if (width > fit.widest)
                fit.widest = width;
            if (width > room)
                fit.word_wide = true;
            ++fit.lines;
        } else {
            int32_t line_width = 0;
            bool any = false;
            const char* word = cursor;
            while (word < line_end) {
                while (word < line_end && word[0] == ' ')
                    ++word;
                const char* word_end = word;
                while (word_end < line_end && word_end[0] != ' ')
                    ++word_end;
                if (word == word_end)
                    break;
                char buffer[160];
                const std::size_t bytes = static_cast<std::size_t>(word_end - word);
                const std::size_t count = bytes < sizeof buffer ? bytes : sizeof buffer - 1;
                if (count > 0)
                    std::memcpy(buffer, word, count);
                buffer[count] = '\0';
                const int32_t width = measure_width(font, buffer);
                if (width > room)
                    fit.word_wide = true;
                if (!any) {
                    line_width = width;
                    any = true;
                    ++fit.lines;
                } else if (line_width + gap + width > room) {
                    if (line_width > fit.widest)
                        fit.widest = line_width;
                    line_width = width;
                    ++fit.lines;
                } else {
                    line_width += gap + width;
                }
                if (line_width > fit.widest)
                    fit.widest = line_width;
                word = word_end;
            }
            if (!any)
                ++fit.lines;
        }
        cursor = line_end;
        if (cursor[0] == '\r')
            ++cursor;
        if (cursor[0] == '\n')
            ++cursor;
    }
    if (fit.lines == 0)
        fit.lines = 1;
    return fit;
}

void check_screen(const char* path, const char* tag) {
    GuiScreen* screen = load_screen(path);
    if (screen == nullptr)
        return;
    const int32_t count = gui_screen_count(screen);
    for (int32_t index = 0; index < count; ++index) {
        const GuiControl* control = gui_screen_at(screen, index);
        if (control == nullptr || control->caption[0] == '\0')
            continue;
        // A font or picture resource has no face. Only a drawn caption can
        // overflow.
        if (control->window == nullptr || control->window->font < 0)
            continue;
        const Handle font = control->window->font;
        int32_t room_w = 0;
        int32_t room_h = 0;
        int32_t style = 0;
        gui_control_caption_room(control, &room_w, &room_h, &style);
        if (missing_glyph(font, control->caption)) {
            std::fprintf(
                stderr,
                "%s %s %s missing glyph in \"%s\"\n",
                tag,
                path,
                control->name,
                control->caption
            );
            ++g_failures;
            continue;
        }
        const Fit fit = fit_caption(font, control->caption, room_w, style);
        const int32_t line_height = font_line_height(font);
        const bool wide = fit.word_wide || fit.widest > room_w;
        const bool tall = fit.lines > 1 && line_height > 0 && fit.lines * line_height > room_h;
        if (wide || tall) {
            std::fprintf(
                stderr,
                "%s %s %s \"%s\" width %d room %d lines %d height %d box %d\n",
                tag,
                path,
                control->name,
                control->caption,
                fit.widest,
                room_w,
                fit.lines,
                line_height,
                room_h
            );
            ++g_failures;
        }
    }
    gui_screen_destroy(screen);
}

void check_languages(const oa::data::languages::InterfaceText& catalogue, GameWords* words) {
    oa::data::languages::TranslationHooks hook{};
    hook.context = words;
    hook.translate = game_translate;
    oa::data::languages::set_translation_hooks(hook);
    for (const char* tag : k_tags) {
        words->word = game_word(tag);
        use_language(catalogue, tag);
        for (const char* path : k_screens)
            check_screen(path, tag);
    }
}

void check_switch(const oa::data::languages::InterfaceText& catalogue) {
    oa::data::languages::TranslationHooks hook{};
    hook.translate = sentinel_translate;
    oa::data::languages::set_translation_hooks(hook);
    use_language(catalogue, "de");
    GuiScreen* locator = load_screen("gui/locator.gui");
    if (locator != nullptr) {
        expect_caption(locator, "Title", "ORTUNG");
        gui_screen_destroy(locator);
    }
    GuiScreen* match = load_screen("gui/matchmaking.gui");
    if (match != nullptr) {
        expect_caption(match, "Title", "PARTNERSUCHE");
        expect_caption(match, "Duel", "31C");
        gui_screen_destroy(match);
    }
    GuiScreen* quick = load_screen("gui/quickplay.gui");
    if (quick != nullptr) {
        expect_caption(quick, "Title", "SCHNELL");
        gui_screen_destroy(quick);
    }
    GuiScreen* registration = load_screen("gui/registration.gui");
    if (registration != nullptr) {
        expect_caption(registration, "Title", "ANMELDUNG");
        gui_screen_destroy(registration);
    }
    GuiScreen* form = load_screen("gui/registration_form.gui");
    if (form != nullptr) {
        expect_caption(form, "Heading", "IM SPIEL");
        gui_screen_destroy(form);
    }
    GuiScreen* unavailable = load_screen("gui/register_unavailable.gui");
    if (unavailable != nullptr) {
        expect_contains(unavailable, "Text", "Webseite");
        expect_caption(unavailable, "Ok", "OK!");
        gui_screen_destroy(unavailable);
    }

    use_language(catalogue, "fr");
    locator = load_screen("gui/locator.gui");
    if (locator != nullptr) {
        expect_caption(locator, "Title", "REPÈRE");
        gui_screen_destroy(locator);
    }
    match = load_screen("gui/matchmaking.gui");
    if (match != nullptr) {
        expect_caption(match, "Title", "PARTENAIRES");
        expect_caption(match, "Duel", "31C");
        gui_screen_destroy(match);
    }
    quick = load_screen("gui/quickplay.gui");
    if (quick != nullptr) {
        expect_caption(quick, "Title", "RAPIDE");
        gui_screen_destroy(quick);
    }
    registration = load_screen("gui/registration.gui");
    if (registration != nullptr) {
        expect_caption(registration, "Title", "DOSSIER");
        gui_screen_destroy(registration);
    }
    form = load_screen("gui/registration_form.gui");
    if (form != nullptr) {
        expect_caption(form, "Heading", "DANS LE JEU");
        gui_screen_destroy(form);
    }
    unavailable = load_screen("gui/register_unavailable.gui");
    if (unavailable != nullptr) {
        expect_contains(unavailable, "Text", "page web");
        expect_caption(unavailable, "Ok", "OK!");
        gui_screen_destroy(unavailable);
    }
}

// Each template's translations take the arguments its English takes.
void check_templates(const std::string& catalogue_text) {
    oa::formats::tdf::OwnedDocument document;
    oa::formats::tdf::ParseError error{};
    CHECK(document.parse(catalogue_text, &error));
    const oa::formats::tdf::Block* root = document.root();
    if (root == nullptr)
        return;
    int templates = 0;
    for (uint32_t i = 0; i < root->child_count; ++i) {
        const oa::formats::tdf::Block* section = root->children[i];
        if (section == nullptr || section->name == nullptr ||
            std::strchr(section->name, '%') == nullptr)
            continue;
        ++templates;
        const std::string want = printf_conversions(section->name);
        for (const char* tag : k_tags) {
            const char* text = oa::formats::tdf::find_value(section, tag);
            if (text == nullptr || printf_conversions(text) == want)
                continue;
            std::fprintf(
                stderr, "%s \"%s\" takes other arguments than \"%s\"\n", tag, text, section->name
            );
            ++g_failures;
        }
    }
    CHECK(templates > 0);
}

// A language pack's template that takes other arguments shows the English.
void check_bad_template() {
    oa::data::languages::set_translation_hooks(oa::data::languages::TranslationHooks{});
    oa::data::languages::InterfaceText bad;
    CHECK(bad.add(
        "[Commanders queued on %s]\n\t{\n\tde=Auf %d Planeten %s;\n\t}\n"
        "[SEARCHING  %d:%02d]\n\t{\n\tde=SUCHE %s;\n\t}\n"
        "[Prefer my favorite: %s]\n\t{\n\tde=Favorit: %s %%;\n\t}\n"
    ));
    use_language(bad, "de");
    screens::MatchmakingChoices choices{};
    char line[128];
    screens::matchmaking_queue_line(&choices, "Fuel Dump", line, sizeof line);
    CHECK(std::strcmp(line, "Commanders queued on Fuel Dump") == 0);
    screens::matchmaking_search_line(&choices, line, sizeof line);
    CHECK(std::strcmp(line, "SEARCHING  0:00") == 0);
    char pattern[64];
    copy_shown_pattern(pattern, sizeof pattern, "Prefer my favorite: %s");
    CHECK(std::strcmp(pattern, "Favorit: %s %%") == 0);
    // A translation cut short of its conversion is not used.
    char cut[11];
    copy_shown_pattern(cut, sizeof cut, "Prefer my favorite: %s");
    CHECK(printf_conversions(cut).empty());
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the translated screens drawn from the install");
    const std::string catalogue_text = read_disk(OA_BONEYARDS_CATALOGUE);
    CHECK(!catalogue_text.empty());
    oa::data::languages::InterfaceText catalogue;
    std::string error;
    if (!catalogue.add(catalogue_text, &error)) {
        std::fprintf(stderr, "catalogue: %s\n", error.c_str());
        return 1;
    }

    oa::AssetStore assets(root);
    assets.mount(root / "by.ccx");
    assets.mount(root / "totala1.hpi");
    std::string translate_text;
    try {
        const oa::AssetData data = assets.read("gamedata/translate.tdf");
        translate_text.assign(reinterpret_cast<const char*>(data.bytes.data()), data.bytes.size());
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "translate.tdf: %s\n", failure.what());
        return 1;
    }
    oa::formats::tdf::OwnedDocument game_table;
    oa::formats::tdf::ParseError parse_error{};
    if (!game_table.parse(translate_text, &parse_error)) {
        std::fprintf(stderr, "translate.tdf did not parse\n");
        return 1;
    }
    GameWords words;
    words.root = game_table.root();

    std::string atlas_text = read_disk(OA_BONEYARDS_ATLAS);
    FontAtlas* atlas =
        font_atlas_open(reinterpret_cast<const uint8_t*>(atlas_text.data()), atlas_text.size());
    if (atlas == nullptr) {
        std::fprintf(stderr, "the analogue atlas did not open\n");
        return 1;
    }

    InstalledAssets installed;
    installed.assets = &assets;
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int index = 0; index < OA_PALETTE_COLORS; ++index) {
        installed.palette.entries[index] = oa::PaletteEntry{
            raw[static_cast<std::size_t>(index) * 4],
            raw[static_cast<std::size_t>(index) * 4 + 1],
            raw[static_cast<std::size_t>(index) * 4 + 2],
            0
        };
    }
    ToolkitConfig config{};
    config.width = 640;
    config.height = 480;
    config.palette = &installed.palette;
    config.files = ToolkitFiles{&installed, read_asset};
    Toolkit* toolkit = toolkit_create(&config);
    if (toolkit == nullptr) {
        font_atlas_close(atlas);
        return 1;
    }
    toolkit_set_font_atlas(toolkit, atlas);
    const Services previous = services_install(toolkit_services(toolkit));
    const oa::data::languages::TranslationHooks previous_hooks =
        oa::data::languages::translation_hooks();
    rover_session_set_virtual_files(read_gui, nullptr);

    GuiScreen* probe = load_screen("gui/locator.gui");
    if (probe != nullptr) {
        const GuiControl* title = gui_screen_find(probe, "Title");
        CHECK(title != nullptr && title->window != nullptr && title->window->font >= 0);
        if (title != nullptr && title->window != nullptr && title->window->font >= 0) {
            const int32_t width = measure_width(title->window->font, "I");
            if (width <= 0) {
                std::fprintf(stderr, "the font did not measure\n");
                ++g_failures;
            }
        }
        gui_screen_destroy(probe);
    }

    if (g_failures == 0) {
        check_switch(catalogue);
        check_languages(catalogue, &words);
        check_templates(catalogue_text);
        check_bad_template();
    }

    rover_session_set_virtual_files(nullptr, nullptr);
    oa::data::languages::set_translation_hooks(previous_hooks);
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    services_install(previous);
    toolkit_destroy(toolkit);
    font_atlas_close(atlas);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("translation tests passed\n");
    return 0;
}
