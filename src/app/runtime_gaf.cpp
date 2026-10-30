// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unit naming, GAF sequence helpers and game cursors.
#include "oa/app/runtime.hpp"
#include "oa/app/asset_files.hpp"
#include "oa_layer.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "world_draws.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {

std::size_t Runtime::match_view_side() const {
    const auto view = match_view_player();
    return view < skirmish_settings_.slots.size()
               ? static_cast<std::size_t>(skirmish_settings_.slots[view].side)
               : 0;
}

std::string Runtime::match_side_prefix() const {
    const auto side = match_view_side();
    if (side < side_table_.count) {
        const auto& field =
            oa::data::defs::side_at(side_table_, static_cast<uint32_t>(side))->name_prefix;
        const std::string_view prefix(field, strnlen(field, sizeof field));
        const auto upper = [](char c) {
            return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        };
        if (prefix.size() == 3 && upper(prefix[0]) == 'C' && upper(prefix[1]) == 'O' &&
            upper(prefix[2]) == 'R')
            return "cor";
    }
    return "arm";
}

std::string Runtime::match_side_tile() const {
    const auto side = match_view_side();
    if (side < side_table_.count) {
        const auto& field =
            oa::data::defs::side_at(side_table_, static_cast<uint32_t>(side))->name_prefix;
        const std::string_view prefix(field, strnlen(field, sizeof field));
        if (!prefix.empty()) {
            const std::string path = "bitmaps/" + std::string(prefix) + "guisidetile.pcx";
            if (assets_.provider(path).kind != oa::ProviderKind::none)
                return path;
        }
    }
    return "bitmaps/" + match_side_prefix() + "guisidetile.pcx";
}

std::string Runtime::match_side_panel_gaf() const {
    return match_side_file(oa::data::defs::SideFile::panels);
}

std::string Runtime::match_side_font() const {
    return match_side_file(oa::data::defs::SideFile::font);
}

std::string Runtime::match_side_file(oa::data::defs::SideFile file) const {
    const oa::data::defs::Files files = asset_files(assets_);
    char path[oa::data::defs::path_capacity];
    if (!oa::data::defs::side_file_path(
            &files,
            side_table_,
            static_cast<uint32_t>(match_view_side()),
            file,
            side_files_language_.c_str(),
            path,
            sizeof path
        ))
        return {};
    // A file the game data lacks is drawn as none named; a mod warns of it
    // (mod_start_gaps), and the game without a mod ends as it starts
    // (require_side_files).
    if (path[0] == '\0' || files.exists == nullptr || !files.exists(files.context, path))
        return {};
    return path;
}

const oa::data::unit_definitions::UnitDefinition* Runtime::definition_for(uint16_t unit) const {
    if (!match_ || unit == 0 || unit >= match_->world().slots.size())
        return nullptr;
    const auto* slot_unit = match_->world().slots[unit].unit;
    if (slot_unit == nullptr)
        return nullptr;
    const auto type = static_cast<uint16_t>(slot_unit->type_index);
    if (type == 0 || type > unit_definitions_.size())
        return nullptr;
    return &unit_definitions_[type - 1U];
}

std::string Runtime::unit_gui_name(uint16_t unit) const {
    if (!match_ || unit == 0 || unit >= match_->world().slots.size())
        return {};
    const auto* slot_unit = match_->world().slots[unit].unit;
    if (slot_unit == nullptr)
        return {};
    const auto type = static_cast<uint16_t>(slot_unit->type_index);
    if (type >= spawn_type_names_.size())
        return {};
    return spawn_type_names_[type];
}

std::string Runtime::unit_info_name(uint16_t unit) const {
    const auto* definition = definition_for(unit);
    if (definition == nullptr)
        return unit_gui_name(unit);
    if (!definition->display_name.empty()) {
        // The name in the language shown, as the bottom bar shows it.
        const std::string name(
            oa::data::languages::unit_display_name(definition->unit_name, definition->display_name)
        );
        if (!definition->side.empty())
            return definition->side + " " + name;
        return name;
    }
    return definition->unit_name.empty() ? unit_gui_name(unit) : definition->unit_name;
}

