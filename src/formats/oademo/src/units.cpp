// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oademo/oademo.hpp"

#include "oa/base/sha256.hpp"

#include <cstdint>
#include <vector>

namespace oa::formats::oademo {
namespace {

constexpr std::size_t record_bytes = 14;
constexpr uint8_t record_type = 0x1a;
constexpr uint8_t checksum_subtype = 2;
constexpr uint8_t verdict_subtype = 3;

uint32_t load_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

void store_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

unsigned bits_of_count_plus_one(uint32_t count) {
    unsigned bits = 0;
    for (uint32_t remaining = count + 1; remaining != 0; remaining >>= 1)
        ++bits;
    return bits;
}

struct Row {
    uint32_t key{};
    uint32_t checksum{};
    uint8_t local{};
    uint8_t remote{};
    bool saw_checksum{false};
    bool saw_verdict{false};
};

Row* find_row(std::vector<Row>& rows, uint32_t key) {
    for (Row& row : rows)
        if (row.key == key)
            return &row;
    return nullptr;
}

Row& upsert(std::vector<Row>& rows, uint32_t key) {
    if (Row* existing = find_row(rows, key))
        return *existing;
    rows.push_back(Row{});
    rows.back().key = key;
    return rows.back();
}

} // namespace

UnitTableDigest unit_table_digest(std::span<const uint8_t> unit_checks) {
    std::vector<Row> rows;
    bool any_verdict = false;
    for (std::size_t at = 0; at + record_bytes <= unit_checks.size(); at += record_bytes) {
        const uint8_t* record = unit_checks.data() + at;
        if (record[0] != record_type)
            continue;
        const uint8_t subtype = record[1];
        const uint32_t key = load_u32(record + 6);
        const uint32_t value = load_u32(record + 10);
        if (subtype == checksum_subtype) {
            Row& row = upsert(rows, key);
            row.checksum = value;
            row.saw_checksum = true;
        } else if (subtype == verdict_subtype) {
            Row& row = upsert(rows, key);
            row.local = static_cast<uint8_t>(value);
            row.remote = static_cast<uint8_t>(value >> 8);
            row.saw_verdict = true;
            any_verdict = true;
        }
    }

    std::vector<uint8_t> packed;
    uint32_t count = 0;
    for (const Row& row : rows) {
        const bool taken = any_verdict ? (row.local != 0 && row.remote != 0) : row.saw_checksum;
        if (!taken)
            continue;
        store_u32(packed, row.key);
        store_u32(packed, row.checksum);
        ++count;
    }

    const auto digest = oa::base::sha256::digest_of(packed);
    const auto hex = oa::base::sha256::to_hex(digest);
    UnitTableDigest result;
    result.sha256.assign(hex.begin(), hex.end());
    result.count = count;
    result.index_bits = bits_of_count_plus_one(count);
    return result;
}

} // namespace oa::formats::oademo
