// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The photo panel on the Open Annihilation layer: the chips, the folding
// sections and the key line, in place of the one line photo mode opened with.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "oa_layer.hpp"
#include "photo_state.hpp"
#include "photo_writer.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/files.hpp"
#include "oa/present/photo/photo_file_name.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/photo_panel/crop.hpp"
#include "oa/ui/photo_panel/draw.hpp"
#include "oa/ui/photo_panel/input.hpp"
#include "oa/ui/photo_panel/words.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace kit = oa::ui::kit;
namespace panel = oa::ui::photo_panel;
namespace photo = oa::present::photo;
namespace words = panel::words;
namespace fs = std::filesystem;

/// The photo screen's name on the Open Annihilation layer.
constexpr std::string_view kPhotoScreen = "photo-mode";

/// How long the notice after a photo shows, from the first frame it is in.
constexpr uint64_t kToastShowsNs = 2'600'000'000ULL;

/// Replaces `{key}` in a catalogue sentence.
std::string fill_key(std::string_view pattern, std::string_view key) {
    std::string text(pattern);
    const std::size_t at = text.find("{key}");
    if (at == std::string::npos)
        return text;
    text.replace(at, 5, key);
    return text;
}

/// A catalogue sentence, copied so the model keeps it.
std::string looked_up(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// One decimal place, for the zoom's value.
std::string tenths(float value) {
    int scaled = static_cast<int>(std::lround(static_cast<double>(value) * 10.0));
    if (scaled < 0)
        scaled = 0;
    return std::to_string(scaled / 10) + "." + std::to_string(scaled % 10);
}

/// The game clock as hh:mm:ss, without the HUD's own label, which is the
/// original game's English and has no translation.
///
/// @param tick the match's tick, 30 a second
/// @return the clock
std::string clock_text(uint32_t tick) {
    const uint32_t seconds = tick / 30U;
    const auto two = [](uint32_t part) {
        const std::string digits = std::to_string(part);
        return part < 10U ? "0" + digits : digits;
    };
    return two(seconds / 3600U) + ":" + two(seconds / 60U % 60U) + ":" + two(seconds % 60U);
}

/// The local date and time a photo's name uses.
photo::LocalMoment local_moment() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    photo::LocalMoment moment;
    moment.year = parts.tm_year + 1900;
    moment.month = parts.tm_mon + 1;
    moment.day = parts.tm_mday;
    moment.hour = parts.tm_hour;
    moment.minute = parts.tm_min;
    moment.second = parts.tm_sec;
    return moment;
}

/// A guide the panel stores as a byte.
panel::Guide guide_of(uint8_t stored) noexcept {
    if (stored >= static_cast<uint8_t>(panel::Guide::count))
        return panel::Guide::off;
    return static_cast<panel::Guide>(stored);
}

/// Box-reduces packed RGB into a thumbnail of at most 64 by 40.
void box_reduce(
    const std::vector<uint8_t>& rgb,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& out,
    uint32_t& out_width,
    uint32_t& out_height
) {
    out.clear();
    out_width = 0;
    out_height = 0;
    constexpr uint32_t kMaxW = 64;
    constexpr uint32_t kMaxH = 40;
    if (width == 0 || height == 0 || rgb.size() != static_cast<std::size_t>(width) * height * 3U)
        return;
    if (width * kMaxH < height * kMaxW) {
        out_height = kMaxH;
        out_width = std::max(uint32_t{1}, width * kMaxH / height);
    } else {
        out_width = kMaxW;
        out_height = std::max(uint32_t{1}, height * kMaxW / width);
    }
    out.assign(static_cast<std::size_t>(out_width) * out_height * 3U, 0);
    for (uint32_t row = 0; row < out_height; ++row) {
        const uint32_t y0 = row * height / out_height;
        const uint32_t y1 = std::max(y0 + 1U, (row + 1U) * height / out_height);
        for (uint32_t column = 0; column < out_width; ++column) {
            const uint32_t x0 = column * width / out_width;
            const uint32_t x1 = std::max(x0 + 1U, (column + 1U) * width / out_width);
            uint32_t red = 0;
            uint32_t green = 0;
            uint32_t blue = 0;
            uint32_t count = 0;
            for (uint32_t y = y0; y < y1 && y < height; ++y) {
                for (uint32_t x = x0; x < x1 && x < width; ++x) {
                    const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3U;
                    red += rgb[at];
                    green += rgb[at + 1U];
                    blue += rgb[at + 2U];
                    ++count;
                }
            }
            if (count == 0)
                count = 1;
            const std::size_t to = (static_cast<std::size_t>(row) * out_width + column) * 3U;
            out[to] = static_cast<uint8_t>(red / count);
            out[to + 1U] = static_cast<uint8_t>(green / count);
            out[to + 2U] = static_cast<uint8_t>(blue / count);
        }
    }
}