std::string Runtime::selected_unit_gui_name() const {
    return unit_gui_name(selected_match_unit_);
}

const oa::formats::gaf::Sequence*
Runtime::gaf_sequence(const oa::formats::gaf::Archive& archive, std::string_view name) const {
    for (const auto& sequence : archive.sequences) {
        if (sequence.name.size() != name.size())
            continue;
        bool same = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            auto a = static_cast<unsigned char>(sequence.name[i]);
            auto b = static_cast<unsigned char>(name[i]);
            if (a >= 'A' && a <= 'Z')
                a = static_cast<unsigned char>(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = static_cast<unsigned char>(b + ('a' - 'A'));
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same)
            return &sequence;
    }
    return nullptr;
}

namespace {

[[nodiscard]] std::string lowered(std::string_view name) {
    std::string key(name);
    for (auto& c : key)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return key;
}

} // namespace

void Runtime::load_explosion_gaf(std::string_view name) {
    auto key = lowered(name);
    if (key.empty() || key == "fx" || match_explosion_gafs_.count(key) != 0)
        return;
    auto& loaded = match_explosion_gafs_[key];
    const auto path = "anims/" + std::string(name) + ".GAF";
    try {
        auto file = assets_.read(path).bytes;
        // Every frame is decoded once to check it; none is kept.
        auto parsed = oa::formats::gaf::parse(file, oa::formats::gaf::PixelData::checked);
        if (!parsed.ok()) {
            std::cerr << "match HUD GAF '" << path << "' parse failed: " << parsed.error->message
                      << '\n';
            return;
        }
        loaded.path = path;
        loaded.archive = std::move(*parsed.archive);
        // A file this large is never decoded whole: its frames are rendered
        // as they are drawn, from the sequences checked here.
        if (draws_frame_by_frame(loaded.archive)) {
            loaded.frame_by_frame = true;
            for (const auto& sequence : loaded.archive.sequences)
                frame_by_frame_sequences_[&sequence] = &loaded;
        }
    } catch (const std::exception& error) {
        const std::string_view what = error.what();
        if (what.find("asset not found") == std::string_view::npos)
            std::cerr << "match HUD GAF '" << path << "' unavailable: " << error.what() << '\n';
    }
}

const oa::formats::gaf::Sequence*
Runtime::explosion_sequence(std::string_view archive, std::string_view entry) {
    const auto key = lowered(archive);
    if (key.empty() || key == "fx")
        return gaf_sequence(match_fx_, entry);
    load_explosion_gaf(archive);
    auto& loaded = match_explosion_gafs_[key];
    const auto* listed = gaf_sequence(loaded.archive, entry);
    if (listed == nullptr || loaded.frame_by_frame)
        return listed;
    const auto index = static_cast<std::size_t>(listed - loaded.archive.sequences.data());
    if (const auto found = loaded.decoded.find(index); found != loaded.decoded.end())
        return &found->second;
    if (loaded.failed.count(index) != 0)
        return nullptr;
    // The file passed its check when it loaded, so its sequence decodes
    // unless its pixels and coverage take more than a decoded sequence may
    // keep; the file is read again for it and its bytes are let go once it
    // has decoded. A sequence that fails is remembered, so that later
    // explosions neither read the file again nor report it again.
    std::string reason;
    try {
        const auto file = assets_.read(loaded.path).bytes;
        auto decoded = oa::formats::gaf::parse_sequence(file, index);
        if (decoded.ok())
            return &loaded.decoded.emplace(index, std::move(*decoded.sequence)).first->second;
        reason = decoded.error ? decoded.error->message : "the sequence did not decode";
    } catch (const std::exception& error) {
        reason = error.what();
    }
    loaded.failed.insert(index);
    std::cerr << "explosion GAF '" << loaded.path << "' sequence '" << listed->name
              << "' is not drawn: " << reason << '\n';
    return nullptr;
}

