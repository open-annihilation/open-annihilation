// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The ad server the last ad_server record named. The console reads it when it
// fetches the banner list; the session may name one for a test instead.
#pragma once

#include <stddef.h>
#include <stdint.h>

/// Copies the ad server the session is using.
///
/// False when none is active, when send_id is not 1, or when the url does not
/// fit. A false result leaves the buffers unchanged.
///
/// @param[out] url buffer for the url; required
/// @param url_cap bytes available in url, including the terminator
/// @param[out] send_id receives the stored send_id; null skips it
/// @param[out] params buffer for the params; null skips them
/// @param params_cap bytes available in params, including the terminator
/// @return true when an ad server with send_id 1 and a url is stored
bool rover_ad_server_copy(
    char* url, size_t url_cap, int32_t* send_id, char* params, size_t params_cap
);
