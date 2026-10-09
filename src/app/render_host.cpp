// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "render_host.hpp"

#include "graphics_report.hpp"
#include "oa/app/acceleration_status.hpp"
#include "oa/app/scaled_world.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/platform/machine.hpp"
#include "oa/platform/memory_status.hpp"
#include "oa/present/world_renderer/scene_filter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace oa::app {

namespace render_probe = oa::platform::render_probe;

std::string refusal_log_line(std::string_view driver, std::string_view reason) {
    std::string line(graphics_log_prefix);
    line += "renderer ";
    line += driver;
    line += " refused: ";
    line += reason;
    return line;
}

std::string framebuffer_hint_log_line(std::string_view value, std::string_view reason) {
    std::string line(graphics_log_prefix);
    line += "the framebuffer hint \"";
    line += value;
    line += "\" was not taken: ";
    line += reason;
    return line;
}

std::string second_walk_log_line() {
    std::string line(graphics_log_prefix);
    line += "no driver left could present; trying SDL's render drivers again from the top, "
            "those recorded as failed too";
    return line;
}

namespace {

/// Logs a line through the hooks, where they log.
///
/// @param hooks the walk's hooks
/// @param line the line, without its line break
void log_line(const CreationHooks& hooks, const std::string& line) {
    if (hooks.log != nullptr)
        hooks.log(hooks.context, line);
}

} // namespace

std::string rebuild_log_line(std::string_view driver, std::string_view reason) {
    std::string line(graphics_log_prefix);
    line += driver;
    line += " failed: ";
    line += reason;
    line += "; making another renderer";
    return line;
}

namespace {

/// Walks the render drivers from where a walk stands until one makes the
/// renderer (walk_render_drivers).
///
/// @param walk the walk, before its first attempt
/// @param inputs the inputs the walk was started with
/// @param hooks what sets the hint, makes the renderer and logs
/// @return every attempt, whether the last made the renderer, and the
///     error when none did
CreationOutcome walk_drivers(
    render_policy::CreationWalk walk,
    const render_policy::CreationInputs& inputs,
    const CreationHooks& hooks
);

} // namespace

CreationOutcome
walk_render_drivers(const render_policy::CreationInputs& inputs, const CreationHooks& hooks) {
    return walk_drivers(render_policy::start_creation(inputs), inputs, hooks);
}

CreationOutcome walk_rebuild_drivers(
    const render_policy::CreationInputs& inputs,
    std::string_view failed_driver,
    const CreationHooks& hooks
) {
    return walk_drivers(render_policy::start_rebuild(inputs, failed_driver), inputs, hooks);
}

namespace {

CreationOutcome walk_drivers(
    render_policy::CreationWalk walk,
    const render_policy::CreationInputs& inputs,
    const CreationHooks& hooks
) {
    CreationOutcome outcome;
    bool second_walk_logged = false;
    for (;;) {
        const render_policy::Attempt attempt = render_policy::next_attempt(walk, inputs);
        if (attempt.kind == render_policy::AttemptKind::none)
            break;
        if (attempt.records_ignored && !second_walk_logged) {
            log_line(hooks, second_walk_log_line());
            second_walk_logged = true;
        }
        if (attempt.set_framebuffer_hint) {
            std::string reason;
            if (hooks.set_framebuffer_hint == nullptr ||
                !hooks.set_framebuffer_hint(hooks.context, attempt.framebuffer_hint, reason))
                log_line(
                    hooks,
                    framebuffer_hint_log_line(
                        attempt.framebuffer_hint,
                        reason.empty() ? unexplained_refusal : std::string_view(reason)
                    )
                );
        }
        CreationAttempt made;
        if (attempt.kind == render_policy::AttemptKind::driver)
            made.driver = std::string(attempt.driver);
        std::string reason;
        made.created = hooks.create != nullptr && hooks.create(hooks.context, made.driver, reason);
        if (made.created) {
            outcome.created = true;
            outcome.skipped_by_record = walk.skipped_by_record && !walk.records_ignored;
            outcome.records_ignored = walk.records_ignored;
            // A start walks SDL's order from the top, so what it passed over
            // by record are the recorded drivers before the one it made.
            if (outcome.skipped_by_record && walk.kind == render_policy::WalkKind::start)
                for (const std::string_view driver : inputs.sdl_order) {
                    if (driver == made.driver)
                        break;
                    if (std::find(
                            inputs.failed_drivers.begin(), inputs.failed_drivers.end(), driver
                        ) != inputs.failed_drivers.end())
                        outcome.skipped.emplace_back(driver);
                }
            outcome.attempts.push_back(std::move(made));
            return outcome;
        }
        made.error = reason.empty() ? std::string(unexplained_refusal) : std::move(reason);
        if (attempt.kind == render_policy::AttemptKind::driver)
            log_line(hooks, refusal_log_line(made.driver, made.error));
        outcome.attempts.push_back(std::move(made));
    }
    outcome.skipped_by_record = walk.skipped_by_record && !walk.records_ignored;
    outcome.records_ignored = walk.records_ignored;
    outcome.error = std::string(renderer_creation_error);
    outcome.error += outcome.attempts.empty() ? no_render_driver
                                              : std::string_view(outcome.attempts.back().error);
    return outcome;
}

#if SDL_VERSION_ATLEAST(3, 4, 0)
/// The first SDL release whose framebuffer hint takes a comma list of
/// drivers.
constexpr int framebuffer_list_version = SDL_VERSIONNUM(3, 4, 0);
#endif

/// Says whether the framebuffer hint takes a comma list of drivers: the
/// SDL the game was built against and the SDL it runs on are both 3.4 or
/// later, since a shared SDL library can be older than its headers.
///
/// @return true where a list is taken; otherwise the hint names one driver
bool framebuffer_hint_takes_list() noexcept {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    return SDL_GetVersion() >= framebuffer_list_version;
#else
    return false;
#endif
}

/// What SDL's creation hooks act on.
struct SdlCreation {
    SDL_Window* window{};             ///< the window the renderer is made for
    SDL_Renderer* renderer{};         ///< the renderer made; null until one is
    const RenderFaultHooks* faults{}; ///< what a check forces; null for nothing
    /// The window presents SDL's software renderer through a framebuffer of
    /// its own, so the framebuffer hint names no driver.
    bool native_framebuffer{};
    /// The framebuffer hint the walk last set; empty before it sets one.
    std::string hint{};
    void* creating_context{}; ///< passed back to creating
    /// Notes the driver about to be tried, with the framebuffer hint's list
    /// it presents through where it is SDL's software renderer on a window
    /// with no framebuffer of its own; null notes nothing.
    void (*creating)(
        void* context, const std::string& driver, const std::vector<std::string>& via
    ){};
};

/// Splits a comma list of drivers.
///
/// @param list the list
/// @return its names, in its order
std::vector<std::string> split_driver_list(std::string_view list) {
    std::vector<std::string> names;
    for (std::size_t start = 0; start < list.size();) {
        const std::size_t comma = std::min(list.find(',', start), list.size());
        if (comma > start)
            names.emplace_back(list.substr(start, comma - start));
        start = comma + 1;
    }
    return names;
}

/// Sets SDL's framebuffer hint at normal priority, so that an environment
/// variable still wins.
///
/// @param context the SdlCreation, which keeps the value
/// @param value the hint's value
/// @param[out] error SDL's reason when the hint was not taken; empty when
///     SDL gave none, as when the hint is already held at a higher priority
/// @return true when SDL took it
bool set_sdl_framebuffer_hint(void* context, const std::string& value, std::string& error) {
    static_cast<SdlCreation*>(context)->hint = value;
    // SDL gives a reason only for some refusals, so an older error must not
    // stand in for one it did not give.
    SDL_ClearError();
    if (SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, value.c_str()))
        return true;
    error = SDL_GetError();
    return false;
}

