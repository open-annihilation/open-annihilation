// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The bytes of a keyframe: the writer's sections and the reader that takes them back in order,
// and what a match writes of itself. A match that holds orders or a unit steered on this machine is
// refused (nothing in a playback holds either); one that holds neither comes back as it was.
#include "combat_fixture.hpp"

#include "oa/sim/match_runtime/keyframe.hpp"
#include "oa/sim/match_runtime/keyframe_padding.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace combat_fixture;
namespace keyframe = oa::sim::match_runtime::keyframe;

/// Takes the bytes of a writer, as a reader over them.
struct Image {
    std::vector<uint8_t> fixed;
    std::vector<uint8_t> varying;

    /// Moves a writer's two regions out.
    ///
    /// @param writer The writer whose sections are taken.
    explicit Image(keyframe::Writer& writer) {
        writer.finish();
        fixed.assign(writer.fixed().begin(), writer.fixed().end());
        varying.assign(writer.varying().begin(), writer.varying().end());
    }

    /// Starts a reader over the image.
    ///
    /// @return The reader.
    keyframe::Reader reader() const { return keyframe::Reader(fixed, varying); }
};

/// Sections come back in the order they went in, from the region they were written in.
void sections_read_back_in_order() {
    keyframe::Writer writer;
    writer.begin("table", true);
    const std::array<uint16_t, 3> table{7, 8, 9};
    writer.value(table);
    writer.begin("counts");
    writer.value(uint32_t{42});
    writer.list(std::span<const uint16_t>(table));
    writer.begin("more", true);
    writer.value(uint8_t{5});
    const Image image(writer);
    CHECK(image.fixed.size() == sizeof table + 1);
    // Three marks (2 + names + 1 + 1 + 4 bytes each) and the counts section.
    CHECK(image.varying.size() > sizeof(uint32_t) + sizeof(uint32_t) + sizeof table);

    auto in = image.reader();
    std::array<uint16_t, 3> back{};
    CHECK(in.begin("table", true));
    CHECK(in.value(&back) && back == table);
    uint32_t count = 0;
    std::vector<uint16_t> items;
    CHECK(in.begin("counts"));
    CHECK(in.value(&count) && count == 42);
    uint32_t listed = 0;
    CHECK(in.list_count(&listed, 10) && listed == 3);
    items.resize(listed);
    CHECK(in.bytes(items.data(), listed * sizeof(uint16_t)) && items[2] == 9);
    uint8_t last = 0;
    CHECK(in.begin("more", true));
    CHECK(in.value(&last) && last == 5);
    CHECK(in.finish());
    CHECK(in.ok() && in.error()[0] == '\0');
}

/// A section the test leaves out holds no bytes and is read as absent.
void an_omitted_section_is_absent() {
    struct Skip {
        static bool omit(void*, std::string_view section) { return section == "units"; }
    };

    keyframe::Writer writer(keyframe::Omission{nullptr, &Skip::omit});
    writer.begin("units", true);
    writer.value(uint32_t{1});
    writer.begin("rest");
    writer.value(uint32_t{2});
    CHECK(writer.omitted() == 1);
    const Image image(writer);
    CHECK(image.fixed.empty());

    auto in = image.reader();
    uint32_t value = 0;
    CHECK(!in.begin("units", true) && in.ok());
    CHECK(in.begin("rest") && in.value(&value) && value == 2);
    CHECK(in.finish());
}

/// Records written with their padding named are written with it zero, whatever it held, and their
/// fields as they were: two records with the same fields are the same bytes.
void records_are_written_without_their_padding() {
    struct Padded {
        uint8_t kind{};
        uint32_t count{};
        uint16_t slot{};
    };

    static_assert(sizeof(Padded) == 12);
    constexpr std::array<keyframe::Padding, 2> padding{{
        {offsetof(Padded, kind) + sizeof(Padded::kind),
         offsetof(Padded, count) - offsetof(Padded, kind) - sizeof(Padded::kind)},
        {offsetof(Padded, slot) + sizeof(Padded::slot),
         sizeof(Padded) - offsetof(Padded, slot) - sizeof(Padded::slot)},
    }};
    static_assert(
        sizeof(Padded::kind) + sizeof(Padded::count) + sizeof(Padded::slot) +
            keyframe::padding_bytes(padding) ==
        sizeof(Padded)
    );
    std::array<Padded, 2> held{};
    const std::array<uint8_t, 2> fills{0xab, 0x5c};
    for (std::size_t index = 0; index < held.size(); ++index) {
        std::memset(reinterpret_cast<uint8_t*>(&held[index]), fills[index], sizeof(Padded));
        held[index].kind = 3;
        held[index].count = 0x01020304;
        held[index].slot = 77;
    }
    keyframe::Writer writer;
    writer.begin("records");
    keyframe::write_records(
        &writer, std::span<const Padded>(held), std::span<const keyframe::Padding>(padding)
    );
    const Image image(writer);
    // The mark, then the two records.
    CHECK(image.varying.size() >= 2 * sizeof(Padded));
    const auto* first = image.varying.data() + image.varying.size() - 2 * sizeof(Padded);
    CHECK(std::memcmp(first, first + sizeof(Padded), sizeof(Padded)) == 0);
    Padded back{};
    std::memcpy(&back, first, sizeof back);
    CHECK(back.kind == 3 && back.count == 0x01020304 && back.slot == 77);
    for (const keyframe::Padding& gap : padding)
        for (std::size_t at = gap.offset; at < gap.offset + gap.size; ++at)
            CHECK(first[at] == 0);
}

