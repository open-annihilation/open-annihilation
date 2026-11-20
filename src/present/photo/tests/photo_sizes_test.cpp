// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The photo sizes a battlefield offers, and an exact box reduction.
#include "oa/present/photo/photo_sizes.hpp"
#include "oa/present/photo/photo_tiles.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
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
using oa::present::photo::Draw;
using oa::present::photo::Field;
using oa::present::photo::Machine;
using oa::present::photo::Offer;
using oa::present::photo::Plan;
using oa::present::photo::SizeLock;
using oa::present::photo::box_reduce;
using oa::present::photo::TileRequest;
using oa::present::photo::full_tile_reach;
using oa::present::photo::TurnedTileRequest;
using oa::present::photo::kTurnedEdge;
using oa::present::photo::plan_draw;
using oa::present::photo::plan_sizes;
using oa::present::photo::plan_tiles;
using oa::present::photo::plan_turned_tiles;
using oa::present::photo::turned_tile_draw_limit;

/// A machine with room for every size the edge limit allows.
///
/// @param kind the render mode
/// @param edge the pixel cap; 0 for none
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

/// The offer whose factor is `factor`.
///
/// @param offers the four sizes or the three smooth steps
/// @param count how many
/// @param factor the factor
/// @return the offer, or a locked stand-in when none matches
Offer find_offer(const Offer* offers, int count, int factor) {
    for (int index = 0; index < count; ++index)
        if (offers[index].factor == factor)
            return offers[index];
    Offer missing;
    missing.lock = SizeLock::edge;
    return missing;
}

} // namespace

