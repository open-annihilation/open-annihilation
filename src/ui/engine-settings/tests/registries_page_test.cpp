// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Add registry window. Its words, fields and buttons in every mode and
// phase, its focus starting on CANCEL once it is ready, its keys and
// typing, and its parts at every class. Every name is unique. The
// Registries list itself is the Library's.

#include "oa/test/check.hpp"
#include "oa/ui/engine_settings/add_registry.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;

using settings::AddRegistry;
using settings::AddRegistryAction;

/// The classes every layout is checked at.
constexpr std::array<kit::SizeClass, 3> kClasses{
    kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large
};

bool same_rect(const renderer::SourceRect& a, const renderer::SourceRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// Returns the control of a name in the dialog's list, or null.
///
/// @param list the list
/// @param name the name, without oa.
/// @return the control
const kit::Control* named(const kit::DisplayList& list, std::string_view name) {
    return kit::control_of(list, kit::control_named(list, name));
}

/// Returns a rectangle's middle.
///
/// @param rect the rectangle
/// @return its middle point
kit::Point centre(const renderer::SourceRect& rect) {
    return {rect.x + rect.width / 2, rect.y + rect.height / 2};
}

/// Checks that every name a list gives is unique and well formed.
///
/// @param list the list
/// @return the names
std::set<std::string> names_of(const kit::DisplayList& list) {
    OA_CHECK(kit::name_problem(list).empty());
    std::set<std::string> names;
    for (const kit::AutomationEntry& entry : kit::automation_parts(list, {}))
        OA_CHECK(names.insert(entry.name).second);
    return names;
}

/// A probe of a registry of maps with one key.
///
/// @return the probe
settings::ProbeView maps_probe() {
    settings::ProbeView shown;
    shown.name = "Example Maps";
    shown.address = "maps.example.org";
    shown.packages = 31;
    shown.kinds = {"maps"};
    shown.fingerprints = {"9fa2 · 77c1 · e04b · 2kw0"};
    shown.downloads = "direct";
    return shown;
}

/// Returns every word a window's list draws.
///
/// @param list the list
/// @return the words, one item a line
std::string words_of(const kit::DisplayList& list) {
    std::string words;
    for (const kit::Item& item : list.items) {
        if (const auto* heading = std::get_if<kit::HeadingLook>(&item.look))
            words += heading->text + "\n";
        else if (const auto* header = std::get_if<kit::HeaderLook>(&item.look))
            words += header->title + "\n";
        else if (!item.text.empty())
            words += item.text + "\n";
    }
    return words;
}

/// Returns a control's text in a window's list.
///
/// @param list the list
/// @param name its name
/// @return its text; none when the list has no such control
std::optional<std::string> label_of(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = named(list, name);
    if (control == nullptr)
        return std::nullopt;
    return control->text;
}

/// Returns the names of a window's buttons, left to right.
///
/// @param list the list
/// @return their names
std::vector<std::string> buttons_of(const kit::DisplayList& list) {
    std::vector<std::string> names;
    for (const kit::Control& control : list.controls)
        if (control.kind == kit::ControlKind::button)
            names.push_back(control.name);
    return names;
}

/// Checks that a window's parts lie inside it, and its buttons apart.
///
/// @param window the window
void window_inside(const AddRegistry& window) {
    const kit::DisplayList list = settings::add_registry_list(window);
    const kit::Rect whole{
        0, 0, settings::add_registry_width(window.size_class), settings::add_registry_height(window)
    };
    OA_CHECK(whole.height >= kit::metrics_of(window.size_class).least_notice_height);
    OA_CHECK(whole.height <= kit::metrics_of(window.size_class).greatest_notice_height);
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::mark)
            continue;
        OA_CHECK(kit::wholly_in(item.rect, kit::grown(whole, kit::compact_metrics.focus_inset)));
    }
    for (std::size_t index = 0; index < list.controls.size(); ++index) {
        OA_CHECK(kit::wholly_in(list.controls[index].rect, whole));
        for (std::size_t other = index + 1; other < list.controls.size(); ++other) {
            if (list.controls[index].enabled && list.controls[other].enabled) {
                const kit::Rect shared =
                    kit::intersect(list.controls[index].rect, list.controls[other].rect);
                OA_CHECK(shared.width == 0 || shared.height == 0);
            }
        }
    }
    static_cast<void>(names_of(list));
}

