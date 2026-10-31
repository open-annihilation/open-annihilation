// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/model_runtime/instance.hpp"
#include "oa/sim/model_runtime/model_host.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Host final : oa::sim::model_runtime::ModelHost {
    using ModelHost::ModelHost;

    void emit_sfx(uint32_t, int32_t) override {}

    void explode(uint32_t, int32_t) override {}

    int32_t get_unit_value(int32_t, int32_t, int32_t, int32_t, int32_t) override { return 0; }

    void set_unit_value(int32_t, int32_t) override {}

    void attach_unit(int32_t, int32_t, int32_t) override {}

    void drop_unit(int32_t) override {}

    void ignored_piece_op(uint32_t, uint32_t, uint32_t) override {}

    void dont_shadow(uint32_t) override {}

    uint32_t is_carrying_unit(uint32_t) override { return 0; }

    uint32_t carrier_unit_id() override { return 0; }

    uint32_t random_bounded(uint32_t) override { return 0; }
};

using oa::formats::objects3d::FixedVector3;
using oa::sim::model_runtime::Instance;
using oa::sim::model_runtime::RotationWords;

bool same_transforms(const Instance& a, const Instance& b) {
    if (a.pieces().size() != b.pieces().size())
        return false;
    const auto same = [](const FixedVector3& u, const FixedVector3& v) {
        return u.x == v.x && u.y == v.y && u.z == v.z;
    };
    for (std::size_t piece = 0; piece < a.pieces().size(); ++piece) {
        const auto& p = a.pieces()[piece];
        const auto& q = b.pieces()[piece];
        if (!same(p.transformed_origin, q.transformed_origin) ||
            p.transform_marker != q.transform_marker ||
            p.transformed_vertices.size() != q.transformed_vertices.size())
            return false;
        for (std::size_t vertex = 0; vertex < p.transformed_vertices.size(); ++vertex)
            if (!same(p.transformed_vertices[vertex], q.transformed_vertices[vertex]))
                return false;
    }
    return true;
}

// The transforms a fresh instance in the same pose gets from its first,
// full rebuild.
Instance rebuilt_afresh(
    const std::shared_ptr<const oa::formats::objects3d::Model>& model,
    std::span<const std::string_view> names,
    const Instance& posed,
    RotationWords root
) {
    auto fresh = oa::sim::model_runtime::make_instance(model, 0, names);
    for (std::size_t piece = 0; piece < fresh.pieces().size(); ++piece) {
        fresh.pieces()[piece].translation = posed.pieces()[piece].translation;
        fresh.pieces()[piece].rotation = posed.pieces()[piece].rotation;
    }
    fresh.rebuild_transforms(root);
    return fresh;
}

