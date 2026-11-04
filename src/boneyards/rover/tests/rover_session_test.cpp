// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The session without a network: the server list file, the login digest's
// MD5, and the handlers the pump runs for records put on the inbound queue.

#include "../src/ad_server.h"
#include "../src/handlers.h"
#include "../src/md5.h"
#include "../src/requests.h"
#include "../src/servers.h"
#include "../src/state.h"
#include "../src/wire_text.h"
#include "oa/rover/interface.h"
#include "oa/rover/platform.h"
#include "oa/rover/strings.h"
#include "oa/rover/wire_text.h"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define make_directory(path) _mkdir(path)
#define change_directory(path) _chdir(path)
#define remove_directory(path) _rmdir(path)
#define process_id() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0755)
#define change_directory(path) chdir(path)
#define remove_directory(path) rmdir(path)
#define process_id() getpid()
#endif

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void write_file(const char* path, const char* text) {
    FILE* file = oa::platform::open_file(path, "wb");
    fputs(text, file);
    fclose(file);
}

bool file_equals(const char* path, const char* expected) {
    FILE* file = oa::platform::open_file(path, "rb");
    if (!file)
        return false;
    char content[4096] = {};
    size_t length = fread(content, 1, sizeof content - 1, file);
    fclose(file);
    return length == strlen(expected) && memcmp(content, expected, length) == 0;
}

// The saved server line of the CorePrime alpha back end.
constexpr char kAlphaLine[] = "alpha\tclusters.coreprime.net\t3\tCorePrime Alpha Test\tD";

void check_server_list() {
    RoverServerList list{};
    write_file("servers.byd", kAlphaLine);
    check(rover_servers_read(list, "servers.byd"), "the alpha line reads");
    check(list.count == 1, "one server");
    const RoverServerEntry& alpha = list.entries[0];
    check(strcmp(alpha.name, "alpha") == 0, "name token");
    check(strcmp(alpha.domain, "clusters.coreprime.net") == 0, "domain token");
    check(alpha.cluster_size == 3, "cluster size token");
    check(strcmp(alpha.location, "CorePrime Alpha Test") == 0, "location token");
    check(alpha.selected && !alpha.persistent, "D selects, no P");
    char host[0x100];
    check(
        !rover_servers_host_name(list, host, sizeof host, "CorePrime Alpha Test", 1) &&
            strcmp(host, "alpha1.clusters.coreprime.net") == 0,
        "the yard master is alpha1.clusters.coreprime.net"
    );
    check(
        !rover_servers_host_name(list, host, sizeof host, "CorePrime Alpha Test", 3) &&
            strcmp(host, "alpha3.clusters.coreprime.net") == 0,
        "member 3 of the cluster"
    );
    check(
        rover_servers_host_name(list, host, sizeof host, "CorePrime Alpha Test", 4),
        "past the cluster"
    );
    check(rover_servers_host_name(list, host, sizeof host, "Elsewhere", 1), "an unknown location");
    check(!alpha.selected, "naming a host clears the selection");
    rover_servers_clear(list);

    // A file written by a Windows editor: CR LF endings, a server without
    // flags, and a blank line that ends the list.
    write_file(
        "servers.byd",
        "alpha\tclusters.coreprime.net\t3\tCorePrime Alpha Test\tD\r\n"
        "bypacific\tboneyards.net\t2\tUSA, West Coast\t\r\n"
        "\r\n"
        "ignored\tafter.blank\t1\tNever Read\tP\r\n"
    );
    check(
        rover_servers_read(list, "servers.byd") && list.count == 2,
        "reading stops at the blank line"
    );
    check(
        !list.entries[1].selected && !list.entries[1].persistent,
        "a flags token of only the line end"
    );
    check(
        strcmp(list.entries[1].location, "USA, West Coast") == 0,
        "a location with spaces and a comma"
    );
    rover_servers_clear(list);

    write_file(
        "servers.byd",
        "bare\t \t2\tNo Domain\tP\n"
        "single\tone.example\t0\tSingle Host\n"
        "cut\tshort.example\t1\x1a\tafter the end of the file\n"
    );
    check(rover_servers_read(list, "servers.byd") && list.count == 2, "Ctrl-Z ends the file");
    check(list.entries[0].persistent && !list.entries[0].selected, "P keeps a server");
    check(
        strcmp(list.entries[1].location, "Single Host\n") == 0,
        "without a flags token the line end stays in the location"
    );
    check(
        !rover_servers_host_name(list, host, sizeof host, "No Domain", 1) &&
            strcmp(host, "bare1") == 0,
        "a domain starting with a space names the bare host"
    );
    check(
        !rover_servers_host_name(list, host, sizeof host, "Single Host\n", 1) &&
            strcmp(host, "single.one.example") == 0,
        "a cluster of size 0 is name.domain"
    );
    check(
        !rover_servers_select(list, "No Domain") && list.entries[0].selected, "select by location"
    );
    check(rover_servers_select(list, "Nowhere") && !list.entries[0].selected, "select nothing");

    rover_servers_add(list, "added", "x.example", 1, "Added", false, true);
    rover_servers_remove_unkept(list);
    check(
        list.count == 1 && strcmp(list.entries[0].name, "bare") == 0, "only persistent servers stay"
    );
    rover_servers_add(list, "added", "x.example", 1, "Added", true, true);
    check(!rover_servers_write(list, "written.byd"), "the list is written");
    check(
        file_equals(
            "written.byd", "bare\t \t2\tNo Domain\tP\r\nadded\tx.example\t1\tAdded\tPD\r\n"
        ),
        "written with P then D and CR LF endings"
    );
    check(
        rover_servers_write(list, "missing-directory/written.byd"),
        "an unwritable path reports failure"
    );
    rover_servers_clear(list);

    // Each dropped entry is erased where it stands: the later
    // 0x21b-byte entries move down one place and keep their order.
    rover_servers_add(list, "a", "a.example", 1, "A", true, false);
    rover_servers_add(list, "b", "b.example", 1, "B", false, false);
    rover_servers_add(list, "c", "c.example", 1, "C", false, true);
    rover_servers_add(list, "d", "d.example", 1, "D", true, true);
    rover_servers_add(list, "e", "e.example", 1, "E", false, false);
    rover_servers_remove_unkept(list);
    check(
        list.count == 2 && strcmp(list.entries[0].name, "a") == 0 &&
            strcmp(list.entries[1].name, "d") == 0 && list.entries[1].selected,
        "unkept servers between kept ones are erased in place"
    );
    rover_servers_clear(list);

    check(!rover_servers_read(list, "absent.byd"), "an absent file");
    write_file("servers.byd", "only\tthree\ttokens\n");
    check(
        !rover_servers_read(list, "servers.byd") && list.count == 0,
        "a line of three tokens ends the list"
    );
    rover_servers_add_default(list);
    check(
        list.count == 1 && strcmp(list.entries[0].name, "alpha") == 0 &&
            strcmp(list.entries[0].domain, "clusters.coreprime.net") == 0 &&
            list.entries[0].cluster_size == 2 &&
            strcmp(list.entries[0].location, "CorePrime (Alpha)") == 0 &&
            list.entries[0].selected && !list.entries[0].persistent,
        "the baked list is Core Prime's Alpha"
    );
    RoverServerList from_file{};
    check(
        rover_servers_read(from_file, OA_BAKED_MASTER_SERVERS_PATH) &&
            from_file.count == list.count &&
            strcmp(from_file.entries[0].name, list.entries[0].name) == 0 &&
            strcmp(from_file.entries[0].domain, list.entries[0].domain) == 0 &&
            from_file.entries[0].cluster_size == list.entries[0].cluster_size &&
            strcmp(from_file.entries[0].location, list.entries[0].location) == 0 &&
            from_file.entries[0].selected == list.entries[0].selected &&
            from_file.entries[0].persistent == list.entries[0].persistent,
        "the baked list is the server file the build was given"
    );
    rover_servers_clear(from_file);
    check(
        !rover_servers_host_name(list, host, sizeof host, "CorePrime (Alpha)", 1) &&
            strcmp(host, "alpha1.clusters.coreprime.net") == 0,
        "the baked yard master is alpha1.clusters.coreprime.net"
    );
    rover_servers_clear(list);
}

