// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The registries' host: the Add registry window on the OA layer, and the
// changes the Library's Registries tab makes at once. The window reads a
// typed address first and refuses one OA cannot fetch without asking the
// content service; every other check and every change is the content
// service's, which applies the registry rules and verifies each catalogue
// with the keys being trusted. Nothing here fetches, verifies or writes a
// registry itself.

#include "add_registry_menu.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"
#include "runtime_game_menu_art.hpp"
#include "user_folder_state.hpp"

#include "oa/app/content/service.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/url.hpp"
#include "oa/ui/engine_settings/add_registry.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace content = oa::app::content;
namespace registry = oa::data::registry;
namespace url = oa::formats::url;

/// The window's name on the OA layer.
constexpr std::string_view kScreenName = "add-registry";

/// One refusal and the sentence the window shows for it. {name} is the
/// checked registry's name (for a mirror, the registry's own), or the
/// address when the check read no descriptor; {built-in name} the built-in
/// registry it would pass for; {most} the most registries a player adds.
struct RefusalSentence {
    registry::Refusal refusal{}; ///< the refusal
    std::string_view sentence{}; ///< what the window says, in English
};

/// The sentence for each refusal the window explains on its own. Every
/// other refusal says what the content service found (kOtherSentence).
constexpr std::array<RefusalSentence, 12> kRefusalSentences{{
    {registry::Refusal::built_in_id, "{name} is built in, so it can't be added again."},
    {registry::Refusal::built_in_name, "{name} is built in, so it can't be added again."},
    {registry::Refusal::built_in_key,
     "This registry uses {built-in name}'s keys or address, so OA refuses it."},
    {registry::Refusal::built_in_host,
     "This registry uses {built-in name}'s keys or address, so OA refuses it."},
    {registry::Refusal::unsigned_needs_developer_mode,
     "Unsigned registries can be added only in Developer mode, from 127.0.0.1."},
    {registry::Refusal::unsigned_needs_loopback,
     "Unsigned registries can be added only in Developer mode, from 127.0.0.1."},
    {registry::Refusal::already_added,
     "{name} is already added. To give it another address, use ADD MIRROR… on its row."},
    {registry::Refusal::too_many_registries,
     "OA keeps at most {most} added registries. Remove one first."},
    {registry::Refusal::built_in_mirror, "Mirrors of {name} come only from its own catalogue."},
    {registry::Refusal::mirror_listed, "{name} already lists that mirror."},
    {registry::Refusal::bad_mirror, "That isn't a web address OA can read."},
    {registry::Refusal::shipped_logo, "{name} can't use the Open Annihilation logo."},
}};
/// An https address this system read as http, when that host served no http.
constexpr std::string_view kNeedsHttps = "This registry needs HTTPS, which this system cannot use";
/// Any other address the address reader refuses, before any check.
constexpr std::string_view kNotAddressSentence = "That isn't a web address OA can read.";
/// A signing key that is not `ed25519:` and 44 characters of base64.
constexpr std::string_view kSigningKeyUnreadable = "The signing key could not be read.";
/// A typed key that did not verify the catalogue.
constexpr std::string_view kCatalogueNotSigned = "The catalogue isn't signed by this key.";

/// Drops leading and trailing spaces and tabs from a signing key.
///
/// @param text the field's text
/// @return the key, or empty when the field was blank
std::string trimmed_signing_key(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    return std::string(text.substr(begin, end - begin));
}

/// Every other refusal: unreadable, and what no row above names. {address}
/// is the address checked, or the file's name, and {detail} what the
/// content service found: the reader's reason, the catalogue's verdict or
/// the fetch's failure.
constexpr std::string_view kOtherSentence = "OA couldn't use {address}: {detail}.";

