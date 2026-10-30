// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GET MAP over the battle room, on the OA layer: one screen, get-map, that is
// not modal, has no backdrop and is not opaque, kept on the layer while the
// multiplayer screens show (Runtime::keep_get_map_screen, once a frame from
// OaLayer::tick). When the host's presence record names the map pack of the
// map it plays and this machine lacks that release, it draws one line by the
// battle room's map name saying what GET MAP fetches, and the button, or the
// download's bar while it downloads. The press offers to add the pack's
// registry first (the Add registry window, which adds only on the player's
// ADD), then queues the host's release, which the queue checks against the
// catalogue's SHA-256, the record's, and the installer puts in place. The
// pack's maps are not mounted until the install ends; the battle room then
// finds the map itself. Nothing readies the player. In a run nobody watches
// the installer takes a catalogue map pack while GET MAP shows
// (ModInstallState::get_map_shows).

#include "map_packs.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace mp = oa::ui::frontend_multiplayer;
namespace kit = oa::ui::kit;
namespace place = oa::ui::frontend_state;

/// GET MAP's screen's name on the OA layer.
constexpr std::string_view get_map_screen_name = "get-map";
/// The button's name, which automation lists as oa.get-map.
constexpr std::string_view get_map_button_name = "get-map";
/// The line's name, which automation lists as oa.get-map-text.
constexpr std::string_view get_map_text_name = "get-map-text";
/// The download's bar's name, which automation lists as oa.get-map-progress.
constexpr std::string_view get_map_progress_name = "get-map-progress";
/// The Add registry window's screen's name on the OA layer.
constexpr std::string_view add_registry_screen_name = "add-registry";

/// The controls' numbers.
constexpr kit::ControlId get_map_button_control = 1;
constexpr kit::ControlId get_map_text_control = 2;
constexpr kit::ControlId get_map_progress_control = 3;

/// The rows of the 640 by 480 picture between the button's bottom and the
/// map name's top: the button stands above the name's box, right-aligned with it.
constexpr int32_t button_gap_rows = 8;
/// The rows of the picture between the map name's bottom and the line's top:
/// the line stands under the name's box.
constexpr int32_t line_gap_rows = 6;
/// The columns of the picture, from the map name's left edge, the line may
/// take: past the chat's box below it, up to the options' column on the right.
constexpr int32_t line_room_columns = 388;
/// A whole bar, in thousandths.
constexpr int32_t bar_whole = 1000;
/// The thousandths of a bar a percent takes.
constexpr int32_t bar_per_percent = 10;

/// Writes a line to standard error, which the log keeps.
///
/// @param line the line
void log_line(std::string_view line) {
    std::cerr << "open-annihilation: GET MAP: " << line << '\n';
}

/// Returns a digest as 64 lower-case hex digits.
///
/// @param digest the digest
/// @return the text
std::string hex_text(const oa::base::sha256::Digest& digest) {
    const auto hex = oa::base::sha256::to_hex(digest);
    return std::string(hex.data(), hex.size());
}

/// Tells whether two ASCII texts are the same, case ignored, as package keys are.
///
/// @param first a text
/// @param second another
/// @return true when they are the same
bool same_key(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size())
        return false;
    for (std::size_t at = 0; at < first.size(); ++at) {
        const auto lower = [](char letter) {
            return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
        };
        if (lower(first[at]) != lower(second[at]))
            return false;
    }
    return true;
}

/// Tells whether a queue item has ended: done, failed or cancelled.
///
/// @param stage where it stands
/// @return true when it has ended
bool ended(content::DownloadState stage) noexcept {
    return stage == content::DownloadState::done || stage == content::DownloadState::failed ||
           stage == content::DownloadState::cancelled;
}

/// Tells whether a registry may be used: on, not set aside for a built-in
/// registry of its id, and allowed by Developer Mode.
///
/// @param registry the registry
/// @return true when it may be used
bool registry_usable(const content::RegistryView& registry) noexcept {
    return registry.registry.enabled && !registry.registry.conflicting &&
           registry.status != content::RegistryStatus::disabled &&
           registry.status != content::RegistryStatus::needs_developer_mode &&
           registry.status != content::RegistryStatus::conflicting;
}

