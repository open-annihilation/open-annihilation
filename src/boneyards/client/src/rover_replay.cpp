// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The recorded Rover interface (test scaffolding; see rover_replay.hpp).
#include "oa/boneyards/rover_replay.hpp"

#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/utf8.hpp"
#include "oa/boneyards/rover_string.hpp"

// oa-rover's record text and its queues, through which launch and record
// lines reach Rover and the records Rover queues for the Boneyards are read.
#include "lobby.h"
#include "record.h"
#include "state.h"
#include "oa/platform/files.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace oa::boneyards {
namespace {

constexpr std::size_t kLineCapacity = 0x800;
constexpr std::size_t kMaxProgress = 16;
constexpr std::size_t kMaxDossierFields = 32;
constexpr std::size_t kKeyCapacity = 0x40;
constexpr std::size_t kTextCapacity = 0x400;
constexpr std::size_t kMessageCapacity = 0x100;
constexpr std::size_t kNameCapacity = 0x40;
constexpr std::size_t kTagCapacity = 0x40;
constexpr std::size_t kFeedCapacity = 0x100;
constexpr std::size_t kMaxCensus = 256;
constexpr std::size_t kQueueCapacity = 512;
constexpr int32_t kMaxDelay = 10000;
constexpr int32_t kDefaultDelay = 1;
constexpr std::size_t kCensusValues = 6;
constexpr std::size_t kMaxLocated = 8;
constexpr std::size_t kMaxLocatedFields = 12;
constexpr std::size_t kLocatedTextCapacity = 0x100;
constexpr const char* kLocatedCommand = "user_located";
constexpr std::size_t kMaxRooms = 64;
constexpr std::size_t kMaxEntries = 32;
constexpr std::size_t kMaxMembers = 64;
constexpr std::size_t kMaxSaid = 64;
constexpr std::size_t kMaxUpdates = 64;
constexpr std::size_t kMaxLeft = 16;
constexpr std::size_t kMaxCreated = 16;
constexpr std::size_t kMaxAlerts = 16;
constexpr std::size_t kMaxSent = 256;
constexpr std::size_t kMaxProfiles = 16;
constexpr std::size_t kMaxLadders = 32;
constexpr std::size_t kMaxProfileLines = 96;
constexpr std::size_t kAllegianceCapacity = 0x10;
constexpr int32_t kRoomAdded = 0;
constexpr int32_t kEnteredOk = 0;
// The login progress report that confirms the login carries the user id.
constexpr int32_t kLoginResultStage = 8;
constexpr int32_t kLoginAccepted = 0;
constexpr int32_t kAlertPage = 2;
constexpr const char* kAlertCommand = "system_alert";
constexpr const char* kUserIdKey = "user_id";
constexpr const char* kNotificationTypeKey = "notification_type";
constexpr std::size_t kNoSend = SIZE_MAX;
// The two content servers Rover keeps, by the file_server record's type.
constexpr int32_t kFileServerCount = 2;
constexpr const char* kFileServerTypes[kFileServerCount] = {"news", "ladder"};
constexpr std::size_t kMaxFileServers = 8;
constexpr std::size_t kFileServerHostCapacity = 0x100;
// A fetch's URL and file as Rover queues them, terminator included.
constexpr std::size_t kFetchTextCapacity = 0x104;
constexpr std::size_t kLocalPathCapacity = 0x400;
constexpr std::size_t kMaxHttp = 32;
constexpr std::size_t kMaxFetches = 64;
constexpr std::size_t kNoHttp = SIZE_MAX;
// The status Rover's fetch worker posts for every fetch, as the game gets it.
constexpr int32_t kFetchDone = 0;
constexpr std::size_t kMaxLaunches = 4;
constexpr std::size_t kMaxInbound = 16;
constexpr std::size_t kMaxMatchReplies = 16;
constexpr std::size_t kMaxBattles = 64;
constexpr int32_t kBattleAdded = 0;
// battle_info's host side: Arm, Core, or unknown.
constexpr int32_t kHostArm = 0;
constexpr int32_t kHostCore = 1;
constexpr int32_t kHostUnknown = 2;
// get_battle's intel when the file names none: commander death ends the battle.
constexpr int32_t kIntelOnDeath = 2;
// A record's text as Rover takes it, with its terminator.
constexpr std::size_t kRecordCapacity = kRoverRecordTextLimit + 1;

struct Progress {
    int32_t stage;
    int32_t result;
    int32_t detail_a;
    int32_t detail_b;
    bool has_message;
    char message[kMessageCapacity];
    int32_t delay;
};

// A dossier, profile or ladder field.
struct TypedField {
    char key[kKeyCapacity];
    bool is_int;
    int32_t number;
    char text[kTextCapacity];
};

// A user's recorded profile: the ladder standings, then the profile.
struct ProfileAnswer {
    uint32_t user_id;
    int32_t delay;
};

struct LadderAnswer {
    std::size_t answer;
    int32_t ladder_id;
    int32_t ladder_subid;
};

// A field of a profile answer, or of one of its ladder standings.
struct ProfileLine {
    std::size_t answer;
    bool ladder;
    std::size_t ladder_index;
    TypedField field;
};

struct Census {
    uint32_t area_id;
    int32_t values[kCensusValues]; // population .. arm_wins
    int32_t delay;
};

struct Feed {
    bool present;
    char text[kFeedCapacity];
    int32_t delay;
};

enum class FieldType : uint8_t { int32, uint32, text };

struct LocatedField {
    char key[kKeyCapacity];
    FieldType type;
    int32_t number;
    char text[kLocatedTextCapacity];
};

struct Located {
    char name[kNameCapacity];
    int32_t code;
    uint32_t user_id;
    int32_t icon;
    LocatedField fields[kMaxLocatedFields];
    std::size_t field_count;
    int32_t delay;
};

// The fields of a room-list row.
struct RoomFields {
    uint32_t area_id;
    uint32_t room_id;
    int32_t users;
    int32_t type;
    bool needs_password;
    bool global_known{}; // the line named global: the sector-wide frequency
    int32_t global{};
    char name[kNameCapacity];
    char description[kMessageCapacity];
    char tag[kTagCapacity];
};

struct Room {
    RoomFields fields;
    int32_t delay;
};

struct Entry {
    uint32_t area_id;
    uint32_t room_id;
    int32_t result;
    int32_t delay;
};

struct Member {
    uint32_t area_id;
    uint32_t room_id;
    uint32_t user_id;
    int32_t latency;
    int32_t icon;
    int32_t rank;
    char allegiance[kAllegianceCapacity];
    char name[kNameCapacity];
    int32_t delay;
};

struct Said {
    uint32_t area_id;
    uint32_t room_id;
    uint32_t user_id;
    char text[kMessageCapacity];
    int32_t delay;
};

// The answers room-list updates and alerts follow.
enum class AnswerKind : uint8_t { entered, created };

struct Update {
    AnswerKind answer;
    std::size_t answer_index;
    int32_t change;
    RoomFields fields;
    int32_t delay;
};

struct Left {
    uint32_t area_id;
    uint32_t room_id;
    int32_t result;
    int32_t delay;
};

struct Created {
    uint32_t area_id;
    uint32_t room_id;
    char name[kNameCapacity];
    int32_t delay;
};

// A system alert: a page carries its sender's user_id, a notification its
// notification_type. The login brings it, or the entered or created answer
// it follows in the file.
struct Alert {
    int32_t kind;
    bool has_user;
    uint32_t user_id;
    char text[kMessageCapacity];
    char notification_type[kKeyCapacity];
    bool follows_answer;
    AnswerKind answer;
    std::size_t answer_index;
    int32_t delay;
};

struct FileServer {
    int32_t type_index;
    char host[kFileServerHostCapacity];
    uint16_t port;
    int32_t delay;
};

// A recorded download: the URL and the file whose bytes it brings.
struct Http {
    char url[kFetchTextCapacity];
    char local[kLocalPathCapacity];
    int32_t delay;
};

// A fetch slot 32 queued: its URL, the file it saves to and the recorded
// download that answers it (kNoHttp: none).
struct Fetch {
    char url[kFetchTextCapacity];
    char destination[kFetchTextCapacity];
    std::size_t http;
};

// A launch_game record answering a host or join request, and a record Rover
// receives after the launch it follows.
struct Launch {
    char text[kRecordCapacity];
    int32_t delay;
};

// Named apart from the tunnel's Inbound enumeration. The style check matches
// an enumeration by its unqualified name, wherever that enumeration is declared.
struct InboundRecord {
    std::size_t launch;
    char text[kRecordCapacity];
    int32_t delay;
};

// A matchmaking or QuickPlay reply that answers the next outbound record whose text contains
// command=matchmaking_ or command=quickplay_. A chained line was written with `then` and is
// delivered in the same answer as the reply it follows.
struct MatchReply {
    char text[kRecordCapacity]{};
    int32_t delay{};
    bool chained{};
};

// One battle_info row of an area's list.
struct Battle {
    uint32_t area_id;
    char game_id[kNameCapacity];
    int32_t type;
    int32_t forming;
    int32_t host_code;
    bool pass_required{};
    bool seats_known{};
    int32_t players{};
    int32_t playerlimit{};
    char name[kNameCapacity];
    int32_t delay;
};

struct Script {
    Progress progress[kMaxProgress];
    std::size_t progress_count;
    TypedField dossier[kMaxDossierFields];
    std::size_t dossier_count;
    int32_t dossier_delay;
    bool has_area_name;
    char area_name[kNameCapacity];
    Feed allegiances;
    Census census[kMaxCensus];
    std::size_t census_count;
    Feed activity;
    Located located[kMaxLocated];
    std::size_t located_count;
    Room rooms[kMaxRooms];
    std::size_t room_count;
    Entry entries[kMaxEntries];
    std::size_t entry_count;
    Member members[kMaxMembers];
    std::size_t member_count;
    Said said[kMaxSaid];
    std::size_t said_count;
    Update updates[kMaxUpdates];
    std::size_t update_count;
    Left left[kMaxLeft];
    std::size_t left_count;
    Created created[kMaxCreated];
    std::size_t created_count;
    Alert alerts[kMaxAlerts];
    std::size_t alert_count;
    bool echo;
    int32_t echo_delay;
    ProfileAnswer profiles[kMaxProfiles];
    std::size_t profile_count;
    LadderAnswer ladders[kMaxLadders];
    std::size_t ladder_count;
    ProfileLine profile_lines[kMaxProfileLines];
    std::size_t profile_line_count;
    FileServer file_servers[kMaxFileServers];
    std::size_t file_server_count;
    Http http[kMaxHttp];
    std::size_t http_count;
    Launch launches[kMaxLaunches];
    std::size_t launch_count;
    InboundRecord inbound[kMaxInbound];
    std::size_t inbound_count;
    MatchReply match_replies[kMaxMatchReplies];
    std::size_t match_count{};
    Battle battles[kMaxBattles];
    std::size_t battle_count;
};

enum class ReplyKind : uint8_t {
    progress,
    dossier,
    allegiances,
    census,
    activity,
    located,
    room,
    entered,
    member,
    said,
    update,
    left,
    created,
    alert,
    echo,
    logout,
    profile,
    file_server,
    fetched,
    launch,
    inbound,
    battle,
    battles_entered,
    battles_left,
    battle_intel,
    game_located,
    match_reply
};

struct Reply {
    ReplyKind kind;
    std::size_t index;
    uint32_t due; // pump count at which it is delivered
};

// The callbacks of the entered room, as Rover keeps them: enter_room arms
// them at once, switch_room once the new room is entered, and leave_room
// drops them. Members, lines and echoes reach them only for their own room,
// room-list updates for the room's area.
struct RoomFeeds {
    uint32_t area_id;
    uint32_t room_id;
    RoverRoomInfoFn room_info;
    RoverRoomUpdateFn room_update;
    RoverChatMsgFn chat_msg;
};

// The one entry or switch Rover waits for.
struct Entering {
    bool switching;
    RoverRoomEnteredFn entered;
    RoomFeeds feeds;
};

struct Leaving {
    uint32_t area_id;
    uint32_t room_id;
    RoverRoomLeftFn left;
};

Script g_script{};
Reply g_queue[kQueueCapacity];
std::size_t g_queue_count = 0;
uint32_t g_pumps = 0;
const RoverInterface* g_real = nullptr;
RoverInterface g_table{};
ReplaySent g_sent[kMaxSent];
std::size_t g_sent_count = 0;
uint32_t g_user_id = 0; // from the replayed login confirmation

RoverLoginProgressFn g_progress = nullptr;
RoverSystemAlertFn g_alert = nullptr;
RoverProfileFn g_profile = nullptr;
RoverLadderProfileFn g_ladder = nullptr;
RoverAllegianceFn g_allegiance = nullptr;
RoverCensusFn g_census = nullptr;
RoverActivityFn g_activity = nullptr;
RoverLogoutFn g_logout = nullptr;
RoverLocateFn g_locate = nullptr;
RoverRoomListFn g_room_list = nullptr;
RoverRoomCreatedFn g_created = nullptr;
RoomFeeds g_room{};
Entering g_entering{};
Leaving g_leaving{};
RoverFileServer g_file_servers[kFileServerCount]{};
RoverHttpCompleteFn g_http_complete = nullptr;
Fetch g_fetches[kMaxFetches];
std::size_t g_fetch_count = 0;
// The launch line the next host or join request brings.
std::size_t g_next_launch = 0;
// The reply line the next matchmaking outbound brings, together with the `then` lines after it.
std::size_t g_next_match_reply = 0;
// The battle list enter_battles opened, and the get_battle it may follow.
uint32_t g_battles_area = 0;
RoverBattlesDoneFn g_battles_entered = nullptr;
RoverBattleInfoFn g_battle_info = nullptr;
RoverBattlesDoneFn g_battles_left = nullptr;
RoverBattleIntelFn g_battle_intel = nullptr;
char g_intel_game[kNameCapacity] = {};
// The battle search slot 66 asked: the recorded battles whose names hold its
// text, in file order. None found is answered by one closing row.
RoverGameLocatedFn g_game_located = nullptr;
std::size_t g_game_hits[kMaxBattles] = {};
std::size_t g_game_hit_count = 0;

void queue(ReplyKind kind, std::size_t index, int32_t delay) {
    if (g_queue_count < kQueueCapacity)
        g_queue[g_queue_count++] = Reply{kind, index, g_pumps + static_cast<uint32_t>(delay)};
}

// Rover waits for one answer of some kinds at a time; a new request drops
// an answer still queued for the last.
void drop_queued(ReplyKind kind) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < g_queue_count; ++i)
        if (g_queue[i].kind != kind)
            g_queue[kept++] = g_queue[i];
    g_queue_count = kept;
}

