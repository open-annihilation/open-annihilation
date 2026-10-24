// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "renderer_proof_gpu.hpp"

namespace oa::app::renderer_proof {

/// Prepared proof geometry and material; these are draw inputs, not game records.
struct Frame {
    std::span<const renderer_proof_gpu::Vertex> vertices{};
    std::span<const uint8_t> texture{};
    uint32_t texture_width{};
    uint32_t texture_height{};
    uint32_t width{};
    uint32_t height{};
};

/// The narrow proof seam; the production world renderer contract remains provisional.
struct RendererHooks {
    void* context{};
    const char* name{}; ///< stable built-in renderer name
    bool accelerated{};
    /// Renders a frame into RGBA bytes; null means unavailable.
    ///
    /// @param context implementation-owned state; null for stateless implementations
    /// @param frame prepared geometry and texture
    /// @param[out] pixels rendered RGBA bytes
    /// @param[out] error reason for unavailable or failed rendering
    /// @return success, unavailable or failed
    renderer_proof_gpu::Result (*draw)(
        void* context, const Frame& frame, std::vector<uint8_t>& pixels, std::string& error
    ){};
};

/// Returns the built-in CPU depth reference and experimental Metal renderer.
///
/// @return tables with process-lifetime names and stateless callbacks
[[nodiscard]] std::span<const RendererHooks> renderers() noexcept;

} // namespace oa::app::renderer_proof