/// The name a photo taken now would be given, without making the folder.
std::string
preview_file_name(std::string_view map, const fs::path& folder, const PhotoWriter* writer) {
    const photo::LocalMoment moment = local_moment();
    const std::size_t folder_characters = folder.native().size();
    const std::size_t path_budget = oa::platform::longest_path();
    std::string file_name = photo::photo_file_name(moment, map, 1, folder_characters, path_budget);
    const auto taken = [&](const std::string& name) {
        if (writer != nullptr && writer->holds(name))
            return true;
        if (folder.empty())
            return false;
        std::error_code error;
        return fs::exists(folder / oa::platform::path_from_utf8(name), error);
    };
    if (!taken(file_name))
        return file_name;
    for (int copy = 2; copy < 10000; ++copy) {
        file_name = photo::photo_file_name(moment, map, copy, folder_characters, path_budget);
        if (!taken(file_name))
            break;
    }
    return file_name;
}

} // namespace

/// The photo screen: the panel beside the battlefield. It is not modal, so a
/// point on the battlefield goes on to the photo pan, which never selects.
struct Runtime::PhotoScreen final : LayerScreen {
    explicit PhotoScreen(Runtime& runtime) : runtime_(runtime) {}

    [[nodiscard]] std::string_view name() const override { return kPhotoScreen; }

    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        remember_size(placed_on);
        return {{0, 0, width_, height_}, width_, height_, 0};
    }

    [[nodiscard]] bool modal() const override { return false; }

    [[nodiscard]] bool backdrop() const override { return false; }

    [[nodiscard]] bool opaque() const override { return false; }

    [[nodiscard]] bool revision_covers_drawing() const override { return false; }

    void lay_out(const LayerView& placed_on) override { remember_size(placed_on); }

    void draw(const kit::Canvas& canvas) const override {
        ensure();
        panel::draw_panel(canvas, model_, placed_, shown_);
    }

    LayerAnswer pointer(ScreenInputKind kind, uint8_t, kit::Point at, int32_t) override {
        ensure();
        if (kind == ScreenInputKind::pointer_up && !chrome_down_ &&
            interaction_.pressed == kit::no_control)
            return LayerAnswer::pass;
        const bool held_chrome = kind == ScreenInputKind::pointer_down ? false : chrome_down_;
        const panel::Answer answer = panel::pointer_event(interaction_, model_, placed_, kind, at);
        if (kind == ScreenInputKind::pointer_down)
            chrome_down_ = answer.consumed;
        if (kind == ScreenInputKind::pointer_up)
            chrome_down_ = false;
        return settle(answer, held_chrome || answer.consumed);
    }

    LayerAnswer key(kit::Key pressed, uint32_t) override {
        ensure();
        const panel::Answer answer = panel::key_event(interaction_, model_, placed_, pressed);
        return settle(answer, answer.consumed);
    }

    LayerAnswer wheel(kit::Point, float) override { return LayerAnswer::pass; }

    [[nodiscard]] kit::DisplayList display_list() const override {
        ensure();
        return shown_;
    }

    [[nodiscard]] kit::Interaction interaction() const override {
        ensure();
        return interaction_;
    }

    LayerAnswer tick() override {
        const uint64_t before = revision_;
        ensure();
        return revision_ == before ? LayerAnswer::none : LayerAnswer::redraw;
    }

    [[nodiscard]] uint64_t revision() const override { return revision_; }

    void close(bool) override { runtime_.leave_photo_mode(); }

  private:

    void remember_size(const LayerView& placed_on) const {
        int32_t across = std::max(placed_on.picture.width, placed_on.room.width);
        int32_t down = std::max(placed_on.picture.height, placed_on.room.height);
        if (across <= 0)
            across = 640;
        if (down <= 0)
            down = 480;
        width_ = across;
        height_ = down;
    }

    /// Builds the model from the match and lays the panel out.
    void ensure() const {
        if (runtime_.photo_ == nullptr)
            return;
        // The notice hides on the frame clock, kToastShowsNs after the first
        // frame that has it. A photo is noted between frames, where the clock
        // still holds an earlier frame's time, so its time starts here. A
        // press hides it sooner.
        PhotoState& notice = *runtime_.photo_;
        if (notice.toast_on && notice.toast_until_ns == 0)
            notice.toast_until_ns = runtime_.frame_time_ns_ + kToastShowsNs;
        else if (notice.toast_on && runtime_.frame_time_ns_ >= notice.toast_until_ns)
            notice.toast_on = false;
        const PhotoState& state = *runtime_.photo_;
        model_ = {};
        model_.width = width_;
        model_.height = height_;
        model_.size_class = kit::size_class_of(width_, height_);
        const bool replay = runtime_.photo_time_is_replay();
        const bool shared = !replay && !runtime_.photo_time_is_local();
        model_.visit = shared ? panel::Visit::shared
                              : (replay ? panel::Visit::replay : panel::Visit::skirmish);
        model_.shown = state.panel_shown;
        model_.camera_open = state.section_camera;
        model_.time_open = state.section_time;
        model_.look_open = state.section_look;
        model_.capture_open = state.section_capture;
        model_.guide = guide_of(state.guide);
        model_.selection = runtime_.photo_show_selection();
        model_.health = runtime_.photo_show_health_bars();
        model_.copy_on = runtime_.photo_copy();
        model_.names_on = runtime_.photo_player_names();
        model_.hot = interaction_.hovered;

        const std::string held = looked_up("Held");
        const bool camera_held = runtime_.photo_camera_locked();
        const auto limits = runtime_.photo_zoom_limits();
        const float least = limits[0];
        const float most = limits[1];
        const std::vector<float> stops = panel::zoom_stops(least, most);
        model_.zoom_stops = static_cast<int32_t>(stops.size());
        if (model_.zoom_stops < 1)
            model_.zoom_stops = 1;
        // The zoom the view is easing to. The wheel and Page Up can leave it
        // between stops: the thumb sits on the nearest stop, and the value is
        // the zoom itself. A key on the slider steps from where the view is
        // going, not from where an ease has reached.
        const float aim =
            runtime_.match_zoom_target_ > 0.F ? runtime_.match_zoom_target_ : runtime_.match_zoom_;
        model_.zoom_stop = panel::zoom_stop_nearest(stops, aim);
        model_.zoom_value = tenths(aim) + "  " + looked_up(panel::zoom_scale_key(aim));
        // One value per scale name, so the track keeps one width.
        for (const float sample : {0.1F, 0.5F, 1.F, 4.F, 8.F})
            model_.zoom_reserve.push_back(
                tenths(most) + "  " + looked_up(panel::zoom_scale_key(sample))
            );
        if (camera_held)
            model_.zoom_lock = held;
        else if (least > 0.F && aim <= least * 1.01F) {
            const std::string zoom_reason = runtime_.photo_zoom_reason();
            if (!zoom_reason.empty())
                model_.zoom_note = zoom_reason;
        }

        const std::array<float, 2> angles = runtime_.live_camera_angles();
        model_.turn_stops = 37;
        model_.tilt_stops = 37;
        model_.turn_stop = std::clamp(
            static_cast<int32_t>(std::lround(static_cast<double>(angles[0]) / 5.0)) + 18,
            int32_t{0},
            int32_t{36}
        );
        model_.tilt_stop = std::clamp(
            static_cast<int32_t>(std::lround(static_cast<double>(angles[1]) / 5.0)) + 18,
            int32_t{0},
            int32_t{36}
        );
        model_.turn_value = std::to_string((model_.turn_stop - 18) * 5);
        model_.tilt_value = std::to_string((model_.tilt_stop - 18) * 5);
        const std::string turn_reason = runtime_.photo_turn_reason();
        if (!turn_reason.empty()) {
            model_.turn_lock = turn_reason;
            model_.tilt_lock = turn_reason;
        } else if (camera_held) {
            model_.turn_lock = held;
            model_.tilt_lock = held;
        }

        const int32_t slow = runtime_.photo_slow_speed();
        if (slow >= 1 && slow <= 9)
            model_.speed_stop = slow - 1;
        model_.speed_stops = 9;
        model_.speed_value = std::to_string((model_.speed_stop + 1) * 10) + "%";
        model_.between_stops = 9;
        model_.between_stop = std::clamp(runtime_.photo_shutter_eighth(), int32_t{0}, int32_t{8});
        model_.between_value = std::to_string(model_.between_stop);
        if (shared) {
            const std::string shared_game = looked_up(words::shared_game);
            model_.slow_lock = shared_game;
            model_.speed_lock = shared_game;
            model_.step_lock = shared_game;
            model_.between_lock = shared_game;
            model_.freeze_caption = looked_up(words::hold_picture);
            model_.time_mode = runtime_.photo_picture_held() ? 2 : 0;
        } else {
            model_.freeze_caption = looked_up(words::freeze);
            if (runtime_.photo_frozen())
                model_.time_mode = 2;
            else if (runtime_.photo_slow_speed() > 0)
                model_.time_mode = 1;
            else
                model_.time_mode = 0;
        }

        const std::string leave_key = chord_or("photo.leave", "F9");
        model_.head = fill_key(looked_up(words::leave_head), leave_key);
        if (runtime_.match_ != nullptr)
            model_.clock = clock_text(runtime_.match_->state().game.tick);
        model_.file_name = preview_file_name(
            runtime_.selected_map_name_runtime_,
            runtime_.photo_shots_folder(),
            runtime_.photo_writer_.get()
        );
        const std::string_view guide_key = panel::guide_name(model_.guide);
        model_.guide_value = guide_key.find(':') == std::string_view::npos ? looked_up(guide_key)
                                                                           : std::string(guide_key);

        model_.camera_title = looked_up(words::camera);
        model_.time_title = looked_up(words::time);
        model_.look_title = looked_up(words::look);
        model_.capture_title = looked_up(words::photo);
        model_.zoom_label = looked_up(words::zoom);
        model_.turn_label = looked_up(words::turn);
        model_.tilt_label = looked_up(words::tilt);
        model_.classic_label = looked_up(words::classic_view);
        model_.follow_label = looked_up(words::follow_unit);
        model_.run_label = looked_up(words::run);
        model_.slow_label = looked_up(words::slow);
        model_.speed_label = looked_up(words::speed);
        model_.step_label = looked_up(words::step);
        model_.between_label = looked_up(words::between_ticks);
        model_.clock_label = looked_up(words::clock);
        model_.selection_label = looked_up(words::show_selection);
        model_.health_label = looked_up(words::show_health);
        model_.guides_label = looked_up(words::guides);
        model_.size_label = looked_up(words::size);
        model_.size_value = looked_up(words::screen);
        model_.copy_switch_label = looked_up(words::copy_to_clipboard);
        model_.names_label = looked_up(words::player_names);
        model_.file_label = looked_up(words::file);
        model_.take_label = looked_up(words::take_photo);
        model_.copy_label = looked_up(words::copy);
        model_.hint_label = fill_key(looked_up(words::shows_panel), chord_or("photo.panel", "H"));
        model_.open_word = looked_up(words::expanded);
        model_.closed_word = looked_up(words::collapsed);
        model_.off_word = looked_up(words::off);
        model_.on_word = looked_up(words::on);

        const std::string freeze_word =
            shared ? looked_up(words::hold_picture) : looked_up(words::freeze);
        model_.key_line = leave_key + " " + looked_up(words::leave) + "   " +
                          chord_or("photo.capture", "Enter") + " " + looked_up(words::take_photo) +
                          "   " + chord_or("photo.freeze", "Space") + " " + freeze_word + "   " +
                          chord_or("photo.panel", "H") + " " + looked_up(words::hide_panel) +
                          "   " + chord_or("photo.guides", "G") + " " + looked_up(words::guides);

        fill_chips(shared, replay);
        model_.toast = state.toast_on;
        model_.toast_name = state.toast_name;
        model_.toast_note = state.toast_copied ? looked_up(words::copied) : std::string{};
        model_.toast_rgb = state.toast_rgb;
        model_.toast_columns = static_cast<int32_t>(state.toast_width);
        model_.toast_rows = static_cast<int32_t>(state.toast_height);

        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        const kit::Fonts fallback;
        placed_ = panel::layout_of(model_, fonts != nullptr ? *fonts : fallback);
        repair_focus();
        shown_ = panel::shown_list(placed_, interaction_);
        const std::string stamp = stamp_of();
        if (stamp != stamp_) {
            stamp_ = stamp;
            ++revision_;
        }
    }

    void fill_chips(bool shared, bool replay) const {
        const std::string clock = model_.clock;
        if (shared) {
            model_.chips[0] = looked_up(words::live);
            model_.live_amber = true;
            if (runtime_.photo_picture_held())
                model_.chips[1] = looked_up("Held");
            std::string alerts = runtime_.photo_latest_under_attack();
            const uint32_t chats = runtime_.photo_chat_lines_since_enter();
            if (chats > 0) {
                if (!alerts.empty())
                    alerts += "  ";
                alerts += std::to_string(chats);
                alerts += ' ';
                alerts += looked_up("chat lines");
            }
            model_.alerts = std::move(alerts);
            return;
        }
        if (replay) {
            const bool paused =
                runtime_.match_ != nullptr &&
                (runtime_.match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
            model_.chips[0] = looked_up(paused ? words::replay_paused : words::photo_mode);
            model_.chips[1] = clock;
            return;
        }
        model_.chips[0] = looked_up(words::photo_mode);
        int slot = 1;
        if (runtime_.photo_frozen())
            model_.chips[static_cast<std::size_t>(slot++)] = looked_up(words::frozen);
        else if (runtime_.photo_slow_speed() > 0)
            model_.chips[static_cast<std::size_t>(slot++)] = looked_up(words::slow);
        if (slot < 3)
            model_.chips[static_cast<std::size_t>(slot)] = clock;
    }

    void repair_focus() const {
        const bool listed = std::any_of(
            placed_.list.tab_order.begin(), placed_.list.tab_order.end(), [&](kit::ControlId id) {
                return id == interaction_.focused;
            }
        );
        if (!opened_ || (interaction_.focused != kit::no_control && !listed)) {
            const bool show = opened_ && interaction_.focus_shown;
            interaction_.focused = kit::no_control;
            interaction_.pointer_hid_ring = false;
            kit::focus_opened(interaction_, placed_.list, show);
            opened_ = true;
        }
    }

    [[nodiscard]] std::string stamp_of() const {
        std::string text;
        text.reserve(256);
        const auto bit = [&](bool on) { text.push_back(on ? '1' : '0'); };
        bit(model_.shown);
        bit(model_.camera_open);
        bit(model_.time_open);
        bit(model_.look_open);
        bit(model_.capture_open);
        bit(model_.live_amber);
        bit(model_.toast);
        bit(model_.selection);
        bit(model_.health);
        bit(model_.copy_on);
        bit(model_.names_on);
        bit(interaction_.focus_shown);
        bit(interaction_.pointer_hid_ring);
        bit(kit::focus_ring_visible(interaction_));
        text += std::to_string(static_cast<int>(model_.guide));
        text += std::to_string(interaction_.focused);
        text += std::to_string(interaction_.hovered);
        text += std::to_string(width_);
        text += 'x';
        text += std::to_string(height_);
        text += model_.head;
        text += model_.key_line;
        text += model_.clock;
        text += model_.file_name;
        text += model_.guide_value;
        text += model_.zoom_value;
        text += model_.zoom_lock;
        text += model_.zoom_note;
        text += std::to_string(model_.time_mode);
        text += model_.turn_value;
        text += model_.turn_lock;
        text += model_.tilt_lock;
        text += model_.speed_value;
        text += model_.speed_lock;
        text += model_.slow_lock;
        text += model_.between_value;
        text += model_.alerts;
        text += model_.toast_name;
        text += model_.toast_note;
        text += model_.freeze_caption;
        for (const std::string& chip : model_.chips)
            text += chip;
        return text;
    }

    /// A chord, or the default when none is bound yet.
    [[nodiscard]] std::string chord_or(std::string_view action, std::string_view fallback) const {
        std::string chord = runtime_.bound_chord_text(action);
        if (chord.empty())
            chord = std::string(fallback);
        return chord;
    }

    LayerAnswer settle(const panel::Answer& answer, bool take) {
        const bool refreshes = answer.press.command == panel::Command::take_photo ||
                               answer.press.command == panel::Command::copy_photo;
        if (answer.dismiss_toast && !refreshes && runtime_.photo_ != nullptr)
            runtime_.photo_->toast_on = false;
        if (answer.leave) {
            runtime_.leave_photo_mode();
            return LayerAnswer::none;
        }
        if (answer.press.command != panel::Command::none)
            apply(answer.press);
        if (!take)
            return LayerAnswer::pass;
        ensure();
        return LayerAnswer::redraw;
    }

    void apply(const panel::Press& press) {
        PhotoState* state = runtime_.photo_.get();
        if (state == nullptr)
            return;
        switch (press.command) {
        case panel::Command::show_panel:
            state->panel_shown = true;
            break;
        case panel::Command::toggle_section:
            if (press.value == panel::kSectionCamera)
                state->section_camera = !state->section_camera;
            else if (press.value == panel::kSectionTime)
                state->section_time = !state->section_time;
            else if (press.value == panel::kSectionLook)
                state->section_look = !state->section_look;
            else if (press.value == panel::kSectionCapture)
                state->section_capture = !state->section_capture;
            break;
        case panel::Command::set_zoom:
            if (!runtime_.photo_camera_locked()) {
                const auto limits = runtime_.photo_zoom_limits();
                const std::vector<float> stops = panel::zoom_stops(limits[0], limits[1]);
                if (!stops.empty()) {
                    const int last = static_cast<int>(stops.size()) - 1;
                    const int stop = std::clamp(press.value, 0, last);
                    EngineSettingsState::ease_zoom_about_centre(
                        runtime_, stops[static_cast<std::size_t>(stop)]
                    );
                }
            }
            break;
        case panel::Command::classic_view:
            if (!runtime_.photo_camera_locked())
                std::ignore = runtime_.answer_live_view_key("camera.classic-view");
            break;
        case panel::Command::follow_unit:
            if (!runtime_.photo_camera_locked())
                runtime_.cycle_match_tracking(false);
            break;
        case panel::Command::set_turn:
        case panel::Command::set_tilt:
            if (runtime_.photo_turn_reason().empty() && !runtime_.photo_camera_locked()) {
                const std::array<float, 2> angles = runtime_.live_camera_angles();
                const float target = static_cast<float>(press.value - 18) * 5.0F;
                if (press.command == panel::Command::set_turn)
                    std::ignore = runtime_.photo_turn(target - angles[0]);
                else
                    std::ignore = runtime_.photo_tilt(target - angles[1]);
            }
            break;
        case panel::Command::run:
            // In a shared match Run is the live picture: it lets go of a held one.
            if (!runtime_.photo_time_is_local() && !runtime_.photo_time_is_replay() &&
                runtime_.photo_picture_held())
                std::ignore = runtime_.photo_hold_picture();
            else
                std::ignore = runtime_.photo_run();
            break;
        case panel::Command::slow:
            std::ignore = runtime_.photo_slow(press.value);
            break;
        case panel::Command::freeze:
            if (runtime_.photo_time_is_local() || runtime_.photo_time_is_replay())
                std::ignore = runtime_.photo_freeze();
            else
                std::ignore = runtime_.photo_hold_picture();
            break;
        case panel::Command::set_speed:
            std::ignore = runtime_.photo_slow(press.value + 1);
            break;
        case panel::Command::step:
            std::ignore = runtime_.photo_step();
            break;
        case panel::Command::between:
            std::ignore = runtime_.photo_between(press.value);
            break;
        case panel::Command::toggle_selection:
            runtime_.set_photo_show_selection(!runtime_.photo_show_selection());
            break;
        case panel::Command::toggle_health:
            runtime_.set_photo_show_health_bars(!runtime_.photo_show_health_bars());
            break;
        case panel::Command::cycle_guides:
            runtime_.cycle_photo_guides();
            break;
        case panel::Command::toggle_copy:
            runtime_.set_photo_copy(!runtime_.photo_copy());
            break;
        case panel::Command::toggle_names:
            runtime_.set_photo_player_names(!runtime_.photo_player_names());
            break;
        case panel::Command::take_photo:
            runtime_.take_photo();
            break;
        case panel::Command::copy_photo:
            runtime_.copy_last_photo();
            break;
        case panel::Command::leave:
        case panel::Command::none:
            break;
        }
    }

    Runtime& runtime_;
    mutable panel::Model model_{};
    mutable panel::Layout placed_{};
    mutable kit::Interaction interaction_{};
    mutable kit::DisplayList shown_{};
    mutable bool chrome_down_{};
    mutable bool opened_{};
    mutable int32_t width_{640};
    mutable int32_t height_{480};
    mutable uint64_t revision_{1};
    mutable std::string stamp_{};
};

