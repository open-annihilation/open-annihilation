// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/sqsh.hpp"
#include <array>
#include <cstdint>
#include <cstring>

namespace oa::formats::sqsh {
namespace {

// A match token's two input bytes produce at most 17 output bytes and a
// literal's one byte produces one, so no stream yields more than 9 bytes for
// each input byte.
constexpr std::size_t max_output_per_input_byte =
    (lz77::token_length_mask + lz77::minimum_match_bytes + 1) / 2;
/// Tokens one flag byte describes, each a literal while its bit is clear.
constexpr std::size_t literals_per_flag_byte = 8;

} // namespace

base::bytes::Decoded<std::vector<uint8_t>> decode_lz77(
    std::span<const uint8_t> input,
    std::size_t output_limit,
    std::span<const uint8_t> initial_dictionary
) {
    using base::bytes::DecodeCode;
    using base::bytes::DecodeError;
    const std::size_t most = input.size() > output_limit / max_output_per_input_byte
                                 ? output_limit
                                 : input.size() * max_output_per_input_byte;
    std::vector<uint8_t> output(most);
    const Lz77Decoded decoded = decode_lz77_into(input, output, initial_dictionary);
    switch (decoded.status) {
    case Lz77Status::ok:
        break;
    case Lz77Status::truncated:
        return DecodeError{DecodeCode::truncated, decoded.consumed, "truncated LZ77 stream"};
    case Lz77Status::output_full:
        return DecodeError{
            DecodeCode::limit_exceeded, decoded.consumed, "LZ77 output limit exceeded"
        };
    case Lz77Status::bad_dictionary:
        return DecodeError{DecodeCode::out_of_range, 0, "LZ77 dictionary must contain 4096 bytes"};
    }
    output.resize(decoded.written);
    return output;
}

Lz77Decoded decode_lz77_into(
    std::span<const uint8_t> input,
    std::span<uint8_t> output,
    std::span<const uint8_t> initial_dictionary
) noexcept {
    std::array<uint8_t, lz77::dictionary_bytes> window{};
    if (!initial_dictionary.empty()) {
        if (initial_dictionary.size() != window.size())
            return {0, Lz77Status::bad_dictionary, 0};
        std::memcpy(window.data(), initial_dictionary.data(), window.size());
    }
    // Plain pointers keep the per-byte loop free of calls in unoptimised builds.
    uint8_t* const dictionary = window.data();
    const uint8_t* const in = input.data();
    const std::size_t in_size = input.size();
    uint8_t* const out = output.data();
    const std::size_t out_size = output.size();
    std::size_t cursor = 0;
    std::size_t written = 0;
    std::size_t write = lz77::initial_write_offset;
    for (;;) {
        if (cursor == in_size)
            return {written, Lz77Status::truncated, cursor};
        const uint8_t flags = in[cursor++];
        // Eight literals with room in the output, written without wrapping
        // around the window, are copied as one run.
        if (flags == 0 && in_size - cursor >= literals_per_flag_byte &&
            out_size - written >= literals_per_flag_byte &&
            write + literals_per_flag_byte <= lz77::dictionary_bytes) {
            std::memcpy(out + written, in + cursor, literals_per_flag_byte);
            std::memcpy(dictionary + write, in + cursor, literals_per_flag_byte);
            cursor += literals_per_flag_byte;
            written += literals_per_flag_byte;
            write = (write + literals_per_flag_byte) & lz77::dictionary_mask;
            continue;
        }
        for (unsigned mask = 1; mask <= lz77::last_flag_bit; mask <<= 1) {
            if ((flags & mask) == 0) {
                if (cursor == in_size)
                    return {written, Lz77Status::truncated, cursor};
                if (written == out_size)
                    return {written, Lz77Status::output_full, cursor};
                const uint8_t value = in[cursor++];
                out[written++] = value;
                dictionary[write] = value;
                write = (write + 1) & lz77::dictionary_mask;
                continue;
            }
            if (in_size - cursor < 2)
                return {written, Lz77Status::truncated, cursor};
            const auto token =
                static_cast<uint16_t>(in[cursor] | (static_cast<unsigned>(in[cursor + 1]) << 8));
            cursor += 2;
            const std::size_t offset = token >> lz77::token_length_bits;
            if (offset == lz77::terminator_offset)
                return {written, Lz77Status::ok, cursor};
            const std::size_t count = (token & lz77::token_length_mask) + lz77::minimum_match_bytes;
            // A match with room in the output, read and written without
            // wrapping around the window, is copied as one run.
            if (out_size - written >= count && offset + count <= lz77::dictionary_bytes &&
                write + count <= lz77::dictionary_bytes) {
                uint8_t* const run = out + written;
                if (offset < write && write < offset + count) {
                    // The match reaches the bytes it writes: past the first
                    // `write - offset`, each repeats the byte that far back.
                    const std::size_t distance = write - offset;
                    std::memcpy(run, dictionary + offset, distance);
                    for (std::size_t i = distance; i < count; ++i)
                        run[i] = run[i - distance];
                } else {
                    std::memcpy(run, dictionary + offset, count);
                }
                std::memcpy(dictionary + write, run, count);
                written += count;
                write = (write + count) & lz77::dictionary_mask;
                continue;
            }
            // Read after each preceding write: overlapping copies are intentional.
            for (std::size_t i = 0; i < count; ++i) {
                if (written == out_size)
                    return {written, Lz77Status::output_full, cursor - 2};
                const uint8_t value = dictionary[(offset + i) & lz77::dictionary_mask];
                out[written++] = value;
                dictionary[write] = value;
                write = (write + 1) & lz77::dictionary_mask;
            }
        }
    }
}

void decrypt_chunk(std::span<uint8_t> bytes) noexcept {
    uint8_t* const data = bytes.data();
    const std::size_t size = bytes.size();
    for (std::size_t i = 0; i < size; ++i) {
        const auto index = static_cast<uint8_t>(i);
        data[i] = static_cast<uint8_t>(static_cast<uint8_t>(data[i] - index) ^ index);
    }
}

void encrypt_chunk(std::span<uint8_t> bytes) noexcept {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto index = static_cast<uint8_t>(i);
        bytes[i] = static_cast<uint8_t>((bytes[i] ^ index) + index);
    }
}

uint32_t chunk_checksum(std::span<const uint8_t> bytes) noexcept {
    const uint8_t* const data = bytes.data();
    const std::size_t size = bytes.size();
    uint32_t sum = 0;
    for (std::size_t i = 0; i < size; ++i)
        sum += data[i];
    return sum;
}
} // namespace oa::formats::sqsh
