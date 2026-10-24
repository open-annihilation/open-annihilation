// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oa::app::renderer_proof_gpu {

/// One world-space vertex and its normalized atlas coordinates.
struct Vertex {
    float x{};
    float height{};
    float z{};
    float u{};
    float v{};
};

/// A missing supported GPU is distinct from a failed rendering operation.
enum class Result { success, unavailable, failed };

/// Render a triangle list without a window using classic projection and depth testing.
/// Input and output pixels are tightly packed RGBA8; untouched pixels are opaque black.
/// The caller initializes SDL. Currently only the Metal backend has shaders.
/// Failure clears output_rgba and supplies an error; no software fallback is used.
///
/// @param vertices nonempty triangle list, world coordinates within +/-32768, UVs in [0,1]
/// @param texture_rgba packed source pixels, exactly texture_width * texture_height * 4 bytes
/// @param texture_width source width, from 1 through 4096
/// @param texture_height source height, from 1 through 4096
/// @param output_width target width, from 1 through 4096
/// @param output_height target height, from 1 through 4096
/// @param camera_x horizontal camera offset within +/-32768
/// @param camera_y vertical camera offset within +/-32768
/// @param[out] output_rgba target pixels after successful readback
/// @param[out] error reason for unavailable or failed rendering
/// @return success, unavailable for a missing Metal device, or failed
Result render(
    std::span<const Vertex> vertices,
    std::span<const uint8_t> texture_rgba,
    uint32_t texture_width,
    uint32_t texture_height,
    uint32_t output_width,
    uint32_t output_height,
    float camera_x,
    float camera_y,
    std::vector<uint8_t>& output_rgba,
    std::string& error
);

} // namespace oa::app::renderer_proof_gpu
