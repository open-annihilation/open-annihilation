// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Windows-1252 and UTF-8 at the lobby wire. The flag is mocked: a later
// extension of the service's API can set it, and no message read from the
// wire does.

#include "wire_text.h"

#include <string.h>

namespace {

// The server accepts UTF-8. Clear until a caller sets it.
bool g_wire_utf8 = false;

// The byte written for a character Windows-1252 cannot hold, and for bytes that are not UTF-8.
constexpr char kMissing = '?';
constexpr char32_t kLastCode = 0x10FFFF;
constexpr char32_t kFirstSurrogate = 0xD800;
constexpr char32_t kLastSurrogate = 0xDFFF;

// Windows-1252 at 0x80-0x9F. The five bytes the code page leaves undefined stand for the control
// characters of the same value, so each byte still round-trips.
constexpr char32_t kPageMarks[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

struct Sequence {
    int32_t bytes{};
    char32_t code{};
    bool ok{};
};

/// Reads one UTF-8 character at the start of text.
///
/// ASCII is one byte. A longer sequence is kept only when it is not overlong, not a surrogate and
/// not past U+10FFFF. Anything else is one byte that is not a character.
///
/// @param text bytes to read
/// @param length bytes available
/// @return the character, or one byte with ok clear
Sequence read_utf8(const char* text, int32_t length) {
    Sequence sequence{};
    sequence.bytes = 1;
    if (length <= 0)
        return sequence;
    uint8_t lead = static_cast<uint8_t>(text[0]);
    if (lead < 0x80) {
        sequence.code = lead;
        sequence.ok = true;
        return sequence;
    }
    int32_t need = 0;
    char32_t code = 0;
    char32_t smallest = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        need = 2;
        code = lead & 0x1FU;
        smallest = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
        need = 3;
        code = lead & 0x0FU;
        smallest = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
        need = 4;
        code = lead & 0x07U;
        smallest = 0x10000;
    } else {
        return sequence;
    }
    if (length < need)
        return sequence;
    for (int32_t index = 1; index < need; ++index) {
        uint8_t byte = static_cast<uint8_t>(text[index]);
        if ((byte & 0xC0U) != 0x80U)
            return sequence;
        code = (code << 6) | (byte & 0x3FU);
    }
    if (code < smallest || code > kLastCode || (code >= kFirstSurrogate && code <= kLastSurrogate))
        return sequence;
    sequence.bytes = need;
    sequence.code = code;
    sequence.ok = true;
    return sequence;
}

/// Gives the character one Windows-1252 byte stands for.
///
/// @param byte the wire byte
/// @return the character
char32_t from_page(uint8_t byte) {
    if (byte >= 0x80 && byte < 0xA0)
        return kPageMarks[byte - 0x80];
    return byte;
}

/// Gives the Windows-1252 byte of a character.
///
/// @param code the character
/// @return the byte, or -1 when the code page has none
int32_t page_byte(char32_t code) {
    if (code < 0x80 || (code >= 0xA0 && code <= 0xFF))
        return static_cast<int32_t>(code);
    for (int32_t index = 0; index < 32; ++index) {
        if (kPageMarks[index] == code)
            return 0x80 + index;
    }
    return -1;
}

/// Appends one character as UTF-8.
///
/// @param[out] out buffer with room for the character
/// @param code a code point up to U+10FFFF, surrogates excluded
/// @return bytes written, or 0 for a code point that is not written
int32_t write_utf8(char* out, char32_t code) {
    uint32_t value = static_cast<uint32_t>(code);
    if (value < 0x80) {
        out[0] = static_cast<char>(value);
        return 1;
    }
    if (value < 0x800) {
        out[0] = static_cast<char>(0xC0U | (value >> 6));
        out[1] = static_cast<char>(0x80U | (value & 0x3FU));
        return 2;
    }
    if (value < 0x10000) {
        if (value >= kFirstSurrogate && value <= kLastSurrogate)
            return 0;
        out[0] = static_cast<char>(0xE0U | (value >> 12));
        out[1] = static_cast<char>(0x80U | ((value >> 6) & 0x3FU));
        out[2] = static_cast<char>(0x80U | (value & 0x3FU));
        return 3;
    }
    if (value <= kLastCode) {
        out[0] = static_cast<char>(0xF0U | (value >> 18));
        out[1] = static_cast<char>(0x80U | ((value >> 12) & 0x3FU));
        out[2] = static_cast<char>(0x80U | ((value >> 6) & 0x3FU));
        out[3] = static_cast<char>(0x80U | (value & 0x3FU));
        return 4;
    }
    return 0;
}

/// Wire bytes one source character takes: the UTF-8 sequence when as_utf8 is set, else one.
///
/// @param sequence the character, or one byte that is not UTF-8
/// @param as_utf8 true to keep a UTF-8 sequence, false for one code-page byte
/// @return the wire bytes
int32_t text_wire_bytes(const Sequence& sequence, bool as_utf8) {
    if (as_utf8)
        return sequence.ok ? sequence.bytes : 1;
    return 1;
}

/// Wire bytes one source character takes while the server-accepts-UTF-8 flag has its current value.
///
/// @param sequence the character, or one byte that is not UTF-8
/// @return the wire bytes
int32_t wire_bytes(const Sequence& sequence) {
    return text_wire_bytes(sequence, g_wire_utf8);
}

// The drop-in library's slot table speaks Windows-1252. Open Annihilation leaves this clear.
bool g_interface_windows1252 = false;

// Converted text handed back through the slot table. One call can present several strings at once.
constexpr int32_t kShowSlots = 16;
constexpr int32_t kShowBytes = 4096;

/// Writes text for the wire, as UTF-8 or as Windows-1252, according to as_utf8.
///
/// @param[out] out buffer of capacity bytes; always terminated when capacity is positive
/// @param capacity size of out in bytes
/// @param text source text; may be null when text_length is 0
/// @param text_length bytes of text
/// @param limit most bytes to write, or negative for none
/// @param[out] source_used bytes of text consumed; may be null
/// @param as_utf8 true to copy UTF-8, false to write Windows-1252
/// @return bytes written, not counting the terminator
int32_t encode_text(
    char* out,
    int32_t capacity,
    const char* text,
    int32_t text_length,
    int32_t limit,
    int32_t* source_used,
    bool as_utf8
) {
    if (source_used)
        *source_used = 0;
    if (!out || capacity <= 0)
        return 0;
    out[0] = '\0';
    if (!text || text_length <= 0)
        return 0;
    int32_t written = 0;
    int32_t at = 0;
    while (at < text_length) {
        Sequence sequence = read_utf8(text + at, text_length - at);
        int32_t consume = sequence.ok ? sequence.bytes : 1;
        int32_t take = text_wire_bytes(sequence, as_utf8);
        if (limit >= 0 && written + take > limit)
            break;
        if (written + take >= capacity)
            break;
        if (as_utf8) {
            memcpy(out + written, text + at, static_cast<size_t>(consume));
            written += consume;
        } else {
            char byte = kMissing;
            if (sequence.ok) {
                int32_t mapped = page_byte(sequence.code);
                if (mapped >= 0)
                    byte = static_cast<char>(mapped);
            }
            out[written++] = byte;
        }
        at += consume;
    }
    out[written] = '\0';
    if (source_used)
        *source_used = at;
    return written;
}

/// Reads wire text into UTF-8, as UTF-8 or as Windows-1252, according to as_utf8.
///
/// @param[out] out buffer of capacity bytes; always terminated when capacity is positive
/// @param capacity size of out in bytes
/// @param wire source bytes; may be null when wire_length is 0
/// @param wire_length bytes of wire
/// @param limit most bytes to write, or negative for none
/// @param as_utf8 true to keep well-formed UTF-8 and read other bytes as Windows-1252
/// @return bytes written, not counting the terminator
int32_t decode_text(
    char* out, int32_t capacity, const char* wire, int32_t wire_length, int32_t limit, bool as_utf8
) {
    if (!out || capacity <= 0)
        return 0;
    out[0] = '\0';
    if (!wire || wire_length <= 0)
        return 0;
    int32_t written = 0;
    int32_t at = 0;
    while (at < wire_length) {
        if (as_utf8) {
            Sequence sequence = read_utf8(wire + at, wire_length - at);
            if (sequence.ok) {
                if (limit >= 0 && written + sequence.bytes > limit)
                    break;
                if (written + sequence.bytes >= capacity)
                    break;
                memcpy(out + written, wire + at, static_cast<size_t>(sequence.bytes));
                written += sequence.bytes;
                at += sequence.bytes;
                continue;
            }
        }
        char utf8[4];
        int32_t bytes = write_utf8(utf8, from_page(static_cast<uint8_t>(wire[at])));
        if (bytes <= 0)
            break;
        if (limit >= 0 && written + bytes > limit)
            break;
        if (written + bytes >= capacity)
            break;
        memcpy(out + written, utf8, static_cast<size_t>(bytes));
        written += bytes;
        ++at;
    }
    out[written] = '\0';
    return written;
}

} // namespace