// ---- parsing -------------------------------------------------------------

struct Line {
    const char* cursor;
};

// What an entry may refer back to: the delay in force, the last entered or
// created answer, which update lines follow, and the last launch, which
// record lines follow.
struct ParseState {
    int32_t delay;
    bool has_answer;
    AnswerKind answer;
    std::size_t answer_index;
    bool has_launch;
    std::size_t launch_index;
    bool has_match_reply{};
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void skip_spaces(Line* line) {
    while (is_space(*line->cursor))
        ++line->cursor;
}

// The next space-separated word, copied; false when none is left or it
// does not fit.
bool next_word(Line* line, char* out, std::size_t capacity) {
    skip_spaces(line);
    std::size_t length = 0;
    while (line->cursor[length] != '\0' && !is_space(line->cursor[length]))
        ++length;
    if (length == 0 || length >= capacity)
        return false;
    std::memcpy(out, line->cursor, length);
    out[length] = '\0';
    line->cursor += length;
    return true;
}

bool next_int(Line* line, int32_t* out) {
    char word[0x20];
    if (!next_word(line, word, sizeof word))
        return false;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(word, &end, 10);
    if (errno != 0 || *end != '\0' || value < INT32_MIN || value > UINT32_MAX)
        return false;
    *out = static_cast<int32_t>(value);
    return true;
}

bool next_id(Line* line, uint32_t* out) {
    int32_t value = 0;
    if (!next_int(line, &value))
        return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

// The rest of the line without its leading and trailing spaces; false when
// it does not fit.
bool rest_of_line(Line* line, char* out, std::size_t capacity) {
    skip_spaces(line);
    std::size_t length = std::strlen(line->cursor);
    while (length > 0 && is_space(line->cursor[length - 1]))
        --length;
    if (length >= capacity)
        return false;
    std::memcpy(out, line->cursor, length);
    out[length] = '\0';
    return true;
}

bool at_end(Line* line) {
    skip_spaces(line);
    return *line->cursor == '\0';
}

bool copy_text(char* out, std::size_t capacity, const char* text) {
    const std::size_t length = std::strlen(text);
    if (length >= capacity)
        return false;
    std::memcpy(out, text, length + 1);
    return true;
}

bool parse_feed(Line* line, Feed* feed, int32_t delay) {
    if (feed->present || !next_word(line, feed->text, sizeof feed->text) || !at_end(line))
        return false;
    feed->present = true;
    feed->delay = delay;
    return true;
}

// The next word without consuming it.
bool peek_word(const Line* line, char* out, std::size_t capacity) {
    Line copy = *line;
    return next_word(&copy, out, capacity);
}

// AREA ROOM USERS TYPE NEEDS_PW [global] NAME|DESCRIPTION|TAG, the
// description and tag optional. global marks the sector-wide frequency.
bool parse_room_fields(Line* line, RoomFields* f) {
    int32_t needs_password = 0;
    char text[kLineCapacity];
    f->global_known = false;
    f->global = 0;
    if (!next_id(line, &f->area_id) || !next_id(line, &f->room_id) || !next_int(line, &f->users) ||
        !next_int(line, &f->type) || !next_int(line, &needs_password))
        return false;
    char word[0x10];
    if (peek_word(line, word, sizeof word) && std::strcmp(word, "global") == 0) {
        next_word(line, word, sizeof word);
        f->global_known = true;
        f->global = 1;
    }
    if (!rest_of_line(line, text, sizeof text))
        return false;
    f->needs_password = needs_password != 0;
    char* description = std::strchr(text, '|');
    char* tag = nullptr;
    if (description != nullptr) {
        *description++ = '\0';
        tag = std::strchr(description, '|');
        if (tag != nullptr) {
            *tag++ = '\0';
            if (std::strchr(tag, '|') != nullptr)
                return false;
        }
    }
    return text[0] != '\0' && copy_text(f->name, sizeof f->name, text) &&
           copy_text(f->description, sizeof f->description, description ? description : "") &&
           copy_text(f->tag, sizeof f->tag, tag ? tag : "");
}

// TEXT[|NOTIFICATION_TYPE], the text not empty.
bool parse_alert_text(Line* line, Alert* a) {
    char text[kLineCapacity];
    if (!rest_of_line(line, text, sizeof text))
        return false;
    char* type = std::strchr(text, '|');
    if (type != nullptr)
        *type++ = '\0';
    return text[0] != '\0' && copy_text(a->text, sizeof a->text, text) &&
           copy_text(a->notification_type, sizeof a->notification_type, type ? type : "");
}

// What is wrong with a field entry: its shape, an int's value, a text's
// length or the type.
struct FieldProblems {
    const char* shape;
    const char* number;
    const char* text;
    const char* type;
};

constexpr FieldProblems kDossierProblems{
    "dossier takes KEY int VALUE or KEY string TEXT",
    "a dossier int takes one number",
    "dossier text too long",
    "a dossier field is int or string"
};
constexpr FieldProblems kProfileProblems{
    "profile takes USER_ID KEY int VALUE or USER_ID KEY string TEXT",
    "a profile int takes one number",
    "profile text too long",
    "a profile field is int or string"
};
constexpr FieldProblems kLadderProblems{
    "ladder_profile takes USER_ID LADDER_ID SUBID KEY int VALUE or ... KEY string TEXT",
    "a ladder_profile int takes one number",
    "ladder_profile text too long",
    "a ladder_profile field is int or string"
};

// KEY int VALUE or KEY string TEXT; null when read, otherwise the problem.
const char* parse_field(Line* line, TypedField* f, const FieldProblems& problems) {
    char type[0x10];
    if (!next_word(line, f->key, sizeof f->key) || !next_word(line, type, sizeof type))
        return problems.shape;
    if (std::strcmp(type, "int") == 0) {
        f->is_int = true;
        if (!next_int(line, &f->number) || !at_end(line))
            return problems.number;
        return nullptr;
    }
    if (std::strcmp(type, "string") == 0)
        return rest_of_line(line, f->text, sizeof f->text) ? nullptr : problems.text;
    return problems.type;
}

// The user's profile answer, added with the delay in force at its first
// line; null when the script holds too many.
ProfileAnswer* profile_answer(Script* s, uint32_t user_id, int32_t delay, std::size_t* index) {
    for (std::size_t i = 0; i < s->profile_count; ++i) {
        if (s->profiles[i].user_id == user_id) {
            *index = i;
            return &s->profiles[i];
        }
    }
    if (s->profile_count == kMaxProfiles)
        return nullptr;
    *index = s->profile_count;
    s->profiles[s->profile_count] = ProfileAnswer{user_id, delay};
    return &s->profiles[s->profile_count++];
}

// A profile or ladder_profile entry: USER_ID [LADDER_ID SUBID] and a field.
const char* parse_profile_line(Script* s, Line* line, bool ladder, int32_t delay) {
    const FieldProblems& problems = ladder ? kLadderProblems : kProfileProblems;
    if (s->profile_line_count == kMaxProfileLines)
        return "too many profile and ladder_profile fields";
    ProfileLine& p = s->profile_lines[s->profile_line_count];
    uint32_t user_id = 0;
    int32_t ladder_id = 0;
    int32_t ladder_subid = 0;
    if (!next_id(line, &user_id) ||
        (ladder && (!next_int(line, &ladder_id) || !next_int(line, &ladder_subid))))
        return problems.shape;
    if (const char* problem = parse_field(line, &p.field, problems))
        return problem;
    if (profile_answer(s, user_id, delay, &p.answer) == nullptr)
        return "too many profile answers";
    p.ladder = ladder;
    if (ladder) {
        std::size_t at = 0;
        while (at < s->ladder_count &&
               (s->ladders[at].answer != p.answer || s->ladders[at].ladder_id != ladder_id ||
                s->ladders[at].ladder_subid != ladder_subid))
            ++at;
        if (at == s->ladder_count) {
            if (s->ladder_count == kMaxLadders)
                return "too many ladder_profile answers";
            s->ladders[s->ladder_count++] = LadderAnswer{p.answer, ladder_id, ladder_subid};
        }
        p.ladder_index = at;
    }
    ++s->profile_line_count;
    return nullptr;
}

// Directory of the replay being read, which a relative download file names
// a file in.
std::filesystem::path g_parse_directory;

// file_server TYPE HOST PORT, TYPE the record's news or ladder.
const char* parse_file_server(Script* s, Line* line, int32_t delay) {
    if (s->file_server_count == kMaxFileServers)
        return "too many file_server entries";
    FileServer& f = s->file_servers[s->file_server_count];
    char type[0x10];
    int32_t port = 0;
    if (!next_word(line, type, sizeof type) || !next_word(line, f.host, sizeof f.host) ||
        !next_int(line, &port) || port <= 0 || port > UINT16_MAX || !at_end(line))
        return "file_server takes news|ladder HOST PORT";
    f.type_index = -1;
    for (int32_t i = 0; i < kFileServerCount; ++i)
        if (std::strcmp(type, kFileServerTypes[i]) == 0)
            f.type_index = i;
    if (f.type_index < 0)
        return "a file_server's type is news or ladder";
    f.port = static_cast<uint16_t>(port);
    f.delay = delay;
    ++s->file_server_count;
    return nullptr;
}

// http URL LOCALFILE, the file relative to the replay's directory.
const char* parse_http(Script* s, Line* line, int32_t delay) {
    if (s->http_count == kMaxHttp)
        return "too many http entries";
    Http& h = s->http[s->http_count];
    char local[kLocalPathCapacity];
    if (!next_word(line, h.url, sizeof h.url) || !next_word(line, local, sizeof local) ||
        !at_end(line))
        return "http takes URL LOCALFILE";
    std::filesystem::path path(local);
    if (path.is_relative())
        path = g_parse_directory / path;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        return "an http entry's file does not exist";
    if (!copy_text(h.local, sizeof h.local, path.string().c_str()))
        return "an http entry's file path is too long";
    h.delay = delay;
    ++s->http_count;
    return nullptr;
}

// HOST of a battle line: A, C or U.
bool parse_host_code(const char* word, int32_t* out) {
    if (word[0] == '\0' || word[1] != '\0')
        return false;
    switch (word[0]) {
    case 'A':
    case 'a':
        *out = kHostArm;
        return true;
    case 'C':
    case 'c':
        *out = kHostCore;
        return true;
    case 'U':
    case 'u':
        *out = kHostUnknown;
        return true;
    default:
        return false;
    }
}

// Reads one entry; null on success, otherwise what is wrong with it.
const char* parse_entry(Script* s, Line* line, ParseState* state) {
    char command[0x20];
    if (!next_word(line, command, sizeof command))
        return nullptr;
    const int32_t delay = state->delay;
    if (std::strcmp(command, "delay") == 0) {
        int32_t pumps = 0;
        if (!next_int(line, &pumps) || pumps < 0 || pumps > kMaxDelay || !at_end(line))
            return "delay takes a pump count from 0 to 10000";
        state->delay = pumps;
        return nullptr;
    }
    if (std::strcmp(command, "progress") == 0) {
        if (s->progress_count == kMaxProgress)
            return "too many progress entries";
        Progress& p = s->progress[s->progress_count];
        if (!next_int(line, &p.stage) || !next_int(line, &p.result) ||
            !next_int(line, &p.detail_a) || !next_int(line, &p.detail_b) ||
            !rest_of_line(line, p.message, sizeof p.message))
            return "progress takes STAGE RESULT DETAIL_A DETAIL_B [MESSAGE]";
        p.has_message = p.message[0] != '\0';
        p.delay = delay;
        ++s->progress_count;
        return nullptr;
    }
    if (std::strcmp(command, "dossier") == 0) {
        if (s->dossier_count == kMaxDossierFields)
            return "too many dossier fields";
        TypedField* field = &s->dossier[s->dossier_count];
        if (const char* problem = parse_field(line, field, kDossierProblems))
            return problem;
        s->dossier_delay = delay;
        ++s->dossier_count;
        return nullptr;
    }
    if (std::strcmp(command, "profile") == 0)
        return parse_profile_line(s, line, false, delay);
    if (std::strcmp(command, "ladder_profile") == 0)
        return parse_profile_line(s, line, true, delay);
    if (std::strcmp(command, "area_name") == 0) {
        if (s->has_area_name || !next_word(line, s->area_name, sizeof s->area_name) ||
            !at_end(line))
            return "area_name takes one name, once";
        s->has_area_name = true;
        return nullptr;
    }
    if (std::strcmp(command, "allegiances") == 0)
        return parse_feed(line, &s->allegiances, delay) ? nullptr
                                                        : "allegiances takes one word, once";
    if (std::strcmp(command, "activity") == 0)
        return parse_feed(line, &s->activity, delay) ? nullptr : "activity takes one word, once";
    if (std::strcmp(command, "census") == 0) {
        if (s->census_count == kMaxCensus)
            return "too many census entries";
        Census& c = s->census[s->census_count];
        bool ok = next_id(line, &c.area_id);
        for (int32_t& value : c.values)
            ok = ok && next_int(line, &value);
        if (!ok || !at_end(line))
            return "census takes AREA and six numbers";
        c.delay = delay;
        ++s->census_count;
        return nullptr;
    }
    if (std::strcmp(command, "located") == 0) {
        if (s->located_count == kMaxLocated)
            return "too many located answers";
        Located& l = s->located[s->located_count];
        if (!next_word(line, l.name, sizeof l.name) || !next_int(line, &l.code) ||
            !next_id(line, &l.user_id) || !next_int(line, &l.icon) || !at_end(line))
            return "located takes NAME CODE USER_ID ICON";
        l.delay = delay;
        ++s->located_count;
        return nullptr;
    }
    if (std::strcmp(command, "located_field") == 0) {
        if (s->located_count == 0)
            return "located_field follows a located answer";
        Located& l = s->located[s->located_count - 1];
        if (l.field_count == kMaxLocatedFields)
            return "too many located fields";
        LocatedField& f = l.fields[l.field_count];
        char type[0x10];
        if (!next_word(line, f.key, sizeof f.key) || !next_word(line, type, sizeof type))
            return "located_field takes KEY int|uint VALUE or KEY string TEXT";
        if (std::strcmp(type, "int") == 0 || std::strcmp(type, "uint") == 0) {
            f.type = type[0] == 'i' ? FieldType::int32 : FieldType::uint32;
            if (!next_int(line, &f.number) || !at_end(line))
                return "a located number takes one number";
        } else if (std::strcmp(type, "string") == 0) {
            f.type = FieldType::text;
            if (!rest_of_line(line, f.text, sizeof f.text))
                return "located text too long";
        } else {
            return "a located field is int, uint or string";
        }
        ++l.field_count;
        return nullptr;
    }
    if (std::strcmp(command, "room") == 0) {
        if (s->room_count == kMaxRooms)
            return "too many rooms";
        Room& r = s->rooms[s->room_count];
        if (!parse_room_fields(line, &r.fields))
            return "room takes AREA ROOM USERS TYPE NEEDS_PW NAME[|DESCRIPTION[|TAG]]";
        r.delay = delay;
        ++s->room_count;
        return nullptr;
    }
    if (std::strcmp(command, "entered") == 0) {
        if (s->entry_count == kMaxEntries)
            return "too many entered answers";
        Entry& e = s->entries[s->entry_count];
        if (!next_id(line, &e.area_id) || !next_id(line, &e.room_id) ||
            !next_int(line, &e.result) || !at_end(line))
            return "entered takes AREA ROOM RESULT";
        e.delay = delay;
        state->has_answer = true;
        state->answer = AnswerKind::entered;
        state->answer_index = s->entry_count;
        ++s->entry_count;
        return nullptr;
    }
    if (std::strcmp(command, "member") == 0) {
        if (s->member_count == kMaxMembers)
            return "too many members";
        Member& m = s->members[s->member_count];
        if (!next_id(line, &m.area_id) || !next_id(line, &m.room_id) ||
            !next_id(line, &m.user_id) || !next_int(line, &m.latency) || !next_int(line, &m.icon) ||
            !next_int(line, &m.rank) || !next_word(line, m.allegiance, sizeof m.allegiance) ||
            !next_word(line, m.name, sizeof m.name) || !at_end(line))
            return "member takes AREA ROOM USER_ID LATENCY ICON RANK ALLEGIANCE NAME";
        m.delay = delay;
        ++s->member_count;
        return nullptr;
    }
    if (std::strcmp(command, "said") == 0) {
        if (s->said_count == kMaxSaid)
            return "too many said lines";
        Said& l = s->said[s->said_count];
        if (!next_id(line, &l.area_id) || !next_id(line, &l.room_id) ||
            !next_id(line, &l.user_id) || !rest_of_line(line, l.text, sizeof l.text) ||
            l.text[0] == '\0')
            return "said takes AREA ROOM USER_ID TEXT";
        l.delay = delay;
        ++s->said_count;
        return nullptr;
    }
    if (std::strcmp(command, "update") == 0) {
        if (!state->has_answer)
            return "update follows an entered or created answer";
        if (s->update_count == kMaxUpdates)
            return "too many updates";
        Update& u = s->updates[s->update_count];
        if (!next_int(line, &u.change) || !parse_room_fields(line, &u.fields))
            return "update takes CHANGE AREA ROOM USERS TYPE NEEDS_PW NAME[|DESCRIPTION[|TAG]]";
        u.answer = state->answer;
        u.answer_index = state->answer_index;
        u.delay = delay;
        ++s->update_count;
        return nullptr;
    }
    if (std::strcmp(command, "left") == 0) {
        if (s->left_count == kMaxLeft)
            return "too many left answers";
        Left& l = s->left[s->left_count];
        if (!next_id(line, &l.area_id) || !next_id(line, &l.room_id) ||
            !next_int(line, &l.result) || !at_end(line))
            return "left takes AREA ROOM RESULT";
        l.delay = delay;
        ++s->left_count;
        return nullptr;
    }
    if (std::strcmp(command, "created") == 0) {
        if (s->created_count == kMaxCreated)
            return "too many created answers";
        Created& c = s->created[s->created_count];
        if (!next_id(line, &c.area_id) || !next_id(line, &c.room_id) ||
            !rest_of_line(line, c.name, sizeof c.name) || c.name[0] == '\0')
            return "created takes AREA ROOM NAME";
        c.delay = delay;
        state->has_answer = true;
        state->answer = AnswerKind::created;
        state->answer_index = s->created_count;
        ++s->created_count;
        return nullptr;
    }
    if (std::strcmp(command, "page") == 0 || std::strcmp(command, "alert") == 0) {
        if (s->alert_count == kMaxAlerts)
            return "too many alerts";
        Alert& a = s->alerts[s->alert_count];
        a = Alert{};
        if (command[0] == 'p') {
            a.kind = kAlertPage;
            a.has_user = true;
            if (!next_id(line, &a.user_id) || !rest_of_line(line, a.text, sizeof a.text) ||
                a.text[0] == '\0')
                return "page takes USER_ID TEXT";
        } else if (!next_int(line, &a.kind) || !parse_alert_text(line, &a)) {
            return "alert takes KIND TEXT[|NOTIFICATION_TYPE]";
        }
        a.follows_answer = state->has_answer;
        a.answer = state->answer;
        a.answer_index = state->answer_index;
        a.delay = delay;
        ++s->alert_count;
        return nullptr;
    }
    if (std::strcmp(command, "echo") == 0) {
        char word[0x10];
        if (!next_word(line, word, sizeof word) || !at_end(line) ||
            (std::strcmp(word, "on") != 0 && std::strcmp(word, "off") != 0))
            return "echo takes on or off";
        s->echo = word[1] == 'n';
        s->echo_delay = delay;
        return nullptr;
    }
    if (std::strcmp(command, "file_server") == 0)
        return parse_file_server(s, line, delay);
    if (std::strcmp(command, "http") == 0)
        return parse_http(s, line, delay);
    if (std::strcmp(command, "battle") == 0) {
        if (s->battle_count == kMaxBattles)
            return "too many battles";
        Battle& b = s->battles[s->battle_count];
        b = Battle{};
        char host[8];
        if (!next_id(line, &b.area_id) || !next_word(line, b.game_id, sizeof b.game_id) ||
            !next_int(line, &b.type) || !next_int(line, &b.forming) ||
            !next_word(line, host, sizeof host) || !parse_host_code(host, &b.host_code))
            return "battle takes AREA GAME_ID TYPE FORMING HOST [pass] [seats N M] NAME";
        for (;;) {
            char word[0x10];
            if (!peek_word(line, word, sizeof word))
                break;
            if (std::strcmp(word, "pass") == 0) {
                next_word(line, word, sizeof word);
                b.pass_required = true;
                continue;
            }
            if (std::strcmp(word, "seats") == 0) {
                next_word(line, word, sizeof word);
                if (!next_int(line, &b.players) || !next_int(line, &b.playerlimit))
                    return "battle seats take N M";
                b.seats_known = true;
                continue;
            }
            break;
        }
        if (!rest_of_line(line, b.name, sizeof b.name) || b.name[0] == '\0')
            return "battle takes AREA GAME_ID TYPE FORMING HOST [pass] [seats N M] NAME";
        b.delay = delay;
        ++s->battle_count;
        return nullptr;
    }
    if (std::strcmp(command, "launch") == 0) {
        if (s->launch_count == kMaxLaunches)
            return "too many launch entries";
        Launch& l = s->launches[s->launch_count];
        if (!rest_of_line(line, l.text, sizeof l.text) || l.text[0] != '{')
            return "launch takes a record";
        l.delay = delay;
        state->has_launch = true;
        state->launch_index = s->launch_count;
        ++s->launch_count;
        return nullptr;
    }
    if (std::strcmp(command, "record") == 0) {
        if (!state->has_launch)
            return "record follows a launch";
        if (s->inbound_count == kMaxInbound)
            return "too many record entries";
        InboundRecord& r = s->inbound[s->inbound_count];
        if (!rest_of_line(line, r.text, sizeof r.text) || r.text[0] != '{')
            return "record takes a record";
        r.launch = state->launch_index;
        r.delay = delay;
        ++s->inbound_count;
        return nullptr;
    }
    if (std::strcmp(command, "reply") == 0 || std::strcmp(command, "then") == 0) {
        const bool chained = std::strcmp(command, "then") == 0;
        if (chained && !state->has_match_reply)
            return "then follows a reply";
        if (s->match_count == kMaxMatchReplies)
            return "too many reply entries";
        MatchReply& match = s->match_replies[s->match_count];
        if (!rest_of_line(line, match.text, sizeof match.text) || match.text[0] != '{')
            return chained ? "then takes a record" : "reply takes a record";
        match.delay = delay;
        match.chained = chained;
        state->has_match_reply = true;
        ++s->match_count;
        return nullptr;
    }
    return "unknown entry";
}

bool parse_file(const char* path, Script* s, char* error, std::size_t capacity) {
    g_parse_directory = std::filesystem::path(path).parent_path();
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr) {
        std::snprintf(error, capacity, "%s: cannot open", path);
        return false;
    }
    char buffer[kLineCapacity];
    std::size_t number = 0;
    ParseState state{kDefaultDelay, false, AnswerKind::entered, 0, false, 0, false};
    const char* problem = nullptr;
    while (problem == nullptr && std::fgets(buffer, sizeof buffer, file) != nullptr) {
        ++number;
        const std::size_t length = std::strlen(buffer);
        if (length + 1 == sizeof buffer && buffer[length - 1] != '\n' && !std::feof(file)) {
            problem = "line too long";
            break;
        }
        Line line{buffer};
        skip_spaces(&line);
        if (*line.cursor == '#' || *line.cursor == '\0')
            continue;
        problem = parse_entry(s, &line, &state);
    }
    std::fclose(file);
    if (problem != nullptr) {
        std::snprintf(error, capacity, "%s:%zu: %s", path, number, problem);
        return false;
    }
    return true;
}

// ---- sends -----------------------------------------------------------------

// Records a request; kNoSend once the record is full.
std::size_t record_send(const ReplaySent& sent) {
    if (g_sent_count == kMaxSent)
        return kNoSend;
    g_sent[g_sent_count] = sent;
    return g_sent_count++;
}

void copy_bounded(char* out, std::size_t capacity, const char* text) {
    utf8::copy(out, capacity, text, 0);
}

// ---- delivery ------------------------------------------------------------

RoverString rover_text(const char* text) {
    RoverString string{};
    rover_string_assign(&string, text, static_cast<uint32_t>(std::strlen(text)));
    return string;
}

void set_text(RoverPropertyBag* bag, const char* key, const char* text) {
    RoverString name = rover_text(key);
    RoverString value = rover_text(text);
    g_real->set_bag_string(bag, &name, &value);
    rover_string_release(&value, true);
    rover_string_release(&name, true);
}

void set_uint(RoverPropertyBag* bag, const char* key, uint32_t value) {
    RoverString name = rover_text(key);
    g_real->set_field_uint(bag, &name, value);
    rover_string_release(&name, true);
}

void set_field(RoverPropertyBag* bag, const TypedField& f) {
    if (!f.is_int) {
        set_text(bag, f.key, f.text);
        return;
    }
    RoverString key = rover_text(f.key);
    g_real->set_field_int(bag, &key, f.number);
    rover_string_release(&key, true);
}

// The dossier as Rover hands it over: a bag of its fields and the user id,
// released once the callback returns.
void deliver_dossier() {
    RoverPropertyBag* bag = g_real->create_property_bag();
    set_uint(bag, kUserIdKey, g_user_id);
    for (std::size_t i = 0; i < g_script.dossier_count; ++i)
        set_field(bag, g_script.dossier[i]);
    if (g_profile != nullptr)
        g_profile(true, bag);
    g_real->destroy_property_bag(bag);
}

// A recorded profile as oa-rover's profile handler hands it over: each
// ladder standing in a bag of its own, then the profile, every bag holding
// the user id and released once its callback returns.
void deliver_profile(std::size_t index) {
    const uint32_t user_id = g_script.profiles[index].user_id;
    for (std::size_t l = 0; l < g_script.ladder_count; ++l) {
        const LadderAnswer& ladder = g_script.ladders[l];
        if (ladder.answer != index)
            continue;
        RoverPropertyBag* bag = g_real->create_property_bag();
        set_uint(bag, kUserIdKey, user_id);
        for (std::size_t i = 0; i < g_script.profile_line_count; ++i) {
            const ProfileLine& p = g_script.profile_lines[i];
            if (p.ladder && p.ladder_index == l)
                set_field(bag, p.field);
        }
        if (g_ladder != nullptr)
            g_ladder(ladder.ladder_id, ladder.ladder_subid, bag);
        g_real->destroy_property_bag(bag);
    }
    RoverPropertyBag* bag = g_real->create_property_bag();
    set_uint(bag, kUserIdKey, user_id);
    for (std::size_t i = 0; i < g_script.profile_line_count; ++i) {
        const ProfileLine& p = g_script.profile_lines[i];
        if (!p.ladder && p.answer == index)
            set_field(bag, p.field);
    }
    if (g_profile != nullptr)
        g_profile(true, bag);
    g_real->destroy_property_bag(bag);
}

// The answer as oa-rover's user_located handler hands it over: its fields
// other than the code, id, name and icon, command included, in a bag
// released once the callback returns.
void deliver_located(const Located& l) {
    RoverPropertyBag* bag = g_real->create_property_bag();
    set_text(bag, "command", kLocatedCommand);
    for (std::size_t i = 0; i < l.field_count; ++i) {
        const LocatedField& f = l.fields[i];
        if (f.type == FieldType::text) {
            set_text(bag, f.key, f.text);
            continue;
        }
        RoverString key = rover_text(f.key);
        if (f.type == FieldType::int32)
            g_real->set_field_int(bag, &key, f.number);
        else
            g_real->set_field_uint(bag, &key, static_cast<uint32_t>(f.number));
        rover_string_release(&key, true);
    }
    if (g_locate != nullptr)
        g_locate(l.code, l.user_id, l.name, l.icon, bag);
    g_real->destroy_property_bag(bag);
}

// An alert as oa-rover's system_alert handler hands it over: the kind, the
// text, and a bag of the command and a page's user_id or a notification's
// notification_type.
void deliver_alert(const Alert& a) {
    RoverPropertyBag* bag = g_real->create_property_bag();
    set_text(bag, "command", kAlertCommand);
    if (a.has_user)
        set_uint(bag, kUserIdKey, a.user_id);
    if (a.notification_type[0] != '\0')
        set_text(bag, kNotificationTypeKey, a.notification_type);
    if (g_alert != nullptr)
        g_alert(a.kind, a.text, bag);
    g_real->destroy_property_bag(bag);
}

// A room-list row; the last recorded row of its area ends the listing.
void deliver_room(std::size_t index) {
    const RoomFields& r = g_script.rooms[index].fields;
    bool last = true;
    for (std::size_t i = index + 1; i < g_script.room_count; ++i)
        last = last && g_script.rooms[i].fields.area_id != r.area_id;
    rover_note_room_global(r.global_known, r.global);
    if (g_room_list != nullptr)
        g_room_list(
            kRoomAdded,
            static_cast<int32_t>(r.area_id),
            static_cast<int32_t>(r.room_id),
            r.name,
            r.description,
            r.needs_password,
            r.users,
            r.type,
            r.tag,
            last
        );
    int32_t dropped_global = 0;
    (void)rover_take_room_global(&dropped_global);
}

bool in_room(uint32_t area_id, uint32_t room_id) {
    return g_room.area_id == area_id && g_room.room_id == room_id;
}

// The room-list updates and alerts recorded after an answer.
void queue_updates(AnswerKind answer, std::size_t index) {
    for (std::size_t i = 0; i < g_script.update_count; ++i) {
        const Update& u = g_script.updates[i];
        if (u.answer == answer && u.answer_index == index)
            queue(ReplyKind::update, i, u.delay);
    }
    for (std::size_t i = 0; i < g_script.alert_count; ++i) {
        const Alert& a = g_script.alerts[i];
        if (a.follows_answer && a.answer == answer && a.answer_index == index)
            queue(ReplyKind::alert, i, a.delay);
    }
}

// The answer to the entry Rover waits for: a switch moves the room feeds to
// the new room first. Once entered, the room's members arrive, then what
// was said there, then the room-list updates recorded after the answer.
void deliver_entered(std::size_t index) {
    const Entry& e = g_script.entries[index];
    if (g_entering.feeds.area_id != e.area_id || g_entering.feeds.room_id != e.room_id)
        return;
    const Entering entering = g_entering;
    g_entering = Entering{};
    if (entering.switching && e.result == kEnteredOk)
        g_room = entering.feeds;
    if (entering.entered != nullptr)
        entering.entered(e.result);
    if (e.result != kEnteredOk)
        return;
    for (std::size_t i = 0; i < g_script.member_count; ++i) {
        const Member& m = g_script.members[i];
        if (m.area_id == e.area_id && m.room_id == e.room_id)
            queue(ReplyKind::member, i, m.delay);
    }
    for (std::size_t i = 0; i < g_script.said_count; ++i) {
        const Said& l = g_script.said[i];
        if (l.area_id == e.area_id && l.room_id == e.room_id)
            queue(ReplyKind::said, i, l.delay);
    }
    queue_updates(AnswerKind::entered, index);
}

void deliver_update(const Update& u) {
    const RoomFields& r = u.fields;
    rover_note_room_global(r.global_known, r.global);
    if (g_room.room_update != nullptr && g_room.area_id == r.area_id)
        g_room.room_update(
            u.change,
            static_cast<int32_t>(r.area_id),
            static_cast<int32_t>(r.room_id),
            r.name,
            r.description,
            r.needs_password,
            r.users,
            r.type,
            r.tag
        );
    int32_t dropped_global = 0;
    (void)rover_take_room_global(&dropped_global);
}

void deliver_left(const Left& l) {
    if (g_leaving.left == nullptr || g_leaving.area_id != l.area_id ||
        g_leaving.room_id != l.room_id)
        return;
    const RoverRoomLeftFn left = g_leaving.left;
    g_leaving = Leaving{};
    left(l.result != 0);
}

// Whatever the answer, the creation is no longer awaited; a frequency made
// brings the room-list updates recorded after the answer.
void deliver_created(std::size_t index) {
    const Created& c = g_script.created[index];
    const RoverRoomCreatedFn created = g_created;
    g_created = nullptr;
    uint32_t area_id = c.area_id;
    uint32_t room_id = c.room_id;
    if (created != nullptr)
        created(&area_id, &room_id, c.name);
    if (c.area_id != 0 && c.room_id != 0)
        queue_updates(AnswerKind::created, index);
}

// As oa-rover's file_server handler keeps it for slot 40.
void deliver_file_server(const FileServer& f) {
    RoverFileServer& server = g_file_servers[f.type_index];
    server.type_index = f.type_index;
    server.port = f.port;
    copy_bounded(server.host, sizeof server.host, f.host);
}

// The recorded file's bytes replace the destination's, as the fetch worker
// saves a body; an unrecorded URL leaves the destination as it was, as a
// failed fetch does. Either way the completion follows with Rover's status.
void deliver_fetched(const Fetch& fetch) {
    if (fetch.http != kNoHttp) {
        std::error_code error;
        std::filesystem::copy_file(
            g_script.http[fetch.http].local,
            fetch.destination,
            std::filesystem::copy_options::overwrite_existing,
            error
        );
    }
    if (g_http_complete != nullptr)
        g_http_complete(fetch.url, fetch.destination, kFetchDone);
}

/// Delivers a record to Rover as it receives one from the Boneyards: parsed
/// from its text and queued for Rover's pump, which hands it to the handler it
/// is for. A text Rover refuses is dropped.
void deliver_record(const char* text) {
    if (g_rover_state == nullptr)
        return;
    RoverRecord* pending = nullptr;
    RoverRecord* record = nullptr;
    int32_t consumed = 0;
    const RoverRecordStatus status = rover_record_receive(
        &pending, text, static_cast<int32_t>(std::strlen(text)), &consumed, &record
    );
    if (status != RoverRecordStatus::Ok || record == nullptr) {
        rover_record_destroy(&pending);
        rover_record_destroy(&record);
        return;
    }
    rover_queue_push(g_rover_state->inbound, record);
}

// One battle_info row of the list enter_battles opened.
void deliver_battle(std::size_t index) {
    const Battle& b = g_script.battles[index];
    if (g_battle_info == nullptr || b.area_id != g_battles_area)
        return;
    g_battle_info(kBattleAdded, b.game_id, b.name, b.type, b.forming, b.host_code);
}

// The list is entered. The callback is dropped first, so a list it opens is
// not answered twice.
void deliver_battles_entered() {
    const RoverBattlesDoneFn done = g_battles_entered;
    g_battles_entered = nullptr;
    if (done != nullptr)
        done(false);
}

// The list is left. The callback is dropped first, as the entered one is.
void deliver_battles_left() {
    const RoverBattlesDoneFn left = g_battles_left;
    g_battles_left = nullptr;
    if (left != nullptr)
        left(false);
}

// get_battle's intel when the file records none: the battle has no password,
// and no host or mission is named. The callback is dropped first.
void deliver_battle_intel() {
    const RoverBattleIntelFn intel = g_battle_intel;
    char game_id[kNameCapacity];
    copy_bounded(game_id, sizeof game_id, g_intel_game);
    g_battle_intel = nullptr;
    if (intel != nullptr)
        intel(game_id, 0, "", 0, "", 0, 0, kIntelOnDeath, false);
}

// One answer to the battle search: the hit at `index`, the last marked as
// ending the search, or the closing row when nothing matched.
void deliver_game_located(std::size_t index) {
    const RoverGameLocatedFn located = g_game_located;
    if (located == nullptr)
        return;
    if (g_game_hit_count == 0) {
        g_game_located = nullptr;
        rover_note_game_seats(false, 0, 0);
        located(0, "", "", 0, 0, 0, 1);
        int32_t dropped_players = 0;
        int32_t dropped_limit = 0;
        (void)rover_take_game_seats(&dropped_players, &dropped_limit);
        return;
    }
    if (index >= g_game_hit_count)
        return;
    const Battle& b = g_script.battles[g_game_hits[index]];
    const bool last = index + 1 == g_game_hit_count;
    if (last)
        g_game_located = nullptr;
    rover_note_game_seats(b.seats_known, b.players, b.playerlimit);
    located(
        b.area_id,
        b.game_id,
        b.name,
        b.type,
        b.forming != 0 ? 1 : 0,
        b.pass_required ? 1 : 0,
        last ? 1 : 0
    );
    int32_t dropped_players = 0;
    int32_t dropped_limit = 0;
    (void)rover_take_game_seats(&dropped_players, &dropped_limit);
}

/// Delivers a launch line's record and queues the records that follow it in the file.
void deliver_launch(std::size_t index) {
    deliver_record(g_script.launches[index].text);
    for (std::size_t i = 0; i < g_script.inbound_count; ++i)
        if (g_script.inbound[i].launch == index)
            queue(ReplyKind::inbound, i, g_script.inbound[i].delay);
}

void answer_matchmaking();

/// Records, with its text, every record Rover queued for the Boneyards since
/// the last pump, taking each as the Boneyards would. A matchmaking or QuickPlay
/// command takes the next reply line, and the then lines chained to it.
void record_outbound() {
    if (g_rover_state == nullptr)
        return;
    for (RoverEventQueue& queue : g_rover_state->outbound) {
        while (RoverRecord* record = rover_queue_pop(queue)) {
            ReplaySent sent{};
            sent.request = ReplayRequest::record;
            int32_t length = 0;
            if (rover_record_format(*record, sent.text, sizeof sent.text - 1, &length) ==
                RoverRecordStatus::Ok) {
                sent.text[length] = '\0';
                if (std::strstr(sent.text, "command=matchmaking_") != nullptr ||
                    std::strstr(sent.text, "command=quickplay_") != nullptr)
                    answer_matchmaking();
            }
            rover_record_destroy(&record);
            record_send(sent);
        }
    }
}

void deliver(const Reply& reply) {
    switch (reply.kind) {
    case ReplyKind::progress: {
        const Progress& p = g_script.progress[reply.index];
        if (p.stage == kLoginResultStage && p.result == kLoginAccepted)
            g_user_id = static_cast<uint32_t>(p.detail_a);
        if (g_progress != nullptr)
            g_progress(
                p.stage, p.result, p.detail_a, p.detail_b, p.has_message ? p.message : nullptr
            );
        break;
    }
    case ReplyKind::dossier:
        deliver_dossier();
        break;
    case ReplyKind::allegiances:
        if (g_allegiance != nullptr)
            g_allegiance(g_script.allegiances.text);
        break;
    case ReplyKind::census: {
        const Census& c = g_script.census[reply.index];
        if (g_census != nullptr)
            g_census(
                c.area_id,
                c.values[0],
                c.values[1],
                c.values[2],
                c.values[3],
                c.values[4],
                c.values[5]
            );
        break;
    }
    case ReplyKind::activity:
        if (g_activity != nullptr)
            g_activity(g_script.activity.text);
        break;
    case ReplyKind::located:
        deliver_located(g_script.located[reply.index]);
        break;
    case ReplyKind::room:
        deliver_room(reply.index);
        break;
    case ReplyKind::entered:
        deliver_entered(reply.index);
        break;
    case ReplyKind::member: {
        const Member& m = g_script.members[reply.index];
        if (in_room(m.area_id, m.room_id) && g_room.room_info != nullptr)
            g_room.room_info(
                kRoomAdded,
                static_cast<int32_t>(m.user_id),
                m.name,
                m.latency,
                m.icon,
                m.rank,
                m.allegiance
            );
        break;
    }
    case ReplyKind::said: {
        // A line naming no user comes from the server.
        const Said& l = g_script.said[reply.index];
        if (in_room(l.area_id, l.room_id) && g_room.chat_msg != nullptr)
            g_room.chat_msg(l.user_id == 0 ? 1 : 0, static_cast<int32_t>(l.user_id), l.text);
        break;
    }
    case ReplyKind::update:
        deliver_update(g_script.updates[reply.index]);
        break;
    case ReplyKind::left:
        deliver_left(g_script.left[reply.index]);
        break;
    case ReplyKind::created:
        deliver_created(reply.index);
        break;
    case ReplyKind::alert:
        deliver_alert(g_script.alerts[reply.index]);
        break;
    case ReplyKind::echo: {
        const ReplaySent& s = g_sent[reply.index];
        if (in_room(s.area_id, s.room_id) && g_room.chat_msg != nullptr)
            g_room.chat_msg(0, static_cast<int32_t>(g_user_id), s.text);
        break;
    }
    case ReplyKind::logout:
        if (g_logout != nullptr)
            g_logout();
        break;
    case ReplyKind::profile:
        deliver_profile(reply.index);
        break;
    case ReplyKind::file_server:
        deliver_file_server(g_script.file_servers[reply.index]);
        break;
    case ReplyKind::fetched:
        deliver_fetched(g_fetches[reply.index]);
        break;
    case ReplyKind::launch:
        deliver_launch(reply.index);
        break;
    case ReplyKind::inbound:
        deliver_record(g_script.inbound[reply.index].text);
        break;
    case ReplyKind::battle:
        deliver_battle(reply.index);
        break;
    case ReplyKind::battles_entered:
        deliver_battles_entered();
        break;
    case ReplyKind::battles_left:
        deliver_battles_left();
        break;
    case ReplyKind::battle_intel:
        deliver_battle_intel();
        break;
    case ReplyKind::game_located:
        deliver_game_located(reply.index);
        break;
    case ReplyKind::match_reply:
        deliver_record(g_script.match_replies[reply.index].text);
        break;
    }
}

// ---- the table's replacements ---------------------------------------------

void ROVER_CALL replay_pump_events() {
    ++g_pumps;
    record_outbound();
    // Replies queued while delivering are considered in the same pass when
    // already due.
    for (std::size_t delivered = 0; delivered < kQueueCapacity; ++delivered) {
        std::size_t at = 0;
        while (at < g_queue_count && g_queue[at].due > g_pumps)
            ++at;
        if (at == g_queue_count)
            break;
        const Reply reply = g_queue[at];
        std::memmove(&g_queue[at], &g_queue[at + 1], sizeof g_queue[0] * (g_queue_count - at - 1));
        --g_queue_count;
        deliver(reply);
    }
    g_real->pump_events();
}

bool ROVER_CALL replay_reset_session() {
    return true;
}

// The login's progress reports, then the alerts no answer brings, each
// after its delay.
bool ROVER_CALL replay_start_login(
    const char*,
    const char*,
    const char*,
    RoverLoginProgressFn progress,
    RoverSystemAlertFn system_alert,
    RoverDisconnectFn
) {
    g_progress = progress;
    g_alert = system_alert;
    for (std::size_t i = 0; i < g_script.progress_count; ++i)
        queue(ReplyKind::progress, i, g_script.progress[i].delay);
    for (std::size_t i = 0; i < g_script.alert_count; ++i)
        if (!g_script.alerts[i].follows_answer)
            queue(ReplyKind::alert, i, g_script.alerts[i].delay);
    for (std::size_t i = 0; i < g_script.file_server_count; ++i)
        queue(ReplyKind::file_server, i, g_script.file_servers[i].delay);
    return true;
}

// Slot 40 as oa-rover answers it: a type other than news or ladder only
// sets the invalid index; a server not yet announced is Rover's invalid
// record.
void ROVER_CALL replay_get_file_server(int32_t server_type, RoverFileServer* out_server) {
    if (server_type < 0 || server_type >= kFileServerCount) {
        out_server->type_index = OA_ROVER_FILE_SERVER_INVALID;
        return;
    }
    const RoverFileServer& server = g_file_servers[server_type];
    out_server->type_index = server.type_index;
    copy_bounded(out_server->host, sizeof out_server->host, server.host);
    out_server->port = server.port;
}

// Slot 32: the latest completion callback is the one held; the recorded
// download for the URL answers after its delay, any other URL after one
// pump.
bool ROVER_CALL replay_fetch_http_file(
    const char* url, const char* dest_path, RoverHttpCompleteFn complete, RoverHttpProgressFn
) {
    g_http_complete = complete;
    ReplaySent sent{};
    sent.request = ReplayRequest::fetch;
    copy_bounded(sent.text, sizeof sent.text, url);
    copy_bounded(sent.path, sizeof sent.path, dest_path);
    record_send(sent);
    if (g_fetch_count == kMaxFetches)
        return true;
    Fetch& fetch = g_fetches[g_fetch_count];
    copy_bounded(fetch.url, sizeof fetch.url, url);
    copy_bounded(fetch.destination, sizeof fetch.destination, dest_path);
    fetch.http = kNoHttp;
    int32_t delay = kDefaultDelay;
    for (std::size_t i = 0; i < g_script.http_count; ++i) {
        if (std::strcmp(g_script.http[i].url, fetch.url) == 0) {
            fetch.http = i;
            delay = g_script.http[i].delay;
            break;
        }
    }
    queue(ReplyKind::fetched, g_fetch_count++, delay);
    return true;
}

bool ROVER_CALL replay_send_logout(int32_t, RoverLogoutFn logged_out) {
    g_logout = logged_out;
    queue(ReplyKind::logout, 0, kDefaultDelay);
    return true;
}

// As Rover keeps one profile request, a new one drops the answer still
// queued for the last. The user's recorded profile answers; without one the
// player's own falls back to the dossier, and anyone else gets no answer.
void ROVER_CALL
replay_request_get_profile(uint32_t user_id, RoverProfileFn profile, RoverLadderProfileFn ladder) {
    g_profile = profile;
    g_ladder = ladder;
    drop_queued(ReplyKind::profile);
    drop_queued(ReplyKind::dossier);
    for (std::size_t i = 0; i < g_script.profile_count; ++i) {
        if (g_script.profiles[i].user_id == user_id) {
            queue(ReplyKind::profile, i, g_script.profiles[i].delay);
            return;
        }
    }
    if (user_id == g_user_id && g_script.dossier_count != 0)
        queue(ReplyKind::dossier, 0, g_script.dossier_delay);
}

void ROVER_CALL replay_request_allegiance(RoverAllegianceFn allegiances) {
    g_allegiance = allegiances;
    if (g_script.allegiances.present)
        queue(ReplyKind::allegiances, 0, g_script.allegiances.delay);
}

void ROVER_CALL
replay_request_census(uint32_t area_id, RoverCensusFn census, RoverActivityFn activity) {
    g_census = census;
    g_activity = activity;
    for (std::size_t i = 0; i < g_script.census_count; ++i)
        if (area_id == 0 || g_script.census[i].area_id == area_id)
            queue(ReplyKind::census, i, g_script.census[i].delay);
    if (area_id == 0 && g_script.activity.present)
        queue(ReplyKind::activity, 0, g_script.activity.delay);
}

bool same_name_any_case(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const auto fold = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (fold(*a) != fold(*b))
            return false;
        if (*a == '\0')
            return true;
    }
}

