// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_model_shading.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace {

struct Corner {
    float x{}, y{}, u{}, v{}, row{};
};

/// Interpolates geometry and texture coordinates by the same fraction.
Corner mix(const Corner& a, const Corner& b, float t) {
    return {
        std::lerp(a.x, b.x, t),
        std::lerp(a.y, b.y, t),
        std::lerp(a.u, b.u, t),
        std::lerp(a.v, b.v, t),
        std::lerp(a.row, b.row, t)
    };
}

/// Computes the area of a convex polygon in draw order.
float area(std::span<const Corner> polygon) {
    float result = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        result += a.x * b.y - b.x * a.y;
    }
    return std::abs(result) / 2;
}

} // namespace

int main() {
    using oa::app::renderer_model_shading::for_shade_bands;
    std::array<Corner, 3> triangle{{{0, 0, 0, 0, 0}, {4, 0, 1, 0, 4}, {0, 4, 0, 1, 0}}};
    std::array<float, 32> areas{};
    OA_CHECK(
        for_shade_bands<Corner>(
            triangle,
            [](const Corner& a) { return a.row; },
            mix,
            [&](uint8_t row, std::span<const Corner> polygon) {
                areas[row] += area(polygon);
                for (const auto& vertex : polygon) {
                    OA_CHECK(vertex.row >= row && vertex.row <= row + 1);
                    OA_CHECK(std::abs(vertex.u - vertex.x / 4) < 0.00001F);
                    OA_CHECK(std::abs(vertex.v - vertex.y / 4) < 0.00001F);
                }
            }
        )
    );
    OA_CHECK(areas[0] == 3.5F && areas[1] == 2.5F && areas[2] == 1.5F && areas[3] == 0.5F);
    OA_CHECK(areas[4] == 0);
    for (auto& vertex : triangle)
        vertex.row = 31;
    int count = 0;
    OA_CHECK(
        for_shade_bands<Corner>(
            triangle,
            [](const Corner& a) { return a.row; },
            mix,
            [&](uint8_t row, std::span<const Corner> polygon) {
                OA_CHECK(row == 31 && area(polygon) == 8);
                ++count;
            }
        )
    );
    OA_CHECK(count == 1);
    triangle[0].row = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(!for_shade_bands<Corner>(
        triangle,
        [](const Corner& a) { return a.row; },
        mix,
        [](uint8_t, std::span<const Corner>) {}
    ));
    return oa::test::check_exit_status();
}