/// Tells whether downloads from a registry are on: its install ID is not
/// off where the registry needs one (Settings › Downloads).
///
/// @param values the preference values
/// @param registry the registry
/// @return true when they are on
bool registry_downloads_on(
    const std::map<std::string, std::string>& values, const content::RegistryView& registry
) {
    if (registry.registry.descriptor.install_id != oa::data::registry::InstallIdUse::required)
        return true;
    return content::read_install_id(values, registry.registry.descriptor.id).state !=
           content::InstallIdState::off;
}

/// Returns the registry of an id in a snapshot: the one in effect when an
/// added registry of that id is set aside for a built-in one.
///
/// @param snapshot the snapshot
/// @param id the registry's id
/// @return the registry; null when the player has none of that id
const content::RegistryView* registry_of(const content::Snapshot& snapshot, std::string_view id) {
    for (const content::RegistryView& registry : snapshot.registries)
        if (registry.registry.descriptor.id == id && !registry.registry.conflicting)
            return &registry;
    return nullptr;
}

/// Returns the catalogue entry of a map pack in a snapshot.
///
/// @param snapshot the snapshot
/// @param host the host's pack, whose registry and id are looked for
/// @return the entry; null when the catalogues in effect do not list it
const oa::data::catalogue::Package*
listed_pack(const content::Snapshot& snapshot, const mp::HostPack& host) {
    for (const content::Entry& entry : snapshot.entries)
        if (entry.package != nullptr && entry.registry == host.registry_id &&
            entry.package->kind == oa::data::catalogue::Kind::oamap &&
            same_key(entry.package->id, host.id))
            return entry.package;
    return nullptr;
}

/// A pack's id and release, which GET MAP's state is kept for.
///
/// @param host the host's pack
/// @return the id and the SHA-256
std::string pack_key(const mp::HostPack& host) {
    return host.id + "\n" + host.sha256;
}

/// What GET MAP shows, in the layer's points.
struct Shown {
    mp::GetMap stage{};    ///< the state
    std::string line;      ///< the line, whole
    std::string drawn;     ///< the line as drawn: cut to its room
    kit::Colour colour{};  ///< the line's colour
    kit::Rect line_rect{}; ///< the line
    std::string caption;   ///< the button's caption
    kit::Rect button{};    ///< the button, or the bar in its place
    bool button_shown{};   ///< the button shows
    bool bar_shown{};      ///< the bar shows in the button's place
    int32_t bar_done{};    ///< how much of the bar is filled, in thousandths
    kit::Rect box{};       ///< the screen's place: the line and the button or bar
};

/// Tells whether two rectangles are the same.
///
/// @param first a rectangle
/// @param second another
/// @return true when they are
bool same_rect(const kit::Rect& first, const kit::Rect& second) noexcept {
    return first.x == second.x && first.y == second.y && first.width == second.width &&
           first.height == second.height;
}

/// Tells whether GET MAP shows the same thing twice.
///
/// @param first one look; none shows nothing
/// @param second another
/// @return true when every part is the same
bool same_shown(const std::optional<Shown>& first, const std::optional<Shown>& second) {
    if (!first || !second)
        return first.has_value() == second.has_value();
    return first->stage == second->stage && first->line == second->line &&
           first->drawn == second->drawn && first->colour == second->colour &&
           same_rect(first->line_rect, second->line_rect) && first->caption == second->caption &&
           same_rect(first->button, second->button) &&
           first->button_shown == second->button_shown && first->bar_shown == second->bar_shown &&
           first->bar_done == second->bar_done && same_rect(first->box, second->box);
}

} // namespace

