// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Live match camera and GPU battlefield composition.
#include "oa/app/runtime.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "bench_timers.hpp"
#include "director_state.hpp"
#include "match_models.hpp"
#include "engine_settings_state.hpp"
#include "renderer_effects.hpp"
#include "graphics_report.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "renderer_live_camera.hpp"
#include "renderer_live_gpu.hpp"
#include "renderer_live_overlay.hpp"
#include "renderer_live_scene.hpp"
#include "renderer_water_surface.hpp"
#include "xrgb_conversion.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/sim/unit_movement/terrain.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oa::app {
namespace {
/// Camera rotation per logical mouse pixel.
constexpr float orbit_degrees_per_pixel = 0.25F;
/// Painted height cell edge and the classic projection's height lift.
constexpr int terrain_cell_edge = 16;
constexpr float height_lift = 0.5F;

/// A scene triangle that can reach the pixels the lights' visibility asks
/// for, with the pixels its bounds cover.
struct VisibilityReach {
    std::size_t index{}; ///< the triangle's first vertex
    int x0{};
    int x1{};
    int y0{};
    int y1{};
};
} // namespace

struct Runtime::LiveRenderer {
    renderer_live_camera::State camera{};
    renderer_live_camera::Matrix projection{};
    renderer_live_camera::View view{};
    bool pivot_held{};
    renderer_live_scene::Scene scene{};
    bool scene_built{}; ///< the last frame's scene was built
    renderer_live_gpu::Context gpu{};
    std::vector<uint8_t> rgba{};
    std::vector<std::array<float, 3>> projected{};
    std::vector<uint16_t> triangle_units{};
    std::unordered_map<uint16_t, oa::Rect32> unit_bounds{};
    /// The bounds of each model the card draws (Scene::cards), in the
    /// window's pixels, as a unit's are: the pick tests the triangles of
    /// those its pointer lies in.
    std::vector<oa::Rect32> card_bounds{};
    bool dragging{};
    uint8_t drag_button{};
    bool orbiting{};
    bool failed{};
    bool unavailable{};
    const void* match{};

    /// The camera of the last frame drawn, turned or classic: a frame that
    /// needs more room than the renderer has goes back to it, rather than to
    /// the classic view, so that a turn or a zoom stops where the frames
    /// could still be drawn.
    struct Drawn {
        bool held{};
        renderer_live_camera::State camera{};
        renderer_live_camera::View view{};
        std::array<double, 2> place{};
        float zoom{};
        Runtime::ViewHold view_hold{};
    };

    Drawn drawn{};
    /// The view has gone back for lack of room since a frame last drew
    /// where it was asked to: reported once until then.
    bool out_of_room{};
    /// The frame being drawn again is drawn from the camera last drawn.
    bool went_back{};

    /// The lists the lights' visibility keeps from one frame to the next,
    /// so that once they have grown a frame allocates nothing.
    struct VisibilityLists {
        /// Each run's triangles, in their order.
        std::vector<std::vector<VisibilityReach>> found;
        /// Each run's triangles by band: their places in the run's list.
        std::vector<std::vector<uint32_t>> by_band;
        /// Where each band's places start in each run's by_band, a run's
        /// bands one after another with the end after its last.
        std::vector<uint32_t> band_starts;
        /// The columns each band of rows asks for.
        std::vector<uint32_t> band_left;
        std::vector<uint32_t> band_right;
    };

    VisibilityLists visibility_lists{};

    /// What the processor paints over a turned frame the card keeps
    /// (renderer_live_overlay.hpp): the battlefield layer is the overlay
    /// canvas, cleared to the key, and over it the operations the painters
    /// would have made on the picture.
    struct Overlay {
        /// The frame drawn last left its picture on the card, and the
        /// battlefield layer holds its canvas; until the next frame's
        /// drawing, for what reads the frame in memory after it shows.
        bool painted{};
        std::array<uint8_t, 3> key{};
        std::vector<renderer_live_overlay::Operation> operations{};
        uint32_t lines{}; ///< lines of text laid on the canvas
        /// What the card is given, kept so that once grown a frame
        /// allocates nothing.
        renderer_live_overlay::Packed packed{};
        std::vector<uint8_t> words{};
        std::vector<uint8_t> bands{};
    };

    Overlay overlay{};
};

void Runtime::destroy_live_renderer(LiveRenderer* renderer) noexcept {
    delete renderer;
}

Runtime::LiveRenderer& Runtime::ensure_live_renderer() {
    if (!live_renderer_) {
        live_renderer_.reset(new LiveRenderer{});
        // A turned view in the game's window draws on the device the window
        // can present through; a check, a director or a headless run draws
        // on a device of its own and reads its pictures back.
        if (render_run_ && render_run_->host != nullptr && sdl_.window != nullptr &&
            director_ == nullptr && !options_.headless_check)
            live_renderer_->gpu.shared_device = render_run_->host->card_device();
    }
    return *live_renderer_;
}

void Runtime::release_live_renderer() noexcept {
    // The window's texture shows the card's, which goes with the renderer.
    if (turned_world_tex_ != nullptr)
        SDL_DestroyTexture(turned_world_tex_);
    turned_world_tex_ = nullptr;
    turned_world_source_ = nullptr;
    live_renderer_.reset();
}

void Runtime::follow_card_presentation() {
    if (!render_run_ || render_run_->host == nullptr || sdl_.window == nullptr ||
        render_run_->device_lost)
        return;
    auto& host = *render_run_->host;
    const bool wanted =
        !card_presentation_refused_ && director_ == nullptr && !options_.headless_check &&
        screen_ == Screen::match && match_ && live_renderer_active() && !live_renderer_failed() &&
        live_renderer_->gpu.shared_device != nullptr && !live_renderer_->gpu.device_unavailable;
    if (wanted == host.presenting_on_card())
        return;
    // Every texture made on the renderer goes with it, and each is made
    // again as it is drawn, as after a device reset.
    forget_render_textures();
    sdl_.renderer = nullptr;
    std::string error;
    if (!wanted) {
        host.present_walked();
    } else if (!host.present_on_card(error)) {
        card_presentation_refused_ = true;
        std::cout << graphics_log_prefix << "the 3D view's pictures are read back: " << error
                  << '\n'
                  << std::flush;
    }
    sdl_.renderer = host.renderer();
    // A new renderer waits for the display only once it is asked to.
    vertical_sync_in_effect_ = false;
    apply_vertical_sync();
    apply_output_mode();
}

bool Runtime::turned_overlay_painted() const noexcept {
    return live_renderer_ && live_renderer_->overlay.painted;
}

bool Runtime::paints_turned_overlay() {
    return turned_overlay_painted() && &paint_target() == &match_world_cpu_;
}

std::size_t Runtime::turned_overlay_operation_count() const noexcept {
    return live_renderer_ ? live_renderer_->overlay.operations.size() : 0;
}

std::array<uint8_t, 3> Runtime::turned_overlay_key() const noexcept {
    return live_renderer_ ? live_renderer_->overlay.key : std::array<uint8_t, 3>{};
}

void Runtime::note_turned_operation(const renderer_live_overlay::Operation& operation) {
    if (turned_overlay_painted())
        live_renderer_->overlay.operations.push_back(operation);
}

void Runtime::begin_turned_text_line() noexcept {
    if (turned_overlay_painted())
        ++live_renderer_->overlay.lines;
}

void Runtime::note_turned_text_run(const oa::present::TextWorldRun& run) {
    if (!turned_overlay_painted())
        return;
    namespace overlay = renderer_live_overlay;
    const overlay::Kind kind =
        run.layer == oa::present::TextWorldLayer::shadow    ? overlay::Kind::shadow
        : run.layer == oa::present::TextWorldLayer::outline ? overlay::Kind::outline
                                                            : overlay::Kind::letter;
    auto& held = live_renderer_->overlay;
    held.operations.push_back(
        {run.x, run.y, run.width, 1, kind, run.color, run.coverage, held.lines}
    );
}

bool Runtime::turned_picture(renderer::Surface& world) {
    if (!turned_overlay_painted())
        return false;
    auto& live = *live_renderer_;
    std::string error;
    if (renderer_live_gpu::read_picture(live.gpu, live.rgba, error) !=
        renderer_live_gpu::Result::success) {
        std::cerr << "3D view: " << error << '\n';
        return false;
    }
    const uint32_t width = match_world_cpu_.width;
    const uint32_t height = match_world_cpu_.height;
    if (live.rgba.size() != std::size_t{width} * height * 4 ||
        match_world_cpu_.rgb.size() != std::size_t{width} * height * 3)
        return false;
    world.width = width;
    world.height = height;
    world.rgb.resize(std::size_t{width} * height * 3);
    for (std::size_t pixel = 0; pixel < world.rgb.size() / 3; ++pixel)
        std::copy_n(live.rgba.data() + pixel * 4, 3, world.rgb.data() + pixel * 3);
    const bool palette = match_palette_ != oa::PaletteBytes{};
    renderer_live_overlay::compose(
        world.rgb,
        width,
        height,
        match_world_cpu_.rgb,
        live.overlay.key,
        live.overlay.operations,
        palette ? std::span<const uint8_t>(match_palette_) : std::span<const uint8_t>{}
    );
    return true;
}