void ROVER_CALL replay_locate_player(const char* name, RoverLocateFn located) {
    g_locate = located;
    drop_queued(ReplyKind::located);
    if (name == nullptr)
        return;
    for (std::size_t i = 0; i < g_script.located_count; ++i) {
        if (same_name_any_case(g_script.located[i].name, name)) {
            queue(ReplyKind::located, i, g_script.located[i].delay);
            return;
        }
    }
}

// True when `text` holds `part`, whatever the case of either.
bool holds_any_case(const char* text, const char* part) {
    const auto fold = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    if (part == nullptr || part[0] == '\0')
        return true;
    for (const char* start = text; *start != '\0'; ++start) {
        std::size_t i = 0;
        while (part[i] != '\0' && start[i] != '\0' && fold(start[i]) == fold(part[i]))
            ++i;
        if (part[i] == '\0')
            return true;
    }
    return false;
}

/// Answers the battle search (slot 66) from the recorded battles, as the game manager does: every
/// battle whose name holds the text, the last marked as ending the search, or one closing row with
/// no game id when none does. A search still queued is dropped.
void ROVER_CALL replay_locate_target(const char* name, RoverGameLocatedFn located) {
    drop_queued(ReplyKind::game_located);
    g_game_located = located;
    g_game_hit_count = 0;
    int32_t delay = kDefaultDelay;
    for (std::size_t i = 0; i < g_script.battle_count; ++i) {
        const Battle& b = g_script.battles[i];
        if (!holds_any_case(b.name, name != nullptr ? name : ""))
            continue;
        if (g_game_hit_count == 0 || b.delay > delay)
            delay = b.delay;
        g_game_hits[g_game_hit_count++] = i;
    }
    const std::size_t answers = g_game_hit_count > 0 ? g_game_hit_count : 1;
    for (std::size_t i = 0; i < answers; ++i)
        queue(ReplyKind::game_located, i, delay);
}

