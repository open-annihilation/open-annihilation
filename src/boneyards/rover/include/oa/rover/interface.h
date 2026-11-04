// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* The Rover interface table: the only surface the game uses to reach the
 * Boneyards lobby client. The library's export fills a caller's pointer with
 * the address of an 82-entry function-pointer table; every entry is
 * __stdcall on 32-bit Windows and only the low byte of a bool result is
 * read.
 *
 * The header compiles as C11 and C++20. On 32-bit Windows the layouts below
 * are the 3.1c ABI, so the same source builds a drop-in Rover library; on
 * other targets they are the native equivalents used by oa-game. */
#ifndef OA_ROVER_INTERFACE_H
#define OA_ROVER_INTERFACE_H

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#if defined(_WIN32) && !defined(_WIN64)
#define ROVER_CALL __stdcall
#else
#define ROVER_CALL
#endif

#if UINTPTR_MAX == 0xffffffffu
#define OA_ROVER_ABI32 1
#else
#define OA_ROVER_ABI32 0
#endif

#ifdef __cplusplus
#define OA_ROVER_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
extern "C" {
#else
#define OA_ROVER_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define OA_ROVER_SLOT_COUNT 82

/* Slot 0 accepts only this pair; the game passes exactly these values. */
#define OA_ROVER_INTERFACE_VERSION 0xe
#define OA_ROVER_MINIMUM_BUILD_STAMP 0x1310a3f /* signed compare; reads as 19991103 */

/* Type tags of a property-bag entry (slot 55). */
#define OA_ROVER_ENTRY_NONE 0
#define OA_ROVER_ENTRY_INT 1
#define OA_ROVER_ENTRY_UINT 2
#define OA_ROVER_ENTRY_STRING 3

/* slot 40 writes this index when the requested server type is out of range. */
#define OA_ROVER_FILE_SERVER_INVALID (-1)

/* The reference-counted string object the game and Rover exchange. The byte
 * before data holds the share count: 0 means one
 * owner, 0xfe and above mark a buffer that must be copied, not shared, and a
 * release frees the buffer at 0 or 0xff. Slots that fill a caller's string
 * assign into it the way a substring assignment does: share
 * the source buffer when its count allows, otherwise grow from the filling
 * module's own heap and copy (oa/rover/strings.h). */
typedef struct RoverString {
    uint32_t allocator_slot; /* first word of the exchanged layout; never read by assign */
    char* data;              /* null for the empty string */
    uint32_t length;
    uint32_t capacity;
} RoverString;

/* Opaque objects handed across the table. */
typedef struct RoverPropertyBag RoverPropertyBag;   /* keyed-value container, slots 44-54 */
typedef struct RoverBagEntry RoverBagEntry;         /* one container entry, slots 52-62 */
typedef struct RoverSectorReader RoverSectorReader; /* open sector map, slots 34-36 */

#pragma pack(push, 2)

/* Host and port of a content download server (slot 40). */
typedef struct RoverFileServer {
    int32_t type_index; /* OA_ROVER_FILE_SERVER_INVALID on failure */
    char host[0x100];   /* copy bounded to 255 characters */
    uint16_t port;
} RoverFileServer;

#pragma pack(pop)

/* One Galactic War planet (slot 35); the caller allocates planet_count. */
typedef struct RoverSectorPlanet {
    int32_t id;
    char name[16];
    int32_t x;
    int32_t y;
    char seqname[32]; /* without its size letter */
    /* 0, 1 or 2 for a seqname ending _L, _M or _S (either case): large, medium, small? */
    int32_t size_class;
    int32_t planet_rot;
    char side;
    bool capitol;
    char desc[1024];
    char ta_map[63];
    char ta_mapsource[4];
    char ta_maptype[16];
    char pan_gaf[32];
    char pan_seq[32];
    int32_t playerlimit;
    int32_t deathopt;
    int32_t energy;
    int32_t metal;
    int32_t location;
    int32_t los;
    int32_t mapping;
    int32_t maxunits;
    int32_t watching;
} RoverSectorPlanet;

/* One planet-to-planet link (slot 35). */
typedef struct RoverSectorConnector {
    int32_t planet1_id;
    int32_t planet2_id;
} RoverSectorConnector;

/* Callbacks the game hands to the request slots. Unless a slot says otherwise
 * Rover stores them and calls them from slot 5 on the thread that pumps. The
 * key/value variants (slots 14, 18, 20, 22), which the game never calls, are
 * taken to follow the conventions of their siblings. */
typedef void(ROVER_CALL* RoverLoginProgressFn)(
    int32_t stage, int32_t result, int32_t detail_a, int32_t detail_b, const char* message
);
/* A system_alert message: its type (0 when absent), its text and a bag of
   its other fields, freed when the callback returns. */
typedef void(ROVER_CALL* RoverSystemAlertFn)(int32_t type, const char* message, void* fields);
/* A lost link: which one (1 the yard master, 0 a boneyard), whether it was
   still connecting and its last socket error. */
typedef void(ROVER_CALL* RoverDisconnectFn)(int32_t which, char connecting, int32_t socket_error);
typedef void(ROVER_CALL* RoverLogoutFn)(void);
typedef void(ROVER_CALL* RoverRoomListFn)(
    int32_t change,
    int32_t area_id,
    int32_t room_id,
    const char* name,
    const char* description,
    bool needs_pw,
    int32_t users,
    int32_t type,
    const char* tag,
    bool last
);
typedef void(ROVER_CALL* RoverRoomUpdateFn)(
    int32_t change,
    int32_t area_id,
    int32_t room_id,
    const char* name,
    const char* description,
    bool needs_pw,
    int32_t users,
    int32_t type,
    const char* tag
);
typedef void(ROVER_CALL* RoverRoomEnteredFn)(int32_t result);
typedef void(ROVER_CALL* RoverRoomInfoFn)(
    int32_t change,
    int32_t user_id,
    const char* username,
    int32_t latency,
    int32_t icon,
    int32_t rank,
    const char* allegiance
);
typedef void(ROVER_CALL* RoverChatMsgFn)(int32_t no_user_id, int32_t user_id, const char* message);
typedef void(ROVER_CALL* RoverRoomLeftFn)(bool failed);
/* The ids arrive by address; one the reply lacks is 0, which the game takes
   as a failed creation. */
typedef void(ROVER_CALL* RoverRoomCreatedFn)(
    uint32_t* area_id, uint32_t* room_id, const char* name
);
typedef void(ROVER_CALL* RoverBattlesDoneFn)(bool result);
typedef void(ROVER_CALL* RoverBattleInfoFn)(
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host_code
);
typedef void(ROVER_CALL* RoverBattleInfoKvFn)(
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    RoverPropertyBag* fields
);
typedef void(ROVER_CALL* RoverIntelMsgFn)(
    int32_t no_game_id, const char* game_id, const char* message
);
typedef void(ROVER_CALL* RoverBattleIntelFn)(
    const char* game_id,
    uint32_t user_id,
    const char* username,
    int32_t has_password,
    const char* mission,
    int32_t skill,
    int32_t players_code,
    int32_t on_death,
    bool result
);
typedef void(ROVER_CALL* RoverBattleReportFn)(
    const char* game_id,
    uint32_t user_id,
    int32_t icon,
    const char* username,
    int32_t kills,
    int32_t losses,
    int32_t energy,
    int32_t metal,
    int32_t score,
    const char* message
);
typedef void(ROVER_CALL* RoverBattleIntelKvFn)(
    const char* game_id,
    uint32_t user_id,
    const char* username,
    int32_t has_password,
    int32_t result,
    RoverPropertyBag* fields
);
typedef void(ROVER_CALL* RoverBattleReportKvFn)(
    const char* game_id,
    uint32_t user_id,
    const char* username,
    const char* message,
    RoverPropertyBag* fields
);
/* creator and lock_options carry meaning in the low byte only. */
typedef void(ROVER_CALL* RoverLaunchGameFn)(
    const char* game_id,
    const char* session,
    const char* address,
    const char* password,
    bool creator,
    bool lock_options,
    const char* mission,
    int32_t playerlimit,
    int32_t deathopt,
    int32_t energy,
    int32_t metal,
    int32_t location,
    int32_t los,
    int32_t mapping,
    int32_t maxunits,
    int32_t watching,
    int32_t cheats,
    int32_t tournament,
    const char* allegiance,
    int32_t team,
    int32_t result
);
typedef void(ROVER_CALL* RoverLaunchGameKvFn)(
    const char* game_id,
    const char* session,
    const char* address,
    const char* password,
    int32_t result,
    RoverPropertyBag* fields
);
typedef void(ROVER_CALL* RoverDebriefingFn)(uint32_t area_id, uint32_t room_id, bool first);
typedef bool(ROVER_CALL* RoverHttpCompleteFn)(const char* url, const char* file, int32_t status);
typedef void(ROVER_CALL* RoverHttpProgressFn)(int32_t percent);
/* Called synchronously inside slot 33, once per record. */
typedef bool(ROVER_CALL* RoverParsedRecordFn)(
    const char* command,
    const char* id,
    const char* name,
    const char* sort,
    const char* category,
    const char* headline,
    const char* file,
    RoverPropertyBag* props,
    void* user
);
/* Called synchronously inside slot 37, once per record. */
typedef void(ROVER_CALL* RoverRecordFieldsFn)(RoverPropertyBag* field_bag, bool is_last);
typedef void(ROVER_CALL* RoverCensusFn)(
    uint32_t area_id,
    int32_t population,
    int32_t activity,
    int32_t in_progress,
    int32_t forming,
    int32_t core_wins,
    int32_t arm_wins
);
typedef void(ROVER_CALL* RoverActivityFn)(const char* activity);
typedef void(ROVER_CALL* RoverAllegianceFn)(const char* allegiances);
typedef void(ROVER_CALL* RoverProfileFn)(bool ok, RoverPropertyBag* properties);
typedef void(ROVER_CALL* RoverLadderProfileFn)(
    int32_t ladder_id, int32_t ladder_subid, RoverPropertyBag* properties
);
typedef void(ROVER_CALL* RoverProfileUpdatedFn)(bool ok);
/* loc_code is where the user is (0 logged out, 1 chatting, 2 examining
   battles, 3 in a battle, 4 on another server, 5 no such user, 6 viewing
   other screens); the bag holds the answer's other fields and is freed
   when the callback returns. */
typedef void(ROVER_CALL* RoverLocateFn)(
    int32_t loc_code,
    uint32_t user_id,
    const char* name,
    int32_t user_icon,
    RoverPropertyBag* fields
);
/* One game_located answer to slot 66. area_id is 0 and type is 0 when the
   reply omits them. game_id is null when omitted; name is "" when omitted.
   forming is 0 or 1. pass_required is 1 when the reply's pass_req is not
   zero, else 0. last is 0 when omitted; any other last disarms the handler
   after the call. A forming value other than 0 or 1 is not reported and
   leaves the handler armed. */
typedef void(ROVER_CALL* RoverGameLocatedFn)(
    uint32_t area_id,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t pass_required,
    int32_t last
);
/* 3.1c's Rover calls it from its HTTP worker thread, outside the game's
   interface lock; slot 67 here takes it and never calls it. Its two
   arguments have no known meaning and are left unnamed. */
typedef void(ROVER_CALL* RoverHttpLoginFn)(void*, int32_t);
/* Called synchronously inside slot 70 with each server's location text. */
typedef void(ROVER_CALL* RoverAreaEntryFn)(const char* location);
/* A callback of no fixed signature. Slots 68 and 80 take one and ignore it. */
typedef void(ROVER_CALL* RoverOpaqueFn)(void);

/* The table, in slot order. A slot returning bool sets only the low byte of
 * its 32-bit result. */
typedef struct RoverInterface {
    int32_t(ROVER_CALL* check_version)(
        int32_t iface_version, int32_t build_stamp
    ); /* 0: the whole 32-bit result is 1 or 0 */
    bool(ROVER_CALL* attach)(void);
    void(ROVER_CALL* detach)(void);
    bool(ROVER_CALL* activate)(void);
    void(ROVER_CALL* deactivate)(void);
    void(ROVER_CALL* pump_events)(void); /* 5: runs every stored callback */
    bool(ROVER_CALL* reset_session)(void);
    bool(ROVER_CALL* start_login)(
        const char* server_name,
        const char* username,
        const char* password,
        RoverLoginProgressFn progress,
        RoverSystemAlertFn system_alert,
        RoverDisconnectFn disconnect
    );
    bool(ROVER_CALL* send_logout)(int32_t user_id, RoverLogoutFn logged_out);
    bool(ROVER_CALL* get_rooms)(int32_t area_id, RoverRoomListFn room_list);
    bool(ROVER_CALL* enter_room)(
        int32_t room_id,
        int32_t area_id,
        const char* password,
        RoverRoomEnteredFn entered,
        RoverRoomInfoFn room_info,
        RoverRoomUpdateFn room_list,
        RoverChatMsgFn chat_msg
    );
    bool(ROVER_CALL* switch_room)(
        int32_t room_id,
        int32_t area_id,
        const char* password,
        int32_t old_room_id,
        int32_t old_area_id,
        RoverRoomEnteredFn entered,
        RoverRoomInfoFn room_info,
        RoverRoomUpdateFn room_list,
        RoverChatMsgFn chat_msg
    );
    bool(ROVER_CALL* leave_room)(int32_t room_id, int32_t area_id, RoverRoomLeftFn left);
    bool(ROVER_CALL* send_chat)(
        int32_t room_id, int32_t area_id, int32_t target_user_id, const char* message
    );
    bool(ROVER_CALL* enter_battles_kv)(
        uint32_t area_id,
        RoverBattlesDoneFn entered,
        RoverBattleInfoKvFn battle_info,
        RoverIntelMsgFn intel_msg
    );
    bool(ROVER_CALL* enter_battles)(
        uint32_t area_id,
        RoverBattlesDoneFn entered,
        RoverBattleInfoFn battle_info,
        RoverIntelMsgFn intel_msg
    );
    bool(ROVER_CALL* leave_battles_kv)(uint32_t area_id, RoverBattlesDoneFn left);
    bool(ROVER_CALL* leave_battles)(uint32_t area_id, RoverBattlesDoneFn left);
    bool(ROVER_CALL* get_battle_kv)(
        uint32_t area_id,
        const char* game_id,
        RoverBattleIntelKvFn battle_intel,
        RoverBattleReportKvFn battle_report
    );
    bool(ROVER_CALL* get_battle)(
        uint32_t area_id,
        const char* game_id,
        RoverBattleIntelFn battle_intel,
        RoverBattleReportFn battle_report
    );
    bool(ROVER_CALL* host_game_kv)(
        uint32_t area_id,
        int32_t type,
        const char* name,
        const char* password,
        const char* description,
        RoverPropertyBag* extra_fields,
        RoverLaunchGameKvFn launch,
        RoverDebriefingFn debriefing
    );
    bool(ROVER_CALL* host_game)(
        uint32_t area_id,
        int32_t game_type,
        const char* name,
        const char* password,
        const char* description,
        const char* keyword,
        int32_t skill_sel,
        int32_t players_sel,
        int32_t on_death_sel,
        RoverLaunchGameFn launch,
        RoverDebriefingFn debriefing
    );
    bool(ROVER_CALL* join_game_with_fields)(
        uint32_t area_id,
        const char* game_id,
        const char* password,
        RoverPropertyBag* extra_fields,
        RoverLaunchGameKvFn launch,
        RoverDebriefingFn debriefing
    );
    bool(ROVER_CALL* join_game)(
        uint32_t area_id,
        const char* game_id,
        const char* password,
        RoverLaunchGameFn launch,
        RoverDebriefingFn debriefing
    );
    bool(ROVER_CALL* send_left_game)(uint32_t area_id, const char* game_id);
    bool(ROVER_CALL* send_game_settings_fields)(
        uint32_t area_id, const char* game_id, RoverPropertyBag* extra_fields
    );
    bool(ROVER_CALL* send_game_settings)(
        uint32_t area_id,
        const char* game_id,
        int32_t lock_options,
        const char* mission,
        int32_t playerlimit,
        int32_t deathopt,
        int32_t energy,
        int32_t metal,
        int32_t location,
        int32_t los,
        int32_t mapping,
        int32_t maxunits,
        int32_t watching,
        int32_t cheats,
        int32_t tournament
    );
    bool(ROVER_CALL* send_player_info_fields)(
        uint32_t area_id,
        const char* game_id,
        uint32_t dpid,
        const char* name,
        RoverPropertyBag* extra_fields
    );
    bool(ROVER_CALL* send_player_info)(
        uint32_t area_id,
        const char* game_id,
        uint32_t dpid,
        const char* name,
        const char* allegiance,
        int32_t team
    );
    bool(ROVER_CALL* send_score_report_props)(
        uint32_t area_id, const char* game_id, uint32_t dpid, RoverPropertyBag* props, bool last
    );
    /* 30: arguments 9 and 10 are never forwarded to the request. */
    bool(ROVER_CALL* send_score_report)(
        uint32_t area_id,
        const char* game_id,
        uint32_t dpid,
        int32_t kills,
        int32_t losses,
        int32_t energy,
        int32_t metal,
        int32_t xs_energy,
        int32_t xs_metal,
        int32_t ignored_9,
        int32_t ignored_10,
        int32_t score,
        bool victory,
        bool last
    );
    bool(ROVER_CALL* send_score_report_message)(
        uint32_t area_id, const char* game_id, uint32_t dpid, const char* message
    );
    bool(ROVER_CALL* fetch_http_file)(
        const char* url,
        const char* dest_path,
        RoverHttpCompleteFn complete,
        RoverHttpProgressFn progress
    );
    bool(ROVER_CALL* parse_record_file)(const char* path, RoverParsedRecordFn record, void* user);
    RoverSectorReader*(ROVER_CALL* open_sector_map)(
        const char* path,
        int32_t* out_sector_id,
        char* out_sector_name,
        int32_t* out_width,
        int32_t* out_height,
        int32_t* out_planet_count,
        int32_t* out_connector_count
    );
    bool(ROVER_CALL* read_sector_planets)(
        RoverSectorReader* reader,
        int32_t sector_width,
        int32_t sector_height,
        RoverSectorPlanet* out_planets,
        int32_t planet_count,
        RoverSectorConnector* out_connectors,
        int32_t connector_count
    );
    void(ROVER_CALL* close_sector_map)(RoverSectorReader* reader);
    bool(ROVER_CALL* enumerate_record_file)(const char* path, RoverRecordFieldsFn record);
    void(ROVER_CALL* request_census)(
        uint32_t area_id, RoverCensusFn census, RoverActivityFn activity
    );
    void(ROVER_CALL* request_allegiance)(RoverAllegianceFn allegiances);
    void(ROVER_CALL* get_file_server)(int32_t server_type, RoverFileServer* out_server);
    void(ROVER_CALL* send_page_by_name)(const char* user_name, const char* message);
    void(ROVER_CALL* send_page_by_id)(int32_t user_id, const char* message);
    void(ROVER_CALL* set_latency_reply_flags)(uint32_t flags); /* 43: bits 0..7 only */
    RoverPropertyBag*(ROVER_CALL* create_property_bag)(void);
    void(ROVER_CALL* destroy_property_bag)(RoverPropertyBag* bag);
    bool(ROVER_CALL* get_field_int)(
        RoverPropertyBag* bag, const RoverString* key, int32_t* out_value
    );
    bool(ROVER_CALL* get_field_uint)(
        RoverPropertyBag* bag, const RoverString* key, uint32_t* out_value
    );
    bool(ROVER_CALL* get_field_string)(
        RoverPropertyBag* bag, const RoverString* key, RoverString* out_value
    );
    bool(ROVER_CALL* set_field_int)(RoverPropertyBag* bag, const RoverString* key, int32_t value);
    bool(ROVER_CALL* set_field_uint)(RoverPropertyBag* bag, const RoverString* key, uint32_t value);
    bool(ROVER_CALL* set_bag_string)(
        RoverPropertyBag* bag, const RoverString* key, const RoverString* value
    );
    RoverBagEntry*(ROVER_CALL* get_first_bag_entry)(RoverPropertyBag* bag);
    RoverBagEntry*(ROVER_CALL* get_next_bag_entry)(RoverPropertyBag* bag);
    RoverBagEntry*(ROVER_CALL* find_bag_entry)(RoverPropertyBag* bag, const RoverString* key);
    uint32_t(ROVER_CALL* get_entry_type)(const RoverBagEntry* entry); /* 55: OA_ROVER_ENTRY_* */
    bool(ROVER_CALL* get_entry_name)(const RoverBagEntry* entry, RoverString* out_name);
    bool(ROVER_CALL* get_entry_int)(const RoverBagEntry* entry, int32_t* out_value);
    bool(ROVER_CALL* get_entry_uint)(const RoverBagEntry* entry, uint32_t* out_value);
    bool(ROVER_CALL* get_entry_string)(const RoverBagEntry* entry, RoverString* out_value);
    bool(ROVER_CALL* set_entry_int)(RoverBagEntry* entry, int32_t value);
    bool(ROVER_CALL* set_entry_uint)(RoverBagEntry* entry, uint32_t value);
    bool(ROVER_CALL* set_entry_string)(RoverBagEntry* entry, const RoverString* value);
    void(ROVER_CALL* request_get_profile)(
        uint32_t user_id, RoverProfileFn profile, RoverLadderProfileFn ladder_profile
    );
    void(ROVER_CALL* request_set_profile)(
        uint32_t user_id, RoverPropertyBag* properties, RoverProfileUpdatedFn updated
    );
    void(ROVER_CALL* locate_player)(const char* username, RoverLocateFn located);
    void(ROVER_CALL* locate_target)(const char* name, RoverGameLocatedFn located);
    void(ROVER_CALL* start_http_login)(
        const char* url_suffix,
        const char* login_name,
        const char* email,
        const char* password,
        const char* cd_key,
        RoverHttpLoginFn complete
    );
    void(ROVER_CALL* request_areas)(uint32_t area_mask, uint32_t area_id, RoverOpaqueFn areas);
    void(ROVER_CALL* create_room)(
        uint32_t area_id,
        const char* room_name,
        const char* description,
        const char* password,
        RoverRoomCreatedFn created
    );
    /* 70: each server's location, in list order */
    void(ROVER_CALL* enumerate_areas)(RoverAreaEntryFn location_fn);
    /* 71: the first selected server's location text */
    void(ROVER_CALL* find_current_area)(const char** out_location);
    /* 72: the last server at the location whose yard master answered, read as its leading name */
    const char*(ROVER_CALL* match_area_name)(void);
    /* 73-78: slots the game never calls. They ignore their arguments. The
       names get_house_info's and find_house's type, set_house_info's
       house_id and create_house's house_name and crest are tentative, and so
       is the reading of the leading unnamed words as the callbacks for each
       request's replies. */
    bool(ROVER_CALL* enter_metagame)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    bool(ROVER_CALL* leave_metagame)(uint32_t);
    bool(ROVER_CALL* get_house_info)(
        uint32_t, uint32_t, uint32_t, uint32_t, const char* type, void* extra_fields
    );
    bool(ROVER_CALL* find_house)(uint32_t, const char* type, void* extra_fields);
    bool(ROVER_CALL* set_house_info)(uint32_t, uint32_t house_id, void* extra_fields);
    bool(ROVER_CALL* create_house)(
        uint32_t, const char* house_name, uint32_t crest, void* extra_fields
    );
    bool(ROVER_CALL* queue_command)(
        const char* destination, const char* command, void* extra_fields
    );
    bool(ROVER_CALL* register_command_handler)(const char* command, RoverOpaqueFn handler);
    bool(ROVER_CALL* unregister_command)(const char* command);
} RoverInterface;

OA_ROVER_STATIC_ASSERT(
    sizeof(RoverInterface) == OA_ROVER_SLOT_COUNT * sizeof(void (*)(void)),
    "RoverInterface holds exactly the 82 slots"
);
OA_ROVER_STATIC_ASSERT(sizeof(RoverFileServer) == 0x106, "RoverFileServer size");
OA_ROVER_STATIC_ASSERT(offsetof(RoverFileServer, port) == 0x104, "RoverFileServer.port offset");
OA_ROVER_STATIC_ASSERT(sizeof(RoverSectorPlanet) == 0x500, "RoverSectorPlanet size");
OA_ROVER_STATIC_ASSERT(offsetof(RoverSectorPlanet, side) == 0x44, "RoverSectorPlanet.side offset");
OA_ROVER_STATIC_ASSERT(
    offsetof(RoverSectorPlanet, pan_seq) == 0x4b9, "RoverSectorPlanet.pan_seq offset"
);
OA_ROVER_STATIC_ASSERT(
    offsetof(RoverSectorPlanet, playerlimit) == 0x4dc, "RoverSectorPlanet.playerlimit offset"
);
OA_ROVER_STATIC_ASSERT(sizeof(RoverSectorConnector) == 8, "RoverSectorConnector size");
#if OA_ROVER_ABI32
OA_ROVER_STATIC_ASSERT(sizeof(RoverInterface) == 0x148, "RoverInterface is 82 dwords");
OA_ROVER_STATIC_ASSERT(offsetof(RoverInterface, start_login) == 0x01c, "slot 7 offset");
OA_ROVER_STATIC_ASSERT(offsetof(RoverInterface, get_file_server) == 0x0a0, "slot 40 offset");
OA_ROVER_STATIC_ASSERT(offsetof(RoverInterface, unregister_command) == 0x144, "slot 81 offset");
OA_ROVER_STATIC_ASSERT(sizeof(RoverString) == 0x10, "RoverString size");
OA_ROVER_STATIC_ASSERT(offsetof(RoverString, data) == 0x4, "RoverString.data offset");
OA_ROVER_STATIC_ASSERT(offsetof(RoverString, length) == 0x8, "RoverString.length offset");
OA_ROVER_STATIC_ASSERT(offsetof(RoverString, capacity) == 0xc, "RoverString.capacity offset");
#endif

/// Stores the address of the table the game reaches every lobby service through.
///
/// The DLL exports it as _GetInterface@4. The table is constant, so it is valid before and after
/// attach.
///
/// @param[out] out receives the table's address
void ROVER_CALL rover_get_interface(const RoverInterface** out);

#ifdef __cplusplus
}
#endif

#endif