void Runtime::take_turned_picture() {
    if (!turned_overlay_painted())
        return;
    renderer::Surface world;
    const bool read = turned_picture(world);
    live_renderer_->overlay.painted = false;
    if (read) {
        match_world_cpu_.rgb = std::move(world.rgb);
        return;
    }
    // A picture that cannot be read back leaves the frame's battlefield as
    // the painters left it, the key where they painted nothing.
}

bool Runtime::send_turned_overlay(std::string& error) {
    auto& live = *live_renderer_;
    auto& held = live.overlay;
    const uint32_t width = match_world_cpu_.width;
    const uint32_t height = match_world_cpu_.height;
    held.words.resize(std::size_t{width} * height * 4);
    held.bands.assign(platform::job_pool::bands_of_rows(height, xrgb_band_rows), 0);
    convert_rgb24_keyed_overlay_argb(
        match_world_cpu_.rgb.data(),
        held.key,
        width,
        height,
        held.words.data(),
        std::size_t{width} * 4,
        nullptr,
        held.bands,
        draw_pool_.get()
    );
    renderer_live_overlay::pack(held.operations, width, height, held.packed);
    const bool palette = match_palette_ != oa::PaletteBytes{};
    const renderer_live_gpu::Overlay painted{
        held.words,
        held.bands,
        xrgb_band_rows,
        &held.packed,
        palette ? std::span<const uint8_t>(match_palette_) : std::span<const uint8_t>{},
        gamma_identity_ ? nullptr : &gamma_table_
    };
    return renderer_live_gpu::compose_overlay(live.gpu, painted, error) ==
           renderer_live_gpu::Result::success;
}

SDL_Texture* Runtime::compose_turned_overlay() {
    if (!turned_overlay_painted() || sdl_.renderer == nullptr)
        return nullptr;
    auto& live = *live_renderer_;
    using bench_timers::Span;
    using bench_timers::Stage;
    const Span timed(Stage::card_overlay);
    const uint32_t width = match_world_cpu_.width;
    const uint32_t height = match_world_cpu_.height;
    std::string error;
    SDL_GPUTexture* composed =
        send_turned_overlay(error) ? renderer_live_gpu::composed(live.gpu) : nullptr;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    if (composed != nullptr && (turned_world_tex_ == nullptr || turned_world_source_ != composed ||
                                turned_world_tex_->w != static_cast<int>(width) ||
                                turned_world_tex_->h != static_cast<int>(height))) {
        if (turned_world_tex_ != nullptr)
            SDL_DestroyTexture(turned_world_tex_);
        turned_world_source_ = nullptr;
        // The card's own texture, drawn by the window's renderer on the
        // device it was made on: RGBA8, opaque, one texel a pixel.
        const SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetNumberProperty(
            properties, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32
        );
        SDL_SetNumberProperty(
            properties, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC
        );
        SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
        SDL_SetPointerProperty(properties, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, composed);
        turned_world_tex_ = SDL_CreateTextureWithProperties(sdl_.renderer, properties);
        SDL_DestroyProperties(properties);
        if (turned_world_tex_ != nullptr &&
            (!SDL_SetTextureBlendMode(turned_world_tex_, SDL_BLENDMODE_NONE) ||
             !SDL_SetTextureScaleMode(turned_world_tex_, SDL_SCALEMODE_NEAREST))) {
            SDL_DestroyTexture(turned_world_tex_);
            turned_world_tex_ = nullptr;
        }
        if (turned_world_tex_ == nullptr)
            error = std::string("the window cannot draw the card's picture: ") + SDL_GetError();
        else
            turned_world_source_ = composed;
    }
#else
    composed = nullptr;
    error = "the window draws the card's pictures with SDL 3.4 or later";
#endif
    if (composed != nullptr && turned_world_tex_ != nullptr)
        return turned_world_tex_;
    std::cout << graphics_log_prefix << "the 3D view's pictures are read back: " << error << '\n'
              << std::flush;
    // The frame is presented as the processor's, read back, and so is
    // every frame after it: the window goes back to the walked renderer.
    card_presentation_refused_ = true;
    take_turned_picture();
    return nullptr;
}

std::string Runtime::draw_turned_frame_both_ways() {
    // What a frame counts as it draws, which the frame's +stats show, and
    // the resource readout's easing are counted afresh for the second
    // drawing.
    const auto counted = frame_draws_;
    const auto eases = readout_eases_;
    render_match_surface();
    if (!live_renderer_active() || live_renderer_failed() || !live_renderer_->scene_built)
        return {};
    const renderer::Surface expected = match_world_cpu_;
    frame_draws_ = counted;
    readout_eases_ = eases;
    turned_overlay_checked_ = true;
    try {
        render_match_surface();
    } catch (...) {
        turned_overlay_checked_ = false;
        throw;
    }
    turned_overlay_checked_ = false;
    auto& live = *live_renderer_;
    // What the window shows of the frame: the card's picture with the
    // overlay, or, where a painter took the picture back, the battlefield
    // layer; each through the display gamma.
    std::vector<uint8_t> shown;
    const bool on_card = live.overlay.painted;
    std::string error;
    if (on_card &&
        (!send_turned_overlay(error) || renderer_live_gpu::read_composed(live.gpu, shown, error) !=
                                            renderer_live_gpu::Result::success)) {
        live.overlay.painted = false;
        match_world_cpu_ = expected;
        return "the card did not add the overlay: " + error;
    }
    const auto shade = [&](uint8_t level) { return gamma_identity_ ? level : gamma_table_[level]; };
    const std::size_t pixels = std::size_t{expected.width} * expected.height;
    std::size_t differing = 0;
    std::size_t first = 0;
    if ((on_card && shown.size() != pixels * 4) ||
        (!on_card && match_world_cpu_.rgb.size() != expected.rgb.size()))
        differing = pixels;
    else
        for (std::size_t pixel = 0; pixel < pixels; ++pixel)
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const uint8_t want = shade(expected.rgb[pixel * 3 + channel]);
                const uint8_t got = on_card ? shown[pixel * 4 + channel]
                                            : shade(match_world_cpu_.rgb[pixel * 3 + channel]);
                if (want != got) {
                    if (differing == 0)
                        first = pixel;
                    ++differing;
                    break;
                }
            }
    live.overlay.painted = false;
    match_world_cpu_ = expected;
    if (differing == 0)
        return {};
    return std::to_string(differing) + " of " + std::to_string(pixels) +
           " pixels differ, the first at " + std::to_string(first % expected.width) + "," +
           std::to_string(first / expected.width) +
           (on_card ? " on the card" : " in the picture taken back");
}

bool Runtime::live_renderer_active() const noexcept {
    // A director's turned frame on the graphics card asks for the turn
    // itself, whatever Allow Full 3D Mode says.
    return (!engine_settings_ || engine_settings_->current.allow_full_3d ||
            director_draws_on_card()) &&
           (full_presentation() || options_.headless_check) && live_renderer_ &&
           (live_renderer_->camera.yaw_degrees != 0 || live_renderer_->camera.pitch_degrees != 0);
}

bool Runtime::turn_live_camera(float yaw_degrees, float pitch_degrees) {
    ensure_live_renderer();
    auto& live = *live_renderer_;
    live.camera.yaw_degrees = yaw_degrees;
    live.camera.pitch_degrees = pitch_degrees;
    // Held within the unlocked range by the camera's own limits, so that
    // the projection takes the angles as they are.
    renderer_live_camera::set_unlocked(live.camera, true);
    live.dragging = false;
    live.drag_button = 0;
    live.pivot_held = false;
    live.failed = false;
    live.unavailable = false;
    return live.camera.yaw_degrees != 0 || live.camera.pitch_degrees != 0;
}

bool Runtime::live_renderer_unlocked() const noexcept {
    return live_renderer_ && live_renderer_->camera.unlocked;
}

bool Runtime::live_renderer_failed() const noexcept {
    return live_renderer_ && live_renderer_->failed;
}

bool Runtime::live_renderer_unavailable() const noexcept {
    return live_renderer_ && live_renderer_->unavailable;
}

std::size_t Runtime::live_renderer_vertex_count() const noexcept {
    return live_renderer_ ? live_renderer_->scene.vertices.size() : 0;
}

std::size_t Runtime::live_renderer_emissive_vertex_count() const noexcept {
    if (!live_renderer_)
        return 0;
    return static_cast<std::size_t>(std::count_if(
        live_renderer_->scene.vertices.begin(),
        live_renderer_->scene.vertices.end(),
        [](const auto& vertex) {
            return vertex.emission_r > 0 || vertex.emission_g > 0 || vertex.emission_b > 0;
        }
    ));
}

std::size_t Runtime::live_renderer_card_model_count() const noexcept {
    return live_renderer_ ? live_renderer_->scene.cards.units.size() : 0;
}