namespace {

/// The file a frame drawn a frame at a time is read from.
struct ExplosionFileReader {
    const oa::AssetStore* assets{};
    const std::string* path{};
};

/// Reads bytes of an explosion file through the asset store, inflating only
/// the blocks of an archive entry the bytes lie in.
///
/// @param context the ExplosionFileReader
/// @param offset first byte, from the start of the file
/// @param[out] output receives the bytes
/// @return true when every byte was read
bool read_explosion_file(void* context, uint32_t offset, std::span<uint8_t> output) {
    const auto& reader = *static_cast<const ExplosionFileReader*>(context);
    oa::ResourceFile* file = nullptr;
    try {
        file = reader.assets->open(*reader.path);
        bool reading = file != nullptr && oa::AssetStore::seek(file, offset) != -1;
        for (std::size_t done = 0; reading && done < output.size();) {
            const auto count = oa::AssetStore::read(file, output.subspan(done));
            reading = count > 0;
            if (reading)
                done += static_cast<std::size_t>(count);
        }
        oa::AssetStore::close(file);
        return reading;
    } catch (const std::exception&) {
        oa::AssetStore::close(file);
        return false;
    }
}

} // namespace

const oa::formats::gaf::RenderedFrame* Runtime::effect_frame(
    WorldDrawList& list, const oa::formats::gaf::Sequence& sequence, std::size_t index
) {
    const auto& frame = sequence.frames[index];
    const auto found = frame_by_frame_sequences_.find(&sequence);
    if (found == frame_by_frame_sequences_.end())
        return decoded_frame(list, frame);
    auto& file = *found->second;
    if (explosion_frame_cache_ == nullptr)
        explosion_frame_cache_ = std::make_shared<GafFrameCache>();
    ExplosionFileReader reader{&assets_, &file.path};
    std::optional<oa::formats::gaf::Error> failure;
    const auto* rendered = ranged_frame(
        list, *explosion_frame_cache_, frame, {&reader, read_explosion_file}, &failure
    );
    if (failure && !file.frame_failure_reported) {
        file.frame_failure_reported = true;
        std::cerr << "explosion GAF '" << file.path << "' sequence '" << sequence.name << "' frame "
                  << index << " is not drawn: " << failure->message << '\n';
    }
    return rendered;
}

void Runtime::append_gaf_file(oa::formats::gaf::Archive& destination, std::string_view path) {
    std::string key(path);
    for (auto& c : key)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    if (missing_gaf_paths_.count(key) != 0)
        return;
    try {
        auto parsed = oa::formats::gaf::parse(assets_.read(std::string(path)).bytes);
        if (!parsed.ok()) {
            missing_gaf_paths_.insert(key);
            if (reported_gaf_paths_.insert(key).second) {
                std::cerr << "match HUD GAF '" << path
                          << "' parse failed: " << parsed.error->message << '\n';
            }
            return;
        }
        const std::size_t first_sequence = destination.sequences.size();
        for (auto& sequence : parsed.archive->sequences)
            destination.sequences.push_back(std::move(sequence));
        caption_gaf_pictures(path, destination, first_sequence);
    } catch (const std::exception& error) {
        missing_gaf_paths_.insert(key);
        const std::string_view what = error.what();
        // A file the store does not hold stays silent. Any other failure is
        // logged once in the run, then tried again in the next match.
        if (what.find("asset not found") == std::string_view::npos &&
            reported_gaf_paths_.insert(key).second)
            std::cerr << "match HUD GAF '" << path << "' unavailable: " << error.what() << '\n';
    }
}

void Runtime::blit_gaf_frame(
    oa::Image& destination,
    const oa::formats::gaf::RenderedFrame& frame,
    int destination_x,
    int destination_y,
    const oa::PaletteBytes& palette
) {
    for (std::size_t row = 0; row < frame.height; ++row) {
        for (std::size_t column = 0; column < frame.width; ++column) {
            const auto offset = row * frame.width + column;
            if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                continue;
            const int x = destination_x + static_cast<int>(column);
            const int y = destination_y + static_cast<int>(row);
            if (x < 0 || y < 0 || x >= static_cast<int>(destination.width) ||
                y >= static_cast<int>(destination.height))
                continue;
            const auto index = frame.pixels[offset];
            const auto pal = static_cast<std::size_t>(index) * 4U;
            if (pal + 2 >= palette.size())
                continue;
            const auto di =
                (static_cast<std::size_t>(y) * destination.width + static_cast<std::size_t>(x)) *
                3U;
            destination.rgb[di] = palette[pal];
            destination.rgb[di + 1] = palette[pal + 1];
            destination.rgb[di + 2] = palette[pal + 2];
        }
    }
}