/// Fills one {place} of a sentence.
///
/// @param text the sentence
/// @param place the place's name, without its braces
/// @param value what stands in its place
/// @return the sentence
std::string filled(std::string text, std::string_view place, std::string_view value) {
    const std::string marker = "{" + std::string(place) + "}";
    for (std::size_t at = text.find(marker); at != std::string::npos;
         at = text.find(marker, at + value.size()))
        text.replace(at, marker.size(), value);
    return text;
}

/// What a refusal's sentence names.
struct Named {
    std::string name;          ///< the registry's name, or the address without one
    std::string built_in_name; ///< the built-in registry it would pass for
    std::string address;       ///< the address checked, or the file's name
    std::string detail;        ///< what the content service found
};

/// Returns the sentence the window shows for a refusal.
///
/// @param refusal the refusal
/// @param named what the sentence names
/// @return one sentence
std::string refusal_sentence(registry::Refusal refusal, const Named& named) {
    for (const RefusalSentence& row : kRefusalSentences) {
        if (row.refusal != refusal)
            continue;
        std::string text = filled(std::string(row.sentence), "name", named.name);
        text = filled(std::move(text), "built-in name", named.built_in_name);
        return filled(std::move(text), "most", std::to_string(registry::max_added_registries));
    }
    // The detail is a sentence of its own or a phrase; the sentence ends it.
    std::string detail = named.detail;
    while (!detail.empty() && (detail.back() == '.' || detail.back() == ' '))
        detail.pop_back();
    if (detail.empty())
        detail = registry::refusal_text(refusal);
    while (!detail.empty() && detail.back() == '.')
        detail.pop_back();
    std::string text = filled(std::string(kOtherSentence), "address", named.address);
    return filled(std::move(text), "detail", detail);
}

/// Returns the hosts a descriptor names: its catalogue's, its mirrors', its
/// download API's and its homepage's.
///
/// @param descriptor the descriptor
/// @return the hosts
std::vector<std::string> hosts_of(const registry::Descriptor& descriptor) {
    std::vector<std::string> hosts{descriptor.catalogue.host};
    for (const url::Url& mirror : descriptor.mirrors)
        hosts.push_back(mirror.host);
    if (descriptor.api)
        hosts.push_back(descriptor.api->host);
    if (descriptor.homepage)
        hosts.push_back(descriptor.homepage->host);
    return hosts;
}

/// Returns the built-in registry a refused descriptor would pass for: the
/// one sharing a key, else the one whose host a host of it is or lies under.
///
/// @param snapshot the registries in effect
/// @param check the check
/// @return its name; the first built-in registry's when none matches
std::string
built_in_name_for(const content::Snapshot& snapshot, const content::RegistryCheck& check) {
    std::string first;
    for (const content::RegistryView& listed : snapshot.registries) {
        if (listed.registry.origin != registry::Origin::built_in)
            continue;
        if (first.empty())
            first = listed.registry.descriptor.name;
        for (const std::string& key : check.fingerprints)
            if (std::find(listed.fingerprints.begin(), listed.fingerprints.end(), key) !=
                listed.fingerprints.end())
                return listed.registry.descriptor.name;
        if (!check.descriptor)
            continue;
        for (const std::string& host : hosts_of(*check.descriptor))
            for (const std::string& built_in : hosts_of(listed.registry.descriptor))
                if (url::host_within(host, built_in))
                    return listed.registry.descriptor.name;
    }
    return first.empty() ? std::string("a built-in registry") : first;
}

/// Returns how a registry's packages are downloaded, as the window says it.
///
/// @param descriptor the descriptor
/// @return "direct", "with download keys", or that and "needs an install ID"
std::string downloads_text(const registry::Descriptor& descriptor) {
    if (descriptor.mode == registry::DownloadMode::direct)
        return "direct";
    if (descriptor.install_id == registry::InstallIdUse::required)
        return "with download keys · needs an install ID";
    return "with download keys";
}