bool Runtime::live_models_on_card() const noexcept {
    return live_renderer_ && renderer_live_gpu::draws_models(live_renderer_->gpu) &&
           renderer_live_gpu::draws_lights(live_renderer_->gpu);
}

void Runtime::set_live_models_on_card(bool allowed) noexcept {
    if (!live_renderer_)
        live_renderer_.reset(new LiveRenderer{});
    live_renderer_->gpu.models_allowed = allowed;
}

std::size_t Runtime::live_renderer_nano_particle_count() const noexcept {
    return live_renderer_ ? live_renderer_->scene.nano_particle_count : 0;
}

void Runtime::apply_live_camera_settings(bool allowed, bool unlocked) {
    if (!live_renderer_)
        return;
    if (!allowed) {
        reset_live_renderer();
        return;
    }
    renderer_live_camera::set_unlocked(live_renderer_->camera, unlocked);
}

void Runtime::reset_live_renderer() {
    if (live_renderer_) {
        renderer_live_camera::reset(live_renderer_->camera);
        live_renderer_->dragging = false;
        live_renderer_->pivot_held = false;
        live_renderer_->drag_button = 0;
        live_renderer_->failed = false;
        live_renderer_->unavailable = false;
    }
}

bool Runtime::answer_live_view_key(std::string_view action) {
    if (action != "camera.classic-view" && action != "camera.unlocked")
        return false;
    // Where no rotated view can be drawn, Alt and its keys stay the game's.
    if ((live_renderer_ && live_renderer_->gpu.device_unavailable) ||
        !engine_settings().allow_full_3d || screen_ != Screen::match || !match_ || !selected_tnt_)
        return false;
    if (!full_presentation() && !options_.headless_check)
        return false;
    // A marker's text takes the letters, and a panel or the question owns the
    // keys.
    if (match_question_open() || chat_composing_ || whiteboard_input_.editing || match_paused_)
        return false;
    ensure_live_renderer();
    if (action == "camera.classic-view") {
        reset_live_renderer();
        return true;
    }
    auto& live = *live_renderer_;
    live.failed = false;
    live.unavailable = false;
    const auto before = engine_settings();
    auto chosen = before;
    chosen.unlocked_camera = !chosen.unlocked_camera;
    apply_engine_settings(chosen);
    if (const auto error = save_engine_settings(before, chosen, false))
        status_ = *error;
    return true;
}

bool Runtime::live_renderer_take_event(SDL_Event& event) {
    // Where no rotated view can be drawn, Alt and its clicks and keys stay
    // the game's: below Full hardware acceleration, which never draws it,
    // and once SDL has made no device for it, for the rest of the run.
    if ((live_renderer_ && live_renderer_->gpu.device_unavailable) ||
        !engine_settings().allow_full_3d || screen_ != Screen::match || !match_ || !selected_tnt_)
        return false;
    if (!full_presentation() && !options_.headless_check) {
        // A camera drag begun on Full ends where Full does.
        if (live_renderer_) {
            live_renderer_->dragging = false;
            live_renderer_->drag_button = 0;
        }
        return false;
    }
    ensure_live_renderer();
    auto& live = *live_renderer_;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        live.dragging = false;
        live.drag_button = 0;
        return false;
    }
    if (match_question_open() || chat_composing_ || whiteboard_input_.editing || match_paused_) {
        // A panel or text entry ends the gesture, including when its button
        // is released while that panel owns the pointer. A marker's text
        // takes Alt+R and Alt+U as the letters they type.
        live.dragging = false;
        live.drag_button = 0;
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const auto actions = match_action_events();
        for (const oa::ui::key_bindings::ActionEvent& action : actions) {
            if (action.phase != oa::ui::key_bindings::Phase::pressed)
                continue;
            if (action.action != "camera.classic-view" && action.action != "camera.unlocked")
                continue;
            return handle_match_action(action);
        }
    }
    SDL_Event converted = event;
    const bool pointer =
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
        event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL;
    if (pointer && sdl_.renderer && !convert_event_to_frame(sdl_.renderer, converted))
        return false;
    const float x = !pointer                                   ? 0.0F
                    : converted.type == SDL_EVENT_MOUSE_MOTION ? converted.motion.x
                    : converted.type == SDL_EVENT_MOUSE_WHEEL  ? converted.wheel.mouse_x
                                                               : converted.button.x;
    const float y = !pointer                                   ? 0.0F
                    : converted.type == SDL_EVENT_MOUSE_MOTION ? converted.motion.y
                    : converted.type == SDL_EVENT_MOUSE_WHEEL  ? converted.wheel.mouse_y
                                                               : converted.button.y;
    const bool battlefield = pointer && x >= match_layout_.left && y >= match_layout_.top &&
                             x < match_layout_.left + match_layout_.battlefield_width() &&
                             y < match_layout_.top + match_layout_.battlefield_height();
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == live.drag_button &&
        live.drag_button != 0) {
        live.dragging = false;
        live.drag_button = 0;
        update_pointer(x, y);
        return true;
    }
    // While an order or a building is armed, Alt on the select button is the
    // click snap's override: that click places it unsnapped. The pan button
    // still orbits or pans.
    const bool order_armed = match_command_ != MatchCommand::none || pending_build_type_ != 0;
    ensure_key_bindings(false);
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && battlefield) {
        const uint8_t button = event.button.button;
        const PointerCameraDrag drag = pointer_camera_drag(button, pointer_modifier_bits());
        const bool blocked = order_armed && button == match_select_button();
        if (drag != PointerCameraDrag::none && !blocked) {
            // An explicit camera gesture retries after a failed frame.
            live.failed = false;
            live.unavailable = false;
            renderer_live_camera::set_unlocked(live.camera, engine_settings().unlocked_camera);
            live.dragging = true;
            live.drag_button = button;
            live.orbiting = drag == PointerCameraDrag::orbit;
            match_drag_.reset();
            update_pointer(x, y);
            return true;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && live.dragging) {
        // Camera gestures consume the event before normal pointer handling.
        // Keep the custom cursor at the physical pointer throughout the drag.
        update_pointer(x, y);
        if (live.orbiting) {
            renderer_live_camera::orbit(
                live.camera,
                converted.motion.xrel * orbit_degrees_per_pixel,
                converted.motion.yrel * orbit_degrees_per_pixel
            );
        } else {
            float dx{}, dz{};
            if (renderer_live_camera::pan_delta(
                    live.projection, converted.motion.xrel, converted.motion.yrel, dx, dz
                )) {
                const auto place = match_view_place();
                place_match_view(place[0] - dx, place[1] - dz);
            }
        }
        return true;
    }
    if (!live_renderer_active())
        return false;
    if (event.type != SDL_EVENT_MOUSE_WHEEL || !battlefield)
        return false;
    // Build rotation and spacing keep their modified-wheel gestures.
    if (handle_build_tool_key(event))
        return true;
    if (!engine_settings().wheel_zoom || live.failed)
        return true;
    const float turned_raw =
        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
    const float turned = pointer_zoom_amount(turned_raw);
    if (!std::isfinite(turned) || turned == 0)
        return true;
    const auto anchor = live_ground_hit(x, y);
    constexpr float fixed_scale = 65536;
    const float anchor_height =
        anchor ? static_cast<float>((*anchor)[1]) / fixed_scale : live.view.focus_y;
    auto view = live.view;
    const float zoom = wheel_zoom_target(turned);
    if (!renderer_live_camera::zoom_about(
            live.camera, view, zoom, x - match_layout_.left, y - match_layout_.top, anchor_height
        ))
        return true;
    stop_match_tracking();
    match_zoom_ = match_zoom_target_ = zoom;
    zoom_focus_ = {false, x, y};
    const float half_width = match_layout_.battlefield_width() / (2 * zoom);
    const float half_height = match_layout_.battlefield_height() / (2 * zoom);
    place_match_view(
        view.focus_x - half_width, view.focus_z - view.focus_y * height_lift - half_height
    );
    view_hold_ = {true, view.focus_x, view.focus_z - view.focus_y * height_lift};
    live.view = view;
    if (renderer_live_camera::projection(live.camera, live.view, live.projection))
        rebuild_live_picking();
    return true;
}