// serv_sav.byd, then server.byd, then the list the build baked in.
void check_master_server_precedence(const RoverInterface* rover) {
    remove("boneyards/serv_sav.byd");
    remove("boneyards/server.byd");
    check(rover->attach(), "attach with no server file");
    check(
        g_rover_state && g_rover_state->servers.count == 1 &&
            strcmp(g_rover_state->servers.entries[0].name, "alpha") == 0 &&
            strcmp(g_rover_state->servers.entries[0].domain, "clusters.coreprime.net") == 0 &&
            g_rover_state->servers.entries[0].cluster_size == 2 &&
            g_rover_state->servers.entries[0].selected,
        "with no server file the baked list is Alpha"
    );
    rover->detach();

    write_file("boneyards/server.byd", "shipped\texample.net\t1\tShipped Yard\tD\r\n");
    check(rover->attach(), "attach with server.byd");
    check(
        g_rover_state && g_rover_state->servers.count == 1 &&
            strcmp(g_rover_state->servers.entries[0].name, "shipped") == 0 &&
            strcmp(g_rover_state->servers.entries[0].location, "Shipped Yard") == 0,
        "server.byd comes before the baked list"
    );
    rover->detach();

    write_file("boneyards/serv_sav.byd", "saved\texample.net\t2\tSaved Yard\tD\r\n");
    check(rover->attach(), "attach with both server files");
    check(
        g_rover_state && g_rover_state->servers.count == 1 &&
            strcmp(g_rover_state->servers.entries[0].name, "saved") == 0 &&
            strcmp(g_rover_state->servers.entries[0].location, "Saved Yard") == 0,
        "serv_sav.byd comes before server.byd"
    );
    rover->detach();

    remove("boneyards/server.byd");
    check(rover->attach(), "attach with serv_sav.byd only");
    check(
        g_rover_state && g_rover_state->servers.count == 1 &&
            strcmp(g_rover_state->servers.entries[0].name, "saved") == 0,
        "serv_sav.byd alone is used"
    );
    rover->detach();
    remove("boneyards/serv_sav.byd");
}

