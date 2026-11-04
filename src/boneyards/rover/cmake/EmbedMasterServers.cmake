# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Writes the C++ source that holds the master server list, run as a script:
# cmake -DINPUT=<server list> -DOUTPUT=<source> -P EmbedMasterServers.cmake.
# INPUT is a file in server.byd's format. OUTPUT defines
# kRoverBakedMasterServers and kRoverBakedMasterServersSize, the file's
# bytes. OUTPUT is rewritten only when its text changes.
if(NOT INPUT OR NOT OUTPUT)
  message(FATAL_ERROR "EmbedMasterServers.cmake needs -DINPUT=<server list> -DOUTPUT=<source>")
endif()
if(NOT EXISTS "${INPUT}" OR IS_DIRECTORY "${INPUT}")
  message(FATAL_ERROR "OA_BONEYARDS_MASTER_SERVERS (${INPUT}) does not name a server list file")
endif()
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_digits)
math(EXPR byte_count "${hex_digits} / 2")
if(byte_count EQUAL 0)
  message(FATAL_ERROR "${INPUT} is empty")
endif()
string(REGEX MATCHALL "09" tabs "${hex}")
list(LENGTH tabs tab_count)
if(tab_count LESS 3)
  message(FATAL_ERROR "${INPUT} is not a server list: a line needs a name, domain, cluster size and location, separated by tabs")
endif()
# Sixteen bytes to a line; CMake's expressions have no counted repeat.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " bytes "${hex}")
string(REPEAT "0x[0-9a-f][0-9a-f], " 16 line)
string(REGEX REPLACE "(${line})" "\\1\n    " bytes "${bytes}")
string(REGEX REPLACE "[ \n]+$" "" bytes "${bytes}")
string(REGEX REPLACE ", \n" ",\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(CONFIGURE OUTPUT "${OUTPUT}" @ONLY CONTENT [[
// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The master server list baked into this build, the bytes of @name@.
// Edit that server list, not this file.

#include <stdint.h>

extern const uint8_t kRoverBakedMasterServers[] = {
    @bytes@
};
extern const int32_t kRoverBakedMasterServersSize = @byte_count@;
]])