bool ROVER_CALL replay_get_rooms(int32_t area_id, RoverRoomListFn room_list) {
    g_room_list = room_list;
    for (std::size_t i = 0; i < g_script.room_count; ++i)
        if (g_script.rooms[i].fields.area_id == static_cast<uint32_t>(area_id))
            queue(ReplyKind::room, i, g_script.rooms[i].delay);
    return true;
}

// Entering a room and switching to one are answered alike, with the result
// recorded for the room; an unrecorded room gets no answer.
void answer_entry(const Entering& entering) {
    drop_queued(ReplyKind::entered);
    g_entering = entering;
    for (std::size_t i = 0; i < g_script.entry_count; ++i) {
        const Entry& e = g_script.entries[i];
        if (e.area_id == entering.feeds.area_id && e.room_id == entering.feeds.room_id) {
            queue(ReplyKind::entered, i, e.delay);
            return;
        }
    }
}

bool ROVER_CALL replay_enter_room(
    int32_t room_id,
    int32_t area_id,
    const char*,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_update,
    RoverChatMsgFn chat_msg
) {
    const RoomFeeds feeds{
        static_cast<uint32_t>(area_id),
        static_cast<uint32_t>(room_id),
        room_info,
        room_update,
        chat_msg
    };
    g_room = feeds;
    answer_entry(Entering{false, entered, feeds});
    return true;
}

