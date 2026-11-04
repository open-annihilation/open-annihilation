// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The launch_game record text, kept while its callback runs. The callback's
// argument list stays as it is; the record's other fields are read from here.

#include <stdint.h>

/// Remembers the launch_game record the callback is about to receive.
///
/// The text is not copied. It must stay valid until rover_clear_launch_wire.
/// A null text clears the memory.
///
/// @param text the record text, which need not be terminated
/// @param length the bytes of text
void rover_set_launch_wire(const char* text, int32_t length);

/// Forgets the record rover_set_launch_wire remembered.
void rover_clear_launch_wire(void);

/// Returns the record text rover_set_launch_wire remembered.
///
/// @param[out] length receives the byte count; may be null
/// @return the text, or null when none is remembered
const char* rover_launch_wire(int32_t* length);