/// Returns a window in a mode and a phase, settled.
///
/// @param mode how the registry came
/// @param phase where it stands
/// @return the window
AddRegistry window_of(AddRegistry::Mode mode, AddRegistry::Phase phase) {
    AddRegistry window;
    window.mode = mode;
    window.phase = phase;
    window.file_name = "maps.oareg";
    window.mirror_of = "Example Maps";
    window.url = "http://maps.example.org/oa/registry.yaml";
    window.caret = window.url.size();
    window.checking = "maps.example.org";
    if (phase == AddRegistry::Phase::ready)
        window.shown = maps_probe();
    if (phase == AddRegistry::Phase::refused)
        window.shown.refusal = "OA couldn't use maps.example.org: cannot be reached.";
    settings::add_registry_settle(window);
    return window;
}

void the_window_shows_each_mode_and_phase() {
    // By address, editing: the field and CHECK, the focus on the field.
    AddRegistry editing = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::editing);
    editing.url.clear();
    editing.caret = 0;
    kit::DisplayList list = settings::add_registry_list(editing);
    OA_CHECK(words_of(list).find("Add registry") != std::string::npos);
    OA_CHECK(
        named(list, "add-registry.url") != nullptr && named(list, "add-registry.url")->enabled
    );
    OA_CHECK(
        (buttons_of(list) == std::vector<std::string>{"add-registry.cancel", "add-registry.check"})
    );
    OA_CHECK(editing.interaction.focused == settings::add_registry_url_control);
    OA_CHECK(settings::add_registry_field(editing).has_value());
    bool url_placeholder = false;
    bool key_placeholder = false;
    for (const kit::Item& item : list.items) {
        const auto* field = std::get_if<kit::SearchLook>(&item.look);
        if (field == nullptr)
            continue;
        if (item.control == settings::add_registry_url_control)
            url_placeholder = field->placeholder == "http://…" && !field->magnifier;
        if (item.control == settings::add_registry_key_field_control)
            key_placeholder = field->placeholder == "ed25519:…" && !field->magnifier;
    }
    OA_CHECK(url_placeholder);
    OA_CHECK(key_placeholder);
    OA_CHECK(
        named(list, "add-registry.key-field") != nullptr &&
        named(list, "add-registry.key-field")->enabled
    );
    OA_CHECK(words_of(list).find("Signing key") != std::string::npos);
    OA_CHECK(list.tab_order.size() >= 4);
    OA_CHECK(list.tab_order[0] == settings::add_registry_url_control);
    OA_CHECK(list.tab_order[1] == settings::add_registry_key_field_control);
    OA_CHECK(list.tab_order[2] == settings::add_registry_cancel_control);
    OA_CHECK(list.tab_order[3] == settings::add_registry_check_control);

    // Checking: the address, and CANCEL alone.
    AddRegistry checking = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::checking);
    list = settings::add_registry_list(checking);
    OA_CHECK(words_of(list).find("Checking maps.example.org…") != std::string::npos);
    OA_CHECK((buttons_of(list) == std::vector<std::string>{"add-registry.cancel"}));
    OA_CHECK(!named(list, "add-registry.url")->enabled);
    OA_CHECK(
        named(list, "add-registry.key-field") != nullptr &&
        !named(list, "add-registry.key-field")->enabled
    );
    OA_CHECK(!settings::add_registry_field(checking).has_value());
    OA_CHECK(checking.interaction.focused == settings::add_registry_cancel_control);

    // Ready: the name, the facts, the warning, CANCEL and ADD, the focus on CANCEL.
    AddRegistry ready = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::ready);
    list = settings::add_registry_list(ready);
    const std::string words = words_of(list);
    OA_CHECK(words.find("Add registry") != std::string::npos);
    OA_CHECK(label_of(list, "add-registry.name") == std::string("Example Maps"));
    OA_CHECK(label_of(list, "add-registry.address") == std::string("Address maps.example.org"));
    OA_CHECK(words.find("31 · maps") != std::string::npos);
    OA_CHECK(
        label_of(list, "add-registry.key") == std::string("Signing key 9fa2 · 77c1 · e04b · 2kw0")
    );
    OA_CHECK(words.find("Downloads") != std::string::npos);
    OA_CHECK(words.find("direct") != std::string::npos);
    OA_CHECK(words.find("Not reviewed by the OA team.") != std::string::npos);
    OA_CHECK(
        (buttons_of(list) == std::vector<std::string>{"add-registry.cancel", "add-registry.add"})
    );
    OA_CHECK(ready.interaction.focused == settings::add_registry_cancel_control);
    OA_CHECK(named(list, "add-registry.add")->text == "Add");
    OA_CHECK(
        !named(list, "add-registry.name")->enabled && !named(list, "add-registry.name")->focusable
    );
    OA_CHECK(named(list, "add-registry.name")->kind == kit::ControlKind::area);

    // Several keys: at most three, then how many more; kinds joined.
    AddRegistry keys = ready;
    keys.shown.fingerprints = {"aaaa", "bbbb", "cccc", "dddd", "eeee"};
    keys.shown.kinds = {"mods", "maps", "languages"};
    keys.shown.downloads = "with download keys · needs an install ID";
    list = settings::add_registry_list(keys);
    OA_CHECK(
        label_of(list, "add-registry.key") == std::string("Signing keys aaaa\nbbbb\ncccc\n+2 more")
    );
    bool key_fits = false;
    for (const kit::Item& item : list.items) {
        if (item.text == "Signing keys")
            key_fits = item.rect.width >= kit::estimated_width(item.text);
    }
    OA_CHECK(key_fits);
    OA_CHECK(words_of(list).find("31 · mods, maps, languages") != std::string::npos);
    OA_CHECK(words_of(list).find("with download keys · needs an install ID") != std::string::npos);

    // Unsigned: the amber line in Key's place.
    AddRegistry unsigned_ready = ready;
    unsigned_ready.shown.is_signed = false;
    unsigned_ready.shown.fingerprints.clear();
    list = settings::add_registry_list(unsigned_ready);
    OA_CHECK(
        label_of(list, "add-registry.key") ==
        std::string("Unsigned · accepted in Developer mode only")
    );
    OA_CHECK(words_of(list).find("Key\n") == std::string::npos);

    // Refused: the refusal in amber, the field editable again, CANCEL and CHECK.
    AddRegistry refused = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::refused);
    list = settings::add_registry_list(refused);
    OA_CHECK(
        label_of(list, "add-registry.refusal") ==
        std::string("OA couldn't use maps.example.org: cannot be reached.")
    );
    bool amber = false;
    for (const kit::Item& item : list.items)
        amber = amber ||
                (item.text.rfind("OA couldn't use", 0) == 0 && item.colour == kit::colour::lock);
    OA_CHECK(amber);
    OA_CHECK(named(list, "add-registry.url")->enabled);
    OA_CHECK(
        (buttons_of(list) == std::vector<std::string>{"add-registry.cancel", "add-registry.check"})
    );
    OA_CHECK(refused.interaction.focused == settings::add_registry_url_control);

    // From a file: "From {file}" in the field's place; refused, CANCEL alone.
    AddRegistry file = window_of(AddRegistry::Mode::from_file, AddRegistry::Phase::checking);
    file.checking = "maps.oareg";
    list = settings::add_registry_list(file);
    OA_CHECK(words_of(list).find("From maps.oareg") != std::string::npos);
    OA_CHECK(named(list, "add-registry.url") == nullptr);
    OA_CHECK(named(list, "add-registry.key-field") == nullptr);
    file = window_of(AddRegistry::Mode::from_file, AddRegistry::Phase::ready);
    list = settings::add_registry_list(file);
    OA_CHECK(label_of(list, "add-registry.name") == std::string("Example Maps"));
    OA_CHECK(
        (buttons_of(list) == std::vector<std::string>{"add-registry.cancel", "add-registry.add"})
    );
    file = window_of(AddRegistry::Mode::from_file, AddRegistry::Phase::refused);
    list = settings::add_registry_list(file);
    OA_CHECK((buttons_of(list) == std::vector<std::string>{"add-registry.cancel"}));
    OA_CHECK(file.interaction.focused == settings::add_registry_cancel_control);
    OA_CHECK(words_of(list).find("From maps.oareg") != std::string::npos);

    // A mirror: "Add mirror", "Mirror of {name}", the keys trusted, ADD MIRROR.
    AddRegistry mirror = window_of(AddRegistry::Mode::mirror, AddRegistry::Phase::editing);
    list = settings::add_registry_list(mirror);
    OA_CHECK(words_of(list).find("Add mirror") != std::string::npos);
    OA_CHECK(label_of(list, "add-registry.name") == std::string("Mirror of Example Maps"));
    OA_CHECK(named(list, "add-registry.url") != nullptr);
    OA_CHECK(named(list, "add-registry.key-field") == nullptr);
    OA_CHECK(mirror.interaction.focused == settings::add_registry_url_control);
    mirror = window_of(AddRegistry::Mode::mirror, AddRegistry::Phase::ready);
    mirror.shown.address = "copy.example.net";
    mirror.shown.fingerprints = {"aaaa", "bbbb", "cccc", "dddd"};
    list = settings::add_registry_list(mirror);
    OA_CHECK(label_of(list, "add-registry.address") == std::string("Address copy.example.net"));
    OA_CHECK(
        words_of(list).find("Checked against the keys you trust for Example Maps") !=
        std::string::npos
    );
    OA_CHECK(label_of(list, "add-registry.key") == std::string("aaaa\nbbbb\ncccc\n+1 more"));
    OA_CHECK(named(list, "add-registry.add")->text == "Add mirror");
    OA_CHECK(mirror.interaction.focused == settings::add_registry_cancel_control);
    OA_CHECK(words_of(list).find("Not reviewed") == std::string::npos);
}