bool ROVER_CALL replay_switch_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    int32_t old_room_id,
    int32_t old_area_id,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_update,
    RoverChatMsgFn chat_msg
) {
    ReplaySent sent{};
    sent.request = ReplayRequest::switch_room;
    sent.area_id = static_cast<uint32_t>(area_id);
    sent.room_id = static_cast<uint32_t>(room_id);
    sent.old_area_id = static_cast<uint32_t>(old_area_id);
    sent.old_room_id = static_cast<uint32_t>(old_room_id);
    copy_bounded(sent.password, sizeof sent.password, password);
    record_send(sent);
    answer_entry(
        Entering{
            true, entered, RoomFeeds{sent.area_id, sent.room_id, room_info, room_update, chat_msg}
        }
    );
    return true;
}

// The room's feeds are dropped at once; a room other than 0 is left with
// the result recorded for it.
bool ROVER_CALL replay_leave_room(int32_t room_id, int32_t area_id, RoverRoomLeftFn left) {
    g_room = RoomFeeds{};
    if (room_id == 0)
        return true;
    ReplaySent sent{};
    sent.request = ReplayRequest::leave_room;
    sent.area_id = static_cast<uint32_t>(area_id);
    sent.room_id = static_cast<uint32_t>(room_id);
    record_send(sent);
    drop_queued(ReplyKind::left);
    g_leaving = Leaving{sent.area_id, sent.room_id, left};
    for (std::size_t i = 0; i < g_script.left_count; ++i) {
        const Left& l = g_script.left[i];
        if (l.area_id == sent.area_id && l.room_id == sent.room_id) {
            queue(ReplyKind::left, i, l.delay);
            break;
        }
    }
    return true;
}