// Prints the baked list and checks it against the file the build was given.
int show_baked_list() {
    RoverServerList baked{};
    rover_servers_add_default(baked);
    RoverServerList file{};
    if (!rover_servers_read(file, OA_BAKED_MASTER_SERVERS_PATH) || file.count != baked.count) {
        fprintf(stderr, "FAIL: the baked list does not match the server file\n");
        return 1;
    }
    for (int32_t i = 0; i < baked.count; ++i) {
        const RoverServerEntry& entry = baked.entries[i];
        const RoverServerEntry& expected = file.entries[i];
        printf("%s\t%s\t%d\t%s\n", entry.name, entry.domain, entry.cluster_size, entry.location);
        if (strcmp(entry.name, expected.name) != 0 || strcmp(entry.domain, expected.domain) != 0 ||
            entry.cluster_size != expected.cluster_size ||
            strcmp(entry.location, expected.location) != 0 || entry.selected != expected.selected ||
            entry.persistent != expected.persistent) {
            fprintf(stderr, "FAIL: baked entry %d differs from the server file\n", i);
            return 1;
        }
    }
    rover_servers_clear(baked);
    rover_servers_clear(file);
    puts("baked master servers match the server file");
    return 0;
}

void check_md5() {
    struct Vector {
        const char* text;
        const char* digest;
    };

    const Vector vectors[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (const Vector& vector : vectors) {
        RoverMd5 md5;
        rover_md5_init(md5);
        rover_md5_update(md5, vector.text, static_cast<uint32_t>(strlen(vector.text)));
        uint8_t digest[kRoverMd5DigestSize];
        rover_md5_final(md5, digest);
        char hex[33];
        for (int i = 0; i < kRoverMd5DigestSize; ++i)
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        check(strcmp(hex, vector.digest) == 0, vector.text);
    }
}

// ---- handlers, through the pump ---------------------------------------------

int32_t g_role = -1;
int32_t g_compression = -1;
char g_seed[32];
int g_connect_infos = 0;
int32_t g_yard_closed = 0;
int g_alerts = 0;
int32_t g_alert_type = -1;
int32_t g_alert_fields = -1;
char g_alert_reason[32];
char g_alert_command[32];
int g_logouts = 0;

void on_connect_info(int32_t role, int32_t compression, const char* seed) {
    g_role = role;
    g_compression = compression;
    snprintf(g_seed, sizeof g_seed, "%s", seed);
    ++g_connect_infos;
}

void on_yard_closed(int32_t yard_number) {
    g_yard_closed = yard_number;
}

// Copies the text of a bag's string entry, or "" when there is none.
void read_bag_text(RoverPropertyBag* bag, const char* name, char* out, size_t capacity) {
    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    RoverString key{};
    RoverString value{};
    rover_string_assign_text(&key, name, static_cast<uint32_t>(strlen(name)));
    RoverBagEntry* entry = rover->find_bag_entry(bag, &key);
    bool found = entry && rover->get_entry_string(entry, &value);
    snprintf(out, capacity, "%s", found ? rover_string_text(&value) : "");
    rover_string_release(&value);
    rover_string_release(&key);
}

// Counts the alert's bag of other fields and keeps its "reason" and
// "command" texts.
void ROVER_CALL on_alert(int32_t type, const char*, void* fields) {
    g_alert_type = type;
    ++g_alerts;
    g_alert_fields = -1;
    g_alert_reason[0] = '\0';
    g_alert_command[0] = '\0';
    if (!fields)
        return;
    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    RoverPropertyBag* bag = static_cast<RoverPropertyBag*>(fields);
    g_alert_fields = 0;
    for (RoverBagEntry* entry = rover->get_first_bag_entry(bag); entry;
         entry = rover->get_next_bag_entry(bag))
        ++g_alert_fields;
    read_bag_text(bag, "reason", g_alert_reason, sizeof g_alert_reason);
    read_bag_text(bag, "command", g_alert_command, sizeof g_alert_command);
}

void ROVER_CALL on_logged_out() {
    ++g_logouts;
}

// Queues one received record and pumps it.
void deliver(const RoverInterface* rover, const char* text) {
    RoverRecord* pending = nullptr;
    RoverRecord* record = nullptr;
    int32_t consumed = 0;
    RoverRecordStatus status = rover_record_receive(
        &pending, text, static_cast<int32_t>(strlen(text)), &consumed, &record
    );
    check(status == RoverRecordStatus::Ok, text);
    if (status != RoverRecordStatus::Ok) {
        rover_record_destroy(&pending);
        return;
    }
    rover_queue_push(g_rover_state->inbound, record);
    rover->pump_events();
}

// Formats the next record queued for a link.
bool next_sent(RoverLinkId link, char* out, int32_t capacity) {
    RoverRecord* record = rover_queue_pop(g_rover_state->outbound[static_cast<int32_t>(link)]);
    if (!record)
        return false;
    int32_t length = 0;
    bool formatted =
        rover_record_format(*record, out, capacity - 1, &length) == RoverRecordStatus::Ok;
    out[length] = '\0';
    rover_record_destroy(&record);
    return formatted;
}

void check_connect_info(const RoverInterface* rover) {
    RoverHandlerSet& handlers = g_rover_state->handlers;

    struct Case {
        const char* record;
        int calls;
        int32_t role;
        int32_t compression;
        const char* what;
    };

    const Case cases[] = {
        {"{[Rover<S:command=connect_info><S:seed=abc><S:type=client><S:compression=text>]}",
         1,
         0,
         0,
         "text"},
        {"{[Rover<S:command=connect_info><S:seed=abc><S:type=client><S:compression=compressed>]}",
         1,
         0,
         1,
         "compressed"},
        {"{[Rover<S:command=connect_info><S:seed=abc><S:type=server><S:compression=encrypted>]}",
         1,
         1,
         2,
         "encrypted from a server"},
        {"{[Rover<S:command=connect_info><S:seed=abc>]}", 1, 0, 2, "no type or compression"},
        {"{[Rover<S:command=connect_info><S:seed=abc><S:compression=none>]}",
         0,
         -1,
         -1,
         "compression none is ignored"},
        {"{[Rover<S:command=connect_info><S:seed=abc><S:type=peer>]}",
         0,
         -1,
         -1,
         "an unknown role"},
        {"{[Rover<S:command=connect_info><S:type=client>]}", 0, -1, -1, "no seed"},
    };
    for (const Case& c : cases) {
        g_connect_infos = 0;
        g_role = -1;
        g_compression = -1;
        handlers.connect_info = on_connect_info;
        rover_handler_arm(handlers, RoverHandlerId::ConnectInfo);
        deliver(rover, c.record);
        check(
            g_connect_infos == c.calls && g_role == c.role && g_compression == c.compression, c.what
        );
    }
    check(strcmp(g_seed, "abc") == 0, "seed passed on");
    rover_handler_disarm(handlers, RoverHandlerId::ConnectInfo);
    g_connect_infos = 0;
    deliver(rover, "{[Rover<S:command=connect_info><S:seed=abc>]}");
    check(g_connect_infos == 0, "a disarmed handler sees nothing");
}

void check_dispatch(const RoverInterface* rover) {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    handlers.connect_info = on_connect_info;
    rover_handler_arm(handlers, RoverHandlerId::ConnectInfo);
    g_connect_infos = 0;
    deliver(
        rover,
        "{[UserManager<S:command=connect_info><S:seed=x>][Rover<S:command=connect_info><S:seed=y>]"
        "[Rover<S:command=connect_info><S:seed=z>]}"
    );
    check(g_connect_infos == 2 && strcmp(g_seed, "z") == 0, "only Rover entries, each in turn");
    deliver(rover, "{[Rover<S:seed=w>]}");
    check(g_connect_infos == 2, "an entry without a command is dropped");
    rover_handler_arm(handlers, RoverHandlerId::ConnectInfo);
    rover_handler_disarm(handlers, RoverHandlerId::ConnectInfo);
    deliver(rover, "{[Rover<S:command=connect_info><S:seed=v>]}");
    check(g_connect_infos == 2, "arming twice leaves one entry for a single disarm to remove");

    handlers.yard_closed[0] = on_yard_closed;
    handlers.yard_closed[1] = on_yard_closed;
    rover_handler_arm(handlers, RoverHandlerId::Yard1Closed);
    rover_handler_arm(handlers, RoverHandlerId::Yard2Closed);
    deliver(rover, "{[Rover<S:command=_boneyard_closed><I:yard_number=2>]}");
    check(g_yard_closed == 2, "the yard 2 handler takes yard 2");
    g_yard_closed = 0;
    deliver(rover, "{[Rover<S:command=_boneyard_closed>]}");
    check(g_yard_closed == 0, "a close without a yard is taken and ignored");
    rover_handlers_disarm_all(handlers);

    handlers.system_alert = on_alert;
    rover_handler_arm(handlers, RoverHandlerId::SystemAlert);
    deliver(
        rover, "{[Rover<S:command=system_alert><I:type=3><S:message=hello><S:reason=promotion>]}"
    );
    check(
        g_alerts == 1 && g_alert_type == 3 && g_alert_fields == 2 &&
            strcmp(g_alert_reason, "promotion") == 0 &&
            strcmp(g_alert_command, "system_alert") == 0,
        "system_alert passes type and message, and its other fields, command included, in a bag"
    );
    deliver(rover, "{[Rover<S:command=system_alert><I:type=3>]}");
    check(g_alerts == 1, "a system_alert without a message is dropped");

    handlers.logout_confirmed = on_logged_out;
    rover_handler_arm(handlers, RoverHandlerId::LogoutConfirmed);
    deliver(rover, "{[Rover<S:command=logout_confirmed><I:x=1>]}");
    check(g_logouts == 1, "logout_confirmed calls the game");
    rover_handlers_disarm_all(handlers);
}

char g_file_host[64];
int32_t g_file_port = -1;
int32_t g_file_type = -1;

void on_file_server(const char* host, int32_t port, int32_t type_index) {
    snprintf(g_file_host, sizeof g_file_host, "%s", host);
    g_file_port = port;
    g_file_type = type_index;
}

// Where each latency flag lands in seq; flags 2, 3, 4 and 6 go inverted.
constexpr uint32_t kLatencyFlagBits[8] = {
    0x8, 0x40, 0x100, 0x2000, 0x40000, 0x100000, 0x800000, 0x1000000
};
constexpr uint32_t kInvertedLatencyFlags = 0x5c;

uint32_t latency_flags_in_seq(uint32_t flags) {
    uint32_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        if (((flags ^ kInvertedLatencyFlags) >> i) & 1u)
            bits |= kLatencyFlagBits[i];
    }
    return bits;
}

