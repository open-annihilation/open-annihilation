// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The poster a battlefield offers, and the strips a processor draws it in.
#include "oa/present/photo/photo_poster.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::present::photo::CaptureKind;
using oa::present::photo::Machine;
using oa::present::photo::PosterAsk;
using oa::present::photo::PosterArea;
using oa::present::photo::PosterExtent;
using oa::present::photo::PosterOffer;
using oa::present::photo::PosterPlan;
using oa::present::photo::PosterStrip;
using oa::present::photo::SizeLock;
using oa::present::photo::kPosterBytesPerPixel;
using oa::present::photo::kProcessorEdge;
using oa::present::photo::plan_poster;
using oa::present::photo::poster_strips;

/// A machine with room for a poster the edge limit allows.
///
/// @param kind the render mode
/// @param edge the card's texture cap; 0 for none
/// @return the machine
Machine open_machine(CaptureKind kind, int32_t edge) {
    Machine machine;
    machine.kind = kind;
    machine.edge_limit = edge;
    machine.cpu_bytes = ~uint64_t{0};
    machine.gpu_bytes = ~uint64_t{0};
    machine.zoom = 1.0F;
    if (kind == CaptureKind::full) {
        machine.supersample = 2;
        machine.grain = 12;
        machine.bytes_per_texel = 4;
    }
    return machine;
}

/// A frozen local view of 320 by 200, on a map with room around it.
///
/// @return the ask
PosterAsk frozen_view() {
    PosterAsk ask;
    ask.view_width = 320;
    ask.view_height = 200;
    ask.map_width = 4096;
    ask.map_height = 4096;
    ask.camera_x = 640;
    ask.camera_z = 480;
    ask.frozen = true;
    return ask;
}

/// The offer for one extent.
///
/// @param plan the plan
/// @param extent which area
/// @return the offer
const PosterOffer& offer_of(const PosterPlan& plan, PosterExtent extent) {
    if (extent == PosterExtent::marked)
        return plan.marked;
    if (extent == PosterExtent::map)
        return plan.map;
    return plan.view;
}

} // namespace

int main() {
    const Machine processor = open_machine(CaptureKind::processor, kProcessorEdge);
    const PosterAsk ask = frozen_view();
    const PosterPlan open = plan_poster(ask, processor);
    CHECK(open.button == SizeLock::open);
    CHECK(open.view.offered && open.view.area.width == 320 && open.view.area.height == 200);
    CHECK(open.view.area.x == 640 && open.view.area.z == 480);
    CHECK(open.marked.offered && open.marked.area.width == 320);
    CHECK(open.map.offered && open.map.area.x == 0 && open.map.area.z == 0);
    CHECK(open.map.area.width == 4096 && open.map.area.height == 4096);

    PosterAsk marked = ask;
    marked.marked = PosterArea{100, 80, 640, 200};
    const PosterPlan wide = plan_poster(marked, processor);
    CHECK(wide.marked.offered && wide.marked.area.width == 640 && wide.marked.area.x == 100);

    PosterAsk running = ask;
    running.frozen = false;
    const PosterPlan moving = plan_poster(running, processor);
    CHECK(moving.button == SizeLock::moving && !moving.view.offered);
    CHECK(moving.map.lock == SizeLock::moving);

    PosterAsk held = ask;
    held.held = true;
    CHECK(plan_poster(held, processor).button == SizeLock::held);

    Machine shared = processor;
    shared.shared = true;
    const PosterPlan together = plan_poster(ask, shared);
    CHECK(together.button == SizeLock::shared && !together.view.offered);
    CHECK(!together.map.offered);

    Machine turned = open_machine(CaptureKind::turned, 4096);
    CHECK(plan_poster(ask, turned).button == SizeLock::above);

    PosterAsk huge = ask;
    huge.map_width = kProcessorEdge + 1;
    huge.map_height = 16;
    const PosterPlan edge = plan_poster(huge, processor);
    CHECK(edge.view.offered);
    CHECK(!edge.map.offered && edge.map.lock == SizeLock::edge && edge.map.limit == kProcessorEdge);

    Machine tight = processor;
    tight.cpu_bytes = static_cast<uint64_t>(320) * 200U * kPosterBytesPerPixel - 1U;
    const PosterPlan memory = plan_poster(ask, tight);
    CHECK(!memory.view.offered && memory.view.lock == SizeLock::memory);
    CHECK(memory.button == SizeLock::open);
    // The PNG writer's copies are counted: half the bytes is not enough.
    tight.cpu_bytes = static_cast<uint64_t>(320) * 200U * 6U;
    CHECK(plan_poster(ask, tight).view.lock == SizeLock::memory);
    tight.cpu_bytes = static_cast<uint64_t>(320) * 200U * kPosterBytesPerPixel;
    CHECK(plan_poster(ask, tight).view.offered);

    Machine tilt = processor;
    tilt.look_bytes_per_pixel = 3;
    tilt.look_bytes = 64;
    tilt.cpu_bytes = static_cast<uint64_t>(320) * 200U * (kPosterBytesPerPixel + 3U) + 63U;
    CHECK(plan_poster(ask, tilt).view.lock == SizeLock::memory);
    tilt.cpu_bytes = static_cast<uint64_t>(320) * 200U * (kPosterBytesPerPixel + 3U) + 64U;
    CHECK(plan_poster(ask, tilt).view.offered);

    Machine card = open_machine(CaptureKind::full, 4096);
    PosterAsk past = ask;
    past.map_width = 8000;
    past.map_height = 4000;
    const PosterPlan tiled = plan_poster(past, card);
    CHECK(tiled.view.offered);
    CHECK(tiled.map.offered);
    card.tile_edge = 300;
    CHECK(plan_poster(ask, card).view.offered);
    card.tile_edge = 0;
    card.gpu_bytes = 1;
    CHECK(plan_poster(ask, card).view.lock == SizeLock::memory);

    const PosterArea two{640, 480, 640, 200};
    const std::vector<PosterStrip> beside = poster_strips(two, 320, 200);
    CHECK(beside.size() == 2U);
    CHECK(beside[0].camera_x == 640 && beside[0].camera_z == 480);
    CHECK(beside[0].width == 320 && beside[0].height == 200 && beside[0].skip_rows == 0);
    CHECK(beside[0].dest_x == 0 && beside[0].dest_y == 0);
    CHECK(beside[1].camera_x == 960 && beside[1].camera_z == 480);
    CHECK(beside[1].width == 320 && beside[1].dest_x == 320 && beside[1].skip_rows == 0);

    const PosterArea stack{10, 5, 100, 250};
    const std::vector<PosterStrip> down = poster_strips(stack, 100, 100);
    CHECK(down.size() == 3U);
    CHECK(down[0].camera_z == 5 && down[0].height == 96 && down[0].skip_rows == 0);
    CHECK(down[1].camera_z == 101 && down[1].height == 96);
    CHECK(down[2].camera_z == 197 && down[2].height == 58);
    CHECK((down[0].camera_z - 5) % 16 == 0);
    CHECK((down[1].camera_z - 5) % 16 == 0);
    CHECK((down[2].camera_z - 5) % 16 == 0);

    const std::vector<PosterStrip> one = poster_strips(PosterArea{0, 0, 20, 20}, 20, 20);
    CHECK(one.size() == 1U && one[0].width == 20 && one[0].height == 20 && one[0].skip_rows == 0);
    CHECK(poster_strips(PosterArea{0, 0, 0, 10}, 10, 10).empty());

    if (failures != 0) {
        std::fprintf(stderr, "%d poster checks failed\n", failures);
        return 1;
    }
    return 0;
}