/// Creates the renderer of a render driver, or SDL's own choice.
///
/// @param context the SdlCreation
/// @param driver SDL's name for the render driver; empty for SDL's own choice
/// @param[out] error SDL's reason when the driver refused
/// @return true when the renderer was made
bool create_sdl_renderer(void* context, const std::string& driver, std::string& error) {
    auto& creation = *static_cast<SdlCreation*>(context);
    if (creation.creating != nullptr && !driver.empty()) {
        // SDL presents its software renderer through the first driver of
        // the hint's list that starts, which the sentinel names.
        std::vector<std::string> via;
        if (driver == render_probe::software_renderer && !creation.native_framebuffer)
            via = split_driver_list(creation.hint);
        creation.creating(creation.creating_context, driver, via);
    }
    if (const auto* faults = creation.faults; faults != nullptr &&
                                              faults->refuse_driver != nullptr && !driver.empty() &&
                                              faults->refuse_driver(faults->context, driver)) {
        error = std::string(refused_by_fault);
        return false;
    }
    creation.renderer =
        SDL_CreateRenderer(creation.window, driver.empty() ? nullptr : driver.c_str());
    if (creation.renderer != nullptr)
        return true;
    error = SDL_GetError();
    return false;
}

/// Logs a line on standard output at once, so that a driver that ends the
/// process while the walk goes on leaves the refusals before it in the log.
///
/// @param line the line, without its line break
void log_graphics_line(void*, const std::string& line) {
    std::cout << line << '\n' << std::flush;
}

/// SDL's render drivers and SDL_RENDER_DRIVER's list, kept while a walk
/// views them.
struct DriverNames {
    std::vector<std::string> sdl_names;              ///< SDL's drivers, in its order
    std::vector<std::string> environment_names;      ///< SDL_RENDER_DRIVER's, in its order
    std::vector<std::string_view> sdl_order;         ///< views of sdl_names
    std::vector<std::string_view> environment_order; ///< views of environment_names
};

/// Reads SDL's render drivers and SDL_RENDER_DRIVER's comma list.
///
/// @param[out] names the names, which the inputs view
/// @return what a walk needs, SDL_RENDER_DRIVER's list where it is set
render_policy::CreationInputs driver_inputs(DriverNames& names) {
    const int driver_count = SDL_GetNumRenderDrivers();
    for (int index = 0; index < driver_count; ++index)
        if (const char* name = SDL_GetRenderDriver(index); name != nullptr)
            names.sdl_names.emplace_back(name);
    const char* named = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    const std::string_view list = named != nullptr ? std::string_view(named) : std::string_view();
    names.environment_names = split_driver_list(list);
    names.sdl_order.assign(names.sdl_names.begin(), names.sdl_names.end());
    names.environment_order.assign(names.environment_names.begin(), names.environment_names.end());
    const char* video_driver = SDL_GetCurrentVideoDriver();
    render_policy::CreationInputs inputs;
    inputs.sdl_order = names.sdl_order;
    inputs.environment_order = names.environment_order;
    // The drivers the records hold failed are the caller's to add.
    inputs.render_driver_named = !list.empty();
    inputs.native_window_framebuffer =
        render_probe::native_window_framebuffer(video_driver != nullptr ? video_driver : "");
    inputs.hint_takes_list = framebuffer_hint_takes_list();
    return inputs;
}

/// The hooks that make SDL's renderer for a walk.
///
/// @param creation what the hooks act on
/// @return the hooks
CreationHooks sdl_creation_hooks(SdlCreation& creation) {
    CreationHooks hooks;
    hooks.context = &creation;
    hooks.set_framebuffer_hint = set_sdl_framebuffer_hint;
    hooks.create = create_sdl_renderer;
    hooks.log = log_graphics_line;
    return hooks;
}

/// The reason a rebuild gives after a lost device take_event saw.
constexpr std::string_view device_lost_reason = "the graphics device was lost";

} // namespace

namespace {

namespace world_renderer = oa::present::world_renderer;

/// Bytes of an RGB24 pixel.
constexpr std::size_t rgb_bytes = 3;
/// Bytes of an ARGB8888 texel.
constexpr std::size_t argb_bytes = 4;
/// An ARGB8888 texel's alpha, opaque.
constexpr uint32_t opaque_alpha = 0xFF000000U;
/// The bits each channel of an ARGB8888 texel is shifted by.
constexpr uint32_t red_shift = 16;
constexpr uint32_t green_shift = 8;

/// (a) and (b): the side of the first target and of the texture reduced by
/// half, and the side of the half.
constexpr int small_side = 4;
constexpr int half_side = 2;
/// (a): the pixel read back, and the colour the target is cleared to.
constexpr int cleared_pixel_x = 1;
constexpr int cleared_pixel_y = 2;
constexpr std::array<uint8_t, 3> cleared_colour{0x30, 0x90, 0xC0};

/// (d): the known pattern's side; the gutter of texels round it, as a
/// tile's; the NEAREST prescale factor; and the side of the LINEAR
/// reduction at 0.75, 1.5 times the pattern's.
constexpr uint32_t pattern_side = 32;
constexpr uint32_t pattern_gutter = 1;
constexpr uint32_t prescale_factor = 2;
constexpr uint32_t reduced_side = 48;
/// The pattern's scale onto the reduction.
constexpr double pattern_scale = 1.5;
/// (d): the overlay's opaque square, from its first texel to the one past
/// its last, across and down.
constexpr uint32_t square_first = 12;
constexpr uint32_t square_end = 30;
/// The seeds of the pattern and of the square's colours.
constexpr uint32_t pattern_seed = 0x2545F491U;
constexpr uint32_t square_seed = 0x6C078965U;
/// The linear congruential sequence the patterns draw from:
/// x = x * 1664525 + 1013904223, its top byte taken.
constexpr uint32_t random_multiplier = 1664525U;
constexpr uint32_t random_increment = 1013904223U;
constexpr uint32_t random_byte_shift = 24;

/// Returns the next byte of a seeded sequence.
///
/// @param[in,out] state the sequence's state
/// @return the byte
uint8_t next_byte(uint32_t& state) noexcept {
    state = state * random_multiplier + random_increment;
    return static_cast<uint8_t>(state >> random_byte_shift);
}

/// Returns an opaque ARGB8888 texel of an RGB24 pixel.
///
/// @param rgb the pixel's three bytes
/// @return the texel
uint32_t opaque_texel(const uint8_t* rgb) noexcept {
    return opaque_alpha | (uint32_t{rgb[0]} << red_shift) | (uint32_t{rgb[1]} << green_shift) |
           uint32_t{rgb[2]};
}

/// The textures and targets the function test makes, destroyed with it, and
/// the renderer's draw colour and target put back.
class FunctionTestRun {
  public:

