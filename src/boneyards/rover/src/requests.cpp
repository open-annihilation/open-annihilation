// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Requests the client sends.

#include "requests.h"

#include "protocol.h"
#include "wire_text.h"

#include <string.h>

namespace {

constexpr const char* kServiceNames[] = {
    "Dispatcher",
    "CommManager",
    "Administrator",
    "Rover",
    "ChatManager",
    "UserManager",
    "GameManager",
    "YardMonitor",
    "YardMaster",
    "Guarddog",
    "WarManager",
    "ExtensionDll",
    "Retriever",
    "GuarddogProxy",
    "Crusades",
    "HouseManager",
};
constexpr int32_t kServiceCount =
    static_cast<int32_t>(sizeof kServiceNames / sizeof kServiceNames[0]);

constexpr const char* kConnectRoles[] = {"client", "peer"};
constexpr const char* kUnknownConnectRole = "unknown";
constexpr const char* kCompressions[] = {"text", "compressed", "encrypted"};
constexpr const char* kLoginKinds[] = {"registered", "anonymous"};

constexpr char kConnectTypeCommand[] = "connect_type";
constexpr char kConnectTypeVersion[] = "0.01";
constexpr char kKeyVersion[] = "version";
constexpr char kKeyRequestedType[] = "requested_type";
constexpr char kKeyCompressionType[] = "compression_type";
constexpr char kLoginCommand[] = "login";
constexpr char kKeyUsername[] = "username";
// Each part of the login digest keeps at most this many bytes.
constexpr int32_t kLoginDigestPartBytes = 15;
constexpr char kKeyPassword[] = "password";
constexpr char kReloginCommand[] = "relogin";
constexpr char kLogoutCommand[] = "logout";
constexpr char kGetServersCommand[] = "get_servers";
constexpr char kLatencyReplyCommand[] = "latency_r";
constexpr char kKeySeconds[] = "sec";
constexpr char kKeyMicroseconds[] = "usec";
constexpr char kKeySequence[] = "seq";
constexpr char kYardMasterConnectCommand[] = "connect";

// Bits of the latency reply's seq that keep the random value; the flag bits
// go in the others.
constexpr uint32_t kLatencySequenceRandomMask = 0xfe6bdeb7u;

// State of the random sequence, one per thread as in 3.1c: the linear
// congruential generator x = x * 214013 + 2531011, read as bits 16-30.
thread_local uint32_t t_rand_state = 1;
constexpr uint32_t kRandMultiplier = 214013u;
constexpr uint32_t kRandIncrement = 2531011u;

// Spreads the eight latency flags over the bits the random value leaves free,
// inverting flags 2, 3, 4 and 6 (part of the latency reply builder).
uint32_t scatter_latency_flags(uint32_t flags) {
    uint32_t inverted = ~flags;
    uint32_t bits = (inverted & 0x40u) | (flags & 0x80u);
    bits = (bits << 2) | (flags & 0x20u);
    bits = (bits << 1) | (inverted & 0x10u);
    bits = (bits << 4) | (inverted & 0x08u);
    bits = (bits << 4) | (inverted & 0x04u);
    bits = (bits << 1) | (flags & 0x02u);
    bits = (bits << 2) | (flags & 0x01u);
    return bits << 3;
}

} // namespace

const char* rover_service_name(RoverService service) {
    int32_t index = static_cast<int32_t>(service);
    if (index < 0 || index >= kServiceCount)
        return nullptr;
    return kServiceNames[index];
}

RoverEntry* rover_add_request(RoverRecord** record, RoverService service, const char* command) {
    if (!*record)
        *record = rover_record_create();
    if (!*record)
        return nullptr;
    RoverEntry* entry = rover_record_add_entry(**record, true, rover_service_name(service));
    if (entry)
        rover_entry_set_string(*entry, kRoverKeyCommand, command);
    return entry;
}

