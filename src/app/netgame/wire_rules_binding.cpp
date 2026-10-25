// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "wire_rules_binding.hpp"

#include <algorithm>

namespace oa::app::netgame {

namespace {

namespace mp = oa::data::mod_profile;
namespace ng = oa::netgame;

constexpr int32_t base_version_major = 3;
constexpr int32_t base_version_minor = 1;

uint8_t byte_of(int32_t value) noexcept {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint16_t seconds_of(int32_t value) noexcept {
    return static_cast<uint16_t>(std::clamp(value, 0, 0xffff));
}

} // namespace

ng::WireRules wire_rules_of(const mp::ModProfile* profile) noexcept {
    ng::WireRules rules{};
    if (profile == nullptr)
        return rules;
    const auto& identity = profile->identity;
    rules.version_major = byte_of(identity.network_version[0]);
    rules.version_minor = byte_of(identity.network_version[1]);
    if (identity.network_version[0] != base_version_major ||
        identity.network_version[1] != base_version_minor) {
        rules.version_rule = ng::VersionRule::equal;
        rules.launch_version_bias = false;
    }
    rules.replay_version_major = byte_of(identity.replay_network_version[0]);
    rules.replay_version_minor = byte_of(identity.replay_network_version[1]);

    const auto& network = profile->network;
    switch (network.chat_extension_channel.framing) {
    case mp::NetworkChatExtensionChannelFraming::sub_id_dispatch:
        rules.private_channel = ng::PrivateChannel::sub_id_dispatch;
        break;
    case mp::NetworkChatExtensionChannelFraming::integrity_only:
        rules.private_channel = ng::PrivateChannel::integrity_only;
        break;
    case mp::NetworkChatExtensionChannelFraming::none:
        break;
    }
    switch (network.vercheck.revision) {
    case mp::NetworkVercheckRevision::single_challenge:
        rules.integrity_check = ng::IntegrityCheck::single_challenge;
        break;
    case mp::NetworkVercheckRevision::periodic_challenge:
        rules.integrity_check = ng::IntegrityCheck::periodic_challenge;
        break;
    case mp::NetworkVercheckRevision::off:
        break;
    }
    // The integrity check's messages ride on the private channel.
    if (rules.integrity_check != ng::IntegrityCheck::off &&
        rules.private_channel == ng::PrivateChannel::none)
        rules.integrity_check = ng::IntegrityCheck::off;
    if (network.vote_reject.enabled &&
        rules.private_channel == ng::PrivateChannel::sub_id_dispatch) {
        rules.vote_reject = true;
        rules.vote_seconds = seconds_of(network.vote_reject.vote_timeout_s);
        rules.timeout_vote_seconds = seconds_of(network.vote_reject.timeout_vote_s);
    }
    rules.lag_guard_ms = seconds_of(network.lag_guard.gap_ms);
    rules.commander_sync_tick = seconds_of(network.commander_start_sync.tick);
    rules.keep_remote_colour = network.preserve_remote_player_colour.enabled;
    rules.clear_session_password = network.session_desc_clear.enabled;
    rules.host_stays_watching = network.host_stays_as_watcher.enabled;
    rules.recorder_session_commands =
        network.recorder_session_commands.enabled && network.recorder_session_commands.available;

    const auto& recorder = profile->recorder;
    if (recorder.ta_demo_recorder.enabled) {
        rules.recorder_protocol = ng::recorder_protocol_current;
        rules.recorder_cheat_notices = recorder.ta_demo_recorder.cheat_notices;
    }
    switch (recorder.ten_player_replay.form) {
    case mp::RecorderTenPlayerReplayForm::watcher_view:
        rules.ten_player_replay = ng::TenPlayerReplay::watcher_view;
        break;
    case mp::RecorderTenPlayerReplayForm::allied_fake_player:
        rules.ten_player_replay = ng::TenPlayerReplay::allied_fake_player;
        break;
    case mp::RecorderTenPlayerReplayForm::off:
        break;
    }

    const auto& speed = profile->rules.console.game_speed_range;
    rules.speed_min = byte_of(speed.minimum);
    rules.speed_max = byte_of(std::max(speed.maximum, speed.minimum));
    rules.speed_lock = speed.enabled && speed.syncon;

    rules.weapon_types = profile->limits.weapon_types.types;
    const auto side_count = profile->limits.sides.count;
    rules.sides = byte_of(side_count > 255u ? 255 : static_cast<int32_t>(side_count));
    return rules;
}

} // namespace oa::app::netgame