void Runtime::open_photo_panel() {
    oa_layer().push(std::make_unique<PhotoScreen>(*this));
}

void Runtime::toggle_photo_panel() {
    if (photo_ == nullptr)
        return;
    photo_->panel_shown = !photo_->panel_shown;
}

void Runtime::cycle_photo_guides() {
    if (photo_ == nullptr)
        return;
    photo_->guide = static_cast<uint8_t>(panel::next_guide(guide_of(photo_->guide)));
}

void Runtime::remember_photo_thumbnail(
    const std::vector<uint8_t>& rgb, uint32_t width, uint32_t height
) {
    if (photo_ == nullptr)
        return;
    box_reduce(rgb, width, height, photo_->toast_rgb, photo_->toast_width, photo_->toast_height);
}

void Runtime::show_photo_toast(std::string name, bool copied) {
    if (photo_ == nullptr)
        return;
    photo_->toast_on = true;
    photo_->toast_name = std::move(name);
    photo_->toast_copied = copied;
    // The panel's next frame starts the notice's time.
    photo_->toast_until_ns = 0;
}

void Runtime::clear_photo_thumbnail() noexcept {
    if (photo_ == nullptr)
        return;
    photo_->toast_rgb.clear();
    photo_->toast_width = 0;
    photo_->toast_height = 0;
}

} // namespace oa::app
