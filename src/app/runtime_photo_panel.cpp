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
#include "oa/present/photo/looks.hpp"
#include "oa/present/photo/photo_file_name.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/photo_panel/crop.hpp"
#include "oa/ui/photo_panel/draw.hpp"
#include "oa/ui/photo_panel/input.hpp"
#include "oa/ui/photo_panel/layout.hpp"
#include "oa/ui/photo_panel/words.hpp"
#include "oa/ui/replay_viewer/replay_viewer.hpp"

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

    LayerAnswer pointer(ScreenInputKind kind, uint8_t, kit::Point at, int32_t reach) override {
        ensure();
        // A finger drag that starts on the scrolling rows moves them. Leave,
        // Take photo and the hint stay put, and a short move is still a tap.
        if (kind == ScreenInputKind::pointer_up && finger_drag_) {
            finger_drag_ = false;
            finger_armed_ = false;
            chrome_down_ = false;
            interaction_.pressed = kit::no_control;
            interaction_.finger_shift = {};
            return LayerAnswer::redraw;
        }
        if (model_.touch && (finger_armed_ || finger_drag_) &&
            kind == ScreenInputKind::pointer_move) {
            const int32_t dy = at.y - finger_y_;
            const int32_t dx = at.x - finger_x_;
            const int32_t distance = dy < 0 ? -dy : dy;
            const int32_t across = dx < 0 ? -dx : dx;
            const int32_t slop = std::max(int32_t{8}, model_.touch_row / 5);
            // A finger that goes sideways past the slop first stays with its
            // control, so a slider's drag is never taken for a scroll.
            if (!finger_drag_ && across >= slop && across >= distance)
                finger_armed_ = false;
            else if (!finger_drag_ && distance >= slop)
                finger_drag_ = true;
            if (finger_drag_) {
                // The rows follow every move of the finger until it lifts.
                finger_armed_ = false;
                scroll_ = finger_scroll_ - dy;
                interaction_.pressed = kit::no_control;
                interaction_.finger_shift = {};
                chrome_down_ = true;
                // The rows stop at either end. The finger's start moves with
                // them there, so a move back scrolls at once.
                ensure();
                finger_scroll_ = scroll_ + dy;
                return LayerAnswer::redraw;
            }
        }
        if (kind == ScreenInputKind::pointer_up && !chrome_down_ &&
            interaction_.pressed == kit::no_control)
            return LayerAnswer::pass;
        const bool held_chrome = kind == ScreenInputKind::pointer_down ? false : chrome_down_;
        const panel::Answer answer =
            panel::pointer_event(interaction_, model_, placed_, kind, at, reach);
        if (kind == ScreenInputKind::pointer_down) {
            chrome_down_ = answer.consumed;
            const bool pinned = interaction_.pressed == panel::kHead ||
                                interaction_.pressed == panel::kTake ||
                                interaction_.pressed == panel::kHint;
            if (model_.touch && reach > 0 && answer.consumed && !pinned) {
                finger_armed_ = true;
                finger_drag_ = false;
                finger_x_ = at.x;
                finger_y_ = at.y;
                finger_scroll_ = scroll_;
            } else {
                finger_armed_ = false;
                finger_drag_ = false;
            }
        }
        if (kind == ScreenInputKind::pointer_up) {
            chrome_down_ = false;
            finger_armed_ = false;
        }
        return settle(answer, held_chrome || answer.consumed);
    }

    LayerAnswer key(kit::Key pressed, uint32_t) override {
        ensure();
        const panel::Answer answer = panel::key_event(interaction_, model_, placed_, pressed);
        return settle(answer, answer.consumed);
    }

    LayerAnswer wheel(kit::Point, float) override { return LayerAnswer::pass; }

    LayerAnswer pad_action(oa::ui::pad_controls::Action action) override {
        // A on a button the ring shows presses it as Enter does, and the ring
        // goes, so the next A takes the photo again. Elsewhere, where the
        // D-pad's left and right change the control, A takes the photo.
        if (action != oa::ui::pad_controls::Action::photo_capture)
            return LayerAnswer::pass;
        ensure();
        if (!kit::focus_ring_visible(interaction_))
            return LayerAnswer::pass;
        const kit::Control* focused = kit::control_of(shown_, interaction_.focused);
        if (focused == nullptr || focused->kind != kit::ControlKind::button)
            return LayerAnswer::pass;
        return key(kit::Key::enter, 0U);
    }

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

    /// The size and smooth-edge strips, and the one line that says why a stop
    /// is locked. A stored factor the battlefield cannot draw snaps to one it can.
    void fill_size_choices(panel::Model& model) const;

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
        if (runtime_.touch_controls_active()) {
            // One section stays open: the first that is open, and the others
            // fold. A keyboard layout is left as the player set it.
            bool kept = false;
            const auto keep = [&](bool& open) {
                if (open && !kept) {
                    kept = true;
                    return;
                }
                open = false;
            };
            keep(notice.section_camera);
            keep(notice.section_time);
            keep(notice.section_look);
            keep(notice.section_capture);
        }
        const PhotoState& state = *runtime_.photo_;
        model_ = {};
        model_.width = width_;
        model_.height = height_;
        model_.size_class = kit::size_class_of(width_, height_);
        const bool replay = runtime_.photo_time_is_replay();
        if (replay)
            runtime_.note_replay_photo_pace();
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
        if (replay && runtime_.photo_slow_speed() > 0)
            model_.speed_value = looked_up("½×");
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
        model_.preset_labels = {
            looked_up("None"),
            looked_up(words::year_1997),
            looked_up(words::dusk),
            looked_up(words::arctic),
            looked_up(words::noir),
            looked_up(words::bleach),
        };
        model_.preset = std::clamp(static_cast<int>(state.look_preset), 0, 5);
        model_.vignette_label = looked_up(words::vignette);
        model_.vignette_stop = std::clamp(state.look_vignette, 0, 100);
        model_.vignette_value = std::to_string(model_.vignette_stop) + "%";
        model_.roundness_label = looked_up(words::roundness);
        model_.roundness_stop = std::clamp(state.look_roundness, 0, 100);
        model_.roundness_value = std::to_string(model_.roundness_stop) + "%";
        model_.grain_label = looked_up(words::grain);
        model_.grain_stop = std::clamp(state.look_grain, 0, 100);
        model_.grain_value = std::to_string(model_.grain_stop) + "%";
        model_.grain_size_label = looked_up(words::grain_size);
        const int grain_size = std::clamp(state.look_grain_size, 1, 16);
        model_.grain_size_stop = grain_size - 1;
        model_.grain_size_value = std::to_string(grain_size);
        model_.frame_label = looked_up(words::frame);
        const std::string_view frame_keys[] = {
            "None", words::thin_border, words::wide_mat, words::caption_bar
        };
        const int frame_index = std::clamp(static_cast<int>(state.look_frame), 0, 3);
        model_.frame_value = looked_up(frame_keys[frame_index]);
        model_.depth_page = state.look_depth;
        model_.depth_label = looked_up(words::depth);
        model_.band_centre_label = looked_up(words::band_centre);
        model_.band_centre_stop = std::clamp(state.look_tilt_centre, 0, 100);
        model_.band_centre_value = std::to_string(model_.band_centre_stop) + "%";
        model_.band_width_label = looked_up(words::band_width);
        const int band = std::clamp(state.look_tilt_width, 0, 160);
        model_.band_width_stop = band / 2;
        model_.band_width_value = std::to_string(model_.band_width_stop * 2);
        model_.blur_label = looked_up(words::blur);
        model_.blur_stop = std::clamp(state.look_tilt_radius, 0, 32);
        model_.blur_value = std::to_string(model_.blur_stop);
        model_.tilt_note = looked_up(words::tilt_note);
        model_.haze_label = looked_up(words::haze);
        model_.haze_stop = std::clamp(state.look_haze, 0, 100);
        model_.haze_value = std::to_string(model_.haze_stop) + "%";
        model_.depth_of_field_label = looked_up(words::depth_of_field);
        model_.depth_of_field_stop = std::clamp(state.look_depth_of_field, 0, 32);
        model_.depth_of_field_value = std::to_string(model_.depth_of_field_stop);
        // The classic view hazes by height and locks depth of field. The
        // turned view uses the same haze slider for distance from the camera,
        // and depth of field, while the card keeps that depth. A card that
        // keeps none locks both and says why.
        const bool turned = runtime_.live_renderer_active();
        const bool keeps_depth = runtime_.photo_card_keeps_depth();
        if (!turned) {
            model_.haze_note = looked_up(words::haze_note);
            model_.haze_lock.clear();
            model_.depth_of_field_lock = looked_up(words::needs_full_3d);
        } else if (!keeps_depth) {
            model_.haze_note.clear();
            model_.haze_lock = looked_up(words::no_depth);
            model_.depth_of_field_lock = looked_up(words::no_depth);
        } else {
            model_.haze_note.clear();
            model_.haze_lock.clear();
            model_.depth_of_field_lock.clear();
        }
        model_.caption_clear = 0;
        if (state.look_frame == static_cast<uint8_t>(photo::Frame::caption)) {
            // The bar lies at the bottom of what the photo keeps: the crop a
            // guide keeps, else the whole battlefield.
            kit::Rect kept = panel::crop_bounds(width_, height_, model_.guide);
            if (kept.width <= 0 || kept.height <= 0)
                kept = {0, 0, width_, height_};
            const int32_t under = std::max(height_ - (kept.y + kept.height), int32_t{0});
            model_.caption_clear = under + photo::caption_bar_height(kept.width, kept.height);
        }
        const bool still = state.time == PhotoState::Time::frozen || state.picture_held;
        const bool costly = runtime_.full_presentation() || runtime_.live_renderer_active();
        model_.preview_note.clear();
        if (runtime_.photo_look_active() && !still && costly)
            model_.preview_note = looked_up(words::preview_rate);
        model_.size_label = looked_up(words::size);
        model_.smooth_label = looked_up(words::smooth_edges);
        fill_size_choices(model_);
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
        // A finger sees words it can tap. A pad that has been used sees its
        // own buttons, on the desktop as on a touch layout, and the desktop
        // hint names Y. The keyboard line stays until either of those.
        const auto pad_line = [&] {
            return std::string("A ") + looked_up(words::take_photo) + " · B " +
                   looked_up(words::leave) + " · X " + freeze_word + " · Y " +
                   looked_up(words::hide_panel);
        };
        model_.touch = runtime_.touch_controls_active();
        if (model_.touch) {
            const double pixels = static_cast<double>(runtime_.touch_px_per_point());
            const int32_t row = static_cast<int32_t>(std::lround(44.0 * pixels));
            model_.touch_row = row > 44 ? row : 44;
            model_.scroll = scroll_;
            if (!finger_drag_ && kit::focus_ring_visible(interaction_))
                model_.reveal = interaction_.focused;
            model_.head = looked_up(words::leave);
            model_.hint_label = looked_up(words::show_panel);
            if (runtime_.pad_used())
                model_.key_line = pad_line();
            else
                model_.key_line.clear();
        } else if (runtime_.pad_used()) {
            model_.key_line = pad_line();
            model_.hint_label = fill_key(looked_up(words::shows_panel), "Y");
        }

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
        if (model_.touch)
            scroll_ = placed_.scroll;
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
            const bool paused = runtime_.photo_frozen();
            model_.chips[0] = looked_up(paused ? words::replay_paused : words::photo_mode);
            uint32_t shown_tick =
                runtime_.match_ != nullptr ? runtime_.match_->state().game.tick : 0;
            uint32_t shown_length = 0;
            if (runtime_.photo_ != nullptr) {
                shown_length = runtime_.photo_->replay_length_seen;
                if (runtime_.photo_->replay_tick_seen != 0 || shown_length != 0)
                    shown_tick = runtime_.photo_->replay_tick_seen;
            }
            model_.chips[1] = oa::ui::replay_viewer::time_text(shown_tick, shown_length);
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
        text += std::to_string(scroll_);
        text += std::to_string(model_.preset);
        text += std::to_string(model_.vignette_stop);
        text += model_.vignette_value;
        text += std::to_string(model_.roundness_stop);
        text += model_.roundness_value;
        text += std::to_string(model_.grain_stop);
        text += model_.grain_value;
        text += std::to_string(model_.grain_size_stop);
        text += model_.grain_size_value;
        text += model_.frame_value;
        text += model_.preview_note;
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
        case panel::Command::toggle_section: {
            bool* open = nullptr;
            if (press.value == panel::kSectionCamera)
                open = &state->section_camera;
            else if (press.value == panel::kSectionTime)
                open = &state->section_time;
            else if (press.value == panel::kSectionLook)
                open = &state->section_look;
            else if (press.value == panel::kSectionCapture)
                open = &state->section_capture;
            if (open == nullptr)
                break;
            *open = !*open;
            // Opening one section on a touch layout folds the others, so the
            // next layout keeps the one the finger just opened. Its header
            // goes to the top of the scrolling rows: the old scroll belonged
            // to the section that just folded, and would hide this one.
            if (runtime_.touch_controls_active() && *open) {
                if (open != &state->section_camera)
                    state->section_camera = false;
                if (open != &state->section_time)
                    state->section_time = false;
                if (open != &state->section_look)
                    state->section_look = false;
                if (open != &state->section_capture)
                    state->section_capture = false;
                const int32_t row = model_.touch_row > 44 ? model_.touch_row : 44;
                const int32_t gap = std::max(int32_t{2}, row / 11);
                int before = 0;
                if (open == &state->section_time)
                    before = 1;
                else if (open == &state->section_look)
                    before = 2;
                else if (open == &state->section_capture)
                    before = 3;
                scroll_ = before * (row + gap);
            }
            break;
        }
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
        case panel::Command::set_preset:
            state->look_preset = static_cast<uint8_t>(std::clamp(press.value, 0, 5));
            break;
        case panel::Command::set_vignette:
            state->look_vignette = std::clamp(press.value, 0, 100);
            break;
        case panel::Command::set_roundness:
            state->look_roundness = std::clamp(press.value, 0, 100);
            break;
        case panel::Command::set_grain:
            state->look_grain = std::clamp(press.value, 0, 100);
            break;
        case panel::Command::set_grain_size:
            state->look_grain_size = std::clamp(press.value, 0, 15) + 1;
            break;
        case panel::Command::cycle_frame:
            state->look_frame = static_cast<uint8_t>((static_cast<int>(state->look_frame) + 1) % 4);
            break;
        case panel::Command::toggle_depth:
            state->look_depth = !state->look_depth;
            if (state->look_depth && !state->section_look) {
                state->section_look = true;
                if (runtime_.touch_controls_active()) {
                    state->section_camera = false;
                    state->section_time = false;
                    state->section_capture = false;
                    const int32_t row = model_.touch_row > 44 ? model_.touch_row : 44;
                    const int32_t gap = std::max(int32_t{2}, row / 11);
                    scroll_ = 2 * (row + gap);
                }
            }
            break;
        case panel::Command::set_band_centre:
            state->look_tilt_centre = std::clamp(press.value, 0, 100);
            break;
        case panel::Command::set_band_width:
            state->look_tilt_width = std::clamp(press.value, 0, 80) * 2;
            break;
        case panel::Command::set_blur:
            state->look_tilt_radius = std::clamp(press.value, 0, 32);
            break;
        case panel::Command::set_haze:
            if (!runtime_.live_renderer_active() || runtime_.photo_card_keeps_depth())
                state->look_haze = std::clamp(press.value, 0, 100);
            break;
        case panel::Command::set_depth_of_field:
            if (runtime_.photo_card_keeps_depth())
                state->look_depth_of_field = std::clamp(press.value, 0, 32);
            break;
        case panel::Command::set_size:
            runtime_.set_photo_size_factor(press.value);
            break;
        case panel::Command::set_smooth:
            runtime_.set_photo_smooth_factor(press.value);
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
    mutable int32_t scroll_{};
    mutable bool finger_armed_{};
    mutable bool finger_drag_{};
    mutable int32_t finger_x_{};
    mutable int32_t finger_y_{};
    mutable int32_t finger_scroll_{};
    mutable int32_t width_{640};
    mutable int32_t height_{480};
    mutable uint64_t revision_{1};
    mutable std::string stamp_{};
};

