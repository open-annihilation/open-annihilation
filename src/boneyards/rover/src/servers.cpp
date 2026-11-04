// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The saved server list, and slots 70-72, through which the game reads it.
// When neither file holds a server, the master server list baked in at
// build time is used.

#include "servers.h"

#include "slots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The region the next rover_servers_add stores. Pending means this add was
// told a region, which may be empty; an add that was not told stores none.
static bool g_region_pending = false;
static char g_region_note[kRoverServerRegionCapacity]{};

// The master server list baked into this build. The generated source defines both.
extern const uint8_t kRoverBakedMasterServers[];
extern const int32_t kRoverBakedMasterServersSize;

namespace {

// The file is read and written as text: CR LF reads as LF, a Ctrl-Z ends
// the file, and LF is written as CR LF.
constexpr char kEndOfTextFile = 0x1a;
// Longest line the reader takes at once, terminator included.
constexpr int32_t kLineCapacity = 0x400;
constexpr char kTokenSeparators[] = "\t";
constexpr char kSelectedFlag = 'D';
constexpr char kPersistentFlag = 'P';
// Line of the written file before its flags.
constexpr char kEntryFormat[] = "%s\t%s\t%d\t%s\t";

// A domain starting with this names no domain: hosts are the bare name.
constexpr char kNoDomainMark = ' ';

struct TextReader {
    RoverFile* file{};
    const uint8_t* memory{}; // set instead of file to read bytes already in hand
    int32_t memory_size{};
    int32_t memory_at{};
    char buffer[512]{};
    int32_t length{};
    int32_t at{};
    bool ended{}; // a read found no more bytes, as the stream's end-of-file flag
};

// Next raw byte, or -1 once the file is exhausted.
int32_t next_byte(TextReader& reader) {
    if (reader.memory) {
        if (reader.memory_at >= reader.memory_size) {
            reader.ended = true;
            return -1;
        }
        return reader.memory[reader.memory_at++];
    }
    if (reader.at == reader.length) {
        if (reader.ended)
            return -1;
        reader.length =
            rover_platform()->file_read(reader.file, reader.buffer, sizeof reader.buffer);
        reader.at = 0;
        if (reader.length <= 0) {
            reader.length = 0;
            reader.ended = true;
            return -1;
        }
    }
    return static_cast<uint8_t>(reader.buffer[reader.at++]);
}

// Next character in text mode, or -1 at the end.
int32_t next_char(TextReader& reader) {
    int32_t c = next_byte(reader);
    if (c == kEndOfTextFile) {
        reader.ended = true;
        reader.at = reader.length;
        return -1;
    }
    if (c != '\r')
        return c;
    int32_t following = next_byte(reader);
    if (following == '\n')
        return '\n';
    if (following >= 0)
        --reader.at;
    return '\r';
}

// fgets: up to capacity - 1 characters through the first newline; false when
// nothing was left to read.
bool read_line(TextReader& reader, char* line, int32_t capacity) {
    int32_t used = 0;
    while (used < capacity - 1) {
        int32_t c = next_char(reader);
        if (c < 0) {
            if (used == 0)
                return false;
            break;
        }
        line[used++] = static_cast<char>(c);
        if (c == '\n')
            break;
    }
    line[used] = '\0';
    return true;
}

// strtok with its own position.
char* next_token(char* text, char** position) {
    char* at = text ? text : *position;
    if (!at)
        return nullptr;
    at += strspn(at, kTokenSeparators);
    if (*at == '\0') {
        *position = at;
        return nullptr;
    }
    char* end = at + strcspn(at, kTokenSeparators);
    if (*end) {
        *end = '\0';
        *position = end + 1;
    } else {
        *position = end;
    }
    return at;
}

// Copy into a fixed field, cut to leave room for the terminator.
void copy_field(char* destination, int32_t size, const char* source) {
    memset(destination, 0, static_cast<size_t>(size));
    rover_copy_bounded(destination, source, size);
}

// The region stored beside entry i, or "" when the list has none.
char* region_at(RoverServerList& list, int32_t index) {
    return list.regions + static_cast<size_t>(index) * kRoverServerRegionCapacity;
}

const char* region_at(const RoverServerList& list, int32_t index) {
    return list.regions + static_cast<size_t>(index) * kRoverServerRegionCapacity;
}

// Drops a trailing CR or LF the line reader left on the last token.
void strip_line_end(char* text) {
    if (!text)
        return;
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        --length;
    }
}

bool write_text(RoverFile* file, const char* text) {
    const RoverPlatform* platform = rover_platform();
    for (const char* line = text; *line;) {
        const char* newline = strchr(line, '\n');
        int32_t length =
            static_cast<int32_t>(newline ? newline - line : static_cast<ptrdiff_t>(strlen(line)));
        if (length && platform->file_write(file, line, length) != length)
            return false;
        if (!newline)
            break;
        if (platform->file_write(file, "\r\n", 2) != 2)
            return false;
        line = newline + 1;
    }
    return true;
}