// With echo on, a line said to the whole room comes back from the user the
// replayed login confirmed; whispers are not echoed.
bool ROVER_CALL
replay_send_chat(int32_t room_id, int32_t area_id, int32_t target_user_id, const char* message) {
    ReplaySent sent{};
    sent.request = ReplayRequest::say;
    sent.area_id = static_cast<uint32_t>(area_id);
    sent.room_id = static_cast<uint32_t>(room_id);
    sent.target_user_id = static_cast<uint32_t>(target_user_id);
    copy_bounded(sent.text, sizeof sent.text, message);
    const std::size_t index = record_send(sent);
    if (g_script.echo && target_user_id == 0 && index != kNoSend)
        queue(ReplyKind::echo, index, g_script.echo_delay);
    return true;
}

void ROVER_CALL replay_send_page_by_name(const char* user_name, const char* message) {
    ReplaySent sent{};
    sent.request = ReplayRequest::page;
    copy_bounded(sent.name, sizeof sent.name, user_name);
    copy_bounded(sent.text, sizeof sent.text, message);
    record_send(sent);
}

// A creation still awaited is dropped; the answer recorded for the name, in
// any case, follows.
void ROVER_CALL replay_create_room(
    uint32_t area_id,
    const char* room_name,
    const char* description,
    const char* password,
    RoverRoomCreatedFn created
) {
    ReplaySent sent{};
    sent.request = ReplayRequest::create_room;
    sent.area_id = area_id;
    copy_bounded(sent.name, sizeof sent.name, room_name);
    copy_bounded(sent.text, sizeof sent.text, description);
    copy_bounded(sent.password, sizeof sent.password, password);
    record_send(sent);
    drop_queued(ReplyKind::created);
    g_created = created;
    for (std::size_t i = 0; i < g_script.created_count; ++i) {
        if (same_name_any_case(g_script.created[i].name, sent.name)) {
            queue(ReplyKind::created, i, g_script.created[i].delay);
            return;
        }
    }
}