void Runtime::overlay_gaf_sequence(
    oa::Image& destination,
    const oa::formats::gaf::Archive& archive,
    std::string_view name,
    int x,
    int y,
    const oa::PaletteBytes& palette
) {
    const auto* sequence = gaf_sequence(archive, name);
    if (sequence == nullptr || sequence->frames.empty())
        return;
    const auto rendered = oa::formats::gaf::render_normal(sequence->frames.front());
    if (!rendered.ok())
        return;
    blit_gaf_frame(destination, *rendered.frame, x, y, palette);
}

void Runtime::load_game_cursors() {
    try {
        append_gaf_file(cursor_gaf_, "anims/CURSORS.GAF");
        if (cursor_gaf_.sequences.empty())
            append_gaf_file(cursor_gaf_, "anims/cursors.gaf");
        cursors_loaded_ = !cursor_gaf_.sequences.empty();
    } catch (const std::exception& error) {
        std::cerr << "CURSORS.GAF unavailable: " << error.what() << '\n';
        cursors_loaded_ = false;
    }
    // The game's cursors take the system pointer's place wherever the game
    // draws them.
    apply_system_pointer(false);
    // Without the game's palette the match keeps the one it had.
    try {
        const auto palette_data = assets_.read("palettes/palette.pal").bytes;
        if (palette_data.size() == match_palette_.size())
            std::copy(palette_data.begin(), palette_data.end(), match_palette_.begin());
        else
            std::cerr << "palettes/palette.pal holds " << palette_data.size()
                      << " bytes, not a palette's " << match_palette_.size() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "palettes/palette.pal unavailable: " << error.what() << '\n';
    }
    // Session start resets the GUI context's cursor to the first
    // frame of the normal cursor.
    bind_gui_context();
    const auto* normal = cursor_sequence(kNormalCursor);
    oa::ui::gui_input::reset_cursor(
        gui_context_,
        normal != nullptr && !normal->frames.empty() ? &normal->frames.front() : nullptr
    );
    cursor_index_ = 0xff;
    select_game_cursor(kNormalCursor);
}

void Runtime::bind_gui_context() {
    auto& host = gui_context_.host;
    host.context = this;
    host.current_tick = [](void* context) {
        return static_cast<Runtime*>(context)->frontend_tick();
    };
    host.current_pointer = [](void* context, oa::ui::gui_input::PointerEvent& out) {
        const auto& runtime = *static_cast<Runtime*>(context);
        out.x = static_cast<int32_t>(runtime.pointer_x_);
        out.y = static_cast<int32_t>(runtime.pointer_y_);
    };
    host.peek_pointer = [](void* context, oa::ui::gui_input::PointerEvent& out) {
        static_cast<Runtime*>(context)->gui_context_.host.current_pointer(context, out);
        return false;
    };
    host.pop_pointer = host.peek_pointer;
    host.set_cursor_image = [](void* context, const oa::formats::gaf::Frame* image) {
        static_cast<Runtime*>(context)->cursor_image_ = image;
    };
    host.cursor_image = [](void* context) { return static_cast<Runtime*>(context)->cursor_image_; };
    // Gadget texts the game sets, and each stage of a multi-stage caption, in
    // the game's language, as gamedata\translate.tdf gives them.
    host.translate = translation_hook;
}

const oa::formats::gaf::Sequence* Runtime::cursor_sequence(uint8_t index) const {
    if (index >= kCursorNames.size() || kCursorNames[index].empty())
        return nullptr;
    return gaf_sequence(cursor_gaf_, kCursorNames[index]);
}