/// Returns a kind of package as the window names it.
///
/// @param kind the kind
/// @return "mods", "maps", "languages", "expansions", "mutators" or "brains"
std::string kind_text(oa::data::catalogue::Kind kind) {
    switch (kind) {
    case oa::data::catalogue::Kind::oamod:
        return "mods";
    case oa::data::catalogue::Kind::oamap:
        return "maps";
    case oa::data::catalogue::Kind::oalang:
        return "languages";
    case oa::data::catalogue::Kind::expansion:
        return "expansions";
    case oa::data::catalogue::Kind::oamut:
        return "mutators";
    case oa::data::catalogue::Kind::oabrain:
        return "brains";
    }
    return "packages";
}

/// Returns the registry in effect of an id.
///
/// @param snapshot the registries in effect
/// @param id the registry's id
/// @return the registry; null when none has the id
const content::RegistryView* view_of(const content::Snapshot& snapshot, std::string_view id) {
    for (const content::RegistryView& listed : snapshot.registries)
        if (!listed.registry.conflicting && listed.registry.descriptor.id == id)
            return &listed;
    return nullptr;
}

/// Writes a line to standard error, which the log keeps.
///
/// @param line the line
void log_line(std::string_view line) {
    std::cerr << "open-annihilation: registries: " << line << '\n';
}

/// What outlives one window: the window showing, how many checks the
/// windows asked the content service for, and why the last change of a
/// registry was refused.
struct RegistriesHold {
    const LayerScreen* live{}; ///< the window showing; null for none
    uint64_t checks_asked{};   ///< checks asked of the content service
    std::string notice;        ///< why the last change was refused; empty when it was kept
};

RegistriesHold hold;

} // namespace

/// The Add registry window as a screen of the OA layer: modal over a
/// backdrop, centred over the front end, named add-registry; over the game's
/// own main menu, and the Library over it, it covers the screen in the
/// Library's look there (AddRegistryGameLook). It reads a
/// typed address before it asks the content service anything, polls the
/// check each frame, and adds through the content service at ADD.
struct Runtime::AddRegistryScreen final : LayerScreen {
    /// Makes the window and remembers it as the one showing.
    ///
    /// @param runtime the runtime
    /// @param window what it shows first
    /// @param registry_id the registry a mirror is for; empty otherwise
    AddRegistryScreen(Runtime& runtime, settings::AddRegistry window, std::string registry_id)
        : runtime_(runtime), model_(std::move(window)), registry_id_(std::move(registry_id)) {
        settings::add_registry_settle(model_);
        hold.live = this;
        // Over the game's own main menu the window looks as the Library
        // there does.
        if (runtime_.main_menu_opens_game_art_screens())
            if (std::optional<GameMenuArt> art =
                    read_game_menu_art(runtime_.assets_, "LIBRARY", "add-registry"))
                game_look_.emplace(std::move(*art));
    }

    AddRegistryScreen(const AddRegistryScreen&) = delete;
    AddRegistryScreen& operator=(const AddRegistryScreen&) = delete;

    /// Sets the keys the player trusts for the registry a mirror is for.
    ///
    /// @param keys their fingerprints
    void set_mirror_keys(std::vector<std::string> keys) { mirror_keys_ = std::move(keys); }

    /// Forgets itself if it is still the window showing.
    ~AddRegistryScreen() override {
        if (hold.live == this)
            hold.live = nullptr;
    }

    /// Returns "add-registry".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kScreenName; }