/// Checks the file-server and latency commands and their handlers.
///
/// @param rover the interface under test
void check_file_server_and_latency(const RoverInterface* rover) {
    RoverState& state = *g_rover_state;
    state.handlers.file_server = on_file_server;
    rover_handler_arm(state.handlers, RoverHandlerId::FileServer);
    deliver(
        rover, "{[Rover<S:command=file_server><S:name=files.example><S:type=ladder><I:port=8080>]}"
    );
    check(
        strcmp(g_file_host, "files.example") == 0 && g_file_port == 8080 && g_file_type == 1,
        "a ladder file server"
    );
    deliver(rover, "{[Rover<S:command=file_server><S:name=news.example><S:type=news>]}");
    check(
        strcmp(g_file_host, "news.example") == 0 && g_file_port == 80 && g_file_type == 0,
        "a news file server on port 80 by default"
    );
    g_file_type = -1;
    deliver(rover, "{[Rover<S:command=file_server><S:name=x.example><S:type=maps>]}");
    check(g_file_type == -1, "an unknown file server type is dropped");
    rover_handler_disarm(state.handlers, RoverHandlerId::FileServer);

    rover_handler_arm(state.handlers, RoverHandlerId::Latency);
    rover->set_latency_reply_flags(0xa5);
    deliver(rover, "{[Rover<S:command=latency><U:sec=12ab><U:usec=34>]}");
    char text[kRoverRecordTextLimit + 1];
    check(next_sent(RoverLinkId::Boneyard1, text, sizeof text), "latency answered on the boneyard");
    // The first three values from seed 1 (x = x * 214013 + 2531011, bits 16-30)
    // are 41, 18467 and 6334.
    uint32_t random = ((41u << 8) + 18467u) << 8;
    random += 6334u;
    uint32_t seq = (random & 0xfe6bdeb7u) | latency_flags_in_seq(0xa5);
    char expected[128];
    snprintf(
        expected,
        sizeof expected,
        "{[CommManager<S:command=latency_r><U:sec=12ab><U:seq=%x><U:usec=34>]}",
        seq
    );
    check(strcmp(text, expected) == 0, "latency_r echoes the times and spreads the flags into seq");
    check(state.latency_reply_flags == 0, "the flags are sent once");
    deliver(rover, "{[Rover<S:command=latency><U:other=1>]}");
    check(
        next_sent(RoverLinkId::Boneyard1, text, sizeof text) && strstr(text, "<U:sec=0>") &&
            strstr(text, "<U:usec=0>"),
        "a probe without times is answered with zeros"
    );
}

