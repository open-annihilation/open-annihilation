// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "renderer_proof_gpu.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace oa::app::renderer_proof_scene {

/// Fixed viewport dimensions for the installed-asset comparison.
inline constexpr uint32_t scene_width = 640;
inline constexpr uint32_t scene_height = 480;

/// Owned geometry, atlas and independently rasterised reference images.
struct Scene {
    uint32_t width{scene_width};
    uint32_t height{scene_height};
    std::vector<renderer_proof_gpu::Vertex> vertices{};
    std::size_t terrain_vertex_count{};
    std::vector<uint8_t> texture_rgba{};
    uint32_t texture_width{};
    uint32_t texture_height{};
    std::vector<uint8_t> classic_rgba{};
    std::vector<uint8_t> classic_terrain_rgba{};
    std::string description{};
};

/// Builds a fixed terrain, unit and wreck scene from an installed game.
///
/// Terrain geometry lifts painted coordinates by half the sampled height;
/// this is a reconstruction hypothesis, not a claim of recovered map geometry.
/// The classic reference uses the existing terrain and model rasterisers.
///
/// @param game_dir directory containing the player's installed game archives
/// @param[out] scene the completed scene; unchanged on failure
/// @param[out] error the failure reason, or empty on success
/// @return true when the scene and both reference images were built
[[nodiscard]] bool build(const std::filesystem::path& game_dir, Scene& scene, std::string& error);

} // namespace oa::app::renderer_proof_scene