bool Runtime::render_live_match_surface() {
    // The overlay the last frame painted goes with it.
    if (live_renderer_)
        live_renderer_->overlay.painted = false;
    if ((!engine_settings().allow_full_3d && !director_draws_on_card()) || !match_ ||
        !selected_tnt_)
        return true;
    ensure_live_renderer();
    auto& live = *live_renderer_;
    if (live.match != match_.get()) {
        live.scene = {};
        live.drawn = {};
        live.pivot_held = false;
        live.match = match_.get();
        live.failed = false;
        live.unavailable = false;
    }
    const auto& map = *selected_tnt_;
    const auto place = match_view_place();
    const float zoom = match_zoom();
    const float width = static_cast<float>(match_layout_.battlefield_width());
    const float height = static_cast<float>(match_layout_.battlefield_height());
    const float focus_x = static_cast<float>(place[0]) + width / (2 * zoom);
    const float focus_y = static_cast<float>(place[1]) + height / (2 * zoom);
    float elevation{};
    if (map.attribute_width && map.attribute_height && !map.attributes.empty()) {
        const auto column = std::clamp(
            static_cast<int>(focus_x) / terrain_cell_edge,
            0,
            static_cast<int>(map.attribute_width) - 1
        );
        const auto row = std::clamp(
            static_cast<int>(focus_y) / terrain_cell_edge,
            0,
            static_cast<int>(map.attribute_height) - 1
        );
        elevation =
            map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column].height;
    }
    // Keep the orbit pivot's height while panning and zooming across terrain cells.
    // A sampled height change would otherwise shift the whole rotated view.
    if (live_renderer_active() && live.pivot_held)
        elevation = live.view.focus_y;
    live.pivot_held = live_renderer_active();
    live.view = {
        focus_x, elevation, focus_y + elevation * height_lift, width * 0.5F, height * 0.5F, zoom
    };
    if (!renderer_live_camera::projection(live.camera, live.view, live.projection))
        return true;
    // A classic frame is the last camera drawn until a turned one draws.
    if (!live_renderer_active())
        live.drawn = {true, live.camera, live.view, place, zoom, view_hold_};
    using bench_timers::Span;
    using bench_timers::Stage;
    const Span live_total(Stage::live_total);
    // The picture the last frame drew, read back, which a frame left on the
    // card leaves on the card: a frame that needs more room than the
    // renderer has shows it again.
    const auto last_picture_read = [&] {
        std::string unread;
        if (renderer_live_gpu::read_picture(live.gpu, live.rgba, unread) !=
            renderer_live_gpu::Result::success)
            live.rgba.clear();
        return live.rgba.size() == match_world_cpu_.rgb.size() / 3 * 4;
    };
    if (live_renderer_active() && !live.failed) {
        auto& models = match_models();
        renderer_live_scene::Inputs inputs{};
        inputs.map = &map;
        inputs.palette = &match_palette_;
        inputs.draws = &models.draws;
        inputs.graphics_flags = models.renderer.graphics_flags;
        inputs.moving_pieces_once_built = models.renderer.moving_pieces_once_built;
        inputs.shade_table = models.display.shade;
        inputs.light = {
            models.renderer.light[0], models.renderer.light[1], models.renderer.light[2]
        };
        inputs.light_scale = models.renderer.light_scale;
        inputs.sea_level = map.sea_level;
        inputs.detail_scale = zoom;
        // The sliders' strengths, which a lava sea leaves at none.
        const auto levels = water_levels();
        inputs.water = {
            levels.ripples,
            levels.coast,
            levels.shimmer,
            levels.reflections,
            float(map.sea_level),
            float(match_->simulation().tick % 108000U) / 30.0F,
            std::clamp((zoom - 0.25F) / 0.25F, 0.0F, 1.0F)
        };
        inputs.water.wind = water_wind();
        inputs.water.fine_detail = renderer_water_surface::fine_detail(zoom);
        inputs.water_surface = water_surface();
        inputs.water_disturbances = water_disturbances();
        inputs.nanolathe_lighting = engine_settings().nanolathe_lighting > 0;
        inputs.nanolathe_intensity = engine_settings().nanolathe_lighting / 100.0F;
        inputs.tick = match_->simulation().tick;
        inputs.translucent_units = match_->state().game.debug_overlay != 0;
        inputs.new_unit_shadows = engine_settings().new_unit_shadows;
        // A recording draws each frame at the plan's detail.
        inputs.tiles_hold_while_moving = director_ == nullptr;
        inputs.pool = draw_pool_.get();
        // The card draws the models it can from their types' models where
        // it adds the lights too: the processor's lights find their
        // surfaces among the frame's own triangles.
        inputs.models_on_card = live_models_on_card();
        inputs.view_direction = {live.projection[8], live.projection[9], live.projection[10]};
        inputs.clip_projection = live.projection;
        inputs.clip_width = match_world_cpu_.width;
        inputs.clip_height = match_world_cpu_.height;
        inputs.camera_x = models.renderer.camera_x;
        inputs.camera_y = models.renderer.camera_y;
        std::copy_n(
            models.renderer.team_colors, full::team_colour_players, inputs.team_colors.begin()
        );
        const auto& sight = match_->sight();
        inputs.sight.coverage = match_->player_coverage(match_view_player());
        inputs.sight.player_bits = sight.player_bits;
        inputs.sight.width = sight.width;
        inputs.sight.height = sight.height;
        inputs.sight.shared_bits = allied_mapped_bits();
        inputs.sight.mapped_bits = static_cast<uint16_t>(
            (1U << (sight.viewpoint_player & 0x1fU)) | inputs.sight.shared_bits
        );
        inputs.sight.mapping = match_mapping_on();
        inputs.sight.line_of_sight = match_line_of_sight_on();
        inputs.sight.dithered =
            (match_->state().game.graphics_flags & init::preference_flags::dithered_fog) != 0;
        const int32_t map_width = static_cast<int32_t>(map.attribute_width) * terrain_cell_edge;
        const int32_t map_height = static_cast<int32_t>(map.attribute_height) * terrain_cell_edge;
        inputs.left = 0;
        inputs.top = 0;
        inputs.right = map_width;
        inputs.bottom = map_height;
        // Include the whole visible footprint at both terrain height limits.
        // Map edges bound the work even when an unlocked camera is nearly horizontal.
        constexpr float terrain_margin = 2 * terrain_cell_edge;
        renderer_live_camera::TerrainBounds visible{};
        if (renderer_live_camera::visible_terrain_bounds(
                live.projection, width, height, terrain_margin, visible
            )) {
            inputs.left =
                static_cast<int32_t>(std::clamp(std::floor(visible.min_x), 0.0F, float(map_width)));
            inputs.top = static_cast<int32_t>(
                std::clamp(std::floor(visible.min_y), 0.0F, float(map_height))
            );
            inputs.right =
                static_cast<int32_t>(std::clamp(std::ceil(visible.max_x), 0.0F, float(map_width)));
            inputs.bottom =
                static_cast<int32_t>(std::clamp(std::ceil(visible.max_y), 0.0F, float(map_height)));
        }
        std::string error;
        Span building(Stage::scene_build);
        // A frame a check draws again on the card takes the scene drawn
        // last, unchanged, so that the frames after it are as they would be.
        const bool built = turned_overlay_checked_
                               ? live.scene_built
                               : renderer_live_scene::build(inputs, live.scene, error);
        live.scene_built = built;
        building.stop();
        bench_timers::note_scene(
            {static_cast<double>(live.scene.vertices.size()),
             static_cast<double>(live.scene.terrain_vertex_count),
             static_cast<double>(live.scene.model_count),
             static_cast<double>(live.scene.sprite_count),
             static_cast<double>(live.scene.particle_count),
             static_cast<double>(live.scene.nano_particle_count),
             static_cast<double>(live.scene.shadow_count),
             static_cast<double>(inputs.right - inputs.left) / terrain_cell_edge *
                 static_cast<double>(inputs.bottom - inputs.top) / terrain_cell_edge,
             bench_timers::timers.last.supersample,
             static_cast<double>(live.scene.cards.units.size())},
            live.scene.ground_revision
        );
        // Picking reads only the frame's triangles, so a frame that draws
        // rebuilds it while the card draws them.
        const auto rebuild_picking = [this] {
            const Span picking(Stage::picking);
            rebuild_live_picking();
        };
        const renderer_live_gpu::KeptGround ground{
            live.scene.terrain_vertex_count, live.scene.ground_revision
        };
        // The lights and heat over the frame, found before the card draws it
        // so that it adds them; a card that cannot leaves them to the
        // processor, which adds them to the picture read back.
        Span lighting(Stage::effects);
        const auto lights = project_match_effects(
            match_world_cpu_.width, match_world_cpu_.height, 0, 0, 1, 1, true
        );
        const renderer_live_gpu::Lights card_lights{
            lights, match_effect_settings(), static_cast<double>(match_->simulation().tick) / 30.0
        };
        lighting.stop();
        // A frame drawn to be presented, in a window that presents through
        // the device the card draws on, stays on the card: the painters
        // paint the overlay canvas and the card adds it as the window
        // presents the frame. Any other frame is read back.
        live.gpu.keep_on_card =
            (presenting_frame_ && match_use_layers_ && director_ == nullptr &&
             !options_.headless_check && render_run_ && render_run_->host != nullptr &&
             render_run_->host->presenting_on_card() &&
             sdl_.renderer == render_run_->host->renderer()) ||
            turned_overlay_checked_;
        const auto model_draws =
            renderer_live_scene::card_draws(live.scene.card_types, live.scene.cards);
        Span drawing(Stage::gpu_total);
        const auto result = built ? renderer_live_gpu::render(
                                        live.gpu,
                                        live.scene.vertices,
                                        live.scene.texture_rgba,
                                        live.scene.texture_width,
                                        live.scene.texture_height,
                                        live.scene.texture_revision,
                                        match_world_cpu_.width,
                                        match_world_cpu_.height,
                                        live.projection,
                                        live.rgba,
                                        error,
                                        static_cast<uint32_t>(engine_settings().anti_aliasing),
                                        &live.scene.atlas_update,
                                        &inputs.water,
                                        &ground,
                                        rebuild_picking,
                                        &card_lights,
                                        &model_draws
                                    )
                                  : renderer_live_gpu::Result::failed;
        drawing.stop();
        bench_timers::note_supersample(live.gpu.actual_supersample);
        const auto& last = live.drawn;
        const bool at_last =
            live.went_back || (last.held && last.camera.yaw_degrees == live.camera.yaw_degrees &&
                               last.camera.pitch_degrees == live.camera.pitch_degrees &&
                               last.place == place && last.zoom == zoom);
        live.went_back = false;
        if (result == renderer_live_gpu::Result::success && live.gpu.kept_on_card) {
            live.drawn = {true, live.camera, live.view, place, zoom, view_hold_};
            live.out_of_room = false;
            // The battlefield layer is the overlay canvas, cleared to the key
            // colour; the card holds the picture with its lights.
            auto& overlay = live.overlay;
            overlay.painted = true;
            overlay.key = full_overlay_key();
            overlay.operations.clear();
            overlay.lines = 0;
            fill_rgb24(match_world_cpu_.rgb.data(), match_world_cpu_.rgb.size() / 3, overlay.key);
        } else if (result == renderer_live_gpu::Result::success) {
            live.drawn = {true, live.camera, live.view, place, zoom, view_hold_};
            live.out_of_room = false;
            Span copying(Stage::rgb_copy);
            for (std::size_t pixel = 0; pixel < match_world_cpu_.rgb.size() / 3; ++pixel)
                std::copy_n(
                    live.rgba.data() + pixel * 4, 3, match_world_cpu_.rgb.data() + pixel * 3
                );
            copying.stop();
            if (!live.gpu.lights_drawn) {
                const Span effects(Stage::effects);
                compose_projected_match_effects(
                    match_world_cpu_.rgb,
                    match_world_cpu_.width,
                    match_world_cpu_.height,
                    match_world_cpu_.width * 3,
                    3,
                    0,
                    0,
                    1,
                    1,
                    true,
                    lights
                );
            }
        } else if (
            !built && live.scene.out_of_room && last.held && director_ == nullptr &&
            (!at_last || last_picture_read())
        ) {
            // A frame that needs more room than the renderer has is no
            // failure of the card: the view goes back to the last camera
            // drawn, its angle, height, place and zoom, and the frame is
            // drawn again from there, so that a turn or a zoom stops at
            // that limit instead of the view going back to classic.
            if (!live.out_of_room)
                std::cerr << "3D view: " << error << "; the view stays where it was last drawn\n";
            live.out_of_room = true;
            if (at_last) {
                // The camera last drawn no longer fits either, as when more
                // comes into view: its last picture stays, with nothing to
                // pick in it, and the lights the card added to it.
                rebuild_live_picking();
                for (std::size_t pixel = 0; pixel < match_world_cpu_.rgb.size() / 3; ++pixel)
                    std::copy_n(
                        live.rgba.data() + pixel * 4, 3, match_world_cpu_.rgb.data() + pixel * 3
                    );
                if (!live.gpu.lights_drawn)
                    compose_match_effects(match_world_cpu_, true);
            } else {
                const auto back = last;
                live.camera = back.camera;
                live.view = back.view;
                live.pivot_held = true;
                match_zoom_ = match_zoom_target_ = back.zoom;
                place_match_view(back.place[0], back.place[1]);
                view_hold_ = back.view_hold;
                live.went_back = true;
                // render_match_surface draws the frame again, from there.
                return false;
            }
        } else {
            live.failed = true;
            live.unavailable = result == renderer_live_gpu::Result::unavailable;
            // The fallback must also restore classic input, including wheel zoom.
            // A later camera gesture can retry from the new position and scale.
            renderer_live_camera::reset(live.camera);
            live.dragging = false;
            live.drag_button = 0;
            live.pivot_held = false;
            std::cerr << "3D view: " << error << '\n';
            // This frame left out the classic raster: render_match_surface
            // draws the frame again, once, classically.
            return false;
        }
    }
    if (!live_renderer_active())
        return true;
    // A director's frame without the interface is the world alone.
    const bool bare = director_ != nullptr && !director_->presentation.show_interface;
    if (!live.failed) {
        if (!bare) {
            Span overlays(Stage::overlays);
            draw_live_overlays();
            overlays.stop();
            Span selection(Stage::selection);
            draw_live_selection();
            selection.stop();
            const Span unit_overlays(Stage::unit_overlays);
            draw_live_unit_overlays();
        }
        const Span on_screen(Stage::on_screen_units);
        rebuild_on_screen_units();
    }
    return true;
}