    /// Returns where the window shows: centred over the front end at the
    /// view's size class; nowhere in a match or before the fonts are ready.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (placed_on.match || fonts == nullptr)
            return {};
        // In the game's look it covers the room, a point a window pixel.
        if (game_look_) {
            if (placed_on.room.width <= 0 || placed_on.room.height <= 0)
                return {};
            return {placed_on.room, placed_on.room.width, placed_on.room.height, 100};
        }
        settings::AddRegistry sized = model_;
        sized.size_class = placed_on.frame.size_class;
        return centred_placement(
            placed_on,
            settings::add_registry_width(sized.size_class),
            settings::add_registry_height(sized, fonts)
        );
    }

    /// Lays the window out at the view's size class.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (game_look_ && !placed_on.match &&
            game_look_->lay_out({placed_on.room.width, placed_on.room.height}))
            ++revision_;
        const kit::SizeClass size_class =
            placed_on.match ? kit::SizeClass::compact : placed_on.frame.size_class;
        if (model_.size_class == size_class)
            return;
        model_.size_class = size_class;
        ++revision_;
    }

    /// Tells that the window takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the window darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return !game_look_; }

    /// Tells whether the window covers its place: in the game's look it
    /// shades the screen and leaves round its frame clear.
    ///
    /// @return false in the game's look
    [[nodiscard]] bool opaque() const override { return !game_look_; }

    /// Draws the window.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        if (game_look_) {
            game_look_->draw(canvas, model_);
            return;
        }
        settings::draw_add_registry(canvas, model_);
    }

    /// Takes a pointer's move, press or release. A finger's press takes the
    /// control within reach.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button; 1 is the left
    /// @param at the pointer, in the window's points
    /// @param finger_reach a finger's reach, in the window's points; 0 for a mouse
    /// @return what the window did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (game_look_)
            return game_pointer(kind, button, at, finger_reach);
        settings::AddRegistryAction action = settings::AddRegistryAction::none;
        switch (kind) {
        case ScreenInputKind::pointer_move:
            action = settings::add_registry_pointer_move(model_, at, fonts);
            break;
        case ScreenInputKind::pointer_down:
            if (button == 1)
                action = finger_reach > 0
                             ? settings::add_registry_finger_down(model_, at, finger_reach, fonts)
                             : settings::add_registry_pointer_down(model_, at, fonts);
            break;
        case ScreenInputKind::pointer_up:
            if (button == 1)
                action = settings::add_registry_pointer_up(model_, at, fonts);
            break;
        default:
            break;
        }
        return take(action);
    }

    /// Takes a key: Escape cancels, Enter checks the address or presses the
    /// focused button, Space on the field starts typing.
    ///
    /// @param pressed the key
    /// @return what the window did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (game_look_)
            return take(settings::add_registry_key(model_, pressed, game_look_->list(model_)));
        return take(
            settings::add_registry_key(model_, pressed, runtime_.oa_layer().screen_fonts())
        );
    }

    /// Takes a turn of the wheel, which does nothing under the window.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Takes typed or pasted text into the field.
    ///
    /// @param utf8 the text
    /// @return what the window did
    LayerAnswer text(std::string_view utf8) override {
        return take(settings::add_registry_text(model_, utf8));
    }

    /// Returns the field while it takes typing and has the focus.
    ///
    /// @return the field's rectangle, in the window's points
    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        if (game_look_)
            return game_look_->field(model_);
        return settings::add_registry_field(model_);
    }

    /// Returns the window's display list.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        if (game_look_)
            return game_look_->list(model_);
        return settings::add_registry_list(model_, runtime_.oa_layer().screen_fonts());
    }

    /// Returns the window's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return model_.interaction; }

    /// Polls the check the window waits on, and shows what it found. The
    /// window closes, dropping its check, once a game starts.
    ///
    /// @return close in a game; redraw once the check has answered
    LayerAnswer tick() override {
        if (runtime_.screen_ == Screen::match)
            return LayerAnswer::close;
        if (model_.phase == settings::AddRegistry::Phase::ready && !model_.shown.icon_url.empty() &&
            model_.shown.icon_rgba.empty()) {
            // The same picture cache as a badge. Nothing is fetched for the
            // logo the game ships: that address is never stored here.
            if (std::optional<oa::ui::library::Badge> picture =
                    runtime_.library_host().cached_picture(
                        model_.shown.icon_registry, model_.shown.icon_url
                    )) {
                model_.shown.icon_width = picture->width;
                model_.shown.icon_height = picture->height;
                model_.shown.icon_rgba = std::move(picture->rgba);
                ++revision_;
                return LayerAnswer::redraw;
            }
            runtime_.library_host().ask_cached_picture(
                model_.shown.icon_registry, model_.shown.icon_url
            );
        }
        if (model_.phase != settings::AddRegistry::Phase::checking || request_ == 0)
            return LayerAnswer::none;
        const std::optional<content::RegistryCheck> answered =
            runtime_.content_service().registry_check(request_);
        if (!answered)
            return LayerAnswer::none;
        show_check(*answered);
        return LayerAnswer::redraw;
    }

    /// Returns the window's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Drops the check: its answer is never used.
    void close(bool /*by_key*/) override { request_ = 0; }

    /// Checks an .oareg file's bytes, or shows why the file could not be read.
    ///
    /// @param bytes the file's bytes; none when it could not be read whole
    /// @param why why it could not be read
    void check_file(std::optional<std::vector<uint8_t>> bytes, std::string_view why) {
        fallback_address_ = model_.file_name;
        if (!bytes) {
            Named named;
            named.name = model_.file_name;
            named.address = model_.file_name;
            named.detail = std::string(why);
            refuse(refusal_sentence(registry::Refusal::unreadable, named));
            return;
        }
        ask(runtime_.content_service().check_registry_file(std::move(*bytes)), model_.file_name);
    }

    /// Checks the address in the field. An address the reader refuses is
    /// refused here. http and https are both asked of the content service.
    void check_address() {
        const std::optional<url::Url> parsed = url::parse_http_url(model_.url);
        if (!parsed) {
            refuse(std::string(kNotAddressSentence));
            return;
        }
        fallback_address_ = url::host_header(*parsed);
        content::Service& service = runtime_.content_service();
        if (model_.mode == settings::AddRegistry::Mode::mirror) {
            ask(service.check_mirror(registry_id_, url::url_text(*parsed)), fallback_address_);
            return;
        }
        const std::string signing_key = trimmed_signing_key(model_.signing_key);
        if (!signing_key.empty() && !base::signing::parse_public_key(signing_key)) {
            refuse(std::string(kSigningKeyUnreadable));
            return;
        }
        ask(service.check_registry_url(url::url_text(*parsed), signing_key), fallback_address_);
    }

  private:

    /// Takes a pointer's move, press or release in the game's look.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button; 1 is the left
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach, in the screen's points; 0 for a mouse
    /// @return what the window did
    LayerAnswer game_pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) {
        const kit::DisplayList list = game_look_->list(model_);
        kit::Interaction& interaction = model_.interaction;
        switch (kind) {
        case ScreenInputKind::pointer_move:
            return take(settings::add_registry_pointer(model_, kit::pointer_move(interaction, list, at)));
        case ScreenInputKind::pointer_down:
            if (button != 1)
                return LayerAnswer::none;
            return take(settings::add_registry_pointer(
                model_, finger_reach > 0 ? kit::finger_down(interaction, list, at, finger_reach)
                                         : kit::pointer_down(interaction, list, at)
            ));
        case ScreenInputKind::pointer_up:
            if (button != 1)
                return LayerAnswer::none;
            return take(settings::add_registry_pointer(model_, kit::pointer_up(interaction, list, at)));
        default:
            return LayerAnswer::none;
        }
    }

    /// Waits on a check the content service started.
    ///
    /// @param request the check; 0 when it could not start
    /// @param address the address it checks, as the window names it
    void ask(uint64_t request, std::string address) {
        ++hold.checks_asked;
        if (request == 0) {
            Named named;
            named.name = address;
            named.address = address;
            named.detail = "the check could not start";
            refuse(refusal_sentence(registry::Refusal::unreadable, named));
            return;
        }
        request_ = request;
        model_.checking = std::move(address);
        model_.phase = settings::AddRegistry::Phase::checking;
        model_.shown = {};
        settings::add_registry_settle(model_);
        ++revision_;
    }

    /// Shows a refusal, the field editable again for an address.
    ///
    /// @param sentence why
    void refuse(std::string sentence) {
        request_ = 0;
        model_.shown = {};
        model_.shown.refusal = std::move(sentence);
        model_.phase = settings::AddRegistry::Phase::refused;
        settings::add_registry_settle(model_);
        ++revision_;
        log_line("refused: " + model_.shown.refusal);
    }

    /// Returns what a refusal's sentence names for a check.
    ///
    /// @param check the check
    /// @param detail what the content service found
    /// @return the names
    Named named_for(const content::RegistryCheck& check, std::string detail) const {
        Named named;
        named.address = !check.address.empty() ? check.address : fallback_address_;
        if (model_.mode == settings::AddRegistry::Mode::mirror)
            named.name = model_.mirror_of;
        else if (check.descriptor)
            named.name = check.descriptor->name;
        else
            named.name = named.address;
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime_.content_service().snapshot();
        if (snapshot)
            named.built_in_name = built_in_name_for(*snapshot, check);
        named.detail = std::move(detail);
        return named;
    }

    /// Shows what a check found: what would be trusted, or why not.
    ///
    /// @param check the check
    void show_check(const content::RegistryCheck& check) {
        if (check.detail == kNeedsHttps) {
            refuse(std::string(kNeedsHttps));
            return;
        }
        if (check.detail == kSigningKeyUnreadable || check.detail == kCatalogueNotSigned) {
            refuse(std::string(check.detail));
            return;
        }
        if (check.refusal != registry::Refusal::none) {
            refuse(refusal_sentence(check.refusal, named_for(check, check.detail)));
            return;
        }
        settings::ProbeView shown;
        shown.address = !check.address.empty() ? check.address : fallback_address_;
        shown.packages = check.packages;
        for (const auto& [kind, count] : check.kinds)
            if (count > 0)
                shown.kinds.push_back(kind_text(kind));
        if (model_.mode == settings::AddRegistry::Mode::mirror) {
            // The keys OA already trusts for the registry checked the mirror.
            shown.name = model_.mirror_of;
            shown.fingerprints = mirror_keys_;
        } else if (check.descriptor) {
            shown.name = check.descriptor->name;
            shown.fingerprints = check.fingerprints;
            shown.downloads = downloads_text(*check.descriptor);
            shown.is_signed = check.descriptor->is_signed();
            // A catalogue icon, resolved against the host that served the
            // catalogue. The logo the game ships is never copied here: an
            // added descriptor that names it is refused before this preview.
            if (!check.descriptor->shipped_logo && check.catalogue &&
                !check.catalogue->icon.empty() && check.served_from) {
                if (const std::optional<url::Url> resolved =
                        url::resolve_reference(*check.served_from, check.catalogue->icon)) {
                    shown.icon_url = url::url_text(*resolved);
                    shown.icon_registry = check.descriptor->id;
                }
            }
        }
        model_.shown = std::move(shown);
        if (!model_.shown.icon_url.empty())
            runtime_.library_host().ask_cached_picture(
                model_.shown.icon_registry, model_.shown.icon_url
            );
        model_.phase = settings::AddRegistry::Phase::ready;
        settings::add_registry_settle(model_);
        ++revision_;
    }

    /// Carries out what an event asked.
    ///
    /// @param action what it asked
    /// @return what the screen answers
    LayerAnswer take(settings::AddRegistryAction action) {
        switch (action) {
        case settings::AddRegistryAction::none:
            return LayerAnswer::none;
        case settings::AddRegistryAction::redraw:
            ++revision_;
            return LayerAnswer::redraw;
        case settings::AddRegistryAction::start_typing:
            runtime_.oa_layer().restart_text_input();
            ++revision_;
            return LayerAnswer::redraw;
        case settings::AddRegistryAction::check:
            check_address();
            return LayerAnswer::redraw;
        case settings::AddRegistryAction::add:
            return add();
        case settings::AddRegistryAction::cancel:
            request_ = 0;
            return LayerAnswer::close;
        }
        return LayerAnswer::none;
    }

    /// Adds what the check allowed. The content service applies the rules
    /// again: a refusal now, the state having moved since the check, shows
    /// as any other.
    ///
    /// @return close once it is added; redraw when refused
    LayerAnswer add() {
        if (request_ == 0)
            return LayerAnswer::none;
        content::Service& service = runtime_.content_service();
        std::string why;
        const bool mirror = model_.mode == settings::AddRegistry::Mode::mirror;
        const registry::Refusal refusal = mirror ? service.add_checked_mirror(request_, &why)
                                                 : service.add_checked_registry(request_, &why);
        if (refusal != registry::Refusal::none) {
            const std::optional<content::RegistryCheck> check = service.registry_check(request_);
            refuse(refusal_sentence(
                refusal, named_for(check ? *check : content::RegistryCheck{}, std::move(why))
            ));
            return LayerAnswer::redraw;
        }
        if (mirror)
            log_line("mirror added for " + model_.mirror_of + ": " + model_.shown.address);
        else
            log_line("added " + model_.shown.name + ": " + model_.shown.address);
        request_ = 0;
        // The new registry is listed at once when the Library is open, and
        // a refusal shown before this add is cleared.
        runtime_.finish_registry_edit({});
        return LayerAnswer::close;
    }

    Runtime& runtime_;                     ///< the runtime
    settings::AddRegistry model_;          ///< what the window shows
    std::optional<AddRegistryGameLook> game_look_; ///< its look over the game's own main menu; none for its own
    std::string registry_id_;              ///< the registry a mirror is for
    std::vector<std::string> mirror_keys_; ///< that registry's trusted keys
    std::string fallback_address_;         ///< the address checked, before a descriptor names one
    uint64_t request_{};                   ///< the check the window waits on; 0 for none
    uint64_t revision_{};                  ///< counts the changes to what it shows
};

