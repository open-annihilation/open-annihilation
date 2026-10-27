// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Joins the interface-catalogue texts each module exports and prints them.

#include "command.hpp"
#include "interface_words.hpp"

#include "oa/app/interface_words.hpp"
#include "oa/app/package_install/interface_words.hpp"
#include "oa/app/update/interface_words.hpp"
#include "oa/formats/json.hpp"
#include "oa/netgame/match/interface_words.hpp"
#include "oa/ui/engine_settings/interface_words.hpp"
#include "oa/ui/first_run/interface_words.hpp"
#include "oa/ui/folder_chooser/interface_words.hpp"
#include "oa/ui/frontend_dialogs/interface_words.hpp"
#include "oa/ui/frontend_multiplayer/interface_words.hpp"
#include "oa/ui/game_files/interface_words.hpp"
#include "oa/ui/key_editor/interface_words.hpp"
#include "oa/ui/library/interface_words.hpp"
#include "oa/ui/map_browser/interface_words.hpp"
#include "oa/ui/mutator_picker/interface_words.hpp"
#include "oa/ui/brain_picker/interface_words.hpp"
#include "oa/ui/touch_hud/interface_words.hpp"

#include <algorithm>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace oa::tool {
namespace {

/// Appends one module's texts.
void append(std::vector<std::string_view>& words, std::span<const std::string_view> more) {
    words.insert(words.end(), more.begin(), more.end());
}

/// Joins, sorts and keeps one of each text the modules export.
std::vector<std::string_view> join_interface_words() {
    std::vector<std::string_view> words;
    append(words, oa::ui::library::interface_words);
    append(words, oa::ui::engine_settings::interface_words);
    append(words, oa::ui::first_run::interface_words);
    append(words, oa::ui::map_browser::interface_words);
    append(words, oa::ui::mutator_picker::interface_words);
    append(words, oa::ui::brain_picker::interface_words);
    append(words, oa::ui::frontend_dialogs::interface_words);
    append(words, oa::ui::frontend_multiplayer::interface_words);
    append(words, oa::app::package_install::interface_words);
    append(words, oa::ui::game_files::interface_words);
    append(words, oa::ui::key_editor::interface_words);
    append(words, oa::ui::folder_chooser::interface_words);
    append(words, oa::ui::touch_hud::interface_words);
    append(words, oa::netgame::match::interface_words);
    append(words, oa::app::interface_words);
    append(words, oa::app::update::interface_words);
    std::erase(words, std::string_view{});
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return words;
}

} // namespace

std::vector<std::string_view> collected_interface_words() {
    return join_interface_words();
}

int run_interface_words(std::span<const std::string> arguments, Output& output) {
    if (arguments.size() != 1 || arguments[0] != "--json") {
        output.err << "oa-tool interface-words: the listing is written with --json\n";
        output.err << "run 'oa-tool help interface-words'\n";
        return exit_usage;
    }
    const std::vector<std::string_view> words = collected_interface_words();
    oa::formats::json::JsonWriter writer;
    writer.begin_array();
    for (const std::string_view word : words)
        writer.string(word);
    writer.end_array();
    output.out << writer.text() << '\n';
    return exit_done;
}

} // namespace oa::tool
