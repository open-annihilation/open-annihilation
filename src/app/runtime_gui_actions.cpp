// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gui actions gathered from a mod's GUI files, and which of them are
// live for the panel on screen.
#include "oa/app/runtime.hpp"

#include "gui_actions.hpp"
#include "panel_first_draw.hpp"
#include "tag_actions.hpp"

#include "oa/app/asset_files.hpp"
#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/sides.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace actions = oa::data::input_actions;
namespace keys = oa::ui::key_bindings;

namespace {

/// The bit of a context, or 0 when the table has no such name.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The file stem of a resource path, folded as a gui id piece.
///
/// @param path a resource path or a file name
/// @return the folded stem, without the extension
std::string stem_of_resource(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = file.find_last_of('.');
    const auto stem = dot == std::string_view::npos ? file : file.substr(0, dot);
    return gui_key_piece(stem);
}

/// The lower-case letter of a `CTRL_<letter>` name, or 0.
///
/// The name is exactly those six characters. A longer category is not a
/// letter tag.
///
/// @param name the category name, or null
/// @return the letter, or 0
char ctrl_letter(const char* name) {
    if (name == nullptr)
        return 0;
    constexpr char prefix[] = "CTRL_";
    for (int index = 0; index < 5; ++index) {
        if (name[index] == '\0')
            return 0;
        const auto have = static_cast<unsigned char>(name[index]);
        const auto want = static_cast<unsigned char>(prefix[index]);
        if (std::toupper(have) != want)
            return 0;
    }
    if (name[5] == '\0' || name[6] != '\0')
        return 0;
    const auto last = static_cast<unsigned char>(name[5]);
    if (last >= 'a' && last <= 'z')
        return static_cast<char>(last);
    if (last >= 'A' && last <= 'Z')
        return static_cast<char>(last - 'A' + 'a');
    return 0;
}

/// Whether a category mask holds any unit type.
///
/// @param mask the mask
/// @return true when some type bit is set
bool mask_holds_a_type(const oa::data::defs::CategoryMask& mask) {
    if (mask.words == nullptr)
        return false;
    for (uint32_t index = 0; index < mask.word_count; ++index) {
        if (mask.words[index] != 0)
            return true;
    }
    return false;
}

/// The stem of a `gui.` action id, between the two dots.
///
/// @param id the action id
/// @return the stem, or empty when the id is not a gui action
std::string_view stem_of_gui_id(std::string_view id) {
    constexpr std::string_view prefix = "gui.";
    if (!id.starts_with(prefix))
        return {};
    const auto rest = id.substr(prefix.size());
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos)
        return rest;
    return rest.substr(0, dot);
}

} // namespace

void copy_button_letter(
    oa::ui::gui_layout::ButtonFields& button, const oa::ui::frontend::Control& control
) {
    button.quick_key = control.quick_key;
}

std::vector<oa::ui::frontend_renderer::ButtonPresentation>
unit_info_button_presentations(std::vector<oa::ui::gui_layout::Gadget>& gadgets) {
    std::vector<oa::ui::frontend_renderer::ButtonPresentation> buttons;
    for (std::size_t gadget_index = 0; gadget_index < gadgets.size(); ++gadget_index) {
        assign_button_quick_key(gadgets, gadget_index);
        const auto* button =
            std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[gadget_index].fields);
        if (button == nullptr)
            continue;
        oa::ui::frontend_renderer::ButtonPresentation shown;
        shown.name = gadgets[gadget_index].common.name;
        shown.quick_key = static_cast<char>(button->quick_key);
        buttons.push_back(std::move(shown));
    }
    return buttons;
}

void Runtime::gather_gui_quick_keys() {
    gui_quick_keys_.clear();
    const char* folder = oa::data::defs::directory_name(oa::data::defs::DataDirectory::guis);
    const std::vector<std::string> names = assets_.list_effective(folder, ".gui");
    for (const std::string& path : names) {
        const std::optional<std::vector<uint8_t>> bytes = assets_.load_file_contents(path);
        if (!bytes)
            continue;
        oa::ui::gui_layout::ParseResult parsed = oa::ui::gui_layout::parse(*bytes);
        if (!parsed.ok() || !parsed.layout)
            continue;
        const std::string stem = stem_of_resource(path);
        // A unit info button the file leaves without a key takes the letter its
        // caption gives it, the same assignment the panel's first draw makes.
        if (stem.starts_with("unitinfo")) {
            auto& gadgets = parsed.layout->gadgets;
            for (std::size_t gadget_index = 0; gadget_index < gadgets.size(); ++gadget_index) {
                const auto* button =
                    std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[gadget_index].fields);
                if (button != nullptr && button->quick_key <= 0)
                    assign_button_quick_key(gadgets, gadget_index);
            }
        }
        for (const oa::ui::gui_layout::Gadget& gadget : parsed.layout->gadgets) {
            const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            if (button == nullptr || button->quick_key <= 0)
                continue;
            GuiQuickKey key;
            key.file_stem = stem;
            key.gadget = gadget.common.name;
            key.action = gui_action_id(stem, gadget.common.name);
            key.letter = static_cast<char32_t>(static_cast<uint8_t>(button->quick_key));
            key.help = gadget.common.source_help;
            gui_quick_keys_.push_back(std::move(key));
        }
    }
    gui_quick_keys_gathered_ = true;
    gather_tag_words();
}