// rebuild_transforms works out again only the pieces whose transforms could
// change, and leaves what a full rebuild leaves: after moves and turns of
// any piece, whether through the script's setters or written straight into
// the pieces as the renderer's copies are, after root turns, in copies of the
// instance posed apart from it, and in an instance that takes another's pose
// each step (take_pose) and keeps its own transforms.
void check_kept_transforms() {
    // base > turret > barrel > tip, base > flare, and a root-level sibling;
    // the script's order puts children before their parents.
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    model->objects.resize(6);
    const auto link = [&](uint32_t object, uint32_t parent, uint32_t child, uint32_t sibling) {
        model->objects[object].parent = parent;
        model->objects[object].first_child = child;
        model->objects[object].next_sibling = sibling;
    };
    constexpr uint32_t none = oa::formats::objects3d::kNoObject;
    link(0, none, 1, 5);
    link(1, 0, 2, 4);
    link(2, 1, 3, none);
    link(3, 2, none, none);
    link(4, 0, none, none);
    link(5, none, none, none);
    const char* const object_names[] = {"base", "turret", "barrel", "tip", "flare", "loose"};
    for (uint32_t object = 0; object < 6; ++object) {
        auto& source = model->objects[object];
        source.name = object_names[object];
        const auto unit = static_cast<int32_t>(object + 1) * 65536;
        source.offset_from_parent = {unit / 3, unit / 2, -unit / 5};
        source.vertices = {{unit, 0, 0}, {0, -unit, unit / 7}, {unit / 3, unit / 5, -unit}};
    }
    constexpr std::string_view names[] = {"tip", "barrel", "flare", "turret", "base"};
    auto instance = oa::sim::model_runtime::make_instance(model, 0, names);
    require(instance.pieces().size() == 6, "the kept-transforms model has six pieces");
    Host host(instance);

    RotationWords root{};
    uint64_t state = 0x5eed;
    const auto next = [&state] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 33);
    };
    instance.rebuild_transforms(root);
    require(
        same_transforms(instance, rebuilt_afresh(model, names, instance, root)),
        "the first rebuild differs from a fresh one"
    );
    // An instance of another model takes the pose and the transforms whole.
    auto other_model = std::make_shared<oa::formats::objects3d::Model>(*model);
    Instance copies = oa::sim::model_runtime::make_instance(other_model, 0, names);
    copies.take_pose(instance);
    require(
        same_transforms(copies, instance) && copies.transforms_dirty() &&
            &copies.model() == model.get(),
        "an instance of another model did not take the pose whole"
    );
    for (int step = 0; step < 4000; ++step) {
        const uint32_t pick = next();
        const uint32_t piece = pick % 6;
        switch ((pick >> 8) % 5) {
        case 0: // a script turn
            host.set_piece_angle(piece, (pick >> 12) % 3, next() & 0xffff);
            break;
        case 1: // a script move
            host.set_piece_position(
                piece, (pick >> 12) % 3, static_cast<int32_t>(next() % 0x40000) - 0x20000
            );
            break;
        case 2: // written straight into the piece, as copies blended between ticks are
            instance.pieces()[piece].rotation.xz = static_cast<int16_t>(next() & 0xffff);
            instance.pieces()[piece].translation.y += 0x100;
            break;
        case 3: // the root turns
            root.xy = static_cast<int16_t>(root.xy + static_cast<int16_t>(next() % 64) - 32);
            break;
        default: // nothing changes
            break;
        }
        instance.rebuild_transforms(root);
        require(
            same_transforms(instance, rebuilt_afresh(model, names, instance, root)),
            "a rebuild after a change differs from a full rebuild"
        );
        // The renderer's copies: the instance's pose taken, some pieces
        // placed elsewhere and the root turned otherwise, then rebuilt.
        copies.take_pose(instance);
        if ((pick >> 20) % 3 != 0)
            copies.pieces()[next() % 6].rotation.xy = static_cast<int16_t>(next() & 0xffff);
        RotationWords copies_root = root;
        if ((pick >> 24) % 4 == 0)
            copies_root.xy = static_cast<int16_t>(copies_root.xy + 5);
        copies.rebuild_transforms(copies_root);
        require(
            same_transforms(copies, rebuilt_afresh(model, names, copies, copies_root)),
            "an instance that took another's pose rebuilds otherwise than a full rebuild"
        );
        if (step % 7 == 0) {
            // A copy posed apart rebuilds what its pose needs from the
            // original's transforms, and leaves the original alone.
            Instance copy = instance;
            copy.pieces()[next() % 6].rotation.yz = static_cast<int16_t>(next() & 0xffff);
            RotationWords copy_root = root;
            if (step % 3 == 0)
                copy_root.yz = static_cast<int16_t>(copy_root.yz + 9);
            copy.rebuild_transforms(copy_root);
            require(
                same_transforms(copy, rebuilt_afresh(model, names, copy, copy_root)),
                "a copy's rebuild differs from a full rebuild"
            );
            require(
                same_transforms(instance, rebuilt_afresh(model, names, instance, root)),
                "a copy's rebuild changed the original"
            );
        }
    }

    // A turn of the tip moves the tip alone; a move of the turret moves it,
    // the barrel and the tip; a turn of the root moves every piece under it.
    // The script's index of each: tip 0, barrel 1, flare 2, turret 3, base 4.
    const auto moved = [&](auto&& change, std::initializer_list<uint32_t> expected) {
        const Instance before = instance;
        change();
        instance.rebuild_transforms(root);
        require(
            same_transforms(instance, rebuilt_afresh(model, names, instance, root)),
            "a rebuild after a single change differs from a full rebuild"
        );
        for (uint32_t piece = 0; piece < 6; ++piece) {
            const auto& was = before.pieces()[piece].transformed_vertices;
            const auto& now = instance.pieces()[piece].transformed_vertices;
            const bool changed = was[0].x != now[0].x || was[0].y != now[0].y ||
                                 was[0].z != now[0].z || was[1].x != now[1].x ||
                                 was[1].y != now[1].y || was[1].z != now[1].z;
            const bool wanted =
                std::find(expected.begin(), expected.end(), piece) != expected.end();
            require(changed == wanted, "a change moved other pieces than those under it");
        }
    };
    moved([&] { host.set_piece_angle(0, 1, 0x2345); }, {0});
    moved([&] { host.set_piece_position(3, 0, 0x30000); }, {0, 1, 3});
    moved([&] { root.xz = static_cast<int16_t>(root.xz + 0x400); }, {0, 1, 2, 3, 4});
    moved([&] {}, {});
}
} // namespace