void Runtime::select_game_cursor(uint8_t index) {
    if (index == cursor_index_)
        return;
    cursor_index_ = index;
    const auto* sequence = cursor_sequence(index);
    oa::ui::gui_input::set_cursor_sequence(
        gui_context_, sequence != nullptr ? sequence : cursor_sequence(kNormalCursor)
    );
}

namespace {
namespace input = oa::sim::gameplay_input;

input::OrderCommand armed_order(MatchCommand command) {
    switch (command) {
    case MatchCommand::move:
        return input::OrderCommand::move;
    case MatchCommand::attack:
        return input::OrderCommand::attack;
    case MatchCommand::dgun:
        return input::OrderCommand::blast;
    case MatchCommand::build:
        return input::OrderCommand::build;
    case MatchCommand::patrol:
        return input::OrderCommand::patrol;
    case MatchCommand::repair:
        return input::OrderCommand::repair;
    case MatchCommand::reclaim:
        return input::OrderCommand::reclaim;
    case MatchCommand::capture:
        return input::OrderCommand::capture;
    case MatchCommand::load:
        return input::OrderCommand::load;
    case MatchCommand::unload:
        return input::OrderCommand::unload;
    case MatchCommand::guard:
        return input::OrderCommand::guard;
    case MatchCommand::none:
        break;
    }
    return input::OrderCommand::default_order;
}
} // namespace

uint8_t Runtime::pick_match_cursor() {
    return shown_match_cursor().value_or(static_cast<uint8_t>(input::OrderCursor::build));
}

std::optional<uint8_t> Runtime::shown_match_cursor() {
    if (hovered_ || !match_)
        return static_cast<uint8_t>(input::OrderCursor::normal);
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    input::set_pointer_command(world.game, armed_order(match_command_));
    refresh_pointer_area();
    const auto ground = match_world_point(pointer_x_, pointer_y_);
    if (ground) {
        input::set_pointer_position(world.game, {(*ground)[0], (*ground)[1], (*ground)[2]});
        store_cursor_cell(*ground);
    }
    const auto hooks = order_cursor_hooks();
    input::OrderCursor cursor{};
    if (!input::pointer_cursor(world, hooks, &cursor))
        return std::nullopt;
    // Where the click snap moves a RECLAIM click on open ground onto a
    // feature, the cursor is Reclaim when the click is one there
    // (ui.click-snap); the snap override key leaves the arrow.
    if (cursor == input::OrderCursor::normal && match_command_ == MatchCommand::reclaim && ground &&
        world.game.cursor_unit_id == 0 &&
        (input::pointer_flags(world.game) & input::pointer_over_view) != 0)
        if (const auto snapped = snapped_reclaim_point(*ground)) {
            input::set_pointer_position(world.game, {(*snapped)[0], (*snapped)[1], (*snapped)[2]});
            input::OrderCursor at_feature{};
            if (input::pointer_cursor(world, hooks, &at_feature) &&
                at_feature == input::OrderCursor::reclaim)
                cursor = at_feature;
            input::set_pointer_position(world.game, {(*ground)[0], (*ground)[1], (*ground)[2]});
        }
    return static_cast<uint8_t>(cursor);
}

uint8_t Runtime::pick_map_cursor(
    uint16_t target, const std::optional<oa::sim::ground_orders::Point>& ground
) {
    if (!match_)
        return static_cast<uint8_t>(input::OrderCursor::normal);
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    input::set_pointer_command(world.game, armed_order(match_command_));
    // The point stands for the battlefield's ground under the pointer.
    input::set_pointer_area(world.game, false, true);
    if (ground) {
        input::set_pointer_position(world.game, {(*ground)[0], (*ground)[1], (*ground)[2]});
        store_cursor_cell(*ground);
    }
    world.game.cursor_unit_id = target;
    input::OrderCursor cursor{};
    if (!input::pointer_cursor(world, order_cursor_hooks(), &cursor))
        return static_cast<uint8_t>(input::OrderCursor::build);
    return static_cast<uint8_t>(cursor);
}