int main() {
    const Field small{320, 180};
    const Machine processor = open_machine(CaptureKind::processor, 16384);
    const Plan open = plan_sizes(small, processor, 1, 1);
    CHECK(
        find_offer(open.sizes, 4, 1).offered && find_offer(open.sizes, 4, 1).width == 320 &&
        find_offer(open.sizes, 4, 1).height == 180
    );
    CHECK(find_offer(open.sizes, 4, 2).offered && find_offer(open.sizes, 4, 2).width == 640);
    CHECK(find_offer(open.sizes, 4, 3).offered && find_offer(open.sizes, 4, 3).width == 960);
    CHECK(
        find_offer(open.sizes, 4, 4).offered && find_offer(open.sizes, 4, 4).width == 1280 &&
        find_offer(open.sizes, 4, 4).height == 720
    );
    CHECK(
        !find_offer(open.smooth, 3, 2).offered &&
        find_offer(open.smooth, 3, 2).lock == SizeLock::pixels
    );
    CHECK(
        !find_offer(open.smooth, 3, 4).offered &&
        find_offer(open.smooth, 3, 4).lock == SizeLock::pixels
    );

    Machine edged = processor;
    edged.edge_limit = 200;
    const Plan at_edge = plan_sizes(Field{100, 50}, edged, 1, 1);
    CHECK(find_offer(at_edge.sizes, 4, 1).offered);
    CHECK(find_offer(at_edge.sizes, 4, 2).offered);
    CHECK(
        !find_offer(at_edge.sizes, 4, 3).offered &&
        find_offer(at_edge.sizes, 4, 3).lock == SizeLock::edge &&
        find_offer(at_edge.sizes, 4, 3).limit == 200
    );
    CHECK(
        !find_offer(at_edge.sizes, 4, 4).offered &&
        find_offer(at_edge.sizes, 4, 4).lock == SizeLock::edge
    );

    Machine tight = processor;
    tight.cpu_bytes = 100U * 50U * 6U;
    const Plan memory = plan_sizes(Field{100, 50}, tight, 1, 1);
    CHECK(find_offer(memory.sizes, 4, 1).offered);
    CHECK(
        !find_offer(memory.sizes, 4, 2).offered &&
        find_offer(memory.sizes, 4, 2).lock == SizeLock::memory
    );
    CHECK(find_offer(memory.sizes, 4, 4).lock == SizeLock::memory);

    Machine shared = processor;
    shared.shared = true;
    const Plan together = plan_sizes(small, shared, 1, 1);
    CHECK(find_offer(together.sizes, 4, 1).offered);
    CHECK(
        find_offer(together.sizes, 4, 2).lock == SizeLock::shared &&
        !find_offer(together.sizes, 4, 2).offered
    );
    CHECK(find_offer(together.sizes, 4, 3).lock == SizeLock::shared);
    CHECK(find_offer(together.sizes, 4, 4).lock == SizeLock::shared);
    CHECK(find_offer(together.smooth, 3, 1).offered);
    CHECK(
        find_offer(together.smooth, 3, 2).lock == SizeLock::pixels &&
        !find_offer(together.smooth, 3, 2).offered
    );
    CHECK(
        find_offer(together.smooth, 3, 4).lock == SizeLock::pixels &&
        !find_offer(together.smooth, 3, 4).offered
    );
    const Draw shared_four = plan_draw(small, shared, 1, 4);
    CHECK(!shared_four.offered && shared_four.lock == SizeLock::pixels);

    Machine full = open_machine(CaptureKind::full, 500);
    const Plan card = plan_sizes(Field{100, 80}, full, 1, 1);
    CHECK(find_offer(card.sizes, 4, 1).offered);
    CHECK(find_offer(card.sizes, 4, 2).offered);
    CHECK(
        !find_offer(card.sizes, 4, 4).offered &&
        find_offer(card.sizes, 4, 4).lock == SizeLock::edge &&
        find_offer(card.sizes, 4, 4).limit == 240
    );
    Machine wide = open_machine(CaptureKind::full, 5136);
    wide.supersample = 4;
    wide.grain = 12;
    const Plan fitted = plan_sizes(Field{640, 480}, wide, 1, 1);
    CHECK(find_offer(fitted.sizes, 4, 2).offered);
    // 3× and 4× pass the card in one piece. Full draws them in tiles.
    CHECK(find_offer(fitted.sizes, 4, 3).offered);
    CHECK(find_offer(fitted.sizes, 4, 4).offered);
    Machine tiled_memory = wide;
    tiled_memory.cpu_bytes = 1000;
    const Draw tiled_short = plan_draw(Field{640, 480}, tiled_memory, 4, 1);
    CHECK(!tiled_short.offered && tiled_short.lock == SizeLock::memory);
    Machine full_shared = open_machine(CaptureKind::full, 16384);
    full_shared.shared = true;
    const Plan full_together = plan_sizes(small, full_shared, 1, 1);
    CHECK(find_offer(full_together.sizes, 4, 1).offered);
    CHECK(find_offer(full_together.sizes, 4, 2).lock == SizeLock::shared);
    CHECK(find_offer(full_together.smooth, 3, 2).offered);
    CHECK(
        find_offer(full_together.smooth, 3, 4).lock == SizeLock::shared &&
        !find_offer(full_together.smooth, 3, 4).offered
    );
    // Smooth edges 2× at Screen size past the card in one piece: a match on
    // this machine alone draws it in tiles, a shared match never does.
    Machine wide_alone = open_machine(CaptureKind::full, 16384);
    wide_alone.supersample = 8;
    const Draw alone_smooth = plan_draw(Field{1280, 720}, wide_alone, 1, 2);
    CHECK(alone_smooth.offered);
    Machine wide_shared = wide_alone;
    wide_shared.shared = true;
    const Draw shared_smooth = plan_draw(Field{1280, 720}, wide_shared, 1, 2);
    CHECK(!shared_smooth.offered && shared_smooth.lock == SizeLock::edge);
    const Plan shared_row = plan_sizes(Field{1280, 720}, wide_shared, 1, 1);
    CHECK(
        !find_offer(shared_row.smooth, 3, 2).offered &&
        find_offer(shared_row.smooth, 3, 2).lock == SizeLock::edge
    );
    // A tilt-shift's copy of the picture and its sums are counted: 3 bytes
    // a pixel of the picture, and the sums once.
    Machine look_room = open_machine(CaptureKind::processor, 16384);
    const uint64_t two_pixels = uint64_t{1280} * 960U;
    look_room.cpu_bytes = two_pixels * 6U;
    CHECK(plan_draw(Field{640, 480}, look_room, 2, 1).offered);
    look_room.look_bytes_per_pixel = 3;
    look_room.look_bytes = oa::present::photo::kLookScratchBytes;
    const Draw no_look_room = plan_draw(Field{640, 480}, look_room, 2, 1);
    CHECK(!no_look_room.offered && no_look_room.lock == SizeLock::memory);
    look_room.cpu_bytes = two_pixels * 9U + oa::present::photo::kLookScratchBytes;
    CHECK(plan_draw(Field{640, 480}, look_room, 2, 1).offered);
    Machine full_memory = full;
    full_memory.edge_limit = 16384;
    full_memory.gpu_bytes = 1;
    const Plan card_memory = plan_sizes(Field{100, 80}, full_memory, 1, 1);
    CHECK(find_offer(card_memory.sizes, 4, 2).lock == SizeLock::memory);

    const Machine turned = open_machine(CaptureKind::turned, 4096);
    const Plan view = plan_sizes(Field{2000, 1000}, turned, 1, 1);
    CHECK(find_offer(view.sizes, 4, 2).offered && find_offer(view.sizes, 4, 2).width == 4000);
    CHECK(find_offer(view.sizes, 4, 3).offered && find_offer(view.sizes, 4, 3).width == 6000);
    CHECK(find_offer(view.sizes, 4, 4).offered && find_offer(view.sizes, 4, 4).width == 8000);
    const Draw past_photo = plan_draw(Field{5000, 5000}, turned, 4, 1);
    CHECK(!past_photo.offered && past_photo.lock == SizeLock::edge && past_photo.limit == 16384);
    Machine turned_dof = turned;
    turned_dof.depth_of_field = 32;
    CHECK(plan_draw(Field{2000, 1000}, turned_dof, 4, 1).offered);
    Machine turned_shared = turned;
    turned_shared.shared = true;
    const Draw shared_turned_smooth = plan_draw(Field{3000, 1500}, turned_shared, 1, 2);
    CHECK(!shared_turned_smooth.offered && shared_turned_smooth.lock == SizeLock::edge);
    CHECK(plan_draw(Field{3000, 1500}, turned, 1, 2).offered);
    const Plan shared_sizes = plan_sizes(Field{2000, 1000}, turned_shared, 1, 1);
    CHECK(
        find_offer(shared_sizes.sizes, 4, 4).lock == SizeLock::shared &&
        !find_offer(shared_sizes.sizes, 4, 4).offered
    );
    Machine turned_memory = turned;
    turned_memory.cpu_bytes = uint64_t{6000} * 3000U * 6U;
    CHECK(plan_draw(Field{2000, 1000}, turned_memory, 1, 1).offered);
    const Draw tight_tiles = plan_draw(Field{2000, 1000}, turned_memory, 3, 1);
    CHECK(!tight_tiles.offered && tight_tiles.lock == SizeLock::memory);
    turned_memory.cpu_bytes = ~uint64_t{0};
    CHECK(plan_draw(Field{2000, 1000}, turned_memory, 3, 1).offered);
    // A wide window's multisampling is limited by area: its tiles stay within
    // the square that keeps it, not the window-shaped sampling edge.
    Machine turned_square = open_machine(CaptureKind::turned, 2730);
    CHECK(turned_tile_draw_limit(turned_square) == 2730);
    turned_square.tile_limit = 2048;
    CHECK(turned_tile_draw_limit(turned_square) == 2048);
    turned_square.tile_limit = 8192;
    CHECK(turned_tile_draw_limit(turned_square) == 2730);
    Machine turned_view = open_machine(CaptureKind::turned, 4096);
    turned_view.viewport_limit = 6000;
    turned_view.viewport_samples = 2;
    CHECK(turned_tile_draw_limit(turned_view) == 3000);
    turned_view.viewport_limit = 0;
    CHECK(turned_tile_draw_limit(turned_view) == 4096);
    turned_square.tile_limit = 2048;
    CHECK(plan_draw(Field{1920, 1080}, turned_square, 2, 1).offered);
    TurnedTileRequest square_request;
    square_request.width = 3840;
    square_request.height = 2160;
    square_request.zoom = 2.0F;
    square_request.draw_limit = turned_tile_draw_limit(turned_square);
    const auto square_plan = plan_turned_tiles(square_request);
    CHECK(square_plan.fits && square_plan.tiles.size() > 1);
    for (const auto& tile : square_plan.tiles)
        CHECK(tile.draw_width <= 2048 && tile.draw_height <= 2048);

    const Field fields[] = {{64, 64}, {320, 180}, {640, 480}, {1920, 1080}};
    const int32_t limits[] = {256, 1024, 4096, 16384};
    const CaptureKind kinds[] = {CaptureKind::processor, CaptureKind::full, CaptureKind::turned};
    for (const Field& field : fields) {
        for (const int32_t limit : limits) {
            for (const CaptureKind kind : kinds) {
                Machine machine = open_machine(kind, limit);
                if (kind != CaptureKind::full)
                    machine.supersample = 1;
                for (const int smooth : {1, 2, 4}) {
                    const Plan plan = plan_sizes(field, machine, 1, smooth);
                    for (const Offer& offer : plan.sizes) {
                        if (!offer.offered)
                            continue;
                        const int32_t draw_edge = std::max(offer.width, offer.height) * smooth;
                        const int32_t piece = kind == CaptureKind::turned && limit > 0
                                                  ? std::min(limit, kTurnedEdge)
                                                  : limit;
                        if (kind == CaptureKind::full && limit > 0 && draw_edge > piece) {
                            TileRequest request;
                            request.width = offer.width * smooth;
                            request.height = offer.height * smooth;
                            request.zoom = static_cast<float>(offer.factor * smooth);
                            request.supersample = machine.supersample;
                            request.grain = machine.grain;
                            request.texture_limit = limit;
                            request.reach = full_tile_reach(request.zoom);
                            CHECK(plan_tiles(request).fits);
                        } else if (kind == CaptureKind::turned && limit > 0 && draw_edge > piece) {
                            TurnedTileRequest request;
                            request.width = offer.width * smooth;
                            request.height = offer.height * smooth;
                            request.zoom = static_cast<float>(offer.factor * smooth);
                            request.draw_limit = piece;
                            CHECK(plan_turned_tiles(request).fits);
                        } else {
                            CHECK(limit <= 0 || draw_edge <= piece);
                        }
                        CHECK(offer.width == field.width * offer.factor);
                        CHECK(offer.height == field.height * offer.factor);
                    }
                }
            }
        }
    }

    const Machine smooth_edge = open_machine(CaptureKind::processor, 150);
    const Plan smoothed = plan_sizes(Field{100, 50}, smooth_edge, 1, 2);
    CHECK(
        !find_offer(smoothed.sizes, 4, 1).offered &&
        find_offer(smoothed.sizes, 4, 1).lock == SizeLock::edge
    );
    const Plan unsmoothed = plan_sizes(Field{100, 50}, smooth_edge, 1, 1);
    CHECK(find_offer(unsmoothed.sizes, 4, 1).offered);
    CHECK(!find_offer(unsmoothed.sizes, 4, 2).offered);

    const uint8_t block[] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    std::vector<uint8_t> reduced;
    int32_t out_w = 0;
    int32_t out_h = 0;
    CHECK(box_reduce(block, sizeof block, 2, 2, 2, reduced, out_w, out_h));
    CHECK(out_w == 1 && out_h == 1 && reduced.size() == 3);
    CHECK(reduced[0] == 64 && reduced[1] == 64 && reduced[2] == 64);

    std::vector<uint8_t> same;
    CHECK(box_reduce(block, sizeof block, 2, 2, 1, same, out_w, out_h));
    CHECK(out_w == 2 && out_h == 2 && same.size() == sizeof block);
    CHECK(same[3] == 255 && same[7] == 255 && same[11] == 255);

    std::vector<uint8_t> flat(4U * 4U * 3U, 10);
    CHECK(box_reduce(flat.data(), flat.size(), 4, 4, 4, reduced, out_w, out_h));
    CHECK(out_w == 1 && out_h == 1 && reduced.size() == 3);
    CHECK(reduced[0] == 10 && reduced[1] == 10 && reduced[2] == 10);

    std::vector<uint8_t> uneven;
    CHECK(!box_reduce(block, sizeof block, 2, 2, 3, uneven, out_w, out_h));
    CHECK(uneven.empty());

    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