    explicit FunctionTestRun(SDL_Renderer* renderer) : renderer_(renderer) {
        saved_colour_ =
            SDL_GetRenderDrawColor(renderer_, &colour_[0], &colour_[1], &colour_[2], &colour_[3]);
    }

    FunctionTestRun(const FunctionTestRun&) = delete;
    FunctionTestRun& operator=(const FunctionTestRun&) = delete;

    /// Puts back the window as the target and the draw colour, and destroys
    /// what was made.
    ~FunctionTestRun() {
        std::ignore = SDL_SetRenderTarget(renderer_, nullptr);
        if (saved_colour_)
            std::ignore =
                SDL_SetRenderDrawColor(renderer_, colour_[0], colour_[1], colour_[2], colour_[3]);
        for (SDL_Texture* texture : made_)
            SDL_DestroyTexture(texture);
    }

    /// Makes an ARGB8888 texture.
    ///
    /// Throws std::runtime_error when SDL refuses it or its modes.
    ///
    /// @param width texels across
    /// @param height texels down
    /// @param access streaming or target
    /// @param blend its blend mode
    /// @param scale its scale mode
    /// @return the texture, destroyed with the run
    SDL_Texture* make(
        int width, int height, SDL_TextureAccess access, SDL_BlendMode blend, SDL_ScaleMode scale
    ) {
        SDL_Texture* texture =
            SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, access, width, height);
        if (texture == nullptr)
            fail("SDL_CreateTexture");
        made_.push_back(texture);
        if (!SDL_SetTextureBlendMode(texture, blend) || !SDL_SetTextureScaleMode(texture, scale))
            fail("a texture's blend or scale mode");
        return texture;
    }

    /// Uploads ARGB8888 texels to a texture.
    ///
    /// Throws std::runtime_error when SDL refuses.
    ///
    /// @param texture the texture
    /// @param texels its texels, row after row
    /// @param width texels a row
    void upload(SDL_Texture* texture, const std::vector<uint32_t>& texels, int width) {
        if (!SDL_UpdateTexture(
                texture, nullptr, texels.data(), width * static_cast<int>(argb_bytes)
            ))
            fail("SDL_UpdateTexture");
    }

    /// Sets a target and clears it to a colour.
    ///
    /// Throws std::runtime_error when SDL refuses.
    ///
    /// @param target the target
    /// @param colour the colour's red, green and blue
    void clear(SDL_Texture* target, const std::array<uint8_t, 3>& colour) {
        if (!SDL_SetRenderTarget(renderer_, target) ||
            !SDL_SetRenderDrawColor(renderer_, colour[0], colour[1], colour[2], SDL_ALPHA_OPAQUE) ||
            !SDL_RenderClear(renderer_))
            fail("clearing a render target");
    }

    /// Draws a texture, or a rectangle of it, into a rectangle of the target.
    ///
    /// Throws std::runtime_error when SDL refuses.
    ///
    /// @param texture the texture
    /// @param source the texels drawn; null for all
    /// @param side the destination's side, from the target's corner
    void draw(SDL_Texture* texture, const SDL_FRect* source, float side) {
        const SDL_FRect destination{0.0F, 0.0F, side, side};
        if (!SDL_RenderTexture(renderer_, texture, source, &destination))
            fail("SDL_RenderTexture");
    }

    /// Reads back a rectangle of the target as RGB24.
    ///
    /// Throws std::runtime_error when SDL refuses.
    ///
    /// @param area the rectangle
    /// @return its pixels, row after row
    std::vector<uint8_t> read(const SDL_Rect& area) {
        SDL_Surface* surface = SDL_RenderReadPixels(renderer_, &area);
        if (surface == nullptr)
            fail("SDL_RenderReadPixels");
        std::vector<uint8_t> rgb(static_cast<std::size_t>(area.w) * area.h * rgb_bytes);
        bool readable = true;
        for (int y = 0; y < area.h && readable; ++y)
            for (int x = 0; x < area.w && readable; ++x) {
                uint8_t* pixel =
                    rgb.data() + (static_cast<std::size_t>(y) * area.w + x) * rgb_bytes;
                uint8_t alpha = 0;
                readable =
                    SDL_ReadSurfacePixel(surface, x, y, &pixel[0], &pixel[1], &pixel[2], &alpha);
            }
        SDL_DestroySurface(surface);
        if (!readable)
            fail("SDL_ReadSurfacePixel");
        return rgb;
    }

  private:

    /// Throws what failed with SDL's reason.
    ///
    /// @param what the call that failed
    [[noreturn]] static void fail(std::string_view what) {
        throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
    }

    SDL_Renderer* renderer_{};
    std::vector<SDL_Texture*> made_{};
    std::array<uint8_t, 4> colour_{};
    bool saved_colour_{};
};

/// Returns the largest difference of two pictures' channels and adds their
/// differences to a sum.
///
/// @param picture one picture
/// @param reference the other, of the same size
/// @param[in,out] sum the sum of the differences
/// @return the largest difference
int largest_difference(
    const std::vector<uint8_t>& picture, const std::vector<uint8_t>& reference, uint64_t& sum
) noexcept {
    int largest = 0;
    for (std::size_t index = 0; index < picture.size() && index < reference.size(); ++index) {
        const int difference = std::abs(int{picture[index]} - int{reference[index]});
        largest = std::max(largest, difference);
        sum += static_cast<uint64_t>(difference);
    }
    return largest;
}

/// (a): a render target clears to a colour and reads it back.
///
/// @param run the test's run
/// @return what failed; empty when it passed
std::string test_target(FunctionTestRun& run) {
    SDL_Texture* target = run.make(
        small_side, small_side, SDL_TEXTUREACCESS_TARGET, SDL_BLENDMODE_NONE, SDL_SCALEMODE_NEAREST
    );
    run.clear(target, cleared_colour);
    const auto read = run.read({cleared_pixel_x, cleared_pixel_y, 1, 1});
    if (!std::equal(cleared_colour.begin(), cleared_colour.end(), read.begin()))
        return "a render target read back another colour than it was cleared to";
    return {};
}