bool Runtime::scroll_live_match_view(double screen_dx, double screen_dy) {
    if (!live_renderer_active() || !live_renderer_ || live_renderer_->failed)
        return false;
    auto& live = *live_renderer_;
    if (live.dragging)
        return true;
    float dx{}, dz{};
    if (!renderer_live_camera::pan_delta(
            live.projection, static_cast<float>(screen_dx), static_cast<float>(screen_dy), dx, dz
        ))
        return false;
    stop_match_tracking();
    const auto place = match_view_place();
    place_match_view(place[0] + dx, place[1] + dz);
    const auto moved = match_view_place();
    remember_placed_view(moved[0], moved[1]);
    return true;
}

void Runtime::rebuild_live_picking() {
    auto& live = *live_renderer_;
    live.projected.clear();
    live.projected.reserve(live.scene.vertices.size());
    const auto& m = live.projection;
    for (const auto& vertex : live.scene.vertices)
        live.projected.push_back(
            {m[0] * vertex.x + m[1] * vertex.height + m[2] * vertex.z + m[3],
             m[4] * vertex.x + m[5] * vertex.height + m[6] * vertex.z + m[7],
             m[8] * vertex.x + m[9] * vertex.height + m[10] * vertex.z + m[11]}
        );
    live.triangle_units.assign(live.scene.vertices.size() / 3, 0);
    live.unit_bounds.clear();
    for (const auto& range : live.scene.unit_ranges) {
        if (range.unit_id == 0 || range.vertex_count == 0 ||
            range.first_vertex + range.vertex_count > live.projected.size() ||
            !match_->unit_shown(match_view_player(), range.unit_id))
            continue;
        auto [entry, inserted] = live.unit_bounds.try_emplace(
            range.unit_id,
            oa::Rect32{
                std::numeric_limits<int32_t>::max(),
                std::numeric_limits<int32_t>::max(),
                std::numeric_limits<int32_t>::min(),
                std::numeric_limits<int32_t>::min()
            }
        );
        auto& bounds = entry->second;
        for (std::size_t index = range.first_vertex;
             index < range.first_vertex + range.vertex_count;
             ++index) {
            const auto& point = live.projected[index];
            bounds.x1 = std::min(
                bounds.x1, static_cast<int32_t>(std::floor(point[0])) + match_layout_.left
            );
            bounds.y1 =
                std::min(bounds.y1, static_cast<int32_t>(std::floor(point[1])) + match_layout_.top);
            bounds.x2 =
                std::max(bounds.x2, static_cast<int32_t>(std::ceil(point[0])) + match_layout_.left);
            bounds.y2 =
                std::max(bounds.y2, static_cast<int32_t>(std::ceil(point[1])) + match_layout_.top);
            live.triangle_units[index / 3] = range.unit_id;
        }
    }
    // The models the card draws: each one's bounds over the corners it
    // draws, which the scene worked out through the same camera as these
    // vertices.
    const auto& cards = live.scene.cards;
    live.card_bounds.assign(cards.units.size(), oa::Rect32{1, 1, 0, 0});
    for (std::size_t at = 0; at < cards.units.size(); ++at) {
        const auto& drawn = cards.units[at];
        if (drawn.index_count == 0)
            continue;
        const oa::Rect32 bounds{
            static_cast<int32_t>(std::floor(drawn.left)) + match_layout_.left,
            static_cast<int32_t>(std::floor(drawn.top)) + match_layout_.top,
            static_cast<int32_t>(std::ceil(drawn.right)) + match_layout_.left,
            static_cast<int32_t>(std::ceil(drawn.bottom)) + match_layout_.top
        };
        live.card_bounds[at] = bounds;
        if (drawn.unit_id == 0 || !match_->unit_shown(match_view_player(), drawn.unit_id))
            continue;
        const auto [entry, inserted] = live.unit_bounds.try_emplace(drawn.unit_id, bounds);
        if (inserted)
            continue;
        auto& merged = entry->second;
        merged.x1 = std::min(merged.x1, bounds.x1);
        merged.y1 = std::min(merged.y1, bounds.y1);
        merged.x2 = std::max(merged.x2, bounds.x2);
        merged.y2 = std::max(merged.y2, bounds.y2);
    }
}

