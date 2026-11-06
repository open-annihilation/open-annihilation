// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The overlay fields of launch_game, from the original client's vectors and
// from the record the session test already carries.
#include "oa/boneyards/overlay_launch.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

using oa::boneyards::OverlayLaunch;
using oa::boneyards::parse_overlay_launch;

bool key_is(const OverlayLaunch& launch, const uint8_t* bytes, std::size_t size) {
    if (launch.key_len != size)
        return false;
    return std::memcmp(launch.key, bytes, size) == 0;
}

bool ip_is(const uint8_t ip[4], uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ip[0] == a && ip[1] == b && ip[2] == c && ip[3] == d;
}

constexpr std::string_view kLaunch =
    "{[Rover<S:command=launch_game><U:area_id=1000><S:game_id=g42>"
    "<S:overlay_ip=10.66.7.10><S:key=0011223344556677><U:key_id=5><U:key_expiry=1893456000>"
    "<S:relay=203.0.113.7:3478><I:creator=1><I:result=0>]}";

constexpr std::string_view kRealHost =
    "{[Rover<S:command=launch_game><U:area_id=1000>"
    "<S:game_id=g42><S:address=10.66.4.104><S:password=abcd><S:session=g42><I:creator=1>"
    "<I:lockOptions=0><I:result=0><S:mission=Comet Catcher><I:playerlimit=4><I:deathopt=2>"
    "<I:energy=1000><I:metal=1000><I:location=2><I:los=1><I:mapping=2><I:maxunits=0>"
    "<I:watching=1><I:cheats=1><I:tournament=0><S:allegiance=A>"
    "<S:relay=relay.coreprime.net%63488><S:overlay_ip=10.66.4.104>"
    "<S:overlay_key=0011223344556677><I:key_id=7><I:key_expiry=1893456000>]}";

constexpr std::string_view kJoiner =
    "{[Rover<S:command=launch_game><U:area_id=1000>"
    "<S:game_id=g9><S:address=10.66.4.245><S:session=g9><I:creator=0><I:result=0>"
    "<S:relay=relay.coreprime.net%63488><S:overlay_ip=10.66.4.248>"
    "<S:overlay_key=0011223344556677><I:key_id=7>]}";

// The session test's launch_game, which names a relay and an overlay address
// and no key. The relay text is the one that record carries.
constexpr std::string_view kSessionRecord =
    "{[Rover<S:command=launch_game><S:game_id=9bfaa0d0f7b93f12><S:address=10.66.5.217>"
    "<S:password=YFCPDVPKETZYKXA1><S:session=9bfaa0d0f7b93f12><I:creator=1><I:lockOptions=1>"
    "<I:result=0><S:mission=Acid Pools><I:playerlimit=2><I:deathopt=2><I:energy=1000>"
    "<I:metal=1000><I:location=1><I:los=0><I:mapping=2><I:maxunits=250><I:watching=0>"
    "<I:cheats=0><I:tournament=1><S:allegiance=A><I:team=0>"
    "<S:relay=virginia.relay.coreprime.net%663479><S:overlay_ip=10.66.5.217><I:key_id=1>]}";