int main() {
    try {
        auto model = std::make_shared<oa::formats::objects3d::Model>();
        model->objects.resize(3);
        model->objects[0].name = "base";
        model->objects[0].first_child = 1;
        model->objects[0].offset_from_parent = {65536, 0, 0};
        model->objects[0].vertices = {{65536, 0, 0}, {0, 65536, 0}, {0, 0, 65536}};
        model->objects[1].name = "Turret";
        model->objects[1].parent = 0;
        model->objects[1].next_sibling = 2;
        model->objects[1].offset_from_parent = {0, 65536, 0};
        model->objects[2].name = "flare";
        model->objects[2].parent = 0;

        require(
            oa::sim::model_runtime::count_linked_objects(*model, 0) == 3 &&
                oa::sim::model_runtime::count_linked_objects(*model, 1) == 2 &&
                oa::sim::model_runtime::count_linked_objects(*model, 2) == 1,
            "linked object count did not follow child then sibling"
        );
        auto rooted_siblings = std::make_shared<oa::formats::objects3d::Model>();
        rooted_siblings->objects.resize(2);
        rooted_siblings->objects[0].next_sibling = 1;
        require(
            oa::sim::model_runtime::count_linked_objects(*rooted_siblings, 0) == 2 &&
                oa::sim::model_runtime::count_linked_objects(*rooted_siblings, 1) == 1,
            "root sibling chain was not counted"
        );
        require(
            oa::sim::model_runtime::count_linked_objects(*model, 3) == 0,
            "an object index past the model counts nothing"
        );

        constexpr std::string_view names[] = {"flare", "turret"};
        auto instance = oa::sim::model_runtime::make_instance(model, 0x1234, names);
        require(
            instance.pieces().size() == 3 && instance.root_piece() == 2,
            "COB reorder/root relink differs"
        );
        require(
            instance.pieces()[0].object_index == 2 && instance.pieces()[1].object_index == 1,
            "case-insensitive COB ordering differs"
        );
        require(
            instance.pieces()[2].first_child == 1 && instance.pieces()[1].next_sibling == 0,
            "relinked hierarchy differs"
        );
        require(
            instance.pieces()[2].flags == 7 && instance.owner_token() == 0x1234,
            "constructor state differs"
        );

        Host host(instance);
        require(
            host.piece_visible(0) == 0 && host.piece_visible(2) == 1,
            "initial visibility does not follow constructor flags"
        );
        host.set_piece_position(1, 0, 32768);
        host.set_piece_angle(1, 2, 0x4000);
        host.set_piece_visible(1, 1);
        require(
            host.piece_position(1, 0) == 32768 && host.piece_angle(1, 2) == 0x4000 &&
                host.piece_visible(1) == 1,
            "model Host bridge differs"
        );
        host.set_piece_visible(1, 0);
        require((instance.pieces()[1].flags & 1U) == 0, "HIDE did not clear render visibility");
        instance.pieces()[1].transform_marker = 1234;
        host.set_piece_visible(1, 2);
        require(
            host.piece_visible(1) == 0 && instance.pieces()[1].transform_marker == 0,
            "visibility callback did not use the low-bit/full-input behavior"
        );
        instance.pieces()[1].transform_marker = 1234;
        host.set_piece_cached(1, 2);
        require(
            host.piece_cached(1) == 0 && instance.pieces()[1].transform_marker == 1234,
            "cache callback changed marker or ignored low-bit normalization"
        );
        const auto attachment = instance.attachment_position(1);
        require(
            attachment.x == -32768 && attachment.y == 65536 && attachment.z == 0,
            "attachment accumulation differs"
        );
        instance.rebuild_transforms();
        const auto& root = instance.pieces()[2];
        require(
            root.transformed_vertices[0].x == -131072,
            "runtime vertex sign/offset transform differs"
        );
        require(!instance.transforms_dirty(), "transform rebuild did not clear dirty state");

        // piece_box takes in the vertices rebuild_transforms gives under the
        // same root rotation, whatever the instance last rebuilt, and changes
        // nothing.
        auto boxed_model = std::make_shared<oa::formats::objects3d::Model>(*model);
        boxed_model->objects[1].vertices = {
            {3 * 65536, -2 * 65536, 5 * 65536}, {-7 * 65536, 4 * 65536, 65536}
        };
        auto boxed = oa::sim::model_runtime::make_instance(boxed_model, 0, names);
        Host boxed_host(boxed);
        boxed_host.set_piece_angle(1, 2, 0x1234);
        boxed_host.set_piece_angle(2, 0, 0x0800);
        boxed_host.set_piece_position(1, 1, 3 * 65536);
        const oa::sim::model_runtime::RotationWords turned{0x0300, 0x2000, -0x0400};
        boxed.rebuild_transforms(turned);
        std::vector<oa::sim::model_runtime::PieceBox> expected(boxed.pieces().size());
        for (std::size_t piece = 0; piece < expected.size(); ++piece)
            for (const auto& vertex : boxed.pieces()[piece].transformed_vertices) {
                auto& box = expected[piece];
                box.low = {
                    std::min(box.low.x, vertex.x),
                    std::min(box.low.y, vertex.y),
                    std::min(box.low.z, vertex.z)
                };
                box.high = {
                    std::max(box.high.x, vertex.x),
                    std::max(box.high.y, vertex.y),
                    std::max(box.high.z, vertex.z)
                };
            }
        boxed.rebuild_transforms();
        const auto unturned = boxed.pieces()[1].transformed_vertices;
        for (uint32_t piece = 0; piece < expected.size(); ++piece) {
            const auto box = boxed.piece_box(piece, turned, {});
            const auto& want = expected[piece];
            require(
                box.low.x == want.low.x && box.low.y == want.low.y && box.low.z == want.low.z &&
                    box.high.x == want.high.x && box.high.y == want.high.y &&
                    box.high.z == want.high.z,
                "piece box differs from the rebuilt transforms' box"
            );
        }
        require(
            boxed.pieces()[1].transformed_vertices.size() == unturned.size() &&
                boxed.pieces()[1].transformed_vertices[0].x == unturned[0].x &&
                boxed.pieces()[1].transformed_vertices[1].z == unturned[1].z,
            "piece box changed the instance's transforms"
        );
        const auto outside = boxed.piece_box(9, turned, {{1, 2, 3}, {4, 5, 6}});
        require(
            outside.low.x == 1 && outside.high.z == 6, "a piece past the pieces changed the box"
        );

        constexpr std::string_view excess[] = {"base", "turret", "flare", "extra"};
        (void)oa::sim::model_runtime::make_instance(model, 0, excess);
        auto malformed = std::make_shared<oa::formats::objects3d::Model>(*model);
        malformed->objects[1].next_sibling = 99;
        require(
            oa::sim::model_runtime::model_hierarchy_error(malformed) != nullptr,
            "invalid public Model link was accepted"
        );
        const auto refused = oa::sim::model_runtime::make_instance(malformed, 7);
        require(
            refused.pieces().empty() && refused.root_piece() == oa::sim::model_runtime::kNoPiece &&
                refused.owner_token() == 7 && refused.find_piece(0) == nullptr,
            "a refused model gives an instance with no pieces"
        );
        require(
            oa::sim::model_runtime::model_hierarchy_error(model) == nullptr,
            "a well-linked model is refused"
        );
        check_kept_transforms();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