void rover_set_wire_utf8(bool accepts) {
    g_wire_utf8 = accepts;
}

bool rover_wire_utf8(void) {
    return g_wire_utf8;
}

void rover_set_interface_windows1252(bool windows1252) {
    g_interface_windows1252 = windows1252;
}

bool rover_interface_windows1252(void) {
    return g_interface_windows1252;
}

const char* rover_take_text(char* out, int32_t capacity, const char* text) {
    if (!text)
        return nullptr;
    if (!g_interface_windows1252)
        return text;
    if (!out || capacity <= 0)
        return text;
    decode_text(out, capacity, text, static_cast<int32_t>(strlen(text)), -1, false);
    return out;
}

const char* rover_show(const char* text) {
    if (!text || !g_interface_windows1252)
        return text;
    for (const char* at = text; *at != '\0'; ++at) {
        if (static_cast<uint8_t>(*at) < 0x80)
            continue;
        static int32_t slot = 0;
        static char ring[kShowSlots][kShowBytes];
        char* shown = ring[slot];
        slot = (slot + 1) % kShowSlots;
        encode_text(
            shown, kShowBytes, text, static_cast<int32_t>(strlen(text)), -1, nullptr, false
        );
        return shown;
    }
    return text;
}

int32_t rover_encode_wire_text(
    char* out,
    int32_t capacity,
    const char* text,
    int32_t text_length,
    int32_t limit,
    int32_t* source_used
) {
    return encode_text(out, capacity, text, text_length, limit, source_used, g_wire_utf8);
}

int32_t rover_decode_wire_text(
    char* out, int32_t capacity, const char* wire, int32_t wire_length, int32_t limit
) {
    return decode_text(out, capacity, wire, wire_length, limit, g_wire_utf8);
}

int32_t rover_keep_for_wire(
    char* out, int32_t capacity, const char* text, int32_t text_length, int32_t limit
) {
    if (!out || capacity <= 0)
        return 0;
    out[0] = '\0';
    if (!text || text_length <= 0)
        return 0;
    int32_t kept = 0;
    int32_t wire = 0;
    int32_t at = 0;
    while (at < text_length) {
        Sequence sequence = read_utf8(text + at, text_length - at);
        int32_t consume = sequence.ok ? sequence.bytes : 1;
        int32_t take = wire_bytes(sequence);
        if (limit >= 0 && wire + take > limit)
            break;
        if (kept + consume >= capacity)
            break;
        memcpy(out + kept, text + at, static_cast<size_t>(consume));
        kept += consume;
        wire += take;
        at += consume;
    }
    out[kept] = '\0';
    return kept;
}