constexpr uint8_t kKeyBytes[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

void parses_overlay_launch_fields() {
    const std::optional<OverlayLaunch> launch = parse_overlay_launch(kLaunch);
    CHECK(launch.has_value());
    if (!launch.has_value())
        return;
    CHECK(ip_is(launch->overlay_ip, 10, 66, 7, 10));
    CHECK(key_is(*launch, kKeyBytes, sizeof kKeyBytes));
    CHECK(launch->key_id == 5);
    CHECK(launch->key_expiry == 1893456000u);
    CHECK(launch->relay_host == "203.0.113.7");
    CHECK(launch->relay_port == 3478);
    CHECK(launch->creator);
}

void parses_real_full_launch_with_overlay_key_tag() {
    const std::optional<OverlayLaunch> launch = parse_overlay_launch(kRealHost);
    CHECK(launch.has_value());
    if (!launch.has_value())
        return;
    CHECK(ip_is(launch->overlay_ip, 10, 66, 4, 104));
    CHECK(key_is(*launch, kKeyBytes, sizeof kKeyBytes));
    CHECK(launch->key_id == 7);
    CHECK(launch->key_expiry == 1893456000u);
    CHECK(launch->relay_host == "relay.coreprime.net");
    CHECK(launch->relay_port == 3488);
    CHECK(launch->creator);
    CHECK(launch->address_present);
    CHECK(ip_is(launch->address, 10, 66, 4, 104));
    uint8_t peer[4]{};
    CHECK(!launch->static_peer(peer));
}

void joiner_static_peer_is_the_launch_address() {
    const std::optional<OverlayLaunch> launch = parse_overlay_launch(kJoiner);
    CHECK(launch.has_value());
    if (!launch.has_value())
        return;
    CHECK(!launch->creator);
    CHECK(ip_is(launch->overlay_ip, 10, 66, 4, 248));
    CHECK(launch->address_present);
    CHECK(ip_is(launch->address, 10, 66, 4, 245));
    uint8_t peer[4]{};
    CHECK(launch->static_peer(peer));
    CHECK(ip_is(peer, 10, 66, 4, 245));
    CHECK(!ip_is(peer, 10, 66, 4, 249));
}

void address_absent_self_or_public_gives_no_peer() {
    constexpr std::string_view no_address =
        "{[Rover<S:command=launch_game><S:overlay_ip=10.66.7.11><S:key=00><U:key_id=1>"
        "<S:relay=1.2.3.4:3478><I:result=0>]}";
    const std::optional<OverlayLaunch> missing = parse_overlay_launch(no_address);
    CHECK(missing.has_value() && !missing->address_present && !missing->creator);
    uint8_t peer[4]{};
    CHECK(missing.has_value() && !missing->static_peer(peer));

    constexpr std::string_view self_address =
        "{[Rover<S:command=launch_game><S:address=10.66.7.11><S:overlay_ip=10.66.7.11><S:key=00>"
        "<U:key_id=1><S:relay=1.2.3.4:3478><I:creator=0><I:result=0>]}";
    const std::optional<OverlayLaunch> same = parse_overlay_launch(self_address);
    CHECK(same.has_value() && !same->static_peer(peer));

    constexpr std::string_view public_address =
        "{[Rover<S:command=launch_game><S:address=203.0.113.7><S:overlay_ip=10.66.7.11><S:key=00>"
        "<U:key_id=1><S:relay=1.2.3.4:3478><I:creator=0><I:result=0>]}";
    const std::optional<OverlayLaunch> routable = parse_overlay_launch(public_address);
    CHECK(routable.has_value() && !routable->address_present && !routable->static_peer(peer));
}

void bundled_launch_is_found() {
    constexpr std::string_view bundled =
        "{[Rover<S:command=battle_info><U:area_id=709>]}"
        "{[Rover<S:command=launch_game><S:overlay_ip=10.66.34.11><S:key=aabb><U:key_id=9>"
        "<S:relay=relay.coreprime.net%63488><I:result=0>]}";
    const std::optional<OverlayLaunch> launch = parse_overlay_launch(bundled);
    CHECK(launch.has_value());
    if (!launch.has_value())
        return;
    CHECK(ip_is(launch->overlay_ip, 10, 66, 34, 11));
    const uint8_t key[] = {0xaa, 0xbb};
    CHECK(key_is(*launch, key, sizeof key));
    CHECK(launch->key_id == 9);
    CHECK(launch->relay_host == "relay.coreprime.net");
    CHECK(launch->relay_port == 3488);
}

void rejected_or_incomplete_records_do_not_arm() {
    constexpr std::string_view rejected =
        "{[Rover<S:command=launch_game><S:overlay_ip=10.66.7.10><S:key=00><U:key_id=1>"
        "<S:relay=1.2.3.4:3478><I:result=1>]}";
    CHECK(!parse_overlay_launch(rejected).has_value());

    constexpr std::string_view absent =
        "{[Rover<S:command=launch_game><S:address=203.0.113.7:2300><I:result=0>]}";
    CHECK(!parse_overlay_launch(absent).has_value());

    constexpr std::string_view other_net =
        "{[Rover<S:command=launch_game><S:overlay_ip=192.168.1.5><S:key=00><U:key_id=1>"
        "<S:relay=1.2.3.4:3478><I:result=0>]}";
    CHECK(!parse_overlay_launch(other_net).has_value());

    constexpr std::string_view no_key =
        "{[Rover<S:command=launch_game><S:overlay_ip=10.66.7.10><U:key_id=1>"
        "<S:relay=1.2.3.4:3478><I:result=0>]}";
    CHECK(!parse_overlay_launch(no_key).has_value());

    constexpr std::string_view no_relay = "{[Rover<S:command=launch_game><S:overlay_ip=10.66.7.10><"
                                          "S:key=00><U:key_id=1><I:result=0>]}";
    CHECK(!parse_overlay_launch(no_relay).has_value());

    CHECK(!parse_overlay_launch("abc").has_value());
    CHECK(!parse_overlay_launch(
               "{[Rover<S:command=launch_game><S:key=zz><U:key_id=1>"
               "<S:overlay_ip=10.66.1.1><S:relay=1.2.3.4:1><I:result=0>]}"
    )
               .has_value());
    CHECK(!parse_overlay_launch(
               "{[Rover<S:command=launch_game><S:key=abc><U:key_id=1>"
               "<S:overlay_ip=10.66.1.1><S:relay=1.2.3.4:1><I:result=0>]}"
    )
               .has_value());
}

void session_record_relay_decodes_and_needs_a_key() {
    CHECK(!parse_overlay_launch(kSessionRecord).has_value());
    std::string with_key(kSessionRecord);
    const std::string needle = "<I:key_id=1>";
    const std::size_t at = with_key.find(needle);
    CHECK(at != std::string::npos);
    if (at == std::string::npos)
        return;
    with_key.insert(at, "<S:overlay_key=00ff>");
    const std::optional<OverlayLaunch> launch = parse_overlay_launch(with_key);
    CHECK(launch.has_value());
    if (!launch.has_value())
        return;
    CHECK(launch->relay_host == "virginia.relay.coreprime.net");
    CHECK(launch->relay_port == 63479);
    CHECK(ip_is(launch->overlay_ip, 10, 66, 5, 217));
    CHECK(launch->key_id == 1);
    CHECK(launch->creator);
    const uint8_t key[] = {0x00, 0xff};
    CHECK(key_is(*launch, key, sizeof key));
    // A relay that has already been decoded is the same endpoint.
    std::string decoded = with_key;
    const std::string escaped = "virginia.relay.coreprime.net%663479";
    const std::size_t relay_at = decoded.find(escaped);
    CHECK(relay_at != std::string::npos);
    if (relay_at != std::string::npos)
        decoded.replace(relay_at, escaped.size(), "virginia.relay.coreprime.net:63479");
    const std::optional<OverlayLaunch> plain = parse_overlay_launch(decoded);
    CHECK(plain.has_value());
    if (plain.has_value()) {
        CHECK(plain->relay_host == "virginia.relay.coreprime.net");
        CHECK(plain->relay_port == 63479);
    }
}

} // namespace

int main() {
    parses_overlay_launch_fields();
    parses_real_full_launch_with_overlay_key_tag();
    joiner_static_peer_is_the_launch_address();
    address_absent_self_or_public_gives_no_peer();
    bundled_launch_is_found();
    rejected_or_incomplete_records_do_not_arm();
    session_record_relay_decodes_and_needs_a_key();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    return 0;
}