/// Checks ad_server: a complete record is kept, a record missing its url is ignored, and a
/// send_id other than 1 clears the host.
///
/// @param rover the interface under test
void check_ad_server(const RoverInterface* rover) {
    RoverState& state = *g_rover_state;
    rover_handler_arm(state.handlers, RoverHandlerId::AdServer);
    deliver(
        rover,
        "{[Rover<S:command=ad_server><S:url=http://ads.example/"
        "banner><I:send_id=1><S:params=id=1>]}"
    );
    char url[0x104];
    char params[0x104];
    int32_t send_id = 0;
    check(
        rover_ad_server_copy(url, sizeof url, &send_id, params, sizeof params) &&
            strcmp(url, "http://ads.example/banner") == 0 && send_id == 1 &&
            strcmp(params, "id=1") == 0,
        "an ad server with a url, send_id 1 and params"
    );
    deliver(rover, "{[Rover<S:command=ad_server><I:send_id=1>]}");
    check(
        rover_ad_server_copy(url, sizeof url, &send_id, params, sizeof params) &&
            strcmp(url, "http://ads.example/banner") == 0,
        "a record without a url leaves the previous server"
    );
    url[0] = 'Q';
    deliver(rover, "{[Rover<S:command=ad_server><S:url=http://other.example><I:send_id=2>]}");
    check(
        !rover_ad_server_copy(url, sizeof url, &send_id, params, sizeof params) && url[0] == 'Q',
        "send_id 2 clears the server and does not write the buffers"
    );
    deliver(rover, "{[Rover<S:command=ad_server><S:url=http://next.example><I:send_id=1>]}");
    check(
        rover_ad_server_copy(url, sizeof url, &send_id, params, sizeof params) &&
            strcmp(url, "http://next.example") == 0 && params[0] == '\0',
        "a later record replaces the server and omitted params are empty"
    );
    deliver(rover, "{[Rover<S:command=ad_server><S:url=http://next.example><I:send_id=2>]}");
    rover_handler_disarm(state.handlers, RoverHandlerId::AdServer);
}

void check_logout(const RoverInterface* rover) {
    check(rover->send_logout(0x2a, on_logged_out), "send_logout reports true");
    char text[kRoverRecordTextLimit + 1];
    check(
        next_sent(RoverLinkId::Boneyard1, text, sizeof text) &&
            strcmp(text, "{[UserManager<S:command=logout><U:user_id=2a>]}") == 0,
        "logout for the user"
    );
    check(
        rover_handler_is_armed(g_rover_state->handlers, RoverHandlerId::LogoutConfirmed),
        "logout_confirmed armed"
    );
}

int32_t g_stage = -1;

void ROVER_CALL on_progress(int32_t stage, int32_t, int32_t, int32_t, const char*) {
    g_stage = stage;
}