std::optional<oa::Rect32> Runtime::live_unit_bounds(uint16_t id) const {
    if (!live_renderer_ || live_renderer_->failed || !match_ || id == 0 ||
        id >= match_->world().slots.size())
        return std::nullopt;
    const auto found = live_renderer_->unit_bounds.find(id);
    if (found == live_renderer_->unit_bounds.end())
        return std::nullopt;
    // A previously drawn unit can leave its slot before the next frame. Empty
    // slots have no model bounds and must never enter the visibility query.
    const auto& slot = match_->world().slots[id];
    if (!slot.unit || slot.record.type_index == 0 || !match_->unit_shown(match_view_player(), id))
        return std::nullopt;
    return found->second;
}

oa::present::world_renderer::ScreenPoint
Runtime::live_project_world(float x, float height, float z) const {
    if (!live_renderer_)
        return {};
    const auto& m = live_renderer_->projection;
    return {
        match_layout_.left +
            static_cast<int32_t>(std::lround(m[0] * x + m[1] * height + m[2] * z + m[3])),
        match_layout_.top +
            static_cast<int32_t>(std::lround(m[4] * x + m[5] * height + m[6] * z + m[7]))
    };
}

std::optional<oa::sim::ground_orders::Point> Runtime::live_ground_point(float x, float y) const {
    const auto hit = live_ground_hit(x, y);
    if (!hit)
        return std::nullopt;
    // The map pixel the hit lies in, at the height the simulation gives that
    // pixel and never below the sea, as terrain_intersection takes it for a
    // classic pick; the mesh's height between cells and the fraction of the
    // pixel are the drawing's alone.
    const auto whole = [](int32_t fixed) {
        return std::bit_cast<int32_t>(static_cast<uint32_t>(fixed) & 0xffff0000U);
    };
    const int32_t map_x = whole((*hit)[0]);
    const int32_t map_z = whole((*hit)[2]);
    const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
    const int32_t height = std::max<int32_t>(terrain.sea_level(), terrain.height(map_x, map_z));
    // The pick reaches no further past the map's shown edges than the
    // classic one: the turned view draws the ground there, 3.1c's view never.
    const auto [shown_width, shown_height] = shown_map_size();
    const auto held = oa::sim::gameplay_input::ground_point_on_shown_map(
        terrain,
        {map_x, std::bit_cast<int32_t>(static_cast<uint32_t>(height) << 16U), map_z},
        shown_width,
        shown_height,
        static_cast<int32_t>(selected_tnt_->attribute_width * 16U),
        static_cast<int32_t>(selected_tnt_->attribute_height * 16U)
    );
    return oa::sim::ground_orders::Point{held.x, held.y, held.z};
}

std::optional<oa::sim::ground_orders::Point> Runtime::live_ground_hit(float x, float y) const {
    if (!live_renderer_ || live_renderer_->failed || !battlefield_contains(x, y))
        return std::nullopt;
    const auto& live = *live_renderer_;
    float best_depth = -std::numeric_limits<float>::infinity();
    std::optional<oa::sim::ground_orders::Point> result;
    constexpr float fixed_scale = 65536;
    for (std::size_t index = 0;
         index + 2 < live.scene.terrain_vertex_count && index + 2 < live.projected.size();
         index += 3) {
        std::array<float, 3> weights{};
        float depth{};
        if (!renderer_live_camera::triangle_hit(
                {live.projected[index], live.projected[index + 1], live.projected[index + 2]},
                x - match_layout_.left,
                y - match_layout_.top,
                weights,
                depth
            ) ||
            depth <= best_depth)
            continue;
        float world_x{}, height{}, world_z{};
        for (std::size_t corner = 0; corner < weights.size(); ++corner) {
            const auto& vertex = live.scene.vertices[index + corner];
            world_x += vertex.x * weights[corner];
            height += vertex.height * weights[corner];
            world_z += vertex.z * weights[corner];
        }
        // The drawn coastal relief may extend beyond the last simulation cell.
        if (world_x < 0 || world_z < 0 ||
            world_x >= selected_tnt_->attribute_width * terrain_cell_edge ||
            world_z >= selected_tnt_->attribute_height * terrain_cell_edge)
            continue;
        best_depth = depth;
        result = oa::sim::ground_orders::Point{
            static_cast<int32_t>(world_x * fixed_scale),
            static_cast<int32_t>(height * fixed_scale),
            static_cast<int32_t>(world_z * fixed_scale)
        };
    }
    return result;
}

uint16_t Runtime::live_unit_under(float x, float y) const {
    if (!live_renderer_ || live_renderer_->failed || !battlefield_contains(x, y))
        return 0;
    const auto& live = *live_renderer_;
    uint16_t picked{};
    float best_depth = -std::numeric_limits<float>::infinity();
    // The models the card draws whose bounds hold the pointer, a pixel
    // wider, are tested where their triangles fall among the frame's, as
    // their vertices would have been.
    const auto& cards = live.scene.cards;
    std::vector<std::size_t> held;
    for (std::size_t at = 0; at < cards.units.size() && at < live.card_bounds.size(); ++at) {
        const auto& bounds = live.card_bounds[at];
        if (bounds.x1 <= bounds.x2 && x >= float(bounds.x1 - 1) && x <= float(bounds.x2 + 1) &&
            y >= float(bounds.y1 - 1) && y <= float(bounds.y2 + 1))
            held.push_back(at);
    }
    std::size_t next_held = 0;
    const auto& m = live.projection;
    const auto test_held = [&](std::size_t before) {
        for (; next_held < held.size() && cards.units[held[next_held]].vertex <= before;
             ++next_held) {
            const auto& drawn = cards.units[held[next_held]];
            const auto indices = std::span<const uint32_t>(cards.indices)
                                     .subspan(drawn.first_index, drawn.index_count);
            for (std::size_t first = 0; first + 2 < indices.size(); first += 3) {
                std::array<renderer_live_scene::CardCorner, 3> corners{};
                std::array<std::array<float, 3>, 3> points{};
                for (std::size_t corner = 0; corner < corners.size(); ++corner) {
                    const auto& made = corners[corner] = renderer_live_scene::card_corner(
                        live.scene.card_types, cards, drawn, indices[first + corner]
                    );
                    points[corner] = {
                        m[0] * made.x + m[1] * made.height + m[2] * made.z + m[3],
                        m[4] * made.x + m[5] * made.height + m[6] * made.z + m[7],
                        m[8] * made.x + m[9] * made.height + m[10] * made.z + m[11]
                    };
                }
                std::array<float, 3> weights{};
                float depth{};
                if (!renderer_live_camera::triangle_hit(
                        points, x - match_layout_.left, y - match_layout_.top, weights, depth
                    ) ||
                    depth <= best_depth)
                    continue;
                float u{}, v{}, opacity{};
                for (std::size_t corner = 0; corner < weights.size(); ++corner) {
                    u += corners[corner].u * weights[corner];
                    v += corners[corner].v * weights[corner];
                    opacity += 1.0F * weights[corner];
                }
                const auto column = std::clamp(
                    static_cast<int32_t>(u * live.scene.texture_width),
                    0,
                    static_cast<int32_t>(live.scene.texture_width) - 1
                );
                const auto row = std::clamp(
                    static_cast<int32_t>(v * live.scene.texture_height),
                    0,
                    static_cast<int32_t>(live.scene.texture_height) - 1
                );
                const auto alpha_index =
                    (static_cast<std::size_t>(row) * live.scene.texture_width + column) * 4 + 3;
                const auto id = drawn.unit_id;
                if (alpha_index >= live.scene.texture_rgba.size() ||
                    live.scene.texture_rgba[alpha_index] * (id != 0 ? 1.0F : opacity) < 128)
                    continue;
                if (id != 0 && !live_unit_bounds(id))
                    continue;
                best_depth = depth;
                picked = id;
            }
        }
    };
    for (std::size_t index = 0; index + 2 < live.projected.size(); index += 3) {
        if (next_held < held.size())
            test_held(index);
        std::array<float, 3> weights{};
        float depth{};
        if (!renderer_live_camera::triangle_hit(
                {live.projected[index], live.projected[index + 1], live.projected[index + 2]},
                x - match_layout_.left,
                y - match_layout_.top,
                weights,
                depth
            ) ||
            depth <= best_depth)
            continue;
        float u{}, v{}, opacity{};
        for (std::size_t corner = 0; corner < weights.size(); ++corner) {
            const auto& vertex = live.scene.vertices[index + corner];
            u += vertex.u * weights[corner];
            v += vertex.v * weights[corner];
            opacity += vertex.opacity * weights[corner];
        }
        const auto column = std::clamp(
            static_cast<int32_t>(u * live.scene.texture_width),
            0,
            static_cast<int32_t>(live.scene.texture_width) - 1
        );
        const auto row = std::clamp(
            static_cast<int32_t>(v * live.scene.texture_height),
            0,
            static_cast<int32_t>(live.scene.texture_height) - 1
        );
        const auto alpha_index =
            (static_cast<std::size_t>(row) * live.scene.texture_width + column) * 4 + 3;
        // Smoke and translucent ground overlays do not block gameplay picks;
        // a unit is picked wherever its texels cover, a cloaked one too.
        const auto id = live.triangle_units[index / 3];
        if (alpha_index >= live.scene.texture_rgba.size() ||
            live.scene.texture_rgba[alpha_index] * (id != 0 ? 1.0F : opacity) < 128)
            continue;
        if (id != 0 && !live_unit_bounds(id))
            continue;
        best_depth = depth;
        picked = id;
    }
    test_held(std::numeric_limits<std::size_t>::max());
    return picked;
}