void the_window_takes_keys_and_typing() {
    AddRegistry window = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::editing);
    window.url.clear();
    window.caret = 0;
    // Typing, and a paste cut at its first line break.
    OA_CHECK(settings::add_registry_text(window, "http://maps") == AddRegistryAction::redraw);
    OA_CHECK(
        settings::add_registry_text(window, ".example.org/x\nsecond line") ==
        AddRegistryAction::redraw
    );
    OA_CHECK(window.url == "http://maps.example.org/x");
    OA_CHECK(settings::add_registry_key(window, kit::Key::backspace) == AddRegistryAction::redraw);
    OA_CHECK(window.url == "http://maps.example.org/");
    OA_CHECK(settings::add_registry_key(window, kit::Key::home) == AddRegistryAction::redraw);
    OA_CHECK(window.caret == 0);
    OA_CHECK(settings::add_registry_key(window, kit::Key::end) == AddRegistryAction::redraw);
    // Space on the field starts typing; Enter checks; Escape cancels.
    OA_CHECK(
        settings::add_registry_key(window, kit::Key::space) == AddRegistryAction::start_typing
    );
    OA_CHECK(window.url == "http://maps.example.org/");
    OA_CHECK(settings::add_registry_key(window, kit::Key::enter) == AddRegistryAction::check);
    OA_CHECK(settings::add_registry_key(window, kit::Key::escape) == AddRegistryAction::cancel);

    // The field holds at most the address reader's limit, cut at a character.
    AddRegistry long_field = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::editing);
    long_field.url = std::string(2046, 'a');
    long_field.caret = long_field.url.size();
    OA_CHECK(settings::add_registry_text(long_field, "éé") == AddRegistryAction::redraw);
    OA_CHECK(long_field.url.size() == 2048);
    OA_CHECK(settings::add_registry_text(long_field, "b") == AddRegistryAction::none);

    // A refused address goes back to editing once it changes.
    AddRegistry refused = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::refused);
    OA_CHECK(settings::add_registry_text(refused, "z") == AddRegistryAction::redraw);
    OA_CHECK(refused.phase == AddRegistry::Phase::editing && refused.shown.refusal.empty());
    refused = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::refused);
    OA_CHECK(settings::add_registry_key(refused, kit::Key::backspace) == AddRegistryAction::redraw);
    OA_CHECK(refused.phase == AddRegistry::Phase::editing);

    // Ready: Enter presses CANCEL, never ADD; Tab reaches ADD.
    AddRegistry ready = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::ready);
    OA_CHECK(settings::add_registry_text(ready, "x") == AddRegistryAction::none);
    OA_CHECK(settings::add_registry_key(ready, kit::Key::enter) == AddRegistryAction::cancel);
    ready = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::ready);
    OA_CHECK(settings::add_registry_key(ready, kit::Key::space) == AddRegistryAction::cancel);
    ready = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::ready);
    OA_CHECK(settings::add_registry_key(ready, kit::Key::tab) == AddRegistryAction::redraw);
    OA_CHECK(ready.interaction.focused == settings::add_registry_add_control);
    OA_CHECK(settings::add_registry_key(ready, kit::Key::enter) == AddRegistryAction::add);

    // Checking: Enter presses CANCEL; nothing else is offered.
    AddRegistry checking = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::checking);
    OA_CHECK(settings::add_registry_key(checking, kit::Key::enter) == AddRegistryAction::cancel);

    // The pointer: a click on ADD adds, on the field focuses it and starts typing.
    ready = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::ready);
    const kit::DisplayList ready_list = settings::add_registry_list(ready);
    const kit::Control* add = named(ready_list, "add-registry.add");
    OA_CHECK(add != nullptr);
    if (add != nullptr) {
        const kit::Point at = centre(add->rect);
        static_cast<void>(settings::add_registry_pointer_move(ready, at));
        OA_CHECK(settings::add_registry_pointer_down(ready, at) == AddRegistryAction::redraw);
        OA_CHECK(settings::add_registry_pointer_up(ready, at) == AddRegistryAction::add);
    }
    AddRegistry clicked = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::refused);
    clicked.interaction.focused = settings::add_registry_cancel_control;
    const kit::DisplayList clicked_list = settings::add_registry_list(clicked);
    const kit::Control* field = named(clicked_list, "add-registry.url");
    OA_CHECK(field != nullptr);
    if (field != nullptr) {
        const kit::Point at = centre(field->rect);
        static_cast<void>(settings::add_registry_pointer_down(clicked, at));
        OA_CHECK(settings::add_registry_pointer_up(clicked, at) == AddRegistryAction::start_typing);
        OA_CHECK(clicked.interaction.focused == settings::add_registry_url_control);
        OA_CHECK(settings::add_registry_field(clicked).has_value());
    }

    // The signing-key field: Tab reaches it, a paste stops at the line break,
    // and it holds at most 128 bytes. A refused key goes back to editing.
    AddRegistry keyed = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::editing);
    keyed.url = "http://maps.example.org/";
    keyed.caret = keyed.url.size();
    OA_CHECK(settings::add_registry_key(keyed, kit::Key::tab) == AddRegistryAction::redraw);
    OA_CHECK(keyed.interaction.focused == settings::add_registry_key_field_control);
    const std::optional<kit::Rect> key_rect = settings::add_registry_field(keyed);
    const kit::DisplayList key_list = settings::add_registry_list(keyed);
    const kit::Control* key_control = named(key_list, "add-registry.key-field");
    OA_CHECK(key_rect.has_value());
    OA_CHECK(key_control != nullptr && key_rect && same_rect(*key_rect, key_control->rect));
    OA_CHECK(settings::add_registry_text(keyed, "ed25519:abc\nrest") == AddRegistryAction::redraw);
    OA_CHECK(keyed.signing_key == "ed25519:abc");
    OA_CHECK(keyed.url == "http://maps.example.org/");
    OA_CHECK(settings::add_registry_key(keyed, kit::Key::backspace) == AddRegistryAction::redraw);
    OA_CHECK(keyed.signing_key == "ed25519:ab");
    OA_CHECK(settings::add_registry_key(keyed, kit::Key::space) == AddRegistryAction::start_typing);

    AddRegistry long_key = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::editing);
    OA_CHECK(settings::add_registry_key(long_key, kit::Key::tab) == AddRegistryAction::redraw);
    long_key.signing_key = std::string(126, 'k');
    long_key.key_caret = long_key.signing_key.size();
    OA_CHECK(settings::add_registry_text(long_key, "éé") == AddRegistryAction::redraw);
    OA_CHECK(long_key.signing_key.size() == 128);
    OA_CHECK(settings::add_registry_text(long_key, "b") == AddRegistryAction::none);

    AddRegistry refused_key = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::refused);
    OA_CHECK(settings::add_registry_key(refused_key, kit::Key::tab) == AddRegistryAction::redraw);
    OA_CHECK(settings::add_registry_text(refused_key, "k") == AddRegistryAction::redraw);
    OA_CHECK(refused_key.phase == AddRegistry::Phase::editing && refused_key.shown.refusal.empty());
    refused_key = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::refused);
    refused_key.signing_key = "ed25519:ab";
    refused_key.key_caret = refused_key.signing_key.size();
    OA_CHECK(settings::add_registry_key(refused_key, kit::Key::tab) == AddRegistryAction::redraw);
    OA_CHECK(
        settings::add_registry_key(refused_key, kit::Key::backspace) == AddRegistryAction::redraw
    );
    OA_CHECK(refused_key.phase == AddRegistry::Phase::editing);
    OA_CHECK(refused_key.signing_key == "ed25519:a");

    AddRegistry key_click = window_of(AddRegistry::Mode::by_url, AddRegistry::Phase::refused);
    key_click.interaction.focused = settings::add_registry_cancel_control;
    const kit::DisplayList key_click_list = settings::add_registry_list(key_click);
    const kit::Control* key_press = named(key_click_list, "add-registry.key-field");
    OA_CHECK(key_press != nullptr);
    if (key_press != nullptr) {
        const kit::Point at = centre(key_press->rect);
        static_cast<void>(settings::add_registry_pointer_down(key_click, at));
        OA_CHECK(
            settings::add_registry_pointer_up(key_click, at) == AddRegistryAction::start_typing
        );
        OA_CHECK(key_click.interaction.focused == settings::add_registry_key_field_control);
        const std::optional<kit::Rect> pressed_field = settings::add_registry_field(key_click);
        OA_CHECK(pressed_field.has_value() && same_rect(*pressed_field, key_press->rect));
    }
}