void Runtime::PhotoScreen::fill_size_choices(panel::Model& model) const {
    if (runtime_.photo_ == nullptr)
        return;
    // One plan for the rows. The factors snap from that same plan.
    const photo::Machine machine = runtime_.photo_capture_machine();
    runtime_.snap_photo_factors(machine);
    const photo::Plan plan = runtime_.photo_size_plan(machine);
    // A held picture is the Screen copy. The rows say so, and the stored
    // factors come back when the hold ends.
    const bool held = runtime_.photo_picture_held();
    const int chosen_size = held ? 1 : runtime_.photo_->size_factor;
    const int chosen_smooth = held ? 1 : runtime_.photo_->smooth_factor;
    const std::string held_reason = looked_up(words::held_picture);
    const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
    const bool modern = fonts != nullptr && kit::modern_look(*fonts);

    const std::string screen = looked_up(words::screen);
    const std::string off = looked_up(words::off);
    const std::string two = looked_up(words::two);
    const std::string three = looked_up(words::three);
    const std::string four = looked_up(words::four);
    const std::string size_word = looked_up(words::size);
    const std::string smooth_word = looked_up(words::smooth_edges);
    const std::string and_word = looked_up(words::and_word);

    std::array<panel::ChoiceStop, 4> sizes{};
    std::array<panel::ChoiceStop, 3> smooth{};
    int size_index = 0;
    int smooth_index = 0;
    for (int index = 0; index < 4; ++index) {
        const photo::Offer& offer = plan.sizes[static_cast<std::size_t>(index)];
        panel::ChoiceStop& stop = sizes[static_cast<std::size_t>(index)];
        stop.factor = offer.factor;
        stop.width = offer.width;
        stop.height = offer.height;
        stop.lock = held ? held_reason : runtime_.photo_offer_reason(plan.kind, offer);
        if (offer.factor == chosen_size)
            size_index = index;
    }
    for (int index = 0; index < 3; ++index) {
        const photo::Offer& offer = plan.smooth[static_cast<std::size_t>(index)];
        panel::ChoiceStop& stop = smooth[static_cast<std::size_t>(index)];
        stop.factor = offer.factor;
        stop.lock = held ? held_reason : runtime_.photo_offer_reason(plan.kind, offer);
        if (offer.factor == chosen_smooth)
            smooth_index = index;
    }

    panel::ChoiceWords copy_words;
    copy_words.screen = screen;
    copy_words.off = off;
    copy_words.two = two;
    copy_words.three = three;
    copy_words.four = four;
    copy_words.size = size_word;
    copy_words.smooth = smooth_word;
    copy_words.and_word = and_word;
    copy_words.times = modern ? std::string_view{"\u00d7"} : std::string_view{"x"};
    const panel::ChoiceCopy copy =
        panel::choice_copy(sizes, smooth, size_index, smooth_index, copy_words);

    model.size_label = copy.size_label;
    model.smooth_label = copy.smooth_label;
    model.size_value = copy.size_value;
    model.size_reason = copy.reason;
    model.size_index = size_index;
    model.smooth_index = smooth_index;
    for (int index = 0; index < 4; ++index) {
        model.size_factors[static_cast<std::size_t>(index)] =
            sizes[static_cast<std::size_t>(index)].factor;
        model.size_captions[static_cast<std::size_t>(index)] =
            copy.size_captions[static_cast<std::size_t>(index)];
        model.size_locks[static_cast<std::size_t>(index)] =
            sizes[static_cast<std::size_t>(index)].lock;
    }
    for (int index = 0; index < 3; ++index) {
        model.smooth_factors[static_cast<std::size_t>(index)] =
            smooth[static_cast<std::size_t>(index)].factor;
        model.smooth_captions[static_cast<std::size_t>(index)] =
            copy.smooth_captions[static_cast<std::size_t>(index)];
        model.smooth_locks[static_cast<std::size_t>(index)] =
            smooth[static_cast<std::size_t>(index)].lock;
    }
}