void check_start_login(const RoverInterface* rover) {
    RoverState& state = *g_rover_state;
    check(
        rover->start_login("Nowhere", "user", nullptr, on_progress, nullptr, nullptr) &&
            g_stage == 1,
        "a fresh login resolves the yard master first"
    );
    check(
        state.links[static_cast<int32_t>(RoverLinkId::YardMaster)].port == 9110 &&
            state.links[static_cast<int32_t>(RoverLinkId::YardMaster)].address[0] == '\0',
        "an unknown server leaves the yard master address empty"
    );
    check(state.password[0] == '\0', "no password");
    check(
        !rover->start_login("Nowhere", "user", "pw", on_progress, nullptr, nullptr),
        "one login at a time"
    );

    state.login_started = false;
    snprintf(state.resolved_server, sizeof state.resolved_server, "Reference");
    check(
        rover->start_login("Reference", "user", "pw", on_progress, nullptr, nullptr) &&
            g_stage == 3,
        "the resolved server asks its yard master for a boneyard"
    );
    char text[kRoverRecordTextLimit + 1];
    check(
        next_sent(RoverLinkId::YardMaster, text, sizeof text) &&
            strcmp(text, "{[YardMaster<S:command=connect>]}") == 0,
        "connect queued for the yard master"
    );
    check(rover_handler_is_armed(state.handlers, RoverHandlerId::ConnectTo), "connect_to armed");

    state.login_started = false;
    g_stage = -1;
    check(
        rover->start_login("Other", "user", "pw", on_progress, nullptr, nullptr) && g_stage == 0,
        "another server waits for the yard master to close"
    );
    check(
        rover_handler_is_armed(state.handlers, RoverHandlerId::YardMasterClosed),
        "yard master close armed"
    );
    state.login_started = false;
    check(
        rover->start_login("Other", nullptr, nullptr, on_progress, nullptr, nullptr) &&
            strcmp(state.username, "user") == 0,
        "a null user name leaves the stored one"
    );
    state.login_started = false;
    rover_handlers_disarm_all(state.handlers);
}

constexpr int kMaxSequenceReports = 16;

struct SequenceReport {
    int32_t stage;
    int32_t result;
    int32_t a;
    int32_t b;
    char message[32];
};

SequenceReport g_sequence[kMaxSequenceReports];
int g_sequence_count = 0;

void ROVER_CALL
on_sequence_progress(int32_t stage, int32_t result, int32_t a, int32_t b, const char* message) {
    if (g_sequence_count == kMaxSequenceReports)
        return;
    SequenceReport& report = g_sequence[g_sequence_count++];
    report = SequenceReport{stage, result, a, b, {}};
    snprintf(report.message, sizeof report.message, "%s", message ? message : "");
}

bool last_report_is(int32_t stage, int32_t result) {
    return g_sequence_count > 0 && g_sequence[g_sequence_count - 1].stage == stage &&
           g_sequence[g_sequence_count - 1].result == result;
}

// The login record for a password digest of MD5(text), each digest byte of 0
// sent as 1 and each reserved byte escaped.
void expected_login(char* out, size_t capacity, const char* username, const char* text) {
    constexpr char kReserved[] = "{}[]<>:=%";
    RoverMd5 md5;
    rover_md5_init(md5);
    rover_md5_update(md5, text, static_cast<uint32_t>(strlen(text)));
    uint8_t digest[kRoverMd5DigestSize];
    rover_md5_final(md5, digest);
    char escaped[kRoverMd5DigestSize * 2 + 1] = {};
    size_t used = 0;
    for (uint8_t byte : digest) {
        char c = static_cast<char>(byte ? byte : 1);
        const char* reserved = strchr(kReserved, c);
        if (reserved) {
            escaped[used++] = '%';
            escaped[used++] = static_cast<char>('0' + (reserved - kReserved));
        } else {
            escaped[used++] = c;
        }
    }
    snprintf(
        out,
        capacity,
        "{[UserManager<S:command=login><S:password=%s><S:type=registered><S:username=%s>]}",
        escaped,
        username
    );
}