RoverRecord* rover_request_connect_type(
    RoverConnectRole role, RoverCompression compression, RoverRecord* record
) {
    const char* role_name = role == RoverConnectRole::Unknown
                                ? kUnknownConnectRole
                                : kConnectRoles[static_cast<int32_t>(role)];
    RoverEntry* entry = rover_add_request(&record, RoverService::CommManager, kConnectTypeCommand);
    if (entry) {
        rover_entry_set_string(*entry, kKeyVersion, kConnectTypeVersion);
        rover_entry_set_string(*entry, kKeyRequestedType, role_name);
        rover_entry_set_string(
            *entry, kKeyCompressionType, kCompressions[static_cast<int32_t>(compression)]
        );
    }
    return record;
}

void rover_login_digest_username(char* out, int32_t capacity, const char* username) {
    if (!out || capacity <= 0)
        return;
    out[0] = '\0';
    char stored[kRoverInterfaceTextBytes];
    const char* text = rover_take_text(stored, kRoverInterfaceTextBytes, username ? username : "");
    if (!text)
        text = "";
    char wire[kLoginDigestPartBytes + 1];
    rover_encode_wire_text(
        wire,
        static_cast<int32_t>(sizeof wire),
        text,
        static_cast<int32_t>(strlen(text)),
        kLoginDigestPartBytes,
        nullptr
    );
    int32_t length = static_cast<int32_t>(strlen(wire));
    if (length >= capacity)
        length = capacity - 1;
    for (int32_t index = 0; index < length; ++index) {
        char byte = wire[index];
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
        out[index] = byte;
    }
    out[length] = '\0';
}

RoverRecord* rover_request_login(
    RoverLoginKind kind, const char* username, const char* digest, RoverRecord* record
) {
    RoverEntry* entry = rover_add_request(&record, RoverService::UserManager, kLoginCommand);
    if (entry) {
        rover_entry_set_string(*entry, kRoverKeyType, kLoginKinds[static_cast<int32_t>(kind)]);
        char name_in[kRoverInterfaceTextBytes];
        rover_entry_set_string(
            *entry, kKeyUsername, rover_take_text(name_in, kRoverInterfaceTextBytes, username)
        );
        if (kind != RoverLoginKind::Anonymous && digest[0] != '\0')
            rover_entry_set_opaque_string(*entry, kKeyPassword, digest);
    }
    return record;
}

RoverRecord* rover_request_relogin(uint32_t user_id, uint32_t magic, RoverRecord* record) {
    RoverEntry* entry = rover_add_request(&record, RoverService::UserManager, kReloginCommand);
    if (entry) {
        rover_entry_set_uint(*entry, kRoverKeyUserId, user_id);
        rover_entry_set_uint(*entry, kRoverKeyMagic, magic);
    }
    return record;
}

RoverRecord* rover_request_logout(RoverRecord* record) {
    rover_add_request(&record, RoverService::UserManager, kLogoutCommand);
    return record;
}

RoverRecord* rover_request_get_servers(RoverRecord* record) {
    rover_add_request(&record, RoverService::YardMaster, kGetServersCommand);
    return record;
}

RoverRecord* rover_request_latency_reply(
    uint32_t seconds, uint32_t microseconds, uint32_t flags, RoverRecord* record
) {
    uint32_t random = static_cast<uint32_t>(rover_rand());
    random = (random << 8) + static_cast<uint32_t>(rover_rand());
    random = (random << 8) + static_cast<uint32_t>(rover_rand());
    RoverEntry* entry = rover_add_request(&record, RoverService::CommManager, kLatencyReplyCommand);
    if (entry) {
        rover_entry_set_uint(*entry, kKeySeconds, seconds);
        rover_entry_set_uint(*entry, kKeyMicroseconds, microseconds);
        rover_entry_set_uint(
            *entry,
            kKeySequence,
            (random & kLatencySequenceRandomMask) | scatter_latency_flags(flags)
        );
    }
    return record;
}

RoverRecord* rover_request_yard_master_connect() {
    RoverRecord* record = rover_record_create();
    if (!record)
        return nullptr;
    RoverEntry* entry =
        rover_record_add_entry(*record, false, rover_service_name(RoverService::YardMaster));
    rover_entry_set_string(*entry, kRoverKeyCommand, kYardMasterConnectCommand);
    return record;
}

int32_t rover_rand() {
    t_rand_state = t_rand_state * kRandMultiplier + kRandIncrement;
    return static_cast<int32_t>((t_rand_state >> 16) & 0x7fffu);
}