void Runtime::open_photo_panel() {
    oa_layer().push(std::make_unique<PhotoScreen>(*this));
}

void Runtime::toggle_photo_panel() {
    if (photo_ == nullptr)
        return;
    photo_->panel_shown = !photo_->panel_shown;
}

bool Runtime::photo_panel_shown() const noexcept {
    return photo_ != nullptr && photo_->panel_shown;
}

bool Runtime::photo_focus_ring_shown() const {
    if (oa_layer_ == nullptr || !photo_mode())
        return false;
    const LayerScreen* screen = oa_layer_->find(kPhotoScreen);
    if (screen == nullptr)
        return false;
    return kit::focus_ring_visible(screen->interaction());
}

bool Runtime::offer_photo_pointer(ScreenInputKind kind, float x, float y) {
    if (!photo_mode() || oa_layer_ == nullptr)
        return false;
    // The press is a finger's, so the layer hands the panel the finger's reach.
    bool& finger = engine_settings_state().finger_pointer;
    const bool was_finger = finger;
    finger = true;
    ScreenInput input{};
    input.kind = kind;
    input.button = 1;
    input.clicks = kind == ScreenInputKind::pointer_down ? uint8_t{1} : uint8_t{0};
    input.x = x;
    input.y = y;
    const bool taken = oa_layer().take_input(input);
    finger = was_finger;
    return taken;
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