// The steps after the yard master answered, fed the link notices and server
// records a session delivers: connect_to, the boneyard connect, connect_info
// and login_confirmed. The digest takes the name in lower case.
void check_login_sequence(const RoverInterface* rover) {
    RoverState& state = *g_rover_state;
    snprintf(state.resolved_server, sizeof state.resolved_server, "Reference");
    check(
        rover->start_login(
            "Reference", "MixedCase", "secret", on_sequence_progress, on_alert, nullptr
        ) && last_report_is(3, 0),
        "sequence: the resolved server reports stage 3"
    );
    char text[kRoverRecordTextLimit + 1];
    check(
        next_sent(RoverLinkId::YardMaster, text, sizeof text) &&
            strcmp(text, "{[YardMaster<S:command=connect>]}") == 0,
        "sequence: connect for the yard master"
    );

    deliver(
        rover, "{[Rover<S:command=connect_to><S:ip=10.1.2.3><I:port=9111><S:message=welcome>]}"
    );
    const RoverConnection& boneyard = state.links[static_cast<int32_t>(RoverLinkId::Boneyard1)];
    check(
        last_report_is(4, 0) && strcmp(g_sequence[g_sequence_count - 1].message, "welcome") == 0,
        "sequence: connect_to reports stage 4 with its message"
    );
    check(
        strcmp(boneyard.address, "10.1.2.3") == 0 && boneyard.port == 9111,
        "sequence: the boneyard address and port"
    );

    deliver(
        rover,
        "{[Rover<S:command=_connect_to_boneyard><S:state=open><I:result=1><I:yard_number=1>]}"
    );
    check(last_report_is(5, 0), "sequence: the boneyard connect started");
    deliver(
        rover,
        "{[Rover<S:command=_connect_to_boneyard><S:state=connect><I:result=1><I:yard_number=1>]}"
    );
    check(last_report_is(6, 0), "sequence: the boneyard connected");
    check(
        next_sent(RoverLinkId::Boneyard1, text, sizeof text) &&
            strcmp(
                text,
                "{[CommManager<S:command=connect_type><S:compression_type=text>"
                "<S:requested_type=client><S:version=0.01>]}"
            ) == 0,
        "sequence: connect_type asks for a text client link"
    );

    deliver(
        rover, "{[Rover<S:command=connect_info><S:seed=s33d><S:type=client><S:compression=text>]}"
    );
    check(last_report_is(7, 0), "sequence: login sent");
    char login[256];
    expected_login(login, sizeof login, "MixedCase", "secretmixedcases33d");
    check(
        next_sent(RoverLinkId::Boneyard1, text, sizeof text) && strcmp(text, login) == 0,
        "sequence: the digest of password, lower-case name and seed"
    );

    deliver(rover, "{[Rover<S:command=login_confirmed><I:result=0><U:user_id=2a><U:area_id=1>]}");
    const SequenceReport& confirmed = g_sequence[g_sequence_count - 1];
    check(
        last_report_is(8, 0) && confirmed.a == 0x2a && confirmed.b == 1 && state.user_id == 0x2a,
        "sequence: login_confirmed reports the user and area"
    );
    check(
        !rover_handler_is_armed(state.handlers, RoverHandlerId::LoginConfirmed),
        "sequence: login_confirmed is taken once"
    );
    state.login_started = false;
    state.resolved_server[0] = '\0';
    rover_handlers_disarm_all(state.handlers);
}

// One registered login through the notices a session delivers, returning the login record.
bool login_record_for(
    const RoverInterface* rover,
    const char* username,
    const char* password,
    char* out,
    int32_t capacity
) {
    RoverState& state = *g_rover_state;
    state.login_started = false;
    state.resolved_server[0] = '\0';
    rover_handlers_disarm_all(state.handlers);
    g_sequence_count = 0;
    snprintf(state.resolved_server, sizeof state.resolved_server, "Reference");
    if (!rover->start_login(
            "Reference", username, password, on_sequence_progress, on_alert, nullptr
        ))
        return false;
    char text[kRoverRecordTextLimit + 1];
    if (!next_sent(RoverLinkId::YardMaster, text, sizeof text))
        return false;
    deliver(
        rover, "{[Rover<S:command=connect_to><S:ip=10.1.2.3><I:port=9111><S:message=welcome>]}"
    );
    deliver(
        rover,
        "{[Rover<S:command=_connect_to_boneyard><S:state=open><I:result=1><I:yard_number=1>]}"
    );
    deliver(
        rover,
        "{[Rover<S:command=_connect_to_boneyard><S:state=connect><I:result=1><I:yard_number=1>]}"
    );
    if (!next_sent(RoverLinkId::Boneyard1, text, sizeof text))
        return false;
    deliver(
        rover, "{[Rover<S:command=connect_info><S:seed=s33d><S:type=client><S:compression=text>]}"
    );
    bool sent = next_sent(RoverLinkId::Boneyard1, out, capacity);
    state.login_started = false;
    state.resolved_server[0] = '\0';
    rover_handlers_disarm_all(state.handlers);
    return sent;
}

// "José" logged in from UTF-8 hashes the same Windows-1252 bytes as the drop-in
// interface, and the same bytes an independent digest covers.
void check_accented_login_digest(const RoverInterface* rover) {
    const char* password = "secret";
    const char* jose_utf8 = "Jos\xC3\xA9";
    const char* jose_page = "Jos\xE9";
    rover_set_wire_utf8(false);
    rover_set_interface_windows1252(false);

    char wire[8];
    int32_t wired =
        rover_encode_wire_text(wire, static_cast<int32_t>(sizeof wire), jose_utf8, 5, 15, nullptr);
    for (char* byte = wire; *byte; ++byte) {
        if (*byte >= 'A' && *byte <= 'Z')
            *byte = static_cast<char>(*byte - 'A' + 'a');
    }
    char preimage[64];
    snprintf(preimage, sizeof preimage, "%s%s%s", password, wire, "s33d");
    char expected[256];
    expected_login(expected, sizeof expected, jose_page, preimage);

    char sent[kRoverRecordTextLimit + 1];
    check(
        wired == 4 && static_cast<uint8_t>(wire[3]) == 0xE9 &&
            login_record_for(rover, jose_utf8, password, sent, sizeof sent) &&
            strcmp(sent, expected) == 0,
        "UTF-8 Jos\xC3\xA9 hashes the Windows-1252 username"
    );

    rover_set_interface_windows1252(true);
    char sent_page[kRoverRecordTextLimit + 1];
    check(
        login_record_for(rover, jose_page, password, sent_page, sizeof sent_page) &&
            strcmp(sent_page, expected) == 0,
        "Windows-1252 Jos\xE9 hashes the same username bytes"
    );
    rover_set_interface_windows1252(false);
    rover_set_wire_utf8(false);
}