/// GET MAP as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels from the window's top left corner, and its place's corner is that
/// many points along. It shows only while there is something to say: the
/// line and the button or the bar, placed by the battle room's map name;
/// elsewhere it does not show.
struct Runtime::GetMapScreen final : LayerScreen {
    /// Makes the screen.
    ///
    /// @param runtime the runtime, whose packs, registries and queue it reads
    explicit GetMapScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "get-map".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return get_map_screen_name; }

    /// Returns where the screen shows: the line and the button or bar, by the
    /// battle room's map name; nowhere while there is nothing to show, and
    /// never in a match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place, at the view's scale
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || !shown_)
            return {};
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Rect at = kit::to_canvas(shown_->box, percent);
        return {{at.x, at.y, at.width, at.height}, shown_->box.width, shown_->box.height, percent};
    }

    /// Tells that the screen lets what it does not take go on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the screen darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the screen covers only its line and its button or bar.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Draws the line and the button or bar with the kit, from the place's corner.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, display_list()); }

    /// Follows the view: the layer's scale, size class and the picture's
    /// place, by which the map name is mapped to the layer's points.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (placed_on.match)
            return;
        view_ = placed_on;
        have_view_ = true;
        refresh();
    }

    /// Takes a press of the button, by the pointer or a finger, and its
    /// release; lets every other pointer input go on to the battle room.
    ///
    /// A mouse presses the drawn rectangle. A finger presses by its reach
    /// when no battle-room gadget, the map name included, is nearer the
    /// point. A point inside a gadget stays that gadget's.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return redraw or none for the button's press and release; pass otherwise
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const kit::DisplayList list = display_list();
        const uint64_t before = revision_;
        LayerAnswer answer = LayerAnswer::pass;
        if (kind == ScreenInputKind::pointer_move) {
            if (kit::pointer_move(interaction_, list, at).result != kit::PointerResult::none)
                ++revision_;
        } else if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            bool yield = false;
            if (finger_reach > 0 && shown_ && shown_->button_shown) {
                const kit::Rect drawn{
                    shown_->button.x - shown_->box.x,
                    shown_->button.y - shown_->box.y,
                    shown_->button.width,
                    shown_->button.height
                };
                const place::PlaceRect button_place{drawn.x, drawn.y, drawn.width, drawn.height};
                yield = place::nearer_gadget(at.x, at.y, button_place, room_gadgets());
            }
            const kit::PointerOutcome outcome =
                finger_reach > 0 && !yield ? kit::finger_down(interaction_, list, at, finger_reach)
                                           : kit::pointer_down(interaction_, list, at);
            if (interaction_.pressed == get_map_button_control) {
                ++revision_;
                answer = LayerAnswer::redraw;
            } else if (outcome.result != kit::PointerResult::none) {
                ++revision_;
            }
        } else if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT) {
            const bool held = interaction_.pressed == get_map_button_control;
            const kit::PointerOutcome outcome = kit::pointer_up(interaction_, list, at);
            if (held) {
                ++revision_;
                answer = LayerAnswer::redraw;
                if (outcome.result == kit::PointerResult::activated &&
                    outcome.control == get_map_button_control)
                    press();
            }
        }
        if (answer == LayerAnswer::pass)
            return answer;
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Lets every key go on to the battle room.
    ///
    /// @return pass
    LayerAnswer key(kit::Key /*pressed*/, uint32_t /*sdl_key*/) override {
        return LayerAnswer::pass;
    }

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// Returns the line, its name for automation, and the button or the bar,
    /// from the place's corner.
    ///
    /// @return the list; empty while there is nothing to show
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        if (!shown_)
            return list;
        const kit::Rect& box = shown_->box;
        const auto moved = [&box](kit::Rect rect) {
            rect.x -= box.x;
            rect.y -= box.y;
            return rect;
        };
        if (shown_->button_shown) {
            kit::ButtonLook look{};
            look.caption = shown_->caption;
            look.style = kit::ButtonStyle::accent;
            look.hovered = interaction_.hovered == get_map_button_control;
            look.held = interaction_.pressed == get_map_button_control && look.hovered;
            kit::add_button(
                list,
                moved(shown_->button),
                look,
                get_map_button_control,
                std::string(get_map_button_name)
            );
        }
        if (shown_->bar_shown) {
            kit::add_progress(list, moved(shown_->button), shown_->bar_done, bar_whole);
            kit::Control bar;
            bar.id = get_map_progress_control;
            bar.rect = moved(shown_->button);
            bar.enabled = false;
            bar.focusable = false;
            bar.name = std::string(get_map_progress_name);
            bar.kind = kit::ControlKind::area;
            bar.text = std::to_string(shown_->bar_done / bar_per_percent) + "%";
            list.controls.push_back(std::move(bar));
        }
        kit::Item words;
        words.role = kit::Role::text;
        words.rect = moved(shown_->line_rect);
        words.text = shown_->drawn;
        words.font = kit::FontRole::small;
        words.align = kit::Align::left;
        words.colour = shown_->colour;
        list.items.push_back(std::move(words));
        kit::Control said;
        said.id = get_map_text_control;
        said.rect = moved(shown_->line_rect);
        said.enabled = false;
        said.focusable = false;
        said.name = std::string(get_map_text_name);
        said.kind = kit::ControlKind::area;
        said.text = shown_->line;
        list.controls.push_back(std::move(said));
        return list;
    }

    /// Returns the button's hover and press.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Brings GET MAP up to date once a frame: closes the screen when the
    /// multiplayer screens go, follows the download, goes on once a registry
    /// the player added lists the host's release, and reads the battle room,
    /// the packs and the registries again.
    ///
    /// @return close when the multiplayer screens no longer show; redraw
    ///     when what it shows changed; otherwise none
    LayerAnswer tick() override {
        if (!mp::multiplayer_showing())
            return LayerAnswer::close;
        const uint64_t before = revision_;
        refresh();
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns a number that changes with what the screen shows.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close: a download goes on, and its pack's
    /// maps stay held until it ends. The rectangle it was showing is cleared.
    void close(bool /*by_key*/) override { runtime_.get_map_shown_box_ = {}; }

  private:

    /// Reads the battle room, the packs, the registries and the queue, decides
    /// the state and lays out what it shows; counts a change.
    void refresh() {
        std::optional<Shown> next;
        mp::BattleRoomMap room;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        const bool joining = have_view_ && fonts != nullptr &&
                             mp::multiplayer_battle_room_map(&room) && room.joiner &&
                             room.host_pack;
        std::vector<content::DownloadView> items;
        if (joining || item_ != 0)
            items = runtime_.content_downloads().view();
        follow_item(items);
        if (joining) {
            host_ = *room.host_pack;
            next = decide(room, items, *fonts);
        } else {
            pending_.clear();
        }
        if (same_shown(next, shown_)) {
            remember_box();
            return;
        }
        shown_ = std::move(next);
        if (!shown_ || !shown_->button_shown)
            interaction_ = {};
        remember_box();
        ++revision_;
    }

    /// Stores the rectangle GET MAP is showing, or clears it while nothing shows.
    void remember_box() {
        if (!shown_ || shown_->box.width <= 0 || shown_->box.height <= 0) {
            runtime_.get_map_shown_box_ = {};
            return;
        }
        runtime_.get_map_shown_box_ = {
            shown_->box.x, shown_->box.y, shown_->box.width, shown_->box.height
        };
    }

    /// Decides what GET MAP shows for the host's pack, going on with a fetch
    /// that waited for its registry to be added.
    ///
    /// @param room the battle room as the joiner sees it
    /// @param items the download queue's items
    /// @param fonts the layer's fonts
    /// @return what it shows; none for the state none
    std::optional<Shown> decide(
        const mp::BattleRoomMap& room,
        const std::vector<content::DownloadView>& items,
        const kit::Fonts& fonts
    ) {
        const std::string release_key = pack_key(host_);
        const mp::LocalPack local = runtime_.local_map_pack(host_.id, host_.sha256);
        mp::Offer offer;
        uint64_t done = 0;
        bool downloading = false;
        bool refreshing = false;
        read_offer(offer, refreshing);
        // A download of the pack that has not ended: GET MAP's, or the Library's.
        for (const content::DownloadView& item : items) {
            if (ended(item.state) || item.target.registry != host_.registry_id ||
                !same_key(item.target.key, host_.id))
                continue;
            downloading = true;
            done = item.done;
            if (offer.bytes == 0)
                offer.bytes = item.target.size;
        }
        const bool failed = !failure_.empty() && failure_for_ == release_key;
        mp::GetMap stage =
            mp::get_map_state(room.host_map, room.host_pack, local, offer, downloading, failed);
        if (!pending_.empty() && pending_ != release_key)
            pending_.clear();
        if (!pending_.empty()) {
            if (stage == mp::GetMap::get) {
                // The registry the player added lists the host's release.
                pending_.clear();
                fetch();
                const std::vector<content::DownloadView> queued =
                    runtime_.content_downloads().view();
                return decide(room, queued, fonts);
            }
            if (stage == mp::GetMap::downloading && offer.registry_known &&
                !offer.catalogue_loaded && !refreshing && !refresh_asked_) {
                // The player asked for the pack: its registry's catalogue is read now.
                refresh_asked_ = true;
                runtime_.content_service().refresh(
                    content::RefreshReason::check_now, host_.registry_id
                );
            }
            const bool waiting = stage == mp::GetMap::downloading ||
                                 (stage == mp::GetMap::add_registry_first &&
                                  runtime_.oa_layer().find(add_registry_screen_name) != nullptr);
            if (!waiting)
                pending_.clear();
        }
        if (stage == mp::GetMap::none)
            return std::nullopt;
        return lay_out_shown(room, stage, local, offer, done, fonts);
    }

    /// Reads what the host pack's registry offers.
    ///
    /// @param[out] offer the offer
    /// @param[out] refreshing the registry's catalogue is being fetched
    void read_offer(mp::Offer& offer, bool& refreshing) {
        content::Service& service = runtime_.content_service();
        const std::shared_ptr<const content::Snapshot> snapshot = service.snapshot();
        if (snapshot == nullptr)
            return;
        const content::RegistryView* registry = registry_of(*snapshot, host_.registry_id);
        if (registry == nullptr)
            return;
        offer.registry_known = true;
        offer.registry_name = registry->registry.descriptor.name;
        offer.registry_enabled = registry_usable(*registry);
        offer.downloads_on = !service.content_folder().empty() &&
                             registry_downloads_on(runtime_.preference_values_, *registry);
        // A refresh that found nothing has read the catalogue too: it lists nothing.
        offer.catalogue_loaded =
            registry->catalogue != nullptr ||
            (registry->status == content::RegistryStatus::failed && !registry->refreshing);
        refreshing = registry->refreshing;
        if (const oa::data::catalogue::Package* listed = listed_pack(*snapshot, host_)) {
            offer.listed = true;
            offer.sha256 = hex_text(listed->sha256);
            offer.pack_name = listed->name;
            offer.version = listed->version;
            offer.release = listed->release;
            offer.bytes = listed->size;
        }
    }

    /// Lays out the line and the button or bar for a state, by the battle
    /// room's map name mapped from the picture to the layer's points.
    ///
    /// @param room the battle room as the joiner sees it
    /// @param stage the state
    /// @param local this machine's pack
    /// @param offer what the registry offers
    /// @param done how much of the download has arrived, in bytes
    /// @param fonts the layer's fonts
    /// @return what it shows
    Shown lay_out_shown(
        const mp::BattleRoomMap& room,
        mp::GetMap stage,
        const mp::LocalPack& local,
        const mp::Offer& offer,
        uint64_t done,
        const kit::Fonts& fonts
    ) const {
        const kit::Metrics& metrics = kit::metrics_of(view_.frame.size_class);
        Shown next;
        next.stage = stage;
        const std::string why = failure_for_ == pack_key(host_) ? failure_ : std::string();
        next.line = mp::get_map_text(stage, host_, local, offer, done, why);
        const bool warning = stage == mp::GetMap::other_release_get ||
                             stage == mp::GetMap::other_release_gone || stage == mp::GetMap::gone ||
                             stage == mp::GetMap::downloads_off || stage == mp::GetMap::failed;
        next.colour = warning ? kit::colour::lock : kit::colour::hint;
        next.button_shown = mp::get_map_button_shown(stage);
        next.bar_shown = stage == mp::GetMap::downloading;
        if (next.bar_shown && offer.bytes > 0) {
            const uint64_t filled = std::min(done, offer.bytes) * bar_whole / offer.bytes;
            next.bar_done = static_cast<int32_t>(filled);
        }

        const int32_t left = to_points(room.label_x, room.label_y, false).x;
        const kit::Point label_end =
            to_points(room.label_x + room.label_width, room.label_y + room.label_height, true);
        const int32_t button_bottom =
            to_points(room.label_x, room.label_y - button_gap_rows, false).y;
        const int32_t line_top =
            to_points(room.label_x, room.label_y + room.label_height + line_gap_rows, true).y;
        const int32_t line_end = to_points(room.label_x + line_room_columns, room.label_y, true).x;

        next.caption = mp::get_map_caption();
        const int32_t button_width = std::max(
            metrics.button_width,
            kit::text_width(fonts, kit::FontRole::regular, next.caption) +
                metrics.prompt_button_padding
        );
        next.button = {
            label_end.x - button_width,
            button_bottom - metrics.button_height,
            button_width,
            metrics.button_height
        };
        const int32_t room_width = std::max(line_end - left, int32_t{1});
        next.drawn = kit::cut_to_width(fonts, kit::FontRole::small, next.line, room_width);
        const int32_t drawn_width =
            std::min(kit::text_width(fonts, kit::FontRole::small, next.drawn), room_width);
        next.line_rect = {left, line_top, std::max(drawn_width, int32_t{1}), metrics.small_line};

        const kit::Rect& line_rect = next.line_rect;
        kit::Rect box = line_rect;
        if (next.button_shown || next.bar_shown) {
            const int32_t box_left = std::min(line_rect.x, next.button.x);
            const int32_t box_top = std::min(line_rect.y, next.button.y);
            const int32_t box_right =
                std::max(line_rect.x + line_rect.width, next.button.x + next.button.width);
            const int32_t box_bottom =
                std::max(line_rect.y + line_rect.height, next.button.y + next.button.height);
            box = {box_left, box_top, box_right - box_left, box_bottom - box_top};
        }
        next.box = box;
        return next;
    }

    /// Returns every battle-room gadget with an area, in the screen's points.
    ///
    /// The map name is among them. Hidden controls that still have an area
    /// are kept. Empty while the line is not showing.
    ///
    /// @return the gadgets
    [[nodiscard]] std::vector<place::PlaceRect> room_gadgets() const {
        std::vector<place::PlaceRect> gadgets;
        if (!shown_ || !have_view_)
            return gadgets;
        const std::vector<mp::BattleRoomControl> controls = mp::multiplayer_battle_room_controls();
        gadgets.reserve(controls.size());
        for (const mp::BattleRoomControl& control : controls) {
            if (control.width <= 0 || control.height <= 0)
                continue;
            const kit::Point origin = to_points(control.x, control.y, false);
            const kit::Point end =
                to_points(control.x + control.width, control.y + control.height, true);
            const place::PlaceRect rect{
                origin.x - shown_->box.x,
                origin.y - shown_->box.y,
                end.x - origin.x,
                end.y - origin.y
            };
            if (rect.width > 0 && rect.height > 0)
                gadgets.push_back(rect);
        }
        return gadgets;
    }

    /// Maps a point of the 640 by 480 picture to the layer's points, through
    /// the picture's place in the window.
    ///
    /// @param column the picture's column
    /// @param row the picture's row
    /// @param up true rounds up, as for a right or bottom edge; false rounds down
    /// @return the point
    [[nodiscard]] kit::Point to_points(int32_t column, int32_t row, bool up) const {
        if (view_.picture_size.x <= 0 || view_.picture_size.y <= 0)
            return {column, row};
        const int64_t percent = std::max(view_.scale_percent, int32_t{100});
        const auto& picture = view_.picture;
        const int64_t across =
            picture.x + static_cast<int64_t>(column) * picture.width / view_.picture_size.x;
        const int64_t down =
            picture.y + static_cast<int64_t>(row) * picture.height / view_.picture_size.y;
        const auto points = [percent, up](int64_t pixels) {
            const int64_t scaled = pixels * 100;
            if (up)
                return static_cast<int32_t>(
                    scaled >= 0 ? (scaled + percent - 1) / percent : -(-scaled / percent)
                );
            return static_cast<int32_t>(
                scaled >= 0 ? scaled / percent : -((-scaled + percent - 1) / percent)
            );
        };
        return {points(across), points(down)};
    }

    /// Acts on a press of GET MAP: offers to add the registry first, or
    /// fetches the host's release, again after a failure.
    void press() {
        if (!shown_)
            return;
        switch (shown_->stage) {
        case mp::GetMap::add_registry_first:
            // The Add registry window checks the address at once and adds the
            // registry only when the player presses its ADD; GET MAP goes on
            // once the registry lists the host's release.
            pending_ = pack_key(host_);
            refresh_asked_ = false;
            log_line("offering to add the registry " + host_.registry_id + " for " + host_.id);
            runtime_.open_add_registry(host_.registry_url);
            break;
        case mp::GetMap::get:
        case mp::GetMap::other_release_get:
        case mp::GetMap::failed:
            failure_.clear();
            failure_for_.clear();
            fetch();
            break;
        case mp::GetMap::none:
        case mp::GetMap::other_release_gone:
        case mp::GetMap::gone:
        case mp::GetMap::downloads_off:
        case mp::GetMap::downloading:
            break;
        }
        refresh();
    }

    /// Queues the host's release, as the catalogue lists it at the record's
    /// SHA-256, releases the mounted layer when it is this pack's, and holds
    /// the pack's maps from being mounted until the install ends.
    void fetch() {
        const std::string release_key = pack_key(host_);
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime_.content_service().snapshot();
        const oa::data::catalogue::Package* listed =
            snapshot != nullptr ? listed_pack(*snapshot, host_) : nullptr;
        // The catalogue's release must be the host's: the queue checks the
        // file against the catalogue's SHA-256.
        if (listed == nullptr || hex_text(listed->sha256) != host_.sha256) {
            fail(release_key, content::failure_text(content::DownloadFailure::not_offered));
            return;
        }
        std::string why;
        const std::optional<uint64_t> queued = runtime_.queue_download(
            host_.registry_id, listed->id, content::DownloadReason::offered_in_lobby, &why
        );
        if (!queued) {
            fail(release_key, why);
            return;
        }
        // Another release's files may be mounted under the same map name:
        // they leave before the install replaces that release's folder.
        if (const PackMap* mounted = runtime_.mounted_pack_map();
            mounted != nullptr && mounted->id == host_.id)
            runtime_.release_pack_map();
        runtime_.hold_pack_maps(host_.id, *queued);
        item_ = *queued;
        item_for_ = release_key;
        log_line(
            "downloading " + host_.id + " release " + std::to_string(host_.release) + " from " +
            host_.registry_id
        );
    }

    /// Remembers why a fetch failed, for the line.
    ///
    /// @param release_key the pack and release it was for
    /// @param reason why, in English as the queue says it
    void fail(const std::string& release_key, std::string_view reason) {
        failure_for_ = release_key;
        failure_ = std::string(oa::data::languages::interface_text(reason));
        if (failure_.empty())
            failure_ = std::string(
                oa::data::languages::interface_text(
                    content::failure_text(content::DownloadFailure::refused)
                )
            );
        log_line("couldn't get " + host_.id + ": " + std::string(reason));
    }

    /// Follows GET MAP's own download: once it ends, its pack's maps may be
    /// mounted again, and a failure is kept for the line with its reason,
    /// the installer's own when it gave one, and logged. A cancelled
    /// download leaves GET MAP as it was before the press.
    ///
    /// @param items the download queue's items
    void follow_item(const std::vector<content::DownloadView>& items) {
        if (item_ == 0)
            return;
        const auto found =
            std::find_if(items.begin(), items.end(), [this](const content::DownloadView& item) {
                return item.item == item_;
            });
        if (found != items.end() && !ended(found->state))
            return;
        if (found != items.end() && found->state == content::DownloadState::failed) {
            failure_for_ = item_for_;
            failure_ = content::failure_reason(*found, &oa::data::languages::interface_text);
            log_line(
                "couldn't get " + item_for_.substr(0, item_for_.find('\n')) + ": " +
                content::failure_reason(*found, nullptr)
            );
        }
        if (found != items.end() && found->state == content::DownloadState::done)
            log_line("installed " + item_for_.substr(0, item_for_.find('\n')));
        // The hold lets go of an ended item when it is asked.
        (void)runtime_.pack_maps_held(item_for_.substr(0, item_for_.find('\n')));
        item_ = 0;
        item_for_.clear();
        ++revision_;
    }

    Runtime& runtime_;               ///< the runtime
    LayerView view_{};               ///< the view it was last laid out for
    bool have_view_{};               ///< view_ holds a view of the front end
    mp::HostPack host_{};            ///< the host's pack, as last read
    std::optional<Shown> shown_{};   ///< what it shows; none while there is nothing to show
    kit::Interaction interaction_{}; ///< the button's hover and press
    std::string pending_{};          ///< the pack waiting for its registry to be added
    bool refresh_asked_{};           ///< a refresh of that registry's catalogue was asked
    uint64_t item_{};                ///< GET MAP's own queue item; 0 for none
    std::string item_for_{};         ///< the pack and release it fetches
    std::string failure_{};          ///< why the last fetch failed, in the language shown
    std::string failure_for_{};      ///< the pack and release it failed for
    uint64_t revision_{};            ///< counts the changes to what it shows
};

