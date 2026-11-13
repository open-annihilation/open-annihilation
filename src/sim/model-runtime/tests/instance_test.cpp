// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"
#include "oa/sim/model_runtime/instance.hpp"
#include "oa/sim/model_runtime/model_host.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
thread_local bool watch_transform_allocations{};
thread_local size_t transform_allocations{};
} // namespace

/// Counts ordinary allocations while a transform reuse is watched.
void* operator new(size_t bytes) {
    if (watch_transform_allocations)
        ++transform_allocations;
    if (void* result = std::malloc(bytes == 0 ? 1 : bytes))
        return result;
    throw std::bad_alloc{};
}

/// Counts ordinary array allocations through the same watch.
void* operator new[](size_t bytes) {
    return ::operator new(bytes);
}

/// Releases ordinary allocation storage.
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

/// Releases array allocation storage.
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

/// Releases sized ordinary allocation storage.
void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}

/// Releases sized array allocation storage.
void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

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

bool same_vector(const FixedVector3& a, const FixedVector3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_words(RotationWords a, RotationWords b) {
    return a.xy == b.xy && a.xz == b.xz && a.yz == b.yz;
}

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

int32_t scalar_low32(int64_t value) {
    return std::bit_cast<int32_t>(static_cast<uint32_t>(value));
}

int32_t scalar_negate(int32_t value) {
    return std::bit_cast<int32_t>(0U - std::bit_cast<uint32_t>(value));
}

// The scalar pair before the zero-coordinate shortcut: only angle zero
// skips the exact products, binary64 sums and integer rounding.
void scalar_old_pair(int32_t& a, int32_t& b, int16_t angle) {
    if (angle == 0)
        return;
    const auto result = oa::base::game_math::rotate_pair(a, b, angle);
    a = scalar_low32(std::llrint(result.first));
    b = scalar_low32(std::llrint(result.second));
}

FixedVector3 scalar_old_rotation(FixedVector3 point, RotationWords words) {
    scalar_old_pair(point.x, point.y, words.xy);
    scalar_old_pair(point.y, point.z, words.yz);
    scalar_old_pair(point.x, point.z, words.xz);
    return point;
}

// Reconstructs the full geometry through the old scalar pair, without
// Instance::rebuild_transforms or model_runtime::rotate_vector.
Instance scalar_old_rebuild(const Instance& posed, RotationWords root) {
    Instance result = posed;
    const auto source_pieces = posed.pieces();
    for (uint32_t index = 0; index < result.pieces().size(); ++index) {
        auto& piece = result.pieces()[index];
        const auto& object = posed.model().objects[piece.object_index];
        piece.transform_marker = 0;
        piece.transformed_vertices.clear();
        for (const auto& point : object.vertices)
            piece.transformed_vertices.push_back(
                {scalar_negate(point.x), point.y, scalar_negate(point.z)}
            );
        piece.transformed_origin = {};
        auto top = index;
        while (source_pieces[top].parent != oa::sim::model_runtime::kNoPiece)
            top = source_pieces[top].parent;
        if (top != posed.root_piece())
            continue;
        for (auto current = index; current != oa::sim::model_runtime::kNoPiece;
             current = source_pieces[current].parent) {
            const auto& state = source_pieces[current];
            const auto& source = posed.model().objects[state.object_index];
            auto turn = state.rotation;
            if (state.parent == oa::sim::model_runtime::kNoPiece) {
                turn.xy = static_cast<int16_t>(turn.xy + root.xy);
                turn.xz = static_cast<int16_t>(turn.xz + root.xz);
                turn.yz = static_cast<int16_t>(turn.yz + root.yz);
            }
            const auto place = [&](FixedVector3& point) {
                point = scalar_old_rotation(point, turn);
                point.x = scalar_low32(
                    int64_t{point.x} + scalar_negate(source.offset_from_parent.x) +
                    state.translation.x
                );
                point.y = scalar_low32(
                    int64_t{point.y} + source.offset_from_parent.y + state.translation.y
                );
                point.z = scalar_low32(
                    int64_t{point.z} + scalar_negate(source.offset_from_parent.z) +
                    state.translation.z
                );
            };
            place(piece.transformed_origin);
            for (auto& point : piece.transformed_vertices)
                place(point);
        }
    }
    return result;
}

void check_zero_pair_rotation() {
    constexpr int32_t lowest = std::numeric_limits<int32_t>::min();
    constexpr int32_t highest = std::numeric_limits<int32_t>::max();
    constexpr std::array<std::array<int32_t, 2>, 13> pairs{{
        {0, 0},
        {0, 1},
        {1, 0},
        {0, -1},
        {-1, 0},
        {0, lowest},
        {lowest, 0},
        {0, highest},
        {highest, 0},
        {lowest, highest},
        {lowest, lowest},
        {65536, -32768},
        {-12345678, 7654321},
    }};
    for (int32_t word = std::numeric_limits<int16_t>::min();
         word <= std::numeric_limits<int16_t>::max();
         ++word) {
        const auto angle = static_cast<int16_t>(word);
        for (const auto& pair : pairs) {
            const std::array<FixedVector3, 3> points{
                FixedVector3{pair[0], pair[1], 0},
                FixedVector3{0, pair[0], pair[1]},
                FixedVector3{pair[0], 0, pair[1]}
            };
            const std::array<RotationWords, 3> turns{
                RotationWords{angle, 0, 0}, RotationWords{0, 0, angle}, RotationWords{0, angle, 0}
            };
            for (size_t plane = 0; plane < points.size(); ++plane)
                require(
                    same_vector(
                        oa::sim::model_runtime::rotate_vector(points[plane], turns[plane]),
                        scalar_old_rotation(points[plane], turns[plane])
                    ),
                    "a pair rotation differs from the old scalar arithmetic"
                );
        }
        const RotationWords mixed{
            angle, static_cast<int16_t>(word + 1), static_cast<int16_t>(word - 1)
        };
        for (const FixedVector3 point : {FixedVector3{}, FixedVector3{0, 0, lowest}})
            require(
                same_vector(
                    oa::sim::model_runtime::rotate_vector(point, mixed),
                    scalar_old_rotation(point, mixed)
                ),
                "successive pair rotations differ after initially zero coordinates"
            );
    }

    // An eleven-level chain with a COB-reordered root, plus a root sibling
    // whose geometry remains untransformed. Every piece includes zero and
    // one-zero vertices, and offsets and translations cross signed limits.
    constexpr size_t piece_count = 12, sibling = piece_count - 1;
    constexpr uint32_t none = oa::formats::objects3d::kNoObject;
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    model->objects.resize(piece_count);
    std::array<std::string_view, piece_count> names{};
    for (size_t index = 0; index < piece_count; ++index) {
        auto& object = model->objects[index];
        object.name = "part" + std::to_string(index);
        object.parent = index == 0 || index == sibling ? none : uint32_t(index - 1);
        object.first_child = index + 1 < sibling ? uint32_t(index + 1) : none;
        object.next_sibling = index == 0 ? uint32_t(sibling) : none;
        object.offset_from_parent =
            index % 2 == 0 ? FixedVector3{} : FixedVector3{lowest, highest, 65536};
        object.vertices = {{0, 0, 0}, {lowest, 0, 0}, {0, highest, lowest}, {65536, -32768, 2}};
    }
    for (size_t index = 0; index < names.size(); ++index)
        names[index] = model->objects[piece_count - 1 - index].name;
    auto instance = oa::sim::model_runtime::make_instance(model, 73, names);
    require(instance.root_piece() != 0, "the scalar hierarchy fixture did not reorder its root");
    for (uint32_t step = 0; step < 128; ++step) {
        for (uint32_t index = 0; index < instance.pieces().size(); ++index) {
            auto& piece = instance.pieces()[index];
            const uint32_t bits = step * 2654435761U + index * 2246822519U;
            piece.translation = {
                std::bit_cast<int32_t>(bits),
                index % 2 == 0 ? lowest : highest,
                std::bit_cast<int32_t>(0U - bits)
            };
            piece.rotation = step % 3 == 0 ? RotationWords{}
                                           : RotationWords{
                                                 static_cast<int16_t>(bits),
                                                 static_cast<int16_t>(bits >> 8),
                                                 static_cast<int16_t>(bits >> 16)
                                             };
            piece.transform_marker = static_cast<uint16_t>(index + 1);
        }
        const RotationWords root{
            static_cast<int16_t>(step * 977U),
            std::numeric_limits<int16_t>::min(),
            static_cast<int16_t>(0U - step * 257U)
        };
        const Instance expected = scalar_old_rebuild(instance, root);
        Instance checked = instance;
        checked.rebuild_transforms(root, true);
        require(
            same_transforms(checked, expected),
            "a checked integer rebuild differs from the scalar hierarchy walk"
        );
        instance.rebuild_transforms(root);
        require(
            same_transforms(instance, expected),
            "a deep model rebuild differs from the old scalar hierarchy walk"
        );
        require(
            instance.owner_token() == 73 && !instance.transforms_dirty(),
            "the zero-pair shortcut changed instance ownership or dirty bookkeeping"
        );
    }
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

bool same_state(const Instance& a, const Instance& b) {
    if (a.model_handle() != b.model_handle() || a.root_piece() != b.root_piece() ||
        a.owner_token() != b.owner_token() || a.transforms_dirty() != b.transforms_dirty() ||
        !same_transforms(a, b))
        return false;
    for (size_t index = 0; index < a.pieces().size(); ++index) {
        const auto& p = a.pieces()[index];
        const auto& q = b.pieces()[index];
        if (p.object_index != q.object_index || p.parent != q.parent ||
            p.first_child != q.first_child || p.next_sibling != q.next_sibling ||
            p.flags != q.flags || !same_vector(p.translation, q.translation) ||
            !same_words(p.rotation, q.rotation) ||
            !same_vector(p.built_translation, q.built_translation) ||
            !same_words(p.built_rotation, q.built_rotation))
            return false;
    }
    return true;
}

void copy_local_pose(Instance& into, const Instance& from) {
    require(
        into.pieces().size() == from.pieces().size(), "pose copy requires the same piece count"
    );
    for (size_t index = 0; index < into.pieces().size(); ++index) {
        into.pieces()[index].translation = from.pieces()[index].translation;
        into.pieces()[index].rotation = from.pieces()[index].rotation;
    }
}

/// Checks an exact final root-plane reuse through wraps and reordered hierarchies.
void check_root_prefix_transforms() {
    constexpr int32_t lowest = std::numeric_limits<int32_t>::min();
    constexpr int32_t highest = std::numeric_limits<int32_t>::max();
    constexpr uint32_t none = oa::formats::objects3d::kNoObject;
    const auto cancelled = [](int16_t angle) {
        return std::bit_cast<int16_t>(static_cast<uint16_t>(0U - static_cast<uint16_t>(angle)));
    };
    const auto word = [](uint32_t bits) {
        return std::bit_cast<int16_t>(static_cast<uint16_t>(bits));
    };
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    model->objects.resize(2);
    auto& base = model->objects[0];
    base.name = "base";
    base.next_sibling = 1;
    base.offset_from_parent = {lowest, highest, highest};
    base.vertices = {{lowest, 0, highest}, {highest, lowest, lowest}, {0, highest, 0}};
    auto& sibling = model->objects[1];
    sibling.name = "loose";
    sibling.vertices = {{lowest, highest, 1}, {7, 0, -3}, {0, 0, lowest}};
    constexpr std::string_view names[]{"loose", "base"};
    auto prefix = oa::sim::model_runtime::make_instance(model, 17, names);
    const auto root_index = prefix.root_piece();
    require(root_index == 1, "the prefix fixture did not reorder the root");
    prefix.pieces()[root_index].translation = {highest, lowest, lowest};
    prefix.pieces()[root_index].rotation = {321, word(0x8123), -123};
    const RotationWords prefix_root{-23, cancelled(prefix.pieces()[root_index].rotation.xz), 400};
    prefix.rebuild_transforms(prefix_root, true);
    const Instance saved_prefix = prefix;
    auto receiver = oa::sim::model_runtime::make_instance(model, 91, names);
    copy_local_pose(receiver, prefix);
    receiver.pieces()[root_index].rotation.xy = 338;
    receiver.pieces()[root_index].rotation.yz = -144;
    for (int32_t angle = std::numeric_limits<int16_t>::min();
         angle <= std::numeric_limits<int16_t>::max();
         ++angle) {
        auto& root_piece = receiver.pieces()[root_index];
        root_piece.rotation.xz = word(uint32_t(angle) * 977U);
        root_piece.translation = {
            std::bit_cast<int32_t>(uint32_t(angle) * 2654435761U),
            angle % 2 == 0 ? lowest : highest,
            std::bit_cast<int32_t>(0U - uint32_t(angle) * 2246822519U)
        };
        const RotationWords root{-40, static_cast<int16_t>(angle), 421};
        const auto oracle = scalar_old_rebuild(receiver, root);
        require(receiver.root_transforms_changed(root), "a changed root was called incremental");
        transform_allocations = 0;
        watch_transform_allocations = true;
        const bool reused = receiver.reuse_root_prefix_transforms(prefix, root);
        watch_transform_allocations = false;
        require(reused && transform_allocations == 0, "a root prefix missed or allocated");
        require(
            same_transforms(receiver, oracle), "a final root plane differs from the scalar walk"
        );
        require(
            receiver.owner_token() == 91 && !receiver.transforms_dirty() &&
                !receiver.root_transforms_changed(root),
            "root prefix reuse changed ownership or remembered the wrong root"
        );
        for (const auto& piece : receiver.pieces())
            require(
                same_vector(piece.translation, piece.built_translation) &&
                    same_words(piece.rotation, piece.built_rotation),
                "root prefix reuse did not record the receiver's raw inputs"
            );
        Instance ordinary = receiver;
        ordinary.rebuild_transforms(root);
        require(
            same_state(receiver, ordinary), "a native rebuild after prefix reuse changed state"
        );
    }
    require(same_state(prefix, saved_prefix), "prefix reuse changed its source");
    const RotationWords last_root{-40, std::numeric_limits<int16_t>::max(), 421};
    const auto refused = [&](Instance bad, const Instance& source, RotationWords root) {
        const auto before = bad;
        require(!bad.reuse_root_prefix_transforms(source, root), "an incompatible prefix was used");
        require(same_state(bad, before), "a refused prefix changed its receiver");
    };
    auto stale = prefix;
    stale.pieces()[root_index].translation.x =
        scalar_low32(int64_t{stale.pieces()[root_index].translation.x} + 1);
    refused(receiver, stale, last_root);
    auto not_prefix = prefix;
    auto wrong_turn = prefix_root;
    ++wrong_turn.xz;
    not_prefix.rebuild_transforms(wrong_turn);
    refused(receiver, not_prefix, last_root);
    auto wrong_plane = last_root;
    ++wrong_plane.xy;
    refused(receiver, prefix, wrong_plane);
    auto changed_sibling = receiver;
    ++changed_sibling.pieces()[0].translation.y;
    refused(changed_sibling, prefix, last_root);
    auto malformed = receiver;
    malformed.pieces()[root_index].parent = 0;
    refused(malformed, prefix, last_root);
    auto insufficient = receiver;
    std::vector<FixedVector3>{}.swap(insufficient.pieces()[root_index].transformed_vertices);
    refused(insufficient, prefix, last_root);
    require(!prefix.reuse_root_prefix_transforms(prefix, prefix_root), "a prefix reused itself");
    Instance empty;
    refused(empty, prefix, last_root);

    // An eleven-level chain plus an untransformed root sibling. Lower poses
    // vary independently; the oracle uses neither prefix API nor fast rotations.
    constexpr uint32_t count = 12, loose = count - 1;
    auto deep_model = std::make_shared<oa::formats::objects3d::Model>();
    deep_model->objects.resize(count);
    std::array<std::string_view, count> deep_names{};
    for (uint32_t index = 0; index < count; ++index) {
        auto& object = deep_model->objects[index];
        object.name = "piece" + std::to_string(index);
        object.parent = index == 0 || index == loose ? none : index - 1;
        object.first_child = index + 1 < loose ? index + 1 : none;
        object.next_sibling = index == 0 ? loose : none;
        object.offset_from_parent = {lowest + int32_t(index), highest - int32_t(index), -17};
        object.vertices = {{lowest, highest, 0}, {1, 0, lowest}, {0, highest, 7}};
    }
    for (uint32_t index = 0; index < count; ++index)
        deep_names[index] = deep_model->objects[count - index - 1].name;
    auto deep = oa::sim::model_runtime::make_instance(deep_model, 333, deep_names);
    for (uint32_t frame = 0; frame < 128; ++frame) {
        for (uint32_t index = 0; index < count; ++index) {
            auto& piece = deep.pieces()[index];
            const uint32_t bits = frame * 2654435761U + index * 2246822519U;
            piece.translation = {
                std::bit_cast<int32_t>(bits), lowest, std::bit_cast<int32_t>(0U - bits)
            };
            piece.rotation = {word(bits), word(bits >> 11), word(bits >> 17)};
        }
        const auto ri = deep.root_piece();
        const RotationWords built_root{
            word(frame * 31), cancelled(deep.pieces()[ri].rotation.xz), word(frame * 11)
        };
        auto saved = deep;
        saved.rebuild_transforms(built_root, true);
        deep.pieces()[ri].translation = {highest, lowest, highest};
        deep.pieces()[ri].rotation.xz = word(frame * 691);
        const RotationWords wanted{built_root.xy, word(frame * 977), built_root.yz};
        const auto oracle = scalar_old_rebuild(deep, wanted);
        require(deep.reuse_root_prefix_transforms(saved, wanted), "a deep prefix was refused");
        require(
            same_transforms(deep, oracle), "a deep wrapped prefix differs from the scalar walk"
        );
        require(!deep.root_transforms_changed(wanted), "a reused deep root remains dirty");
        uint32_t child{};
        while (deep.pieces()[child].object_index != 1)
            ++child;
        ++deep.pieces()[child].translation.y;
        require(
            !deep.root_transforms_changed(wanted), "a lower-piece change disabled incremental reuse"
        );
        auto changed_prefix = deep;
        auto changed_root = wanted;
        changed_root.xz = cancelled(changed_prefix.pieces()[ri].rotation.xz);
        changed_prefix.rebuild_transforms(changed_root, true);
        const auto partial_oracle = scalar_old_rebuild(deep, wanted);
        require(
            deep.reuse_root_prefix_transforms(changed_prefix, wanted),
            "partial dirty prefix refused"
        );
        require(
            same_transforms(deep, partial_oracle), "partial dirty prefix changed kept geometry"
        );
    }
}

void check_reused_transforms() {
    // The root has a deep subtree and an untransformed root sibling. COB
    // ordering places children before parents and moves the root from slot 0.
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    model->objects.resize(6);
    constexpr uint32_t none = oa::formats::objects3d::kNoObject;
    const auto links = [&](uint32_t at, uint32_t parent, uint32_t child, uint32_t sibling) {
        model->objects[at].parent = parent;
        model->objects[at].first_child = child;
        model->objects[at].next_sibling = sibling;
    };
    links(0, none, 1, 5);
    links(1, 0, 2, 4);
    links(2, 1, 3, none);
    links(3, 2, none, none);
    links(4, 0, none, none);
    links(5, none, none, none);
    constexpr int32_t lowest = std::numeric_limits<int32_t>::min();
    constexpr int32_t highest = std::numeric_limits<int32_t>::max();
    constexpr std::array<const char*, 6> labels{
        "base", "turret", "barrel", "tip", "flare", "loose"
    };
    for (uint32_t object = 0; object < model->objects.size(); ++object) {
        auto& part = model->objects[object];
        part.name = labels[object];
        const auto offset = static_cast<int32_t>(object) * 101;
        part.offset_from_parent = {highest - offset, lowest + offset, -offset};
        part.vertices = {
            {lowest, highest - offset, -1},
            {highest - offset, lowest + offset, offset},
            {offset, -65536, lowest}
        };
    }
    constexpr std::string_view names[]{"tip", "barrel", "flare", "turret", "base"};
    constexpr uintptr_t source_owner = 17, destination_owner = 91;
    auto source = oa::sim::model_runtime::make_instance(model, source_owner, names);
    for (size_t index = 0; index < source.pieces().size(); ++index) {
        auto& piece = source.pieces()[index];
        const auto offset = static_cast<int32_t>(index) * 313;
        piece.translation = {lowest + offset, highest - offset, -offset};
        piece.rotation = {
            static_cast<int16_t>(std::numeric_limits<int16_t>::max() - index),
            static_cast<int16_t>(std::numeric_limits<int16_t>::min() + index),
            static_cast<int16_t>(0x2345 + index)
        };
    }
    const RotationWords root{
        std::numeric_limits<int16_t>::max(), std::numeric_limits<int16_t>::min(), 0x1234
    };
    source.rebuild_transforms(root);
    require(
        same_transforms(source, rebuilt_afresh(model, names, source, root)),
        "the transform reuse source differs from a full rebuild"
    );
    source.pieces().front().transform_marker = 66;
    auto receiver = oa::sim::model_runtime::make_instance(model, destination_owner, names);
    copy_local_pose(receiver, source);
    constexpr auto visible = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
    constexpr auto cached = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    constexpr auto shaded = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::shaded);
    constexpr std::array<uint16_t, 6> flags{
        visible, 0, cached, shaded, visible | shaded, cached | shaded
    };
    std::array<const FixedVector3*, 6> storage{};
    std::array<size_t, 6> capacities{};
    for (size_t index = 0; index < receiver.pieces().size(); ++index) {
        auto& piece = receiver.pieces()[index];
        piece.flags = flags[index];
        piece.transform_marker = static_cast<uint16_t>(index + 1);
        storage[index] = piece.transformed_vertices.data();
        capacities[index] = piece.transformed_vertices.capacity();
    }
    receiver.pieces()[0].transformed_vertices.clear();
    auto& spare = receiver.piece_for_script_index(100);
    spare.translation = {7, 8, 9};
    spare.flags = shaded;
    spare.transform_marker = 123;
    spare.transformed_vertices.assign(7, {3, 4, 5});
    const auto spare_before = spare;
    const auto bytes_before = receiver.transform_storage_bytes();
    const Instance source_before = source;
    transform_allocations = 0;
    watch_transform_allocations = true;
    const bool hit = receiver.reuse_transforms(source, root);
    watch_transform_allocations = false;
    require(hit && transform_allocations == 0, "transform reuse allocated or missed an exact pose");
    require(
        receiver.owner_token() == destination_owner && !receiver.transforms_dirty() &&
            same_transforms(receiver, rebuilt_afresh(model, names, receiver, root)) &&
            receiver.transform_storage_bytes() == bytes_before && same_state(source, source_before),
        "transform reuse changed ownership/storage/source or differs from a full rebuild"
    );
    require(
        same_vector(spare.translation, spare_before.translation) &&
            spare.flags == spare_before.flags &&
            spare.transform_marker == spare_before.transform_marker &&
            spare.transformed_vertices.size() == spare_before.transformed_vertices.size() &&
            same_vector(
                spare.transformed_vertices.front(), spare_before.transformed_vertices.front()
            ),
        "transform reuse changed the destination's spare piece"
    );
    for (size_t index = 0; index < receiver.pieces().size(); ++index) {
        const auto& piece = receiver.pieces()[index];
        require(
            piece.flags == flags[index] && piece.transformed_vertices.data() == storage[index] &&
                piece.transformed_vertices.capacity() == capacities[index] &&
                same_vector(piece.translation, source.pieces()[index].translation) &&
                same_words(piece.rotation, source.pieces()[index].rotation),
            "transform reuse changed local pose, flags or destination allocation"
        );
    }
    require(
        same_vector(receiver.pieces().back().transformed_origin, {}) &&
            receiver.pieces().back().transformed_vertices.front().x == lowest,
        "transform reuse transformed a root-level sibling"
    );

    const auto reject_unchanged = [&](Instance& into, const Instance& from, RotationWords turn) {
        const Instance before = into;
        const auto bytes = into.transform_storage_bytes();
        std::vector<const FixedVector3*> pointers;
        std::vector<size_t> kept_capacities;
        for (const auto& piece : into.pieces()) {
            pointers.push_back(piece.transformed_vertices.data());
            kept_capacities.push_back(piece.transformed_vertices.capacity());
        }
        transform_allocations = 0;
        watch_transform_allocations = true;
        const bool reused = into.reuse_transforms(from, turn);
        watch_transform_allocations = false;
        require(
            !reused && transform_allocations == 0 && same_state(into, before) &&
                into.transform_storage_bytes() == bytes,
            "a refused transform reuse changed state or allocated"
        );
        for (size_t index = 0; index < pointers.size(); ++index)
            require(
                into.pieces()[index].transformed_vertices.data() == pointers[index] &&
                    into.pieces()[index].transformed_vertices.capacity() == kept_capacities[index],
                "a refused transform reuse changed vector storage"
            );
    };
    Instance empty;
    reject_unchanged(empty, source, root);
    reject_unchanged(receiver, empty, root);
    reject_unchanged(receiver, receiver, root);
    auto unbuilt = oa::sim::model_runtime::make_instance(model, source_owner, names);
    copy_local_pose(unbuilt, source);
    reject_unchanged(receiver, unbuilt, root);
    auto dirty = source;
    Host dirty_host(dirty);
    dirty_host.set_piece_position(0, 0, 1);
    reject_unchanged(receiver, dirty, root);
    auto uncompleted_copy = source;
    uncompleted_copy.take_pose(source);
    reject_unchanged(receiver, uncompleted_copy, root);
    auto stale = source;
    stale.pieces()[0].rotation.xy = 0;
    require(!stale.transforms_dirty(), "direct pose mutation unexpectedly marked the source dirty");
    reject_unchanged(receiver, stale, root);
    auto wrong_turn = root;
    wrong_turn.xy = std::numeric_limits<int16_t>::min();
    reject_unchanged(receiver, source, wrong_turn);
    auto wrong_pose = receiver;
    wrong_pose.pieces()[1].translation.y = 0;
    reject_unchanged(wrong_pose, source, root);
    auto wrong_model = oa::sim::model_runtime::make_instance(
        std::make_shared<oa::formats::objects3d::Model>(*model), destination_owner, names
    );
    copy_local_pose(wrong_model, source);
    reject_unchanged(wrong_model, source, root);
    auto reordered = oa::sim::model_runtime::make_instance(model, destination_owner);
    copy_local_pose(reordered, source);
    reject_unchanged(reordered, source, root);
    auto wrong_links = receiver;
    wrong_links.pieces()[wrong_links.root_piece()].first_child = none;
    reject_unchanged(wrong_links, source, root);
    auto changed_topology = source;
    changed_topology.pieces()[changed_topology.root_piece()].first_child = none;
    reject_unchanged(wrong_links, changed_topology, root);
    auto short_source = source;
    short_source.pieces().back().transformed_vertices.pop_back();
    reject_unchanged(receiver, short_source, root);
    auto short_storage = receiver;
    std::vector<FixedVector3>{}.swap(short_storage.pieces().back().transformed_vertices);
    reject_unchanged(short_storage, source, root);

    // Piece flags and owner association do not enter transform arithmetic.
    // A completed immutable copy remains useful after its original slot is reused.
    const Instance completed = source;
    source = oa::sim::model_runtime::make_instance(model, 203, names);
    auto replacement = oa::sim::model_runtime::make_instance(model, 207, names);
    copy_local_pose(replacement, completed);
    replacement.pieces().front().flags = 0;
    require(
        replacement.reuse_transforms(completed, root) && replacement.owner_token() == 207 &&
            replacement.pieces().front().flags == 0,
        "a copied transform source retained another unit's owner or flags"
    );
    for (uint32_t step = 0; step < 128; ++step) {
        replacement.take_pose(completed);
        require(
            replacement.reuse_transforms(completed, root), "a matching taken pose missed reuse"
        );
        auto& changed = replacement.pieces()[step % replacement.pieces().size()];
        changed.translation.x =
            std::bit_cast<int32_t>(std::bit_cast<uint32_t>(changed.translation.x) + step + 1);
        auto turned = root;
        turned.xz = static_cast<int16_t>(turned.xz + static_cast<int16_t>(step));
        replacement.rebuild_transforms(turned);
        require(
            same_transforms(replacement, rebuilt_afresh(model, names, replacement, turned)),
            "a native rebuild after transform reuse differs from a full rebuild"
        );
    }

    auto measured = oa::sim::model_runtime::make_instance(model, destination_owner, names);
    const auto base_bytes = measured.transform_storage_bytes();
    auto& points = measured.pieces().front().transformed_vertices;
    const auto previous_capacity = points.capacity();
    points.reserve(previous_capacity + 11);
    require(
        measured.transform_storage_bytes() ==
            base_bytes + (points.capacity() - previous_capacity) * sizeof(FixedVector3),
        "transform storage did not count actual vertex capacity"
    );
    auto& measured_spare = measured.piece_for_script_index(100);
    const auto before_spare = measured.transform_storage_bytes();
    measured_spare.transformed_vertices.reserve(13);
    require(
        measured.transform_storage_bytes() ==
                before_spare +
                    measured_spare.transformed_vertices.capacity() * sizeof(FixedVector3) &&
            measured.transform_storage_bytes() >=
                measured.pieces().size() * sizeof(oa::sim::model_runtime::PieceState) &&
            empty.transform_storage_bytes() == 0,
        "transform storage omitted a piece allocation or counted the shared model"
    );
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
    static constexpr std::string_view names[] = {"tip", "barrel", "flare", "turret", "base"};
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
        Instance checked = instance;
        checked.rebuild_transforms(root, true);
        instance.rebuild_transforms(root);
        require(
            same_state(checked, instance),
            "checked integer rotations changed transforms or rebuild bookkeeping"
        );
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
        check_reused_transforms();
        check_root_prefix_transforms();
        check_zero_pair_rotation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
