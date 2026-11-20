// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Loaded 3DO drawing data: texture archives, per-model primitive records in
// their load-time draw order with bound textures, and the display lookup
// tables the model renderer reads (alpha, shade, blue).

#include "oa/formats/gaf.hpp"
#include "oa/present/display.hpp"
#include "oa/present/surface.h"
#include "oa/sim/sprite_animation.hpp"
#include "oa/formats/objects3d.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace oa {
class AssetStore;
}

namespace oa::present::model {

// Bits of PreparedPrimitive::flags, the loaded primitive's flag word.
inline constexpr uint32_t primitive_colored = 0x1;  // fill with the colour index
inline constexpr uint32_t primitive_animated = 0x2; // texture is a frame sequence
inline constexpr uint32_t primitive_team = 0x4;     // frame chosen by the owner's team colour
// Colour given to a primitive whose texture is not found.
inline constexpr uint8_t missing_texture_color = 0xd1;

// One texture sequence of a loaded archive. Frames the samplers cannot read
// (compressed or layered) keep their slot with null data.
struct TextureSequence {
    const formats::gaf::Sequence* sequence{};
    std::vector<Sprite> frames;
    bool team_archive{}; // found only in logos.gaf with ten frames
};

// Texture archives in search order. Frame pixels live in one arena padded so
// every frame start has the sampler window readable after it.
struct TextureLibrary {
    std::vector<std::unique_ptr<formats::gaf::Archive>> archives;
    std::vector<uint8_t> pixels;
    std::unordered_map<std::string, TextureSequence> sequences; // folded name, first match
    /// The arena's team frames hold a colour-vision mode's ramps. Off, before any recolour, it is
    /// clear and the arena is the indices load_texture_library copied.
    bool team_colours_applied{};
};

// A loaded primitive record with its texture binding.
struct PreparedPrimitive {
    uint32_t source_index{}; // index into Object::primitives
    uint32_t flags{};        // primitive_* bits
    uint8_t color{};
    const Sprite* frame{};                  // fixed texture frame
    const TextureSequence* texture{};       // primitive_animated / primitive_team
    sim::sprite_animation::Cursor cursor{}; // animated, not team
};

struct PreparedObject {
    std::vector<PreparedPrimitive> primitives; // load-time draw order
    bool skips_first{};                        // the first record is the selection primitive
};

struct PreparedModel {
    const formats::objects3d::Model* model{}; // address prepared for; the library's key
    // Weak reference to the handle it was prepared from. It does not keep the
    // model alive; it keeps the handle's control block allocated, so a later
    // model's handle can never pass for this one.
    std::weak_ptr<const formats::objects3d::Model> owner;
    std::vector<PreparedObject> objects; // indexed like Model::objects
};

// Prepared models keyed by model address, with the texture animations they step.
//
// Lifetime contract:
// - The library owns its texture archives, every PreparedModel and the
//   animation cursors inside them. `cursors` points into those entries.
// - It never owns a model. Each entry holds a weak reference to the handle it
//   was prepared from, so a model dies with its last owning handle. Handles
//   must own their model; an aliasing handle into storage that can move or be
//   reused is not supported.
// - prepare_model returns a model's entry to any handle that reaches it
//   while the model it was prepared for is alive. When that model has been
//   freed and another allocated at its address, the old entry and its cursors
//   are erased and the new model is prepared afresh; find_prepared_model
//   reports no entry for it until then.
// - release_expired_models erases every entry whose model has been freed,
//   with its cursors, so dead models stop stepping.
// - A caller keeps a model alive for as long as it uses the model's entry;
//   the entry stays valid while its model lives and the library exists. Its
//   frames and sequences point into `textures`, which must not be replaced
//   while entries exist.
struct ModelLibrary {
    TextureLibrary textures;
    std::unordered_map<const formats::objects3d::Model*, std::unique_ptr<PreparedModel>> models{};
    std::vector<sim::sprite_animation::Cursor*> cursors; // registration order
};

/// Loads every textures/*.gaf in mount order, logos.gaf last, into a texture library.
///
/// Each sequence name (case-folded) is bound to the first archive that has
/// it. Readable frames are copied into one pixel arena padded by the 64 KiB
/// sampler window; unreadable ones keep a slot with null data. Throws
/// std::runtime_error when an archive cannot be parsed.
///
/// @param assets asset store to list and read
/// @return the library
[[nodiscard]] TextureLibrary load_texture_library(AssetStore& assets);

/// Recolours the team frames in the texture arena, or writes their originals back.
///
/// The archives keep the indices they were loaded with. With colour vision off, before any
/// recolour, the arena is left as `load_texture_library` wrote it. A palette shorter than 256
/// entries restores the originals and does not remap. A mode sends each shade of a player colour's
/// old ramp to the new ramp, so the units and the 32×32 logos draw in that colour.
///
/// @param[in,out] library texture library whose team frames are rewritten
/// @param palette palette in use, 4 bytes an entry
void recolour_team_textures(TextureLibrary& library, std::span<const uint8_t> palette);

/// Computes the load-time draw order of an object's primitives.
///
/// The selection primitive is swapped to the front, then the rest are
/// bubble-sorted by mean vertex Y.
///
/// @param object 3DO object
/// @param[out] order primitive indices in draw order
void sort_object_primitives(const formats::objects3d::Object& object, std::vector<uint32_t>& order);

/// Binds every primitive of a model to its texture and registers animated textures for stepping.
///
/// A primitive whose texture is not found becomes a coloured primitive of
/// missing_texture_color. Textures of two or more frames animate; team
/// textures (ten frames in logos.gaf) pick their frame by team colour and are
/// not stepped. A model is prepared once and then returned from the library
/// to every handle that reaches it while it lives. An entry left at the
/// model's address by a freed model is erased with its cursors first.
///
/// Throws std::invalid_argument for a null handle.
///
/// @param[in,out] library library that owns the prepared model
/// @param handle handle that owns the model; the library keeps only a weak reference
/// @return the prepared model
const PreparedModel& prepare_model(
    ModelLibrary& library, const std::shared_ptr<const formats::objects3d::Model>& handle
);

/// Returns the entry prepared for a live model the caller knows only by address.
///
/// @param library library to search
/// @param model a live model
/// @return the prepared model, or null when the model has not been prepared
///     or the entry at its address belongs to a freed model
[[nodiscard]] const PreparedModel*
find_prepared_model(const ModelLibrary& library, const formats::objects3d::Model& model) noexcept;

/// Erases every prepared model whose model has been freed, with its animation cursors.
///
/// The remaining cursors keep their registration order.
///
/// @param[in,out] library library whose entries and cursors are pruned
/// @return the number of entries erased
std::size_t release_expired_models(ModelLibrary& library);

/// Returns the texture frame a primitive shows.
///
/// @param primitive prepared primitive
/// @param first_frame true to take frame 0 of an animated texture instead of its running frame
/// @param team_color frame index of a team texture
/// @return the frame, or null for an untextured primitive, a frame out of
///     range or a frame the samplers cannot read
[[nodiscard]] const Sprite* primitive_texture(
    const PreparedPrimitive& primitive, bool first_frame, uint8_t team_color
) noexcept;

/// Advances every registered texture animation by one tick, last registered first.
///
/// @param[in,out] library library whose animation cursors are stepped
void step_texture_animations(ModelLibrary& library) noexcept;

// Display tables for the model renderer, built from the game palette.
struct ModelDisplay {
    present::DisplayContext context{};
    std::vector<uint8_t> alpha;
    std::vector<uint8_t> shade;
    std::vector<uint8_t> blue;
    Palette palette{};
};

/// Builds a model display's alpha, shade and blue tables from a palette.
///
/// @param[out] display display whose context, tables and palette are replaced
/// @param palette game palette
void build_model_display(ModelDisplay& display, const Palette& palette);

// Binds a ModelDisplay for the duration of a draw when the bound display
// lacks the tables; restores the previous binding on destruction.
struct DisplayScope {
    present::DisplayContext* previous{};
    bool bound{};
};

/// Binds a model display for a draw when the bound display lacks the alpha, shade or blue table.
///
/// @param display model display to bind; null binds nothing
/// @return the scope to hand to leave_display
[[nodiscard]] DisplayScope enter_display(ModelDisplay* display) noexcept;

/// Restores the display binding enter_display replaced.
///
/// @param scope scope from enter_display; nothing happens when it bound nothing
void leave_display(const DisplayScope& scope) noexcept;

} // namespace oa::present::model