void Runtime::gather_tag_words() {
    gathered_tags_.clear();
    oa::data::defs::CategoryRegistry scratch{};
    oa::data::defs::category_registry_init(&scratch);
    const oa::data::defs::Files files = asset_files(assets_);
    std::vector<std::string> unit_files;
    files.list(
        files.context,
        oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
        oa::data::defs::unit_extension(),
        [](void* user, const char* name) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(name);
        },
        &unit_files
    );
    oa::UnitDef unit{};
    unit.type_id = 1;
    for (const std::string& name : unit_files) {
        char path[oa::data::defs::path_capacity];
        oa::data::defs::build_variant_path(
            &files,
            path,
            sizeof path,
            oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
            name.c_str(),
            oa::data::defs::unit_extension(),
            nullptr
        );
        oa::formats::tdf::Document document;
        oa::formats::tdf::document_init(&document);
        const bool loaded = oa::data::defs::load_tdf_file(&files, path, &document, nullptr) &&
                            oa::formats::tdf::select_section(&document, "UNITINFO");
        if (loaded) {
            // The same 100-byte read load_unit_def uses, so this scratch
            // registry holds the words the match registry will.
            constexpr std::size_t category_text = 100;
            char text[category_text];
            oa::formats::tdf::get_string(
                oa::formats::tdf::cursor(&document), "category", text, sizeof text, ""
            );
            oa::data::defs::register_unit_categories(&scratch, &unit, text);
        }
        oa::formats::tdf::document_free(&document);
    }
    std::vector<std::string> skipped;
    const std::vector<TagWord> words = tag_words(scratch, skipped);
    oa::data::defs::category_registry_clear(&scratch);
    gathered_tags_.reserve(words.size());
    for (const TagWord& word : words)
        gathered_tags_.push_back(GatheredTag{word.word, word.action});
    for (const std::string& word : skipped) {
        std::fprintf(stderr, "category word \"%s\" is not a selection of its own\n", word.c_str());
    }
}

keys::ModData Runtime::gui_mod_data(std::string_view order_stem) const {
    keys::ModData mod;
    mod.keys.reserve(gui_quick_keys_.size());
    for (const GuiQuickKey& key : gui_quick_keys_) {
        keys::GuiKey row;
        row.action = key.action;
        row.letter = key.letter;
        row.help = key.help;
        if (key.file_stem == order_stem)
            row.gadget = key.gadget;
        mod.keys.push_back(std::move(row));
    }
    mod.categories = loaded_category_letters();
    mod.tags.reserve(gathered_tags_.size());
    for (const GatheredTag& tag : gathered_tags_)
        mod.tags.push_back(tag.word);
    return mod;
}

std::string Runtime::loaded_category_letters() const {
    const oa::data::defs::CategoryRegistry& registry = unit_table_.tables.categories;
    std::string letters;
    if (registry.entries == nullptr || registry.masks == nullptr)
        return letters;
    for (uint32_t index = 0; index < registry.count; ++index) {
        const char letter = ctrl_letter(registry.entries[index].name);
        if (letter == 0)
            continue;
        const uint32_t mask_index = registry.entries[index].mask;
        if (mask_index >= registry.count)
            continue;
        if (!mask_holds_a_type(registry.masks[mask_index]))
            continue;
        if (letters.find(letter) != std::string::npos)
            continue;
        letters.push_back(letter);
    }
    std::sort(letters.begin(), letters.end());
    return letters;
}

void Runtime::apply_shown_gui_file(keys::Resolved& resolved, std::string_view shown) const {
    const uint32_t play = context_bit("play");
    const uint32_t panel = context_bit("panel");
    const uint32_t unit_info = context_bit("unit-info");
    for (keys::Bound& bound : resolved.actions) {
        if (!bound.id.starts_with("gui."))
            continue;
        const std::string_view stem = stem_of_gui_id(bound.id);
        if (stem.starts_with("unitinfo"))
            bound.contexts = unit_info;
        else if (!shown.empty() && stem == shown)
            bound.contexts = play | panel;
    }
}

std::string Runtime::order_side_stem() const {
    if (side_table_.count == 0)
        return {};
    std::size_t side = 0;
    if (match_ != nullptr)
        side = match_view_side();
    if (side >= side_table_.count)
        side = 0;
    const auto* record = oa::data::defs::side_at(side_table_, static_cast<uint32_t>(side));
    if (record == nullptr)
        return {};
    const auto* field = record->name_prefix;
    const std::string_view prefix(field, strnlen(field, sizeof record->name_prefix));
    std::string stem = gui_key_piece(prefix);
    stem += "gen";
    return stem;
}

std::string Runtime::shown_hud_stem() const {
    if (!match_hud_)
        return {};
    return stem_of_resource(match_hud_panel_);
}

std::vector<Runtime::OrderFileKey> Runtime::order_file_quick_keys() {
    if (!gui_quick_keys_gathered_)
        gather_gui_quick_keys();
    std::vector<OrderFileKey> found;
    for (const GuiQuickKey& key : gui_quick_keys_) {
        if (key.file_stem != "armgen" && key.file_stem != "corgen")
            continue;
        const std::string_view action = order_action_id(key.gadget);
        if (action.empty())
            continue;
        OrderFileKey row;
        row.action = std::string(action);
        row.file = key.file_stem == "armgen" ? "ARMGEN.GUI" : "CORGEN.GUI";
        row.gadget = key.gadget;
        row.letter = key.letter;
        found.push_back(std::move(row));
    }
    return found;
}

} // namespace oa::app