/// (b): a texture drawn LINEAR at half its size reads back the average of
/// each block of texels it covers.
///
/// @param run the test's run
/// @param nearest draw it NEAREST instead, as a test forces
/// @return what failed; empty when it passed
std::string test_linear_half(FunctionTestRun& run, bool nearest) {
    constexpr int texels = small_side * small_side;
    std::vector<uint8_t> rgb(static_cast<std::size_t>(texels) * rgb_bytes);
    uint32_t state = pattern_seed;
    for (auto& level : rgb)
        level = next_byte(state);
    std::vector<uint32_t> words(texels);
    for (int index = 0; index < texels; ++index)
        words[static_cast<std::size_t>(index)] =
            opaque_texel(rgb.data() + static_cast<std::size_t>(index) * rgb_bytes);
    SDL_Texture* source = run.make(
        small_side,
        small_side,
        SDL_TEXTUREACCESS_STREAMING,
        SDL_BLENDMODE_NONE,
        nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR
    );
    run.upload(source, words, small_side);
    SDL_Texture* half = run.make(
        half_side, half_side, SDL_TEXTUREACCESS_TARGET, SDL_BLENDMODE_NONE, SDL_SCALEMODE_NEAREST
    );
    run.clear(half, {0, 0, 0});
    run.draw(source, nullptr, static_cast<float>(half_side));
    const auto read = run.read({0, 0, half_side, half_side});
    constexpr int block = small_side / half_side;
    for (int y = 0; y < half_side; ++y)
        for (int x = 0; x < half_side; ++x)
            for (std::size_t channel = 0; channel < rgb_bytes; ++channel) {
                int sum = 0;
                for (int dy = 0; dy < block; ++dy)
                    for (int dx = 0; dx < block; ++dx)
                        sum +=
                            rgb[(static_cast<std::size_t>(y * block + dy) * small_side +
                                 static_cast<std::size_t>(x * block + dx)) *
                                    rgb_bytes +
                                channel];
                const int average = (sum + block * block / 2) / (block * block);
                const int got =
                    read[(static_cast<std::size_t>(y) * half_side + x) * rgb_bytes + channel];
                if (std::abs(got - average) > function_test_half_most_difference)
                    return "a LINEAR reduction by half did not read back the texels' average";
            }
    return {};
}

/// (d): the known pattern drawn as the accelerated tier draws reads back as
/// the processor computes it.
///
/// @param run the test's run
/// @param nearest reduce the prescaled pattern NEAREST instead of LINEAR,
///     as a test forces
/// @return what failed; empty when it passed
std::string test_known_pattern(FunctionTestRun& run, bool nearest) {
    constexpr uint32_t pattern_pixels = pattern_side * pattern_side;
    std::vector<uint8_t> pattern(pattern_pixels * rgb_bytes);
    uint32_t state = pattern_seed;
    for (auto& level : pattern)
        level = next_byte(state);
    // The pattern with a gutter round it that repeats its edge, as a tile's.
    constexpr uint32_t framed_side = pattern_side + 2 * pattern_gutter;
    std::vector<uint32_t> framed(framed_side * framed_side);
    for (uint32_t y = 0; y < framed_side; ++y)
        for (uint32_t x = 0; x < framed_side; ++x) {
            const uint32_t source_x =
                std::min(pattern_side - 1, x > pattern_gutter ? x - pattern_gutter : 0U);
            const uint32_t source_y =
                std::min(pattern_side - 1, y > pattern_gutter ? y - pattern_gutter : 0U);
            framed[y * framed_side + x] =
                opaque_texel(pattern.data() + (source_y * pattern_side + source_x) * rgb_bytes);
        }
    // The overlay: transparent but for an opaque square of seeded colours.
    std::vector<uint32_t> overlay(reduced_side * reduced_side, 0);
    uint32_t square_state = square_seed;
    for (uint32_t y = square_first; y < square_end; ++y)
        for (uint32_t x = square_first; x < square_end; ++x) {
            std::array<uint8_t, 3> colour{};
            for (auto& level : colour)
                level = next_byte(square_state);
            overlay[y * reduced_side + x] = opaque_texel(colour.data());
        }
    SDL_Texture* source = run.make(
        static_cast<int>(framed_side),
        static_cast<int>(framed_side),
        SDL_TEXTUREACCESS_STREAMING,
        SDL_BLENDMODE_NONE,
        SDL_SCALEMODE_NEAREST
    );
    run.upload(source, framed, static_cast<int>(framed_side));
    constexpr uint32_t prescaled_side = pattern_side * prescale_factor;
    SDL_Texture* prescaled = run.make(
        static_cast<int>(prescaled_side),
        static_cast<int>(prescaled_side),
        SDL_TEXTUREACCESS_TARGET,
        SDL_BLENDMODE_NONE,
        nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR
    );
    run.clear(prescaled, {0, 0, 0});
    const SDL_FRect inside{
        static_cast<float>(pattern_gutter),
        static_cast<float>(pattern_gutter),
        static_cast<float>(pattern_side),
        static_cast<float>(pattern_side)
    };
    run.draw(source, &inside, static_cast<float>(prescaled_side));
    SDL_Texture* reduced = run.make(
        static_cast<int>(reduced_side),
        static_cast<int>(reduced_side),
        SDL_TEXTUREACCESS_TARGET,
        SDL_BLENDMODE_NONE,
        SDL_SCALEMODE_NEAREST
    );
    run.clear(reduced, {0, 0, 0});
    run.draw(prescaled, nullptr, static_cast<float>(reduced_side));
    SDL_Texture* layer = run.make(
        static_cast<int>(reduced_side),
        static_cast<int>(reduced_side),
        SDL_TEXTUREACCESS_STREAMING,
        SDL_BLENDMODE_BLEND,
        SDL_SCALEMODE_NEAREST
    );
    run.upload(layer, overlay, static_cast<int>(reduced_side));
    run.draw(layer, nullptr, static_cast<float>(reduced_side));
    const auto read =
        run.read({0, 0, static_cast<int>(reduced_side), static_cast<int>(reduced_side)});

    std::vector<uint8_t> reference(reduced_side * reduced_side * rgb_bytes);
    const world_renderer::RgbSource scene{pattern.data(), pattern_side, pattern_side, pattern_side};
    const world_renderer::RgbTarget picture{
        reference.data(), reduced_side, reduced_side, reduced_side
    };
    world_renderer::ScenePlacement placement{};
    placement.scale_x = pattern_scale;
    placement.scale_y = pattern_scale;
    world_renderer::sharp_bilinear_rgb24(scene, placement, prescale_factor, picture);
    world_renderer::overlay_rgb24(picture, overlay.data(), reduced_side);
    uint64_t sum = 0;
    const int largest = largest_difference(read, reference, sum);
    const double mean = static_cast<double>(sum) / static_cast<double>(reference.size());
    if (largest > function_test_pattern_most_difference ||
        mean > function_test_most_mean_difference)
        return "the known pattern did not read back as the processor computes it";
    return {};
}

/// Returns the machine's physical memory as the system reports it.
///
/// @return bytes; 0 when the system does not say
uint64_t physical_memory() noexcept {
    oa::platform::SystemMemorySample sample{};
    if (oa::platform::sample_system_memory(&sample) && sample.physical != 0)
        return sample.physical;
    return oa::platform::read_machine_traits().memory;
}