void the_window_lies_inside_at_every_class() {
    const std::array<AddRegistry::Mode, 3> modes{
        AddRegistry::Mode::by_url, AddRegistry::Mode::from_file, AddRegistry::Mode::mirror
    };
    const std::array<AddRegistry::Phase, 4> phases{
        AddRegistry::Phase::editing,
        AddRegistry::Phase::checking,
        AddRegistry::Phase::ready,
        AddRegistry::Phase::refused,
    };
    for (const kit::SizeClass size_class : kClasses) {
        for (const AddRegistry::Mode mode : modes) {
            for (const AddRegistry::Phase phase : phases) {
                AddRegistry window = window_of(mode, phase);
                window.size_class = size_class;
                window.shown.fingerprints = {"aaaa", "bbbb", "cccc", "dddd"};
                window.shown.refusal = phase == AddRegistry::Phase::refused
                                           ? std::string(400, 'r') + " end."
                                           : std::string();
                window_inside(window);
                OA_CHECK(
                    settings::add_registry_width(size_class) ==
                    kit::metrics_of(size_class).notice_width
                );
                if (const std::optional<kit::Rect> field = settings::add_registry_field(window)) {
                    const kit::DisplayList drawn_list = settings::add_registry_list(window);
                    const kit::Control* drawn = named(drawn_list, "add-registry.url");
                    OA_CHECK(drawn != nullptr && same_rect(*field, drawn->rect));
                }
            }
        }
    }
}

} // namespace

int main() {
    the_window_shows_each_mode_and_phase();
    the_window_takes_keys_and_typing();
    the_window_lies_inside_at_every_class();
    return oa::test::check_exit_status();
}