void Runtime::open_add_registry(std::optional<std::string> address) {
    if (hold.live != nullptr)
        return;
    settings::AddRegistry window;
    window.mode = settings::AddRegistry::Mode::by_url;
    if (address) {
        window.url = *address;
        window.caret = window.url.size();
    }
    auto screen = std::make_unique<AddRegistryScreen>(*this, std::move(window), std::string());
    AddRegistryScreen& shown = *screen;
    oa_layer().push(std::move(screen));
    if (address)
        shown.check_address();
}

void Runtime::open_add_mirror(std::string_view registry_id) {
    if (hold.live != nullptr)
        return;
    const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot();
    const content::RegistryView* view = snapshot ? view_of(*snapshot, registry_id) : nullptr;
    if (view == nullptr || view->registry.origin == registry::Origin::built_in) {
        log_line("no mirror is offered for " + std::string(registry_id));
        return;
    }
    settings::AddRegistry window;
    window.mode = settings::AddRegistry::Mode::mirror;
    window.mirror_of = view->registry.descriptor.name;
    auto screen =
        std::make_unique<AddRegistryScreen>(*this, std::move(window), std::string(registry_id));
    screen->set_mirror_keys(view->fingerprints);
    oa_layer().push(std::move(screen));
}

void Runtime::take_registry_files() {
    namespace install = oa::app::package_install;
    namespace frontend_state = oa::ui::frontend_state;
    if (!install::registry_files_waiting() || hold.live != nullptr)
        return;
    // One at a time, on the settled main menu, with nothing taking every
    // input over it and no game running.
    if (screen_ != Screen::main_menu || match_ != nullptr || frame_owned_by_package() ||
        state_.state != frontend_state::state_id::main_menu ||
        state_.pending_signal == frontend_state::signal_id::multiplayer || saves_notice_shown() ||
        oa::ui::frontend_dialogs::dialog_count() != 0 || oa_layer().modal_shown(false))
        return;
    if (UserFolderState::unwatched(options_.unattended) && !options_.check_registries)
        return;
    const std::optional<std::filesystem::path> taken = install::take_registry_file();
    if (!taken)
        return;
    log_line("opening " + path_to_utf8(*taken));

    // The platform may bring a file the system opened into the game's own
    // storage first.
    std::filesystem::path readable = *taken;
    std::string why;
    bool brought = true;
    const PlatformHooks& hooks = platform_hooks();
    if (hooks.take_opened_file != nullptr) {
        std::string copy;
        brought = hooks.take_opened_file(hooks.context, path_to_utf8(*taken).c_str(), &copy, &why);
        if (brought && !copy.empty())
            readable = path_from_utf8(copy);
    }
    // The whole file, and no more than a descriptor may hold.
    std::optional<std::vector<uint8_t>> bytes;
    if (brought) {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(readable, error);
        if (error) {
            why = "it cannot be read";
        } else if (size > registry::max_descriptor_bytes) {
            why = "it is larger than " + std::to_string(registry::max_descriptor_bytes / 1024) +
                  " KiB";
        } else {
            std::ifstream in(readable, std::ios::binary);
            std::vector<uint8_t> contents(static_cast<std::size_t>(size));
            if (in &&
                in.read(
                    reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(size)
                ))
                bytes = std::move(contents);
            else
                why = "it cannot be read";
        }
    }
    if (brought && hooks.release_opened_file != nullptr && readable != *taken)
        hooks.release_opened_file(hooks.context, path_to_utf8(readable).c_str());

    settings::AddRegistry window;
    window.mode = settings::AddRegistry::Mode::from_file;
    window.file_name = path_to_utf8(taken->filename());
    auto screen = std::make_unique<AddRegistryScreen>(*this, std::move(window), std::string());
    AddRegistryScreen& shown = *screen;
    oa_layer().push(std::move(screen));
    shown.check_file(std::move(bytes), why);
}