/// Fills what the starting rung is sized from that the machine itself
/// reports: its logical processors, its memory, whether it is a light
/// machine or a Raspberry Pi, and whether its processor is an ARM one that
/// has not been run on the accelerated tier.
///
/// @param[out] start the facts filled; the others are left as they are
/// @param memory the machine's physical memory in bytes (physical_memory)
void take_machine(render_policy::StartInputs& start, uint64_t memory) {
    const oa::platform::MachineTraits machine = oa::platform::read_machine_traits();
    start.processors = machine.processors;
    start.memory = memory;
    start.light_machine = oa::platform::light_machine(machine);
    start.raspberry_pi = oa::platform::running_on_raspberry_pi();
    start.other_arm = render_probe::untried_arm_processor();
}

/// The line logged when the start-up function test fails.
///
/// @param failure what failed
/// @return the line, without its line break
std::string function_test_log_line(std::string_view failure) {
    std::string line(graphics_log_prefix);
    line += "the start-up test failed: ";
    line += failure;
    return line;
}

/// Returns what the log adds after "the window opens at the display's own
/// pixel density" to say why.
///
/// @param reason the reason a window opens at native density
/// @return " (--native-density)", " (the Native pixel density setting)",
///     " (as every window of this platform)", or empty when every condition
///     of the rule holds
std::string_view native_density_reason(render_policy::DensityReason reason) noexcept {
    switch (reason) {
    case render_policy::DensityReason::asked:
        return " (--native-density)";
    case render_policy::DensityReason::chosen:
        return " (the Native pixel density setting)";
    case render_policy::DensityReason::platform:
        return " (as every window of this platform)";
    default:
        return {};
    }
}

} // namespace

render_policy::DensityDecision decide_window_density(const DensityRequest& request) {
    render_policy::DensityInputs inputs;
    inputs.platform_native = request.platform_native;
    inputs.asked = request.asked;
    inputs.chosen = request.chosen;
    inputs.memory = physical_memory();
    inputs.flag = render_policy::acceleration_flag(request.flag);
    inputs.setting = request.setting;
    const char* named = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    inputs.render_driver_named = named != nullptr && named[0] != '\0';
    const char* video_driver = SDL_GetCurrentVideoDriver();
    inputs.virtual_video_driver =
        render_policy::windowless_video_driver(video_driver != nullptr ? video_driver : "");
    inputs.unattended = request.unattended;
    inputs.capture = request.capture;
    inputs.class_measured = render_policy::native_density_measured;
    // The budget the machine starts at with the driver the record names,
    // the one an earlier run passed the function test on.
    render_policy::StartInputs machine;
    take_machine(machine, inputs.memory);
    machine.legacy_windows = oa::platform::running_on_windows_before_vista();
    machine.run_class = render_probe::accelerated_tier_run(request.record_driver)
                            ? render_policy::ClassTesting::tested
                            : render_policy::ClassTesting::untested;
    inputs.budget = render_policy::start_budget(machine);
    inputs.remembered = request.remembered;
    inputs.record = !request.record_driver.empty();
    const render_policy::DensityDecision decision = render_policy::decide_native_density(inputs);
    if (decision.native)
        std::cout << graphics_log_prefix << "the window opens at the display's own pixel density"
                  << native_density_reason(decision.reason) << '\n'
                  << std::flush;
    return decision;
}

FunctionTestResult run_function_test(SDL_Renderer* renderer, const FunctionTestFaults& faults) {
    FunctionTestResult result;
    if (renderer == nullptr) {
        result.failure = "there is no renderer";
        return result;
    }
    try {
        FunctionTestRun run(renderer);
        result.failure = test_target(run);
        if (result.failure.empty())
            result.failure = test_linear_half(run, faults.half_nearest);
        if (result.failure.empty())
            result.failure = test_known_pattern(run, faults.pattern_nearest);
    } catch (const std::runtime_error& error) {
        result.failure = error.what();
    }
    result.passed = result.failure.empty();
    // The pixel-art scale mode only chooses the filter, so it is asked even
    // where the rest failed, and its own textures go with it.
    result.pixelart = result.passed && probe_pixelart(renderer, nullptr);
    return result;
}