/// A reader that does not find what it asks for stops, and says what it was reading.
void a_reader_refuses_what_does_not_fit() {
    keyframe::Writer writer;
    writer.begin("first");
    writer.value(uint32_t{1});
    writer.begin("second");
    writer.value(uint32_t{2});
    const Image image(writer);

    {
        auto in = image.reader();
        CHECK(!in.begin("second"));
        CHECK(!in.ok() && std::string(in.error()).find("second") != std::string::npos);
        // Every later read fails too.
        uint32_t value = 0;
        CHECK(!in.value(&value));
    }
    {
        // A section read short of its end.
        auto in = image.reader();
        CHECK(in.begin("first"));
        CHECK(!in.begin("second") && !in.ok());
    }
    {
        // A section read past its end.
        auto in = image.reader();
        CHECK(in.begin("first"));
        std::array<uint32_t, 2> two{};
        CHECK(!in.value(&two) && !in.ok());
    }
    {
        // A list longer than its table.
        keyframe::Writer listed;
        listed.begin("list");
        const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
        listed.list(std::span<const uint8_t>(bytes));
        const Image long_image(listed);
        auto in = long_image.reader();
        uint32_t count = 0;
        CHECK(in.begin("list"));
        CHECK(!in.list_count(&count, 3) && !in.ok());
    }
    {
        // Bytes left over once every section was read.
        auto in = image.reader();
        uint32_t value = 0;
        CHECK(in.begin("first") && in.value(&value));
        CHECK(in.begin("second") && in.value(&value));
        std::vector<uint8_t> more = image.varying;
        more.push_back(0);
        keyframe::Reader extra(image.fixed, more);
        CHECK(extra.begin("first") && extra.value(&value));
        CHECK(extra.begin("second") && extra.value(&value));
        CHECK(!extra.finish() && !extra.ok());
    }
}

/// A match with nothing in it writes its sections and takes them back, with the clocks and
/// the random streams where the keyframe left them.
void a_match_comes_back_as_it_was() {
    Fixture f;
    f.run(20);
    keyframe::Writer writer;
    CHECK(f.match->write_keyframe(&writer) == nullptr);
    const Image image(writer);
    const auto tick = f.match->state().game.tick;
    const auto random = f.match->random_state();
    const auto lcg = f.match->lcg_state();
    const auto census = f.match->keyframe_census();
    CHECK(census.live_units == 0 && census.orders == 0 && census.ground_runtimes == 0);

    f.run(40);
    CHECK(f.match->state().game.tick == tick + 40);
    auto in = image.reader();
    CHECK(f.match->read_keyframe(&in));
    CHECK(in.finish());
    CHECK(f.match->state().game.tick == tick);
    CHECK(f.match->random_state() == random && f.match->lcg_state() == lcg);

    // A second look at the restored match is the same bytes: nothing was read in half.
    keyframe::Writer again;
    CHECK(f.match->write_keyframe(&again) == nullptr);
    const Image second(again);
    CHECK(second.fixed == image.fixed);
    CHECK(second.varying == image.varying);
}

/// A match that holds orders, or a unit it steers itself, is refused, and says why.
void a_match_that_cannot_be_kept_is_refused() {
    Fixture f;
    auto& unit = f.spawn(0, 40, 40);
    f.run(2);
    keyframe::Writer steered;
    const char* steering = f.match->write_keyframe(&steered);
    CHECK(steering != nullptr && std::string(steering) == "a unit is steered on this machine");
    CHECK(f.match->keyframe_census().ground_runtimes == 1);
    CHECK(f.match->keyframe_census().live_units == 1);
    (void)f.match->issue_ground_move(unit.unit_index, {200 * 65536, 0, 200 * 65536}, false);
    keyframe::Writer ordered;
    const char* refusal = f.match->write_keyframe(&ordered);
    CHECK(refusal != nullptr);
    CHECK(
        std::string(refusal) == "the match owns orders" ||
        std::string(refusal) == "a unit has orders queued"
    );
    CHECK(f.match->keyframe_census().orders != 0);
}

} // namespace

int main() {
    try {
        sections_read_back_in_order();
        an_omitted_section_is_absent();
        records_are_written_without_their_padding();
        a_reader_refuses_what_does_not_fit();
        a_match_comes_back_as_it_was();
        a_match_that_cannot_be_kept_is_refused();
    } catch (const std::exception& error) {
        std::cerr << "keyframe test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "match keyframe tests passed\n";
    return 0;
}
