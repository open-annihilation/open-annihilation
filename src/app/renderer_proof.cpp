// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_proof.hpp"
#include "renderer_proof_backend.hpp"
#include "renderer_proof_scene.hpp"
#include "oa/formats/png.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace oa::app::renderer_proof {
namespace {
constexpr size_t channels = 4;
constexpr uint8_t opaque = 255;
constexpr uint32_t difference_gain = 4;
constexpr int unavailable_exit = 77;

/// Releases SDL after the offscreen check, including exceptional exits.
struct Video {
    bool ready{};

    /// Opens video services without creating a window; failure means unavailable.
    Video() : ready(SDL_Init(SDL_INIT_VIDEO)) {}

    /// Releases the video's services and device state.
    ~Video() {
        if (ready)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
};

/// Encodes and writes one proof capture, refusing short output.
void save(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> rgba
) {
    std::vector<uint8_t> encoded;
    const oa::formats::png::Image image{
        {width,
         height,
         8,
         oa::formats::png::ColorType::rgb_alpha,
         oa::formats::png::Interlace::none},
        {},
        rgba
    };
    if (!oa::formats::png::write(image, &encoded))
        throw std::runtime_error("could not encode proof image");
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size())
    );
    if (!out)
        throw std::runtime_error("could not write " + path.string());
}

/// Summarises RGB differences, keeping the alpha channel opaque in the difference image.
size_t difference(
    std::span<const uint8_t> reference,
    std::span<const uint8_t> actual,
    std::vector<uint8_t>& image,
    uint32_t& largest
) {
    if (reference.size() != actual.size() || reference.size() % channels)
        throw std::runtime_error("proof image size mismatch");
    image.assign(reference.size(), 0);
    largest = 0;
    size_t changed = 0;
    for (size_t i = 0; i < reference.size(); i += channels) {
        bool differs = false;
        for (size_t c = 0; c < channels - 1; ++c) {
            const auto delta =
                static_cast<uint32_t>(std::abs(int(reference[i + c]) - int(actual[i + c])));
            largest = std::max(largest, delta);
            differs |= delta != 0;
            image[i + c] =
                static_cast<uint8_t>(std::min<uint32_t>(opaque, delta * difference_gain));
        }
        image[i + 3] = opaque;
        changed += differs ? 1 : 0;
    }
    return changed;
}

/// Writes the classic, GPU and amplified difference together, left to right.
void save_comparison(
    const std::filesystem::path& path,
    const Frame& frame,
    std::span<const uint8_t> classic,
    std::span<const uint8_t> gpu,
    std::span<const uint8_t> delta
) {
    constexpr uint32_t panels = 3;
    const size_t row = size_t{frame.width} * channels;
    std::vector<uint8_t> joined(row * panels * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y) {
        auto* dest = joined.data() + size_t{y} * row * panels;
        std::copy_n(classic.data() + y * row, row, dest);
        std::copy_n(gpu.data() + y * row, row, dest + row);
        std::copy_n(delta.data() + y * row, row, dest + 2 * row);
    }
    save(path, frame.width * panels, frame.height, joined);
}
} // namespace

int run(const std::filesystem::path& game_dir, const std::filesystem::path& snapshot) {
    try {
        const auto output =
            snapshot.empty() ? std::filesystem::path("local/reports/renderer-proof") : snapshot;
        std::filesystem::create_directories(output);
        renderer_proof_scene::Scene scene;
        std::string error;
        if (!renderer_proof_scene::build(game_dir, scene, error))
            throw std::runtime_error(error);
        Frame frame{
            scene.vertices,
            scene.texture_rgba,
            scene.texture_width,
            scene.texture_height,
            scene.width,
            scene.height
        };
        save(output / "classic.png", frame.width, frame.height, scene.classic_rgba);
        save(output / "classic-terrain.png", frame.width, frame.height, scene.classic_terrain_rgba);
        std::ostringstream report;
        report << "Experimental renderer proof; no gameplay renderer is replaced.\n"
               << scene.description
               << "\nReference: existing engine rasterizer, not a base-game capture.\n"
               << "Terrain reconstruction uses painted y + height / 2; gameplay heightfield "
                  "alignment remains unproven.\n"
               << "Image: " << frame.width << 'x' << frame.height
               << "; triangles: " << frame.vertices.size() / 3 << '\n';
        std::vector<uint8_t> cpu, gpu, delta;
        const auto backends = renderers();
        if (backends[0].draw(backends[0].context, frame, cpu, error) !=
            renderer_proof_gpu::Result::success)
            throw std::runtime_error(error);
        save(output / "cpu-depth.png", frame.width, frame.height, cpu);
        Video video;
        const auto started = std::chrono::steady_clock::now();
        auto rendered = renderer_proof_gpu::Result::unavailable;
        if (video.ready)
            rendered = backends[1].draw(backends[1].context, frame, gpu, error);
        else
            error = std::string("SDL video unavailable: ") + SDL_GetError();
        if (rendered != renderer_proof_gpu::Result::success) {
            report << "GPU "
                   << (rendered == renderer_proof_gpu::Result::unavailable ? "unavailable: "
                                                                           : "failed: ")
                   << error << '\n';
            std::ofstream out(output / "report.txt");
            out << report.str();
            if (!out)
                throw std::runtime_error("could not write proof report");
            std::cout << report.str();
            return rendered == renderer_proof_gpu::Result::unavailable ? unavailable_exit : 1;
        }
        report << "GPU setup, upload, render and readback ms (not steady-state frame time): "
               << std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - started
                  )
                      .count()
               << '\n';
        save(output / "gpu.png", frame.width, frame.height, gpu);
        uint32_t largest{};
        const size_t scene_changed = difference(scene.classic_rgba, gpu, delta, largest);
        report << "Classic vs GPU scene: " << scene_changed << " changed pixels; largest RGB delta "
               << largest << "; exact parity " << (scene_changed ? "NO" : "YES") << '\n';
        save(output / "difference.png", frame.width, frame.height, delta);
        save_comparison(output / "comparison.png", frame, scene.classic_rgba, gpu, delta);
        const size_t reference_changed = difference(cpu, gpu, delta, largest);
        report << "CPU depth vs GPU scene: " << reference_changed
               << " changed pixels; largest RGB delta " << largest << '\n';
        save(output / "depth-difference.png", frame.width, frame.height, delta);
        frame.vertices = frame.vertices.first(scene.terrain_vertex_count);
        if (backends[1].draw(backends[1].context, frame, gpu, error) !=
            renderer_proof_gpu::Result::success)
            throw std::runtime_error(error);
        const size_t terrain_changed = difference(scene.classic_terrain_rgba, gpu, delta, largest);
        report << "Classic vs GPU terrain: " << terrain_changed
               << " changed pixels; largest RGB delta " << largest << "; exact parity "
               << (terrain_changed ? "NO" : "YES") << '\n'
               << "Completion does not establish base-game or full-scene parity.\n"
               << "Comparison panels: classic | experimental GPU | absolute RGB difference x4.\n";
        save(output / "gpu-terrain.png", frame.width, frame.height, gpu);
        save(output / "terrain-difference.png", frame.width, frame.height, delta);
        std::ofstream out(output / "report.txt");
        out << report.str();
        if (!out)
            throw std::runtime_error("could not write proof report");
        std::cout << report.str() << "Captures: " << std::filesystem::absolute(output) << '\n';
        return terrain_changed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "renderer proof: " << e.what() << '\n';
        return 1;
    }
}
} // namespace oa::app::renderer_proof