std::string failing_call_name(std::string_view error) {
    const std::string_view words = error.substr(0, error.find(':'));
    std::string name;
    bool separated = false;
    for (const char letter : words) {
        const bool kept = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
                          (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
        if (!kept) {
            separated = !name.empty();
            continue;
        }
        if (separated) {
            if (name.size() + 1 >= renderer_state::max_name_bytes)
                break;
            name += '-';
            separated = false;
        }
        if (name.size() >= renderer_state::max_name_bytes)
            break;
        name += letter;
    }
    while (!name.empty() && name.back() == '-')
        name.pop_back();
    return name.empty() ? std::string(unnamed_call) : name;
}

std::string trial_unwritten_log_line() {
    std::string line(graphics_log_prefix);
    line += "the start-up test is skipped: its trial cannot be written to ";
    line += renderer_state::records_file_name;
    line += ", so the processor draws everything";
    return line;
}

std::string path_trial_unwritten_log_line(renderer_state::AcceleratedPath path) {
    std::string line(graphics_log_prefix);
    line += "the graphics card's ";
    switch (path) {
    case renderer_state::AcceleratedPath::magnify:
        line += "magnification";
        break;
    case renderer_state::AcceleratedPath::prescale:
        line += "prescale target";
        break;
    case renderer_state::AcceleratedPath::blend:
        line += "two-level reduction";
        break;
    case renderer_state::AcceleratedPath::full:
        line += "drawing of the battlefield";
        break;
    }
    line += " is not used: its trial cannot be written to ";
    line += renderer_state::records_file_name;
    return line;
}

namespace {

/// Writes a line of the renderer records' log through the host.
///
/// @param context the host's log of the records' lines
/// @param text the line, without the game's prefix
void keep_records_line(void* context, std::string_view text) {
    auto& lines = *static_cast<std::vector<std::string>*>(context);
    std::string line(graphics_log_prefix);
    line += text;
    std::cout << line << '\n' << std::flush;
    lines.push_back(std::move(line));
}

/// Returns the steady clock's time.
///
/// @return nanoseconds since the clock's epoch
uint64_t steady_clock_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

} // namespace

RendererHost::~RendererHost() {
    destroy();
}

void RendererHost::create(
    SDL_Window* window, const RenderFaultHooks& faults, const std::optional<RecordsPlace>& records
) {
    destroy();
    window_ = window;
    faults_ = faults;
    adapter_asked_ = false;
    // A start decides its tier afresh.
    tier_ = {};
    machine_.legacy_windows = oa::platform::running_on_windows_before_vista();
    if (records)
        open_records(*records);
    make_renderer({}, false);
}

void RendererHost::make_renderer(const std::string& failed_driver, bool rebuilding) {
    DriverNames names;
    render_policy::CreationInputs inputs = driver_inputs(names);
    if (!rebuilding)
        named_ = inputs.render_driver_named;
    // The records are advice: the walk skips the drivers they hold failed
    // and walks again with them ignored when nothing else could present,
    // which holds for the rest of the run.
    const std::vector<std::string_view> failed =
        records_.storage() == renderer_state::Storage::disabled || (rebuilding && records_ignored_)
            ? std::vector<std::string_view>{}
            : renderer_state::failed_driver_list(records_.records());
    inputs.failed_drivers = failed;
    SdlCreation creation;
    creation.window = window_;
    creation.faults = &faults_;
    creation.native_framebuffer = inputs.native_window_framebuffer;
    creation.creating_context = this;
    creation.creating = [](void* context,
                           const std::string& driver,
                           const std::vector<std::string>& via) {
        auto& host = *static_cast<RendererHost*>(context);
        host.via_ = via;
        std::ignore = host.step_sentinel(
            renderer_state::LifeEvent::creating, renderer_state::AcceleratedPath::magnify, driver
        );
    };
    CreationOutcome outcome =
        rebuilding ? walk_rebuild_drivers(inputs, failed_driver, sdl_creation_hooks(creation))
                   : walk_render_drivers(inputs, sdl_creation_hooks(creation));
    attempts_ = std::move(outcome.attempts);
    // What the start's walk skipped, or ignored, holds for the run.
    if (!rebuilding) {
        records_ignored_ = outcome.records_ignored;
        skipped_drivers_ = std::move(outcome.skipped);
    }
    if (!outcome.created)
        throw std::runtime_error(outcome.error);
    renderer_ = creation.renderer;
    driver_made_ = attempts_.back().driver;
    // Probe items 1 to 3, the intro and the first standard frames stand
    // under `standard`; a crash there strikes the driver like one in its
    // creation.
    function_test_ran_ = false;
    std::ignore = step_sentinel(renderer_state::LifeEvent::probing);
    begin_stage();
    take_renderer();
    // Each driver describes the adapter in words of its own, so it is noted
    // only where the start made the driver every start makes with no record
    // in its way. A driver a record's skip or a rebuild lands on would read
    // as another adapter and clear the records that sent the walk there.
    const bool steered = rebuilding || outcome.skipped_by_record;
    if (!steered && facts_.adapter_state == render_probe::AdapterState::read &&
        records_.storage() != renderer_state::Storage::disabled &&
        renderer_state::note_adapter(records_.records(), facts_.adapter).changed)
        std::ignore = records_.write_records();
    sync_record_facts();
}

void RendererHost::rebuild(std::string_view reason, const renderer_state::Strike& failure) {
    const std::string failed = facts_.renderer;
    std::cout << rebuild_log_line(failed, reason) << '\n' << std::flush;
    // The failure is a strike against the driver, which becomes a record
    // when the same fails in the next run on it.
    if (failure.stage != renderer_state::StrikeStage::none)
        std::ignore = note_running_failure(failure);
    // The accelerated tier stops with the renderer, so the stages it
    // covered are over; the new renderer's own stand as a start's do.
    end_path_stage();
    std::ignore = records_.erase_trial();
    life_.trial = false;
    destroy();
    lost_noted_ = false;
    make_renderer(failed, true);
    // The driver failed, so the processor draws everything for the rest of
    // the run unless it was dropped for another reason already; the new
    // renderer's function test is to run again.
    tier_.function_test = render_policy::FunctionTest::not_run;
    if (tier_.drop == render_policy::Drop::none)
        tier_.drop = render_policy::Drop::driver_failure;
    std::string tier(standard_tier_description);
    tier += " (";
    tier += failed_driver_reason;
    tier += ')';
    std::cout << graphics_log_line(facts_, tier) << '\n' << std::flush;
}

void RendererHost::take_renderer() {
    facts_ = describe_game_renderer(renderer_, adapter_asked_);
    if (!faults_.adapter.empty()) {
        facts_.adapter = faults_.adapter;
        facts_.adapter_state = render_probe::AdapterState::read;
    }
    oa::base::float_precision::restore_program_float_control();
    tier_.renderer = renderer_ != nullptr;
    // The renderer is assessed at the host's texture limit, which a fault
    // hook may lower. No command line option accepts a virtual adapter.
    render_policy::RendererFacts renderer = renderer_facts(facts_);
    renderer.max_texture_size = texture_limit();
    tier_.capability = render_policy::assess_renderer(renderer, machine_.legacy_windows, false);
    machine_.driver = driver_traits(facts_.renderer);
    machine_.run_class = render_probe::accelerated_tier_run(facts_.renderer)
                             ? render_policy::ClassTesting::tested
                             : render_policy::ClassTesting::untested;
    layer_formats_ = render_policy::layer_formats(
        facts_.renderer == render_probe::software_renderer, rgb565_window(), named_
    );
}

bool RendererHost::rgb565_window() const {
    if (faults_.rgb565_window != nullptr)
        return faults_.rgb565_window(faults_.context);
    return SDL_GetWindowPixelFormat(window_) == SDL_PIXELFORMAT_RGB565;
}

bool RendererHost::take_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_RENDER_DEVICE_LOST:
        if (renderer_ != nullptr && event.render.windowID == SDL_GetWindowID(window_))
            lost_noted_ = true;
        return true;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_RESET:
        return true;
    default:
        return false;
    }
}

void RendererHost::service() {
    if (lost_noted_ && renderer_ != nullptr) {
        renderer_state::Strike lost;
        lost.stage = renderer_state::StrikeStage::lost;
        rebuild(device_lost_reason, lost);
    }
}

void RendererHost::destroy() noexcept {
    if (renderer_ != nullptr)
        SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
    facts_ = {};
    tier_.renderer = false;
}

SDL_Renderer* RendererHost::renderer() const noexcept {
    return renderer_;
}

const render_probe::AdapterFacts& RendererHost::facts() const noexcept {
    return facts_;
}

std::span<const CreationAttempt> RendererHost::attempts() const noexcept {
    return attempts_;
}

bool RendererHost::named() const noexcept {
    return named_;
}

const render_policy::LayerFormats& RendererHost::layer_formats() const noexcept {
    return layer_formats_;
}

render_policy::LayerFormat RendererHost::opaque_format() const {
    if (facts_.renderer != render_probe::software_renderer)
        return layer_formats_.opaque;
    return render_policy::layer_formats(true, rgb565_window(), named_).opaque;
}

uint32_t RendererHost::texture_limit() const noexcept {
    return faults_.texture_limit > 0 ? faults_.texture_limit : corrected_texture_limit(facts_);
}

render_probe::DeviceState RendererHost::device_state() const {
    if (faults_.device_state != nullptr)
        return faults_.device_state(faults_.context);
    return render_probe::device_state(renderer_);
}

RenderFaultHooks& RendererHost::faults() noexcept {
    return faults_;
}