// A host lookup that refuses, so the login never opens a socket.
bool lookup_refuses(const char* /*host*/, uint32_t* /*address*/) {
    return false;
}

void ROVER_CALL ignore_disconnect(int32_t /*which*/, char /*connecting*/, int32_t /*error*/) {
}

// The saved server's location text differs from the yard master's. The row with the same name and
// domain is the selected one, under the yard master's location, and the area name is that row's
// name. Rows that only share the old location are not selected.
void check_renamed_yard_master_list(const RoverInterface* rover) {
    RoverPlatform platform = *rover_native_platform();
    platform.ipv4_lookup = lookup_refuses;
    rover_use_platform(&platform);

    write_file("boneyards/serv_sav.byd", "alpha\tinvalid\t0\tLocal Name\tD\r\n");
    check(rover->attach(), "attach for a renamed yard master list");
    if (!g_rover_state) {
        rover_use_platform(nullptr);
        return;
    }
    check(
        rover->start_login("Local Name", "user", nullptr, on_progress, nullptr, ignore_disconnect),
        "login to the saved server"
    );
    deliver(rover, "{[Rover<S:command=_connect_to_yardmaster><S:state=connect><I:result=1>]}");
    char text[kRoverRecordTextLimit + 1];
    check(
        next_sent(RoverLinkId::YardMaster, text, sizeof text) &&
            strcmp(text, "{[YardMaster<S:command=get_servers>]}") == 0,
        "the yard master is asked for its list"
    );
    check(rover->match_area_name() == nullptr, "no area until the yard master's list arrives");

    deliver(
        rover,
        "{[Rover<S:command=server_list><S:cluster_name=alpha><S:domain=other.invalid>"
        "<I:cluster_size=1><S:name=Local Name><I:last=0>]}"
    );
    deliver(
        rover,
        "{[Rover<S:command=server_list><S:cluster_name=decoy><S:domain=invalid>"
        "<I:cluster_size=0><S:name=Local Name><I:last=0>]}"
    );
    deliver(
        rover,
        "{[Rover<S:command=server_list><S:cluster_name=alpha><S:domain=invalid>"
        "<I:cluster_size=0><S:name=Named By Server><I:last=1>]}"
    );
    const RoverServerEntry* entries = g_rover_state->servers.entries;
    check(
        g_rover_state->servers.count == 3 && !entries[0].selected && !entries[1].selected &&
            entries[2].selected && strcmp(entries[2].location, "Named By Server") == 0 &&
            strcmp(g_rover_state->resolved_server, "Named By Server") == 0,
        "the connected server is selected under the yard master's location"
    );
    const char* area = rover->match_area_name();
    check(area && strcmp(area, "alpha") == 0, "the area name is the connected server's name");
    check(
        file_equals(
            "boneyards/serv_sav.byd",
            "alpha\tother.invalid\t1\tLocal Name\t\r\n"
            "decoy\tinvalid\t0\tLocal Name\t\r\n"
            "alpha\tinvalid\t0\tNamed By Server\tD\r\n"
        ),
        "the saved list selects the yard master's row"
    );
    RoverServerList reread{};
    check(
        rover_servers_read(reread, "boneyards/serv_sav.byd") && reread.count == 3 &&
            !reread.entries[0].selected && reread.entries[2].selected &&
            strcmp(reread.entries[2].location, "Named By Server") == 0 &&
            strcmp(reread.entries[2].name, "alpha") == 0,
        "reading the saved list keeps that selection"
    );
    rover_servers_clear(reread);
    rover->detach();
    rover_use_platform(nullptr);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--baked") == 0)
        return show_baked_list();
    check_md5();

    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-session-test-%d",
        temporary && *temporary ? temporary : ".",
        static_cast<int>(process_id())
    );
    make_directory(directory);
    if (change_directory(directory) != 0) {
        fprintf(stderr, "cannot prepare %s\n", directory);
        return 1;
    }
    make_directory("boneyards");
    check_server_list();

    write_file("boneyards/serv_sav.byd", kAlphaLine);
    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    check(rover->attach(), "attach");
    if (g_rover_state) {
        check(
            g_rover_state->servers.count == 1 && g_rover_state->servers.entries[0].selected,
            "attach reads serv_sav.byd"
        );
        check_connect_info(rover);
        check_dispatch(rover);
        check_file_server_and_latency(rover);
        check_ad_server(rover);
        check_logout(rover);
        check_start_login(rover);
        check_login_sequence(rover);
        check_accented_login_digest(rover);
        rover->detach();
        check(g_rover_state == nullptr, "detach frees the session");
    }

    check_renamed_yard_master_list(rover);

    remove("boneyards/serv_sav.byd");
    check_master_server_precedence(rover);

    remove("servers.byd");
    remove("written.byd");
    remove_directory("boneyards");
    if (change_directory("..") == 0)
        remove_directory(directory);
    if (failures)
        return 1;
    puts("rover session: ok");
    return 0;
}