// Appends every server line. Reading stops at the first line of fewer than four tokens.
bool append_servers(RoverServerList& list, TextReader& reader) {
    bool found = false;
    char line[kLineCapacity];
    while (!reader.ended) {
        if (!read_line(reader, line, kLineCapacity))
            break;
        char* position = nullptr;
        char* name = next_token(line, &position);
        char* domain = next_token(nullptr, &position);
        char* cluster_size = next_token(nullptr, &position);
        char* location = next_token(nullptr, &position);
        if (!location)
            break;
        // The tail is "flags", "flags\tregion" or "\tregion". next_token would
        // skip an empty flags field and read the region as the flags.
        char* flags = position != nullptr ? position : const_cast<char*>("");
        char* region = const_cast<char*>("");
        char* tab = strchr(flags, '\t');
        if (tab != nullptr) {
            *tab = '\0';
            region = tab + 1;
        }
        strip_line_end(flags);
        strip_line_end(region);
        bool selected = strchr(flags, kSelectedFlag) != nullptr;
        bool persistent = strchr(flags, kPersistentFlag) != nullptr;
        rover_note_server_region(region);
        rover_servers_add(
            list, name, domain, rover_decimal_value(cluster_size), location, persistent, selected
        );
        found = true;
    }
    return found;
}

void append_baked_servers(RoverServerList& list) {
    TextReader reader{};
    reader.memory = kRoverBakedMasterServers;
    reader.memory_size = kRoverBakedMasterServersSize;
    append_servers(list, reader);
}

} // namespace

bool rover_servers_read(RoverServerList& list, const char* path) {
    if (!path)
        return false;
    TextReader reader{};
    reader.file = rover_platform()->file_open(path);
    if (!reader.file)
        return false;
    bool found = append_servers(list, reader);
    rover_platform()->file_close(reader.file);
    return found;
}

void rover_servers_add(
    RoverServerList& list,
    const char* name,
    const char* domain,
    int32_t cluster_size,
    const char* location,
    bool persistent,
    bool selected
) {
    // Take the note even when the list cannot grow, so the next add does not
    // inherit this server's region.
    char region[kRoverServerRegionCapacity]{};
    if (g_region_pending) {
        copy_field(region, kRoverServerRegionCapacity, g_region_note);
        rover_clear_server_region_note();
    }
    if (list.count == list.capacity) {
        int32_t capacity = list.capacity ? list.capacity * 2 : 4;
        void* regions =
            realloc(list.regions, static_cast<size_t>(capacity) * kRoverServerRegionCapacity);
        if (!regions)
            return;
        memset(
            static_cast<char*>(regions) +
                static_cast<size_t>(list.capacity) * kRoverServerRegionCapacity,
            0,
            static_cast<size_t>(capacity - list.capacity) * kRoverServerRegionCapacity
        );
        void* grown =
            realloc(list.entries, static_cast<size_t>(capacity) * sizeof(RoverServerEntry));
        if (!grown) {
            list.regions = static_cast<char*>(regions);
            return;
        }
        list.regions = static_cast<char*>(regions);
        list.entries = static_cast<RoverServerEntry*>(grown);
        list.capacity = capacity;
    }
    const int32_t index = list.count;
    RoverServerEntry& entry = list.entries[list.count++];
    copy_field(entry.name, sizeof entry.name, name);
    copy_field(entry.domain, sizeof entry.domain, domain);
    entry.cluster_size = cluster_size;
    copy_field(entry.location, sizeof entry.location, location);
    entry.persistent = persistent;
    entry.selected = selected;
    if (list.regions != nullptr)
        copy_field(region_at(list, index), kRoverServerRegionCapacity, region);
}

void rover_servers_add_default(RoverServerList& list) {
    append_baked_servers(list);
}

bool rover_servers_select(RoverServerList& list, const char* location) {
    bool none = true;
    for (int32_t i = 0; i < list.count; ++i) {
        RoverServerEntry& entry = list.entries[i];
        entry.selected = false;
        if (strcmp(entry.location, location) == 0) {
            entry.selected = true;
            none = false;
        }
    }
    return none;
}

void rover_servers_remove_unkept(RoverServerList& list) {
    for (int32_t i = list.count - 1; i >= 0; --i) {
        if (list.entries[i].persistent)
            continue;
        memmove(
            &list.entries[i],
            &list.entries[i + 1],
            static_cast<size_t>(list.count - i - 1) * sizeof(RoverServerEntry)
        );
        if (list.regions != nullptr) {
            memmove(
                region_at(list, i),
                region_at(list, i + 1),
                static_cast<size_t>(list.count - i - 1) * kRoverServerRegionCapacity
            );
        }
        --list.count;
    }
}