std::vector<uint8_t> Runtime::live_terrain_visibility_mask() const {
    std::vector<uint8_t> visibility;
    std::vector<float> depth;
    live_scene_visibility(visibility, depth, true);
    return visibility;
}

float Runtime::live_project_depth(float x, float height, float z) const {
    if (!live_renderer_ || live_renderer_->failed)
        return -std::numeric_limits<float>::infinity();
    const auto& m = live_renderer_->projection;
    return m[8] * x + m[9] * height + m[10] * z + m[11];
}

void Runtime::live_scene_visibility(
    std::vector<uint8_t>& visibility,
    std::vector<float>& depths,
    bool terrain_only,
    std::span<const uint32_t> row_left,
    std::span<const uint32_t> row_right,
    std::span<const uint8_t> requested_pixels
) const {
    // Preserve surface depth and alpha cutouts for lighting and ground overlays.
    visibility.clear();
    depths.clear();
    constexpr uint32_t maximum_dimension = 4096;
    const uint32_t width = match_world_cpu_.width;
    const uint32_t height = match_world_cpu_.height;
    if (!live_renderer_ || live_renderer_->failed || width == 0 || height == 0 ||
        width > maximum_dimension || height > maximum_dimension)
        return;
    if ((!row_left.empty() || !row_right.empty()) &&
        (row_left.size() != height || row_right.size() != height))
        return;
    const bool bounded = !row_left.empty();
    const auto& live = *live_renderer_;
    if (live.scene.texture_width == 0 || live.scene.texture_height == 0)
        return;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (!requested_pixels.empty() && requested_pixels.size() != count)
        return;
    visibility.resize(count);
    depths.assign(count, -std::numeric_limits<float>::infinity());
    // Most scene triangles lie outside the small islands reached by lights.
    // Reject against short row bands before preparing barycentric coefficients.
    constexpr uint32_t rows_per_band = 16;
    const uint32_t band_count = (height + rows_per_band - 1) / rows_per_band;
    auto& lists = live_renderer_->visibility_lists;
    auto& band_left = lists.band_left;
    auto& band_right = lists.band_right;
    uint32_t requested_left = width, requested_right = 0;
    uint32_t requested_top = height, requested_bottom = 0;
    if (bounded) {
        band_left.assign(band_count, width);
        band_right.assign(band_count, 0);
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t left = std::min(row_left[y], width);
            const uint32_t right = std::min(row_right[y], width);
            if (left >= right)
                continue;
            const auto band = y / rows_per_band;
            band_left[band] = std::min(band_left[band], left);
            band_right[band] = std::max(band_right[band], right);
            requested_left = std::min(requested_left, left);
            requested_right = std::max(requested_right, right);
            requested_top = std::min(requested_top, y);
            requested_bottom = y + 1;
        }
        if (requested_left >= requested_right)
            return;
    }

    // Each band of rows takes the triangles that reach it in their order and
    // writes its own rows alone, so each pixel takes the triangles in the
    // same order whatever the bands and threads. One thread takes the rows
    // as one band.
    const uint32_t raster_band_rows =
        draw_pool_ && draw_pool_->threads() > 1 ? rows_per_band : height;
    const uint32_t raster_bands = platform::job_pool::bands_of_rows(height, raster_band_rows);

    // Runs of triangles are bounded on the pool, each run into its own list,
    // which the run then sorts by the bands its triangles reach, keeping
    // their order within each band.
    const auto vertex_count = std::min(live.projected.size(), live.scene.vertices.size());
    constexpr std::size_t triangles_per_run = 4096;
    const std::size_t triangle_count = vertex_count / 3;
    const auto runs =
        static_cast<uint32_t>((triangle_count + triangles_per_run - 1) / triangles_per_run);
    if (lists.found.size() < runs) {
        lists.found.resize(runs);
        lists.by_band.resize(runs);
    }
    const std::size_t starts_per_run = static_cast<std::size_t>(raster_bands) + 1;
    lists.band_starts.resize(runs * starts_per_run);
    platform::job_pool::run_bands(draw_pool_.get(), runs, [&](uint32_t run) {
        auto& found = lists.found[run];
        found.clear();
        const std::size_t run_end =
            std::min(triangle_count, (static_cast<std::size_t>(run) + 1) * triangles_per_run) * 3;
        for (std::size_t index = static_cast<std::size_t>(run) * triangles_per_run * 3;
             index < run_end;
             index += 3) {
            const std::array triangle{
                live.projected[index], live.projected[index + 1], live.projected[index + 2]
            };
            const float left = std::min({triangle[0][0], triangle[1][0], triangle[2][0]});
            const float right = std::max({triangle[0][0], triangle[1][0], triangle[2][0]});
            const float top = std::min({triangle[0][1], triangle[1][1], triangle[2][1]});
            const float bottom = std::max({triangle[0][1], triangle[1][1], triangle[2][1]});
            if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) ||
                !std::isfinite(bottom))
                continue;
            if (bounded && (right <= requested_left || left >= requested_right ||
                            bottom <= requested_top || top >= requested_bottom))
                continue;
            const int x0 = static_cast<int>(std::clamp(std::floor(left), 0.0F, float(width)));
            const int x1 = static_cast<int>(std::clamp(std::ceil(right), 0.0F, float(width)));
            const int y0 = static_cast<int>(std::clamp(std::floor(top), 0.0F, float(height)));
            const int y1 = static_cast<int>(std::clamp(std::ceil(bottom), 0.0F, float(height)));
            if (x0 >= x1 || y0 >= y1)
                continue;
            if (bounded) {
                bool intersects = false;
                for (auto band = static_cast<uint32_t>(y0) / rows_per_band;
                     band <= static_cast<uint32_t>(y1 - 1) / rows_per_band;
                     ++band) {
                    if (x1 > static_cast<int>(band_left[band]) &&
                        x0 < static_cast<int>(band_right[band])) {
                        intersects = true;
                        break;
                    }
                }
                if (!intersects)
                    continue;
            }
            found.push_back({index, x0, x1, y0, y1});
        }
        // The run's places in each band, counted, then filled in order.
        uint32_t* const starts = lists.band_starts.data() + run * starts_per_run;
        std::fill(starts, starts + starts_per_run, 0U);
        for (const auto& reach : found)
            for (auto band = static_cast<uint32_t>(reach.y0) / raster_band_rows;
                 band <= static_cast<uint32_t>(reach.y1 - 1) / raster_band_rows;
                 ++band)
                ++starts[band + 1];
        for (uint32_t band = 0; band < raster_bands; ++band)
            starts[band + 1] += starts[band];
        auto& by_band = lists.by_band[run];
        by_band.resize(starts[raster_bands]);
        for (uint32_t place = 0; place < found.size(); ++place)
            for (auto band = static_cast<uint32_t>(found[place].y0) / raster_band_rows;
                 band <= static_cast<uint32_t>(found[place].y1 - 1) / raster_band_rows;
                 ++band)
                by_band[starts[band]++] = place;
        // Each band's start has moved on to the next band's: move them back.
        std::copy_backward(starts, starts + raster_bands, starts + starts_per_run);
        starts[0] = 0;
    });
    platform::job_pool::run_bands(draw_pool_.get(), raster_bands, [&](uint32_t band) {
        // The band's own copies of what each pixel reads, which the pixels'
        // writes cannot change.
        const auto& vertices = live.scene.vertices;
        const std::size_t terrain_vertex_count = live.scene.terrain_vertex_count;
        const uint32_t texture_width = live.scene.texture_width;
        const uint32_t texture_height = live.scene.texture_height;
        const uint8_t* const texture = live.scene.texture_rgba.data();
        const std::size_t texture_size = live.scene.texture_rgba.size();
        const uint8_t* const requested =
            requested_pixels.empty() ? nullptr : requested_pixels.data();
        float* const pixel_depths = depths.data();
        uint8_t* const pixel_visibility = visibility.data();
        const int band_top = static_cast<int>(band * raster_band_rows);
        const int band_end = std::min(static_cast<int>(height), band_top + int(raster_band_rows));
        // The pixels a triangle hits, each weighed before any is drawn, so
        // that no pixel waits for the answer of the one before.
        constexpr int most_hits = 128;
        std::array<uint32_t, most_hits> hit_pixels;
        std::array<renderer_live_camera::PreparedHit, most_hits> hits;
        // A triangle's rows in the band, a few at a time: each row's first
        // column and the column past its last, and one place more for the
        // first column after the last row.
        constexpr int most_spans = 16;
        std::array<int, most_spans + 1> span_rows{}, span_firsts{}, span_ends{};
        constexpr float pixel_centre = 0.5F;
        for (uint32_t run = 0; run < runs; ++run) {
            const uint32_t* const starts = lists.band_starts.data() + run * starts_per_run;
            const uint32_t* const places = lists.by_band[run].data();
            const VisibilityReach* const found = lists.found[run].data();
            for (uint32_t slot = starts[band]; slot < starts[band + 1]; ++slot) {
                const auto [index, x0, x1, y0, y1] = found[places[slot]];
                const std::array triangle{
                    live.projected[index], live.projected[index + 1], live.projected[index + 2]
                };
                renderer_live_camera::PreparedTriangle preparing;
                if (!renderer_live_camera::prepare_triangle(triangle, preparing))
                    continue;
                // A copy of its own, as of the corners' attributes below.
                const auto prepared = preparing;
                // A triangle a few columns wide has no row to narrow.
                const auto narrowing = x1 - x0 > renderer_live_camera::most_columns_left_whole
                                           ? renderer_live_camera::prepare_narrowing(prepared)
                                           : renderer_live_camera::RowNarrowing{};
                // The corners' texture places, opacities and brightnesses.
                std::array<float, 3> corner_u{}, corner_v{}, corner_opacity{}, corner_brightness{};
                for (std::size_t corner = 0; corner < corner_u.size(); ++corner) {
                    const auto& vertex = vertices[index + corner];
                    corner_u[corner] = vertex.u;
                    corner_v[corner] = vertex.v;
                    corner_opacity[corner] = vertex.opacity;
                    corner_brightness[corner] = vertex.brightness;
                }
                // Unmapped black terrain must not reveal its contours.
                const bool lit = !terrain_only || index < terrain_vertex_count;
                // Draws the first pixels hit, in order.
                const auto draw = [&](int hit_count) {
                    for (int at = 0; at < hit_count; ++at) {
                        const uint32_t pixel = hit_pixels[at];
                        if (requested != nullptr && requested[pixel] == 0)
                            continue;
                        const auto& weights = hits[at].weights;
                        const float depth = hits[at].depth;
                        if (depth <= pixel_depths[pixel])
                            continue;
                        float u{}, v{}, opacity{}, brightness{};
                        for (std::size_t corner = 0; corner < weights.size(); ++corner) {
                            u += corner_u[corner] * weights[corner];
                            v += corner_v[corner] * weights[corner];
                            opacity += corner_opacity[corner] * weights[corner];
                            brightness += corner_brightness[corner] * weights[corner];
                        }
                        constexpr float opaque_alpha = 128;
                        constexpr float full_alpha = 255;
                        if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(opacity) ||
                            opacity * full_alpha < opaque_alpha)
                            continue;
                        const auto column = static_cast<uint32_t>(
                            std::clamp(u * texture_width, 0.0F, float(texture_width - 1))
                        );
                        const auto row = static_cast<uint32_t>(
                            std::clamp(v * texture_height, 0.0F, float(texture_height - 1))
                        );
                        const auto alpha =
                            (static_cast<std::size_t>(row) * texture_width + column) * 4 + 3;
                        if (alpha >= texture_size || texture[alpha] * opacity < opaque_alpha)
                            continue;
                        pixel_depths[pixel] = depth;
                        // Known terrain carries the same smooth fog attenuation as its draw.
                        pixel_visibility[pixel] =
                            lit && std::isfinite(brightness)
                                ? static_cast<uint8_t>(
                                      std::clamp(brightness, 0.0F, 1.0F) * full_alpha
                                  )
                                : 0;
                    }
                };
                int hit_count = 0;
                int y = std::max(y0, band_top);
                const int rows_end = std::min(y1, band_end);
                while (y < rows_end) {
                    // The columns the triangle can cover on its next rows,
                    // rows it cannot cover left out.
                    int spans = 0;
                    int columns = 0;
                    for (const int spans_end = std::min(rows_end, y + most_spans); y < spans_end;
                         ++y) {
                        int left_limit =
                            bounded ? std::max(x0, static_cast<int>(std::min(row_left[y], width)))
                                    : x0;
                        int right_limit =
                            bounded ? std::min(x1, static_cast<int>(std::min(row_right[y], width)))
                                    : x1;
                        renderer_live_camera::narrow_row(
                            narrowing, y + pixel_centre, pixel_centre, left_limit, right_limit
                        );
                        const int span_columns = std::max(0, right_limit - left_limit);
                        span_rows[spans] = y;
                        span_firsts[spans] = left_limit;
                        span_ends[spans] = right_limit;
                        columns += span_columns;
                        spans += span_columns > 0 ? 1 : 0;
                    }
                    // Their pixels in one run, row after row: the next row is
                    // taken up by choosing values, not by a loop of its own,
                    // so the weighing does not stall at the end of each row.
                    int span = 0;
                    int x = span_firsts[0];
                    for (int column = 0; column < columns; ++column) {
                        if (hit_count == most_hits) {
                            draw(hit_count);
                            hit_count = 0;
                        }
                        const int row = span_rows[span];
                        hit_pixels[hit_count] =
                            static_cast<uint32_t>(row) * width + static_cast<uint32_t>(x);
                        hits[hit_count] = renderer_live_camera::weigh_prepared(
                            prepared, x + pixel_centre, row + pixel_centre
                        );
                        // Each place is written whether or not the triangle
                        // hits it; only a hit moves on to the next.
                        hit_count += hits[hit_count].hit;
                        ++x;
                        const bool row_done = x == span_ends[span];
                        span += row_done ? 1 : 0;
                        x = row_done ? span_firsts[span] : x;
                    }
                }
                if (hit_count > 0)
                    draw(hit_count);
            }
        }
    });
}