void Runtime::keep_get_map_screen() {
    if (screen_ == Screen::match || !oa::ui::frontend_multiplayer::multiplayer_showing())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(get_map_screen_name) != nullptr)
        return;
    layer.push(std::make_unique<GetMapScreen>(*this));
}

bool Runtime::ModInstallState::get_map_shows(const Runtime& runtime) {
    // The battle room shows with no dialog over it, and GET MAP has a place
    // there: it shows nowhere while it has nothing to say.
    mp::BattleRoomMap room;
    if (!runtime.oa_layer_ || !mp::multiplayer_battle_room_map(&room))
        return false;
    const LayerPlacement placed = runtime.oa_layer_->placement_of(get_map_screen_name);
    return placed.shown.width > 0 && placed.shown.height > 0;
}

Runtime::ShownBox Runtime::get_map_box() const noexcept {
    return get_map_shown_box_;
}

oa::ui::frontend_multiplayer::LocalPack
Runtime::local_map_pack(std::string_view id, std::string_view sha256) const {
    oa::ui::frontend_multiplayer::LocalPack local;
    for (const PackMap& map : map_packs().maps()) {
        if (map.id != id)
            continue;
        // The release asked for wins over another of the same id.
        if (local.installed && map.sha256 != sha256)
            continue;
        local.installed = true;
        local.sha256 = map.sha256;
        local.registry_id = map.registry;
        local.release = map.release;
        if (map.sha256 == sha256)
            break;
    }
    return local;
}

bool Runtime::registry_fetches(const content::RegistryView& view) const {
    return registry_usable(view) && registry_downloads_on(preference_values_, view);
}

} // namespace oa::app
