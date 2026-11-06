// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Names on the lobby wire shared by the link, the handlers and the requests.
#pragma once

#include <stdint.h>

// Entry name of records addressed to the client, including the link's own notices.
inline constexpr char kRoverClientService[] = "Rover";

// Field keys.
inline constexpr char kRoverKeyCommand[] = "command";
inline constexpr char kRoverKeyState[] = "state";
inline constexpr char kRoverKeyResult[] = "result";
inline constexpr char kRoverKeyYardNumber[] = "yard_number";
inline constexpr char kRoverKeyMessage[] = "message";
inline constexpr char kRoverKeyType[] = "type";
inline constexpr char kRoverKeyName[] = "name";
inline constexpr char kRoverKeyPort[] = "port";
inline constexpr char kRoverKeyUserId[] = "user_id";
inline constexpr char kRoverKeyAreaId[] = "area_id";
inline constexpr char kRoverKeyAddress[] = "ip";
inline constexpr char kRoverKeyMagic[] = "magic";

// The link's notices: a connect started ("open") or finished ("connect"), and a close.
inline constexpr char kRoverCommandConnectToYardMaster[] = "_connect_to_yardmaster";
inline constexpr char kRoverCommandConnectToBoneyard[] = "_connect_to_boneyard";
inline constexpr char kRoverCommandYardMasterClosed[] = "_yardmaster_closed";
inline constexpr char kRoverCommandBoneyardClosed[] = "_boneyard_closed";
inline constexpr char kRoverLinkStateOpen[] = "open";
inline constexpr char kRoverLinkStateConnect[] = "connect";

// Link results in the notices.
inline constexpr int32_t kRoverLinkResultFailed = 0;
inline constexpr int32_t kRoverLinkResultOk = 1;

// The yard master's fixed port.
inline constexpr uint16_t kRoverYardMasterPort = 9110;