void RendererHost::decide_start_tier(const TierRequest& request) {
    // SDL_RENDER_DRIVER on its own reads no adapter, but a flag that asks
    // for more than SDL's own start has it read, as every other start does,
    // so that an adapter that cannot be read or a software rasteriser keeps
    // the standard tier there too.
    if (named_ && renderer_ != nullptr && !adapter_asked_ &&
        (render_policy::flag_asks_for_card(render_policy::acceleration_flag(request.flag)) ||
         request.force_capable)) {
        adapter_asked_ = true;
        take_renderer();
    }
    tier_.renderer = renderer_ != nullptr;
    tier_.flag = render_policy::acceleration_flag(request.flag);
    tier_.force_capable = request.force_capable;
    tier_.render_driver_named = named_;
    const char* video_driver = SDL_GetCurrentVideoDriver();
    tier_.virtual_video_driver =
        render_policy::windowless_video_driver(video_driver != nullptr ? video_driver : "");
    tier_.players_own_profile = request.players_own_profile;
    tier_.setting = request.setting;
    tier_.memory = machine_memory();
    take_machine(machine_, tier_.memory);
    // The trial written before the test guards it, so the test runs at
    // start wherever the tier could be accelerated but for it.
    tier_.function_test = render_policy::FunctionTest::not_run;
    tier_.records_unreadable_after_unclean_start =
        records_.records_unreadable_after_unclean_start();
    sync_record_facts();
    const render_policy::TierDecision decision =
        render_policy::step_tier(tier_, false, function_test_hooks()).decision;
    AccelerationFacts facts = tier_acceleration_facts(tier_, start_rung(), decision.tier);
    fill_record_facts(facts);
    const AccelerationReport report = report_acceleration(facts);
    std::cout
        << graphics_log_line(
               facts_,
               tier_description(report.status, decision.tier == render_policy::RenderTier::full)
           )
        << '\n'
        << std::flush;
}

void RendererHost::test_function() {
    // The trial stands from before the test until the start-up stage
    // passes, so a crash in the test or the first accelerated frames, even
    // one that takes the whole system down, is struck at the next start.
    if (!step_sentinel(renderer_state::LifeEvent::function_test)) {
        tier_.function_test = render_policy::FunctionTest::trial_unwritten;
        std::cout << trial_unwritten_log_line() << '\n' << std::flush;
        return;
    }
    const FunctionTestResult result = run_function_test(renderer_, faults_.function_test);
    oa::base::float_precision::restore_program_float_control();
    tier_.function_test =
        result.passed ? render_policy::FunctionTest::passed : render_policy::FunctionTest::failed;
    machine_.pixelart = result.pixelart;
    function_test_ran_ = true;
    std::ignore = step_sentinel(renderer_state::LifeEvent::function_test_done);
    begin_stage();
    if (!result.passed)
        std::cout << function_test_log_line(result.failure) << '\n' << std::flush;
}

render_policy::FunctionTestHooks RendererHost::function_test_hooks() noexcept {
    render_policy::FunctionTestHooks hooks;
    hooks.context = this;
    hooks.run = [](void* context) {
        auto& host = *static_cast<RendererHost*>(context);
        host.test_function();
        return host.tier_.function_test;
    };
    return hooks;
}

render_policy::TierInputs& RendererHost::tier_inputs() noexcept {
    return tier_;
}

const render_policy::TierInputs& RendererHost::tier_inputs() const noexcept {
    return tier_;
}

render_policy::LadderState RendererHost::start_rung() const noexcept {
    return render_policy::start_rung(machine_);
}

void RendererHost::open_records(const RecordsPlace& place) {
    place_ = place;
    records_log_.clear();
    // SDL_RENDER_DRIVER gives SDL's own start, which reads and writes no
    // records and keeps no sentinel.
    const char* named = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    const bool render_driver_named = named != nullptr && *named != '\0';
    renderer_state::RecordRules rules;
    rules.evidence = faults_.crash_evidence.value_or(
        renderer_state::crash_evidence(
            oa::platform::running_on_windows_before_vista(), oa::platform::running_on_linux()
        )
    );
    rules.below_two_gib = machine_memory() < render_policy::smallest_accelerated_memory;
    const renderer_state::LogHooks log{&records_log_, keep_records_line};
    if (render_driver_named)
        records_ = renderer_state::RendererState::disabled();
    else if (place.folder.empty())
        records_ = renderer_state::RendererState::in_memory(place.engine_build, rules, log);
    else
        records_ = renderer_state::RendererState::open_folder(
            place.folder, place.engine_build, rules, log
        );
    leftovers_ = records_.resolve_leftovers();
    // A strike or record made of what the last run left reaches the file
    // now, so that the same again at the next start counts as a second.
    std::ignore = records_.write_records();
    life_ = renderer_state::start_sentinel_life(render_driver_named);
    via_.clear();
    paths_begun_ = 0;
    path_frames_ = 0;
    stage_frames_ = 0;
    function_test_ran_ = false;
}

renderer_state::RendererState& RendererHost::records() noexcept {
    return records_;
}

const renderer_state::RendererState& RendererHost::records() const noexcept {
    return records_;
}

const RecordsPlace& RendererHost::records_place() const noexcept {
    return place_;
}

const renderer_state::LeftoverOutcome& RendererHost::leftovers() const noexcept {
    return leftovers_;
}

std::span<const std::string> RendererHost::records_log() const noexcept {
    return records_log_;
}

std::string RendererHost::record_driver() const {
    if (renderer_ == nullptr)
        return {};
    if (!faults_.record_driver.empty())
        return faults_.record_driver;
    if (!facts_.renderer.empty())
        return facts_.renderer;
    return driver_made_;
}

std::span<const std::string> RendererHost::skipped_drivers() const noexcept {
    return skipped_drivers_;
}

bool RendererHost::records_ignored() const noexcept {
    return records_ignored_;
}

void RendererHost::set_stage_clock(const StageClock& clock) noexcept {
    clock_ = clock;
}

void RendererHost::note_first_accelerated_frame() {
    std::ignore = step_sentinel(renderer_state::LifeEvent::first_accelerated_frame);
}

bool RendererHost::start_stage_open() const noexcept {
    using renderer_state::SentinelStage;
    return life_.stage &&
           (*life_.stage == SentinelStage::standard || *life_.stage == SentinelStage::accelerated);
}

void RendererHost::note_presented_frame(PathSet drawn) {
    using renderer_state::SentinelStage;
    if (!life_.stage)
        return;
    switch (*life_.stage) {
    case SentinelStage::standard:
    case SentinelStage::accelerated: {
        ++stage_frames_;
        if (!renderer_state::start_stage_passed(stage_frames_, stage_now_ns() - stage_started_ns_))
            return;
        // The sentinel the start passed, with the framebuffer hint's list
        // a strike of SDL's software renderer is made against.
        renderer_state::Sentinel passed;
        passed.stage = *life_.stage;
        passed.driver = record_driver();
        passed.via = via_;
        std::ignore = step_sentinel(renderer_state::LifeEvent::start_passed);
        if (renderer_state::note_start_passed(
                records_.records(), passed, function_test_ran_, records_.rules()
            )
                .changed)
            std::ignore = records_.write_records();
        sync_record_facts();
        return;
    }
    case SentinelStage::path: {
        if ((drawn & path_bit(life_.path)) == 0 ||
            ++path_frames_ < renderer_state::path_stage_frames)
            return;
        const renderer_state::AcceleratedPath path = life_.path;
        std::ignore = step_sentinel(renderer_state::LifeEvent::path_passed, path);
        if (renderer_state::note_path_passed(records_.records(), record_driver(), path).changed)
            std::ignore = records_.write_records();
        return;
    }
    case SentinelStage::create:
    case SentinelStage::probe:
    case SentinelStage::running:
        return;
    }
}

