// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The saved server list, ./boneyards/serv_sav.byd. Each line holds five
// tab-separated tokens, and an optional sixth:
//
//   name <TAB> domain <TAB> cluster size <TAB> location <TAB> flags [<TAB> region]
//
// A line with no region tab is an older list: that server's region is empty.
// The region is kept beside the entry. The entry the game reads stays 0x21b.
//
// The game lists servers by location and passes it to slot 7. The yard
// master of a server is reached at name1.domain (name.domain when the
// cluster size is 0, and name1 alone when the domain starts with a space).
// Flags holding 'D' mark the selected server, 'P' one kept when a yard
// master sends its own list, which replaces the others and is saved back.
// When neither file holds a server, the master server list baked in at
// build time is used. That list is the file OA_BONEYARDS_MASTER_SERVERS
// names, in this same form. The default file lists Core Prime's Alpha.
#pragma once

#include "state.h"

// Bytes of one server's region, terminator included. "US East" fits.
inline constexpr int32_t kRoverServerRegionCapacity = 32;

inline constexpr char kRoverSavedServerFile[] = "./boneyards/serv_sav.byd";
inline constexpr char kRoverServerFile[] = "./boneyards/server.byd";

/// Reads a tab-separated server file into the list.
///
/// Reading stops at the first line of fewer than four tokens; flags 'D' and 'P' mark a server
/// selected and kept.
///
/// @param[in,out] list list the servers are appended to
/// @param path server file, or null
/// @return false when the file cannot be opened or holds no server
bool rover_servers_read(RoverServerList& list, const char* path);

/// Appends a server to the list.
///
/// Names and domains keep 254 characters, locations 22. A list that cannot grow drops the server.
///
/// @param[in,out] list list to append to
/// @param name the server's host name stem
/// @param domain the server's domain; one starting with a space names none
/// @param cluster_size number of cluster members; 0 means the bare name.domain host
/// @param location location text the game lists the server by
/// @param persistent true to keep the server when a yard master sends its own list
/// @param selected true to mark the server selected
void rover_servers_add(
    RoverServerList& list,
    const char* name,
    const char* domain,
    int32_t cluster_size,
    const char* location,
    bool persistent,
    bool selected
);

/// Appends the master server list the build baked in, used when no file holds one.
///
/// The list is the server file the build was given, in the same form as server.byd. The default
/// file lists Core Prime's Alpha, selected: alpha on clusters.coreprime.net with two shards, so the
/// yard master is alpha1.clusters.coreprime.net.
///
/// @param[in,out] list list to append to
void rover_servers_add_default(RoverServerList& list);

/// Selects every server at location and deselects the rest.
///
/// @param[in,out] list list to change
/// @param location location to select
/// @return true when no server matched
bool rover_servers_select(RoverServerList& list, const char* location);

/// Removes, from the back, the servers not marked persistent.
///
/// @param[in,out] list list to change
void rover_servers_remove_unkept(RoverServerList& list);

/// Writes every server as a line of the file, flags 'P' then 'D', with CR LF line ends.
///
/// @param list list to write
/// @param path file to create, or null
/// @return true when the file could not be written, false once it was
bool rover_servers_write(const RoverServerList& list, const char* path);

/// Writes the host of one cluster member of the server at location.
///
/// The host is name<index>.domain, name.domain for a cluster size of 0, and the bare name (with the
/// index when not 0) for a domain starting with a space. Every server scanned loses its selection;
/// the last match wins.
///
/// @param[in,out] list list to search; selections are cleared
/// @param[out] out buffer for the host name
/// @param capacity size of out in bytes
/// @param location location of the server
/// @param index cluster member, 1 for the yard master; 0 or a cluster size of 0 gives the bare
///     member
/// @return true when no server matched or the index is past the cluster
bool rover_servers_host_name(
    RoverServerList& list, char* out, int32_t capacity, const char* location, int32_t index
);

/// Finds the last server listed at location.
///
/// @param list list to search
/// @param location location to match exactly
/// @return the server, or null when none is at location
const RoverServerEntry*
rover_servers_find_last_at(const RoverServerList& list, const char* location);

/// Frees the list's servers and empties it.
///
/// @param[in,out] list list to clear
void rover_servers_clear(RoverServerList& list);

/// Notes the region the next rover_servers_add stores, including an empty one.
///
/// A null region stores empty. The note is taken by that add, so a later add
/// that was not noted stores no region. One note is enough: the yard master's
/// server_list callback adds the server before it returns.
///
/// @param region region text, or null
void rover_note_server_region(const char* region);

/// Drops a region note the add did not take.
void rover_clear_server_region_note();

/// The region of the last server listed at location.
///
/// @param location location to match exactly
/// @return the region, or null when no server is there or its region is empty
const char* rover_server_region(const char* location);