void Runtime::draw_live_selection() {
    // Draw directly into the GPU readback so feedback follows the shown geometry.
    const auto line = [&](int x0, int y0, int x1, int y1, const std::array<uint8_t, 3>& color) {
        const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        if (steps > 32768)
            return;
        for (int step = 0; step <= steps; ++step) {
            const float share = steps == 0 ? 0 : static_cast<float>(step) / steps;
            const int x =
                static_cast<int>(std::lround(x0 + (x1 - x0) * share)) - match_layout_.left;
            const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * share)) - match_layout_.top;
            if (x < 0 || y < 0 || x >= static_cast<int>(match_world_cpu_.width) ||
                y >= static_cast<int>(match_world_cpu_.height))
                continue;
            const auto at = (static_cast<std::size_t>(y) * match_world_cpu_.width + x) * 3;
            std::copy(
                color.begin(),
                color.end(),
                match_world_cpu_.rgb.begin() + static_cast<std::ptrdiff_t>(at)
            );
        }
    };
    const auto box = [&](const oa::Rect32& bounds, const std::array<uint8_t, 3>& color) {
        line(bounds.x1, bounds.y1, bounds.x2, bounds.y1, color);
        line(bounds.x2, bounds.y1, bounds.x2, bounds.y2, color);
        line(bounds.x2, bounds.y2, bounds.x1, bounds.y2, color);
        line(bounds.x1, bounds.y2, bounds.x1, bounds.y1, color);
    };
    constexpr std::array<uint8_t, 3> selection_color{0, 255, 0};
    constexpr std::array<uint8_t, 3> hover_color{255, 255, 255};
    for (const auto id : selected_local_ids())
        if (const auto bounds = live_unit_bounds(id))
            box(*bounds, selection_color);
    if (const auto bounds = live_unit_bounds(hovered_match_unit_))
        box(*bounds, hover_color);
    const bool queue_drag = build_queue_drag_ && build_queue_drag_->moved;
    if (match_command_ == MatchCommand::build || queue_drag) {
        const auto ground = build_cursor_point(match_pointer_x_, match_pointer_y_);
        const auto site =
            !ground ? std::nullopt
            : queue_drag
                ? build_site_for(build_queue_drag_->type, build_queue_drag_->facing, *ground)
                : pending_build_site(*ground);
        if (site) {
            const float x0 = static_cast<float>(site->cell_x * terrain_cell_edge);
            const float z0 = static_cast<float>(site->cell_z * terrain_cell_edge);
            const float x1 = x0 + site->footprint_x * terrain_cell_edge;
            const float z1 = z0 + site->footprint_z * terrain_cell_edge;
            constexpr float fixed_scale = 65536;
            const float height = site->world[1] / fixed_scale;
            const std::array corners{
                live_project_world(x0, height, z0),
                live_project_world(x1, height, z0),
                live_project_world(x1, height, z1),
                live_project_world(x0, height, z1)
            };
            const auto color = site->legal ? selection_color : std::array<uint8_t, 3>{255, 0, 0};
            for (std::size_t edge = 0; edge < corners.size(); ++edge) {
                const auto next = (edge + 1) % corners.size();
                line(corners[edge].x, corners[edge].y, corners[next].x, corners[next].y, color);
            }
        }
    }
    if (match_drag_) {
        const auto corners = match_drag_corners(live_viewport(match_camera_x_, match_camera_z_));
        box({corners[0].x, corners[0].y, corners[1].x, corners[1].y}, selection_color);
    }
}

} // namespace oa::app