bool Runtime::pointer_over_top_panel() {
    const auto* gadgets = screen_ == Screen::match
                              ? (match_hud_ ? &match_hud_->layout.gadgets : nullptr)
                              : &resources_.layout.gadgets;
    if (gadgets == nullptr || gadgets->empty())
        return false;
    const auto& root = gadgets->front().common;
    CanvasRect rect{root.x, root.y, root.width, root.height};
    if (screen_ == Screen::match) {
        const auto mapped = oa::ui::display_layout::source_rect_to_canvas(
            match_layout_, root.x, root.y, root.width, root.height
        );
        rect = {mapped.x, mapped.y, mapped.width, mapped.height};
    }
    const auto x = static_cast<int32_t>(pointer_x_);
    const auto y = static_cast<int32_t>(pointer_y_);
    return x >= rect.x && x <= rect.x + rect.w - 1 && y >= rect.y && y <= rect.y + rect.h - 1;
}

void Runtime::tick_and_draw_cursor() {
    clear_menu_button_cursor_hold();
    if (!cursors_loaded_ || frame_without_cursor_)
        return;
    const auto tick = frontend_tick();
    gui_context_.tick_delta = tick - gui_context_.last_tick;
    gui_context_.last_tick = tick;
    oa::ui::gui_input::tick_cursor_and_read_pointer(gui_context_);
    // Build placement over the battlefield keeps the cursor the pointer
    // brought, whose point is the footprint's middle.
    if (screen_ == Screen::match)
        if (const auto cursor = shown_match_cursor())
            select_game_cursor(*cursor);
    oa::ui::gui_input::follow_hover_cursor(gui_context_, pointer_over_top_panel());
    // A middle-click pan draws its own direction at the pointer.
    if (middle_pan_shows_direction())
        return;
    if (match_use_layers_ && screen_ == Screen::match)
        return;
    // Over a screen of the OA layer the cursor is presented above the layer
    // (present_front_end), never under it in the frame. Over a main-menu
    // button a larger window draws again, it is presented above that button
    // too: in the picture it would cover the button, and the window-pixel
    // icon would not replace it.
    if (sdl_.renderer != nullptr && screen_ != Screen::match && oa_layer().shows(false))
        return;
    if (hold_menu_button_cursor())
        return;
    if (surface_.rgb.empty() || cursor_image_ == nullptr)
        return;
    // With touch controls the cursor is hidden while no finger rests, or
    // lifted above the finger on the battlefield.
    float cursor_x = pointer_x_;
    float cursor_y = pointer_y_;
    if (const auto touch = touch_cursor(); touch.replaces_pointer) {
        if (!touch.visible)
            return;
        cursor_x = touch.x;
        cursor_y = touch.y;
    } else if (!pointer_shows_cursor())
        return;
    const auto rendered = oa::formats::gaf::render_normal(*cursor_image_);
    if (!rendered.ok())
        return;
    const auto& frame = *rendered.frame;
    const auto& pal = match_palette_.size() >= 1024 ? match_palette_ : resources_.gui_palette;
    const int destination_x = static_cast<int>(cursor_x) - frame.origin_x;
    const int destination_y = static_cast<int>(cursor_y) - frame.origin_y;
    for (std::size_t row = 0; row < frame.height; ++row) {
        for (std::size_t column = 0; column < frame.width; ++column) {
            const auto offset = row * frame.width + column;
            if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                continue;
            const int x = destination_x + static_cast<int>(column);
            const int y = destination_y + static_cast<int>(row);
            if (x < 0 || y < 0 || x >= static_cast<int>(surface_.width) ||
                y >= static_cast<int>(surface_.height))
                continue;
            const auto pal_i = static_cast<std::size_t>(frame.pixels[offset]) * 4U;
            if (pal_i + 2 >= pal.size())
                continue;
            const auto di =
                (static_cast<std::size_t>(y) * surface_.width + static_cast<std::size_t>(x)) * 3U;
            surface_.rgb[di] = pal[pal_i];
            surface_.rgb[di + 1] = pal[pal_i + 1];
            surface_.rgb[di + 2] = pal[pal_i + 2];
        }
    }
}

} // namespace oa::app