uint64_t Runtime::registry_checks_asked() const noexcept {
    return hold.checks_asked;
}

namespace {

/// Returns the sentence the Registries tab shows when a change is refused,
/// and writes the refusal to the log. Empty when the change was kept.
///
/// @param id the registry's id
/// @param refusal what the content service answered
/// @param why the content service's own words; empty to use the refusal's
/// @return the sentence; empty when nothing was refused
std::string refused_notice(std::string_view id, registry::Refusal refusal, const std::string& why) {
    if (refusal == registry::Refusal::none)
        return {};
    const std::string reason = why.empty() ? std::string(registry::refusal_text(refusal)) : why;
    log_line(
        std::string(id) + ": not changed: " + std::string(registry::refusal_text(refusal)) +
        (why.empty() ? std::string() : " (" + why + ")")
    );
    std::string notice = "OA could not change the registries: " + reason;
    while (!notice.empty() && notice.back() == '.')
        notice.pop_back();
    notice += '.';
    return notice;
}

} // namespace

void Runtime::finish_registry_edit(std::string notice) {
    hold.notice = std::move(notice);
    if (library_)
        library_->rebuild(*this);
}

std::string Runtime::registry_edit_notice() const {
    return hold.notice;
}

void Runtime::change_library_registry(std::string_view id, bool on) {
    std::string why;
    const registry::Refusal refusal = content_service().set_registry_enabled(id, on, &why);
    finish_registry_edit(refused_notice(id, refusal, why));
}

void Runtime::remove_library_registry(std::string_view id) {
    std::string why;
    const registry::Refusal refusal = content_service().remove_registry(id, &why);
    finish_registry_edit(refused_notice(id, refusal, why));
}

} // namespace oa::app