// Drops the battle-list replies a new enter or leave replaces.
void drop_battle_replies() {
    drop_queued(ReplyKind::battle);
    drop_queued(ReplyKind::battles_entered);
    drop_queued(ReplyKind::battles_left);
    drop_queued(ReplyKind::battle_intel);
}

/// Opens an area's battle list: its recorded rows, then the entered answer. An area with no rows
/// still completes. A list still queued for an earlier enter is dropped.
bool ROVER_CALL replay_enter_battles(
    uint32_t area_id, RoverBattlesDoneFn entered, RoverBattleInfoFn battle_info, RoverIntelMsgFn
) {
    drop_battle_replies();
    g_battles_area = area_id;
    g_battles_entered = entered;
    g_battle_info = battle_info;
    g_battles_left = nullptr;
    int32_t delay = kDefaultDelay;
    bool any = false;
    for (std::size_t i = 0; i < g_script.battle_count; ++i) {
        const Battle& b = g_script.battles[i];
        if (b.area_id != area_id)
            continue;
        queue(ReplyKind::battle, i, b.delay);
        if (!any || b.delay > delay)
            delay = b.delay;
        any = true;
    }
    queue(ReplyKind::battles_entered, 0, delay);
    return true;
}

/// Leaves an area's battle list. Rows and an entered answer still queued are dropped, and the
/// left answer follows.
bool ROVER_CALL replay_leave_battles(uint32_t, RoverBattlesDoneFn left) {
    drop_battle_replies();
    g_battle_info = nullptr;
    g_battles_entered = nullptr;
    g_battle_intel = nullptr;
    g_battles_left = left;
    queue(ReplyKind::battles_left, 0, kDefaultDelay);
    return true;
}