bool rover_servers_write(const RoverServerList& list, const char* path) {
    if (!path)
        return true;
    RoverFile* file = rover_platform()->file_create(path);
    if (!file)
        return true;
    char line[sizeof(RoverServerEntry) + 32 + kRoverServerRegionCapacity];
    for (int32_t i = 0; i < list.count; ++i) {
        const RoverServerEntry& entry = list.entries[i];
        int32_t length = snprintf(
            line,
            sizeof line,
            kEntryFormat,
            entry.name,
            entry.domain,
            entry.cluster_size,
            entry.location
        );
        if (length < 0 || static_cast<size_t>(length) >= sizeof line)
            length = 0;
        if (entry.persistent && static_cast<size_t>(length) + 1 < sizeof line)
            line[length++] = kPersistentFlag;
        if (entry.selected && static_cast<size_t>(length) + 1 < sizeof line)
            line[length++] = kSelectedFlag;
        const char* region = list.regions != nullptr ? region_at(list, i) : "";
        if (region[0] != '\0') {
            const int32_t extra =
                snprintf(line + length, sizeof line - static_cast<size_t>(length), "\t%s", region);
            if (extra > 0 && static_cast<size_t>(length) + static_cast<size_t>(extra) < sizeof line)
                length += extra;
        }
        if (static_cast<size_t>(length) + 1 < sizeof line)
            line[length++] = '\n';
        line[length] = '\0';
        write_text(file, line);
    }
    rover_platform()->file_close(file);
    return false;
}

bool rover_servers_host_name(
    RoverServerList& list, char* out, int32_t capacity, const char* location, int32_t index
) {
    bool none = true;
    for (int32_t i = 0; i < list.count; ++i) {
        RoverServerEntry& entry = list.entries[i];
        entry.selected = false;
        if (strcmp(entry.location, location) != 0)
            continue;
        if (entry.cluster_size == 0)
            index = 0;
        if (index > entry.cluster_size)
            return true;
        if (entry.domain[0] == kNoDomainMark) {
            if (index == 0)
                snprintf(out, static_cast<size_t>(capacity), "%s", entry.name);
            else
                snprintf(out, static_cast<size_t>(capacity), "%s%d", entry.name, index);
        } else if (index == 0) {
            snprintf(out, static_cast<size_t>(capacity), "%s.%s", entry.name, entry.domain);
        } else {
            snprintf(
                out, static_cast<size_t>(capacity), "%s%d.%s", entry.name, index, entry.domain
            );
        }
        none = false;
    }
    return none;
}

const RoverServerEntry*
rover_servers_find_last_at(const RoverServerList& list, const char* location) {
    const RoverServerEntry* found = nullptr;
    for (int32_t i = 0; i < list.count; ++i) {
        if (strcmp(list.entries[i].location, location) == 0)
            found = &list.entries[i];
    }
    return found;
}

void ROVER_CALL rover_enumerate_areas(RoverAreaEntryFn location_fn) {
    if (!location_fn || !g_rover_state)
        return;
    const RoverServerList& list = g_rover_state->servers;
    for (int32_t i = 0; i < list.count; ++i)
        location_fn(list.entries[i].location);
}

void ROVER_CALL rover_find_current_area(const char** out_location) {
    *out_location = nullptr;
    if (!g_rover_state)
        return;
    const RoverServerList& list = g_rover_state->servers;
    for (int32_t i = 0; i < list.count; ++i) {
        if (list.entries[i].selected) {
            *out_location = list.entries[i].location;
            return;
        }
    }
}

const char* ROVER_CALL rover_match_area_name(void) {
    if (!g_rover_state)
        return nullptr;
    const RoverServerEntry* entry =
        rover_servers_find_last_at(g_rover_state->servers, g_rover_state->resolved_server);
    return entry ? entry->name : nullptr;
}

void rover_servers_clear(RoverServerList& list) {
    free(list.regions);
    free(list.entries);
    list = RoverServerList{};
}

void rover_note_server_region(const char* region) {
    g_region_pending = true;
    copy_field(g_region_note, kRoverServerRegionCapacity, region != nullptr ? region : "");
}

void rover_clear_server_region_note() {
    g_region_pending = false;
    g_region_note[0] = '\0';
}

const char* rover_server_region(const char* location) {
    if (g_rover_state == nullptr || location == nullptr)
        return nullptr;
    const RoverServerList& list = g_rover_state->servers;
    const char* found = nullptr;
    for (int32_t i = 0; i < list.count; ++i) {
        if (strcmp(list.entries[i].location, location) != 0)
            continue;
        found = list.regions != nullptr ? region_at(list, i) : "";
    }
    if (found == nullptr || found[0] == '\0')
        return nullptr;
    return found;
}