bool RendererHost::begin_path(renderer_state::AcceleratedPath path) {
    if ((paths_begun_ & path_bit(path)) != 0)
        return true;
    if (!step_sentinel(renderer_state::LifeEvent::path_first_use, path)) {
        std::cout << path_trial_unwritten_log_line(path) << '\n' << std::flush;
        return false;
    }
    paths_begun_ = static_cast<PathSet>(paths_begun_ | path_bit(path));
    path_frames_ = 0;
    return true;
}

void RendererHost::end_path_stage() {
    if (life_.stage == renderer_state::SentinelStage::path)
        std::ignore = step_sentinel(renderer_state::LifeEvent::path_passed, life_.path);
}

renderer_state::Change RendererHost::note_running_failure(const renderer_state::Strike& failure) {
    const std::string driver = record_driver();
    if (records_.storage() == renderer_state::Storage::disabled || driver.empty())
        return {};
    // SDL's software renderer is never recorded failed-driver, and draws
    // the accelerated tier only under --force-capable.
    if (driver == render_probe::software_renderer && !tier_.force_capable)
        return {};
    renderer_state::DriverFacts facts;
    facts.loses_device_in_ordinary_use = driver_traits(driver).loses_device_in_normal_use;
    const renderer_state::Change change = renderer_state::note_running_failure(
        records_.records(), driver, failure, facts, records_.rules()
    );
    if (change.changed)
        std::ignore = records_.write_records();
    sync_record_facts();
    return change;
}

renderer_state::Change RendererHost::clear_records() {
    const renderer_state::Change change = records_.clear();
    tier_.records_unreadable_after_unclean_start = false;
    sync_record_facts();
    return change;
}

void RendererHost::keep_cleared_records() noexcept {
    try {
        std::ignore = records_.confirm_clear();
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the renderer records were not written: " << error.what()
                  << '\n';
    }
}

void RendererHost::restore_records() noexcept {
    records_.restore_failures();
    sync_record_facts();
    try {
        std::ignore = records_.write_records();
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the renderer records were not written: " << error.what()
                  << '\n';
    }
}

void RendererHost::set_match_running(bool running) {
    records_.set_match_running(running);
    if (!running)
        std::ignore = records_.write_records();
}

void RendererHost::fill_record_facts(AccelerationFacts& facts) const {
    if (records_.storage() == renderer_state::Storage::disabled)
        return;
    const auto trouble_of = [](renderer_state::RecordedFailure failure) {
        return failure == renderer_state::RecordedFailure::stopped ? RecordedTrouble::stopped
                                                                   : RecordedTrouble::failure;
    };
    const renderer_state::Records& records = records_.records();
    facts.driver_skipped = !skipped_drivers_.empty();
    bool skipped_standing = false;
    for (const std::string& driver : skipped_drivers_) {
        const renderer_state::DriverRecords* entry = renderer_state::find_driver(records, driver);
        if (entry == nullptr ||
            entry->failed_driver.failure == renderer_state::RecordedFailure::none)
            continue;
        // A crash or hang says more than a failure the driver reported.
        const RecordedTrouble trouble = trouble_of(entry->failed_driver.failure);
        if (!skipped_standing || trouble == RecordedTrouble::stopped)
            facts.skipped_recorded = trouble;
        skipped_standing = true;
    }
    facts.skipped_cleared = facts.driver_skipped && !skipped_standing;
    if (const renderer_state::DriverRecords* entry =
            renderer_state::find_driver(records, record_driver());
        entry != nullptr &&
        entry->accelerated_unusable.failure != renderer_state::RecordedFailure::none)
        facts.recorded = trouble_of(entry->accelerated_unusable.failure);
}

void RendererHost::finish_records() noexcept {
    try {
        // A failure struck in an earlier run that this one did not see
        // again on its driver is cleared.
        const std::string driver = record_driver();
        if (!driver.empty() && records_.storage() != renderer_state::Storage::disabled)
            std::ignore =
                renderer_state::note_clean_run(records_.records(), driver, records_.rules());
        records_.set_match_running(false);
        records_.clean_exit();
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the renderer records were not ended cleanly: "
                  << error.what() << '\n';
    }
}

bool RendererHost::step_sentinel(
    renderer_state::LifeEvent event, renderer_state::AcceleratedPath path, const std::string& driver
) {
    const renderer_state::SentinelWrite write = renderer_state::sentinel_step(life_, event, path);
    const std::string named = driver.empty() ? record_driver() : driver;
    if (write.write_trial) {
        renderer_state::Trial trial;
        trial.stage = write.trial_stage;
        trial.path = write.trial_path;
        trial.driver = named;
        if (!records_.write_trial(trial)) {
            renderer_state::note_trial_unwritten(life_);
            return false;
        }
    }
    if (write.set_sentinel && renderer_state::valid_driver_name(named)) {
        renderer_state::Sentinel sentinel;
        sentinel.stage = write.sentinel;
        sentinel.path = write.sentinel_path;
        sentinel.driver = named;
        // Only SDL's software renderer presents through the hint's list.
        if (named == render_probe::software_renderer)
            for (const std::string& through : via_)
                if (renderer_state::valid_driver_name(through) &&
                    sentinel.via.size() < renderer_state::max_via_drivers)
                    sentinel.via.push_back(through);
        records_.set_sentinel(sentinel);
    }
    if (write.erase_trial)
        std::ignore = records_.erase_trial();
    return true;
}

void RendererHost::begin_stage() noexcept {
    stage_frames_ = 0;
    try {
        stage_started_ns_ = stage_now_ns();
    } catch (...) {
        stage_started_ns_ = 0;
    }
}

uint64_t RendererHost::stage_now_ns() const {
    return clock_.now_ns != nullptr ? clock_.now_ns(clock_.context) : steady_clock_ns();
}

uint64_t RendererHost::machine_memory() const noexcept {
    return faults_.physical_memory.value_or(physical_memory());
}

void RendererHost::sync_record_facts() noexcept {
    try {
        const std::string driver = record_driver();
        const bool recorded =
            records_.storage() != renderer_state::Storage::disabled && !driver.empty();
        tier_.accelerated_unusable_record =
            recorded && !renderer_state::acceleration_allowed(records_.records(), driver, false);
        tier_.full_unusable_record =
            recorded && !renderer_state::full_allowed(records_.records(), driver, false);
    } catch (const std::exception&) {
        // The name could not be copied; the fact stays as it was.
    }
}

} // namespace oa::app