/// Answers get_battle after one pump. The file records no intel of its own: the battle has no
/// password, and no host or mission is named.
bool ROVER_CALL replay_get_battle(
    uint32_t, const char* game_id, RoverBattleIntelFn battle_intel, RoverBattleReportFn
) {
    drop_queued(ReplyKind::battle_intel);
    g_battle_intel = battle_intel;
    copy_bounded(g_intel_game, sizeof g_intel_game, game_id);
    queue(ReplyKind::battle_intel, 0, kDefaultDelay);
    return true;
}

/// Queues the next reply, and every then line chained to it, for the matchmaking command just sent.
void answer_matchmaking() {
    if (g_next_match_reply >= g_script.match_count)
        return;
    if (g_script.match_replies[g_next_match_reply].chained)
        return;
    queue(
        ReplyKind::match_reply, g_next_match_reply, g_script.match_replies[g_next_match_reply].delay
    );
    ++g_next_match_reply;
    while (g_next_match_reply < g_script.match_count &&
           g_script.match_replies[g_next_match_reply].chained) {
        queue(
            ReplyKind::match_reply,
            g_next_match_reply,
            g_script.match_replies[g_next_match_reply].delay
        );
        ++g_next_match_reply;
    }
}

/// Queues the file's next launch line as the answer to a host or join request,
/// which oa-rover queues while it waits for its launch_game.
void answer_launch() {
    if (g_next_launch < g_script.launch_count) {
        queue(ReplyKind::launch, g_next_launch, g_script.launches[g_next_launch].delay);
        ++g_next_launch;
    }
}

/// Passes a host request to oa-rover; once it is queued, the file's next launch line answers it.
bool ROVER_CALL replay_host_game(
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
) {
    const bool queued = g_real->host_game(
        area_id,
        game_type,
        name,
        password,
        description,
        keyword,
        skill_sel,
        players_sel,
        on_death_sel,
        launch,
        debriefing
    );
    if (queued)
        answer_launch();
    return queued;
}

/// Passes a join request to oa-rover; once it is queued, the file's next launch line answers it.
bool ROVER_CALL replay_join_game(
    uint32_t area_id,
    const char* game_id,
    const char* password,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
) {
    const bool queued = g_real->join_game(area_id, game_id, password, launch, debriefing);
    if (queued)
        answer_launch();
    return queued;
}

const char* ROVER_CALL replay_match_area_name() {
    return g_script.has_area_name ? g_script.area_name : nullptr;
}

void ROVER_CALL replay_get_interface(const RoverInterface** out) {
    *out = &g_table;
}

void reset_replies() {
    g_queue_count = 0;
    g_pumps = 0;
    g_sent_count = 0;
    g_user_id = 0;
    g_progress = nullptr;
    g_alert = nullptr;
    g_profile = nullptr;
    g_ladder = nullptr;
    g_allegiance = nullptr;
    g_census = nullptr;
    g_activity = nullptr;
    g_logout = nullptr;
    g_locate = nullptr;
    g_game_located = nullptr;
    g_game_hit_count = 0;
    g_room_list = nullptr;
    g_created = nullptr;
    g_room = RoomFeeds{};
    g_entering = Entering{};
    g_leaving = Leaving{};
    for (RoverFileServer& server : g_file_servers) {
        server = RoverFileServer{};
        server.type_index = OA_ROVER_FILE_SERVER_INVALID;
    }
    g_http_complete = nullptr;
    g_fetch_count = 0;
    g_next_launch = 0;
    g_next_match_reply = 0;
    g_battles_area = 0;
    g_battles_entered = nullptr;
    g_battle_info = nullptr;
    g_battles_left = nullptr;
    g_battle_intel = nullptr;
    g_intel_game[0] = '\0';
}

} // namespace

bool rover_replay_install(const char* path, char* error, std::size_t error_capacity) {
    static Script parsed;
    parsed = Script{};
    parsed.allegiances.delay = kDefaultDelay;
    parsed.activity.delay = kDefaultDelay;
    parsed.dossier_delay = kDefaultDelay;
    if (!parse_file(path, &parsed, error, error_capacity))
        return false;
    g_script = parsed;
    reset_replies();
    rover_get_interface(&g_real);
    g_table = *g_real;
    g_table.pump_events = replay_pump_events;
    g_table.reset_session = replay_reset_session;
    g_table.start_login = replay_start_login;
    g_table.send_logout = replay_send_logout;
    g_table.request_get_profile = replay_request_get_profile;
    g_table.request_allegiance = replay_request_allegiance;
    g_table.request_census = replay_request_census;
    g_table.locate_player = replay_locate_player;
    g_table.locate_target = replay_locate_target;
    g_table.get_rooms = replay_get_rooms;
    g_table.enter_room = replay_enter_room;
    g_table.switch_room = replay_switch_room;
    g_table.leave_room = replay_leave_room;
    g_table.send_chat = replay_send_chat;
    g_table.send_page_by_name = replay_send_page_by_name;
    g_table.create_room = replay_create_room;
    g_table.match_area_name = replay_match_area_name;
    g_table.get_file_server = replay_get_file_server;
    g_table.fetch_http_file = replay_fetch_http_file;
    g_table.host_game = replay_host_game;
    g_table.join_game = replay_join_game;
    g_table.enter_battles = replay_enter_battles;
    g_table.leave_battles = replay_leave_battles;
    g_table.get_battle = replay_get_battle;
    rover_module_set_source(replay_get_interface);
    return true;
}

void rover_replay_uninstall() {
    rover_module_set_source(rover_get_interface);
    g_script = Script{};
    reset_replies();
}

bool rover_replay_census(uint32_t area_id, CensusReport* out) {
    for (std::size_t i = 0; i < g_script.census_count; ++i) {
        const Census& c = g_script.census[i];
        if (c.area_id == area_id) {
            *out = CensusReport{
                c.area_id,
                c.values[0],
                c.values[1],
                c.values[2],
                c.values[3],
                c.values[4],
                c.values[5]
            };
            return true;
        }
    }
    return false;
}

std::size_t rover_replay_room_count(uint32_t area_id) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < g_script.room_count; ++i)
        count += g_script.rooms[i].fields.area_id == area_id ? 1 : 0;
    return count;
}

std::size_t rover_replay_sent_count() {
    return g_sent_count;
}

bool rover_replay_sent(std::size_t index, ReplaySent* out) {
    if (index >= g_sent_count)
        return false;
    *out = g_sent[index];
    return true;
}

bool rover_replay_profile(uint32_t user_id, ReplayProfile* out) {
    for (std::size_t a = 0; a < g_script.profile_count; ++a) {
        if (g_script.profiles[a].user_id != user_id)
            continue;
        *out = ReplayProfile{};
        for (std::size_t i = 0; i < g_script.profile_line_count; ++i) {
            const ProfileLine& p = g_script.profile_lines[i];
            if (p.ladder || p.answer != a || p.field.is_int)
                continue;
            if (std::strcmp(p.field.key, "LOGINNAME") == 0)
                copy_bounded(out->name, sizeof out->name, p.field.text);
            else if (std::strcmp(p.field.key, "CLAN") == 0)
                copy_bounded(out->clan, sizeof out->clan, p.field.text);
        }
        return true;
    }
    return false;
}

bool rover_replay_room(uint32_t area_id, std::size_t index, ReplayRoom* out) {
    for (std::size_t i = 0; i < g_script.room_count; ++i) {
        const RoomFields& f = g_script.rooms[i].fields;
        if (f.area_id != area_id || index-- != 0)
            continue;
        *out = ReplayRoom{f.room_id, f.users, f.type, f.needs_password, {}};
        copy_bounded(out->name, sizeof out->name, f.name);
        return true;
    }
    return false;
}

bool rover_replay_member(uint32_t area_id, uint32_t room_id, std::size_t index, ReplayMember* out) {
    for (std::size_t i = 0; i < g_script.member_count; ++i) {
        const Member& m = g_script.members[i];
        if (m.area_id != area_id || m.room_id != room_id || index-- != 0)
            continue;
        *out = ReplayMember{m.user_id, {}};
        copy_bounded(out->name, sizeof out->name, m.name);
        return true;
    }
    return false;
}

bool rover_replay_alert(std::size_t index, ReplayAlert* out) {
    if (index >= g_script.alert_count)
        return false;
    const Alert& a = g_script.alerts[index];
    *out = ReplayAlert{a.kind, a.user_id, 0, 0, {}, {}};
    if (a.follows_answer && a.answer == AnswerKind::entered) {
        out->area_id = g_script.entries[a.answer_index].area_id;
        out->room_id = g_script.entries[a.answer_index].room_id;
    } else if (a.follows_answer) {
        out->area_id = g_script.created[a.answer_index].area_id;
        out->room_id = g_script.created[a.answer_index].room_id;
    }
    copy_bounded(out->text, sizeof out->text, a.text);
    copy_bounded(out->notification_type, sizeof out->notification_type, a.notification_type);
    return true;
}

bool rover_replay_said(uint32_t area_id, uint32_t room_id, std::size_t index, ReplaySaid* out) {
    for (std::size_t i = 0; i < g_script.said_count; ++i) {
        const Said& said = g_script.said[i];
        if (said.area_id != area_id || said.room_id != room_id || index-- != 0)
            continue;
        *out = ReplaySaid{said.user_id, {}};
        copy_bounded(out->text, sizeof out->text, said.text);
        return true;
    }
    return false;
}

} // namespace oa::boneyards
