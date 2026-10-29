// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The first-run setup: whether it shows, and the steps a player walks.
// A player who already has the game walks the welcome (language, online
// features, the OA buttons). A player who does not walks the setup: language,
// the game, copying, then those two. The host fills the model and applies
// what the player asks. This module reads no preference, clock or network.

#pragma once

#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/first_run.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/library/library.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::first_run {

/// The preference that records a finished setup. Absent, or any other word, means it
/// has not been finished.
inline constexpr std::string_view preference_key = "open-annihilation.first-run";

/// The value a finished setup writes. Show again erases the key.
inline constexpr std::string_view preference_done = "done";

/// Why the setup stays hidden, or that it shows.
enum class Hold : uint8_t {
    show,             ///< the setup shows
    done,             ///< the preference is done
    headless_check,   ///< a headless check
    preferences_file, ///< the run named a preferences file
    benchmark,        ///< a benchmark
    stage,            ///< a stage run
    fark,             ///< a FARK run
    unattended,       ///< nobody is watching
};

/// The facts the gate reads. The host fills them from the command line and the preference.
struct GateFacts {
    bool force{};            ///< --first-run
    bool done{};             ///< the preference is done
    bool headless_check{};   ///< a headless check
    bool preferences_file{}; ///< --preferences-file
    bool benchmark{};        ///< a benchmark
    bool stage{};            ///< a stage file
    bool fark{};             ///< --fark
    bool unattended{};       ///< an unattended run, or a run nobody watches
};

/// Decides whether the setup shows.
///
/// --first-run shows it even when it is done and even in an automated run.
/// Otherwise a finished setup stays hidden, then a headless check, a preferences
/// file, a benchmark, a stage run, a FARK run and an unattended run, in that order.
///
/// @param facts the command line and the preference
/// @return show, or the first reason it stays hidden
[[nodiscard]] Hold decide(const GateFacts& facts) noexcept;

/// The words a log uses after "not showing: ". Empty when the setup shows.
///
/// @param hold what decide returned
/// @return the reason; empty for show
[[nodiscard]] std::string_view hold_reason(Hold hold) noexcept;

/// One step of the setup.
enum class Step : uint8_t {
    language, ///< the language carousel
    game,     ///< where the game is, or locate another
    copying,  ///< the copy and its check
    online,   ///< online features
    buttons,  ///< the OA buttons over the main menu
};

/// Which walk the step line counts. The welcome is what a player who already
/// has the game sees, and what SHOW AGAIN runs. The setup is the five steps
/// a player walks the first time the game is copied.
enum class Walk : uint8_t {
    welcome, ///< language, online features, the OA buttons
    setup,   ///< language, the game, copying, online features, the OA buttons
};

/// What the left footer button does on every step of one walk. The host sets
/// it for the whole walk. The module does not read the preference.
enum class Leave : uint8_t {
    quit, ///< Quit: leave, and do not record the welcome as done
    skip, ///< Skip: record the welcome as done
};

/// Where a step sits on its walk's step line, and the catalogue word of its name.
struct StepPlace {
    int32_t index{1};        ///< from 1
    int32_t count{3};        ///< how many steps the walk has
    std::string_view name{}; ///< the catalogue word, such as "WELCOME"
};

/// The word a stage line and a log use for a step.
///
/// @param step the step
/// @return language, game, copying, online or buttons
[[nodiscard]] std::string_view step_token(Step step) noexcept;

/// Reads a stage line's step word.
///
/// @param token language, game, copying, online or buttons
/// @return the step; nothing for any other word
[[nodiscard]] std::optional<Step> step_named(std::string_view token) noexcept;

/// Where a step sits on a walk. A welcome never counts the game steps.
///
/// @param step the step
/// @param walk which walk
/// @return the place and the step's catalogue name
[[nodiscard]] StepPlace step_place(Step step, Walk walk) noexcept;

/// Where a language card's language comes from.
enum class CardKind : uint8_t {
    built_in,  ///< a language the game ships
    installed, ///< a language pack already installed
    pack,      ///< a language pack the catalogue lists
};

/// What a catalogue pack's card is doing.
enum class PackState : uint8_t {
    ready,       ///< it can be downloaded
    downloading, ///< bytes are arriving
    checking,    ///< the signature is being checked
    failed,      ///< the download did not finish
    offline,     ///< the catalogue cannot be read, or downloads are off
};

/// Why catalogue packs are not offered as downloads.
enum class CatalogueNote : uint8_t {
    none,          ///< packs that are listed can be downloaded
    unread,        ///< the catalogue could not be read
    downloads_off, ///< downloads are off
};

/// One language card.
struct Card {
    std::string tag{};                 ///< the language's tag
    std::string own_name{};            ///< its name in itself
    std::string english_name{};        ///< its name in English
    CardKind kind{CardKind::built_in}; ///< where it comes from
    PackState pack{PackState::ready};  ///< a pack's progress; ignored for the others
    bool current{};                    ///< the language the game shows
    std::string size_text{};           ///< a pack's size, as the player reads it
    std::string progress_done_text{};  ///< how much has arrived
    std::string progress_size_text{};  ///< how much the pack is
    int32_t progress_done{};           ///< the bar's filled part
    int32_t progress_whole{};          ///< the bar's whole; 0 fills nothing
    std::string failure{};             ///< why a download failed, already looked up
};

/// Which kind of place a source row names.
enum class SourceMark : uint8_t {
    folder,  ///< a folder of game files
    gog,     ///< a GOG installer
    package, ///< a package
    demo,    ///< the demo installer
    disc,    ///< a disk image
};

/// One bundle a source holds or lacks, drawn as a pill.
struct SourceChip {
    std::string name{}; ///< the bundle's name, already the words to show
    bool held{true};    ///< the source holds it
    std::string id{};   ///< the bundle's id, which the host matches it by; never shown
};

/// One place the game can be copied from. The host fills it.
struct GameSource {
    std::string token{};                 ///< the host's name for this place
    std::string title{};                 ///< the row's title
    std::string detail{};                ///< the line under the title
    SourceMark mark{SourceMark::folder}; ///< the badge
    std::string status{};                ///< a short line such as a match, or empty
    std::string footnote{};              ///< the size and the free space, or how the source is read
    std::vector<SourceChip> chips{};     ///< what the source holds
};

/// How far one bundle of a copy has got.
enum class CopyPhase : uint8_t {
    waiting, ///< not started
    copying, ///< bytes are moving
    done,    ///< copied, and the check matches
    differ,  ///< copied, and the check differs
};

/// One bundle on the copying step.
struct CopyBundle {
    std::string name{};                  ///< the bundle's name, in the language shown
    std::string id{};                    ///< the bundle's id, which the host matches it by
    std::string file{};                  ///< the file being copied, or empty
    std::string amount{};                ///< how much, as the player reads it
    int32_t done{};                      ///< the bar's filled part
    int32_t whole{1};                    ///< the bar's whole
    CopyPhase phase{CopyPhase::waiting}; ///< where this bundle is
    /// The amount is a note, such as tracks the source did not hold. It is
    /// not a failure, and the bundle still counts as matched.
    bool note{false};
};

/// An archive of the base game or of an expansion that the place copied
/// from holds at another size than Total Annihilation 3.1c's, as a mod or
/// another program leaves it. Such an archive stops the copy before
/// anything is copied.
struct ChangedFile {
    std::string name{}; ///< the archive's name, as the game stores it
    /// The English name of the base game or the expansion it belongs to,
    /// such as "Core Contingency"; the question shows it in the language in
    /// use.
    std::string bundle{};
    uint64_t size{};     ///< its size in the place copied from, in bytes
    uint64_t expected{}; ///< the size it should be, in bytes
};

/// The copying step's progress. The host keeps it current.
struct CopyView {
    std::string from{};                ///< where the bytes are read from
    std::string overall{};             ///< how far the whole copy has got
    std::string remaining{};           ///< about how long is left, or empty
    int32_t done{};                    ///< the overall bar's filled part
    int32_t whole{1};                  ///< the overall bar's whole
    std::vector<CopyBundle> bundles{}; ///< one row a bundle
    bool running{};                    ///< a copy is under way
    bool paused{};                     ///< the copy is paused
    bool checked{};                    ///< the copy has finished and been checked
    std::string verdict{};             ///< what the check said, when it has
    bool hashing{};                    ///< the check after the bytes is running
    uint32_t check_done{};             ///< files the check has considered
    uint32_t check_total{};            ///< files the check will consider
    bool bytes_landed{};               ///< the bytes finished, whether or not the check did
    /// The archives that stopped the copy, in the game's order. While there
    /// are any, the step asks over itself (changed_question) whether to
    /// choose another place to copy from or to leave.
    std::vector<ChangedFile> changed{};
};

/// Where downloading the game from the player's GOG account is. The game
/// step asks over itself (gog_question) while it is anything but none.
enum class GogPhase : uint8_t {
    none,        ///< nothing is asked
    sign_in,     ///< GOG's sign-in page is open in the browser; the step waits for its address
    working,     ///< signing in, and asking GOG where the installer is
    downloading, ///< the installer's bytes are arriving
    failed,      ///< it stopped; GogView::failure says why
};

/// Downloading the game from GOG, as the game step shows it. The host keeps
/// it current.
struct GogView {
    GogPhase phase{GogPhase::none}; ///< where it is
    /// Why it stopped, already looked up; on sign_in, why the address the
    /// player pasted did not do. Empty for none.
    std::string failure{};
    /// On sign_in, GOG's page is in the game's own sign-in window, which
    /// hands its address over; false when it is in the web browser and the
    /// player copies the address.
    bool in_window{};
    std::string amount{}; ///< how much has arrived, as the player reads it
    int32_t done{};       ///< the bar's filled part
    int32_t whole{1};     ///< the bar's whole
};

/// What the steps show. The host keeps it current.
struct Model {
    Step step{Step::language}; ///< the step on screen
    Walk walk{Walk::welcome};  ///< which step line this walk uses
    /// Quit on a first start that has nothing to return to. Skip on a re-run,
    /// and on the welcome a player who already has the game sees. The same
    /// word on every step of the walk.
    Leave leave{Leave::skip};
    std::vector<Card> cards{};                    ///< the carousel, built-in languages then packs
    int32_t source{-1};                           ///< the chosen source; -1 is none
    std::vector<GameSource> sources{};            ///< the places the game step lists
    CopyView copy{};                              ///< the copying step
    /// The game step offers Download from GOG…, which the host leaves out
    /// where the game cannot reach GOG.
    bool gog_offered{};
    GogView gog{}; ///< downloading the game from GOG
    bool controller{};                            ///< a controller is connected, so the hint shows
    bool online_on{true};                         ///< Turn on online features is chosen
    CatalogueNote catalogue{CatalogueNote::none}; ///< why packs stay offline
    kit::Rect library_button{};                   ///< the Library button, in the screen's points
    kit::Rect settings_button{};                  ///< the Settings button, in the screen's points
    int32_t library_count{};                      ///< the number the Library button shows
    std::string sample_own{};                     ///< the sample Library's language, in itself
    std::string sample_english{};                 ///< that language's English name
    /// How the sample Library looks its words up. The same hook the live
    /// Library uses: each pattern from the Library's texts, whole, then filled.
    /// Empty shows those words in English.
    library::TextHooks sample_text{};
    /// The game runs in a window, whose own title bar names it, so the
    /// header leaves out the mark and "Open Annihilation". Full screen
    /// keeps them.
    bool windowed{};
};

/// The room the setup lays out in on a large window, in points: Regular's
/// least. A larger window draws this room larger, so the setup's words,
/// buttons and cards grow with the window together.
inline constexpr kit::Point setup_room = kit::regular_from;

/// Returns the scale the setup is drawn at in a room of window pixels.
///
/// The scale is the largest at which the room still holds setup_room, so
/// a larger window draws the same arrangement larger. It is never below
/// `least_percent`, the display's density or the Interface size in effect,
/// and never below 100: a room smaller than setup_room is laid out in its
/// own points, Compact below 960 by 540. Without fractional scales the
/// largest whole step that holds setup_room is taken.
///
/// @param room_width the room's width, in window pixels
/// @param room_height the room's height, in window pixels
/// @param least_percent the smallest scale, in percent
/// @param fractional true when the setup can be drawn at a fraction, on the smooth form
/// @return the scale, in percent
[[nodiscard]] int32_t setup_scale_percent(
    int32_t room_width, int32_t room_height, int32_t least_percent, bool fractional
) noexcept;

/// Returns the frame the setup lays out in on a room of window pixels: the
/// room at setup_scale_percent, in whole points.
///
/// @param room_width the room's width, in window pixels
/// @param room_height the room's height, in window pixels
/// @param least_percent the smallest scale, in percent
/// @param fractional true when the setup can be drawn at a fraction, on the smooth form
/// @return the frame, from the room's top left
[[nodiscard]] kit::Frame setup_frame(
    int32_t room_width, int32_t room_height, int32_t least_percent, bool fractional
) noexcept;

/// How long a finished, matching copy shows the countdown on Continue.
inline constexpr int32_t copy_continue_wait_ms = 3000;

/// How far a list of a step scrolls for a notch of the wheel and for Up or
/// Down, in points.
inline constexpr int32_t list_step = 2 * kit::compact_metrics.small_line;

/// Where a step's list is scrolled, and the press that drags it.
///
/// A list that does not fit between what is above it and the lines under
/// it scrolls in the room left. Until the player scrolls it, it shows the
/// row it follows: the chosen place on the game step, the bundle being
/// copied on the copying step.
struct ListScroll {
    /// How far the list is moved up, in points: where the player left it,
    /// and where following starts from.
    int32_t offset{};
    /// The player scrolled the list, so it stays where they left it.
    bool held{};
    kit::WheelCarry wheel{}; ///< the part of a turn of the wheel not yet scrolled
    bool dragging{};         ///< a press on the list or its scroll bar is held
    bool by_bar{};           ///< the press is on the scroll bar, so the thumb follows it
    int32_t press_y{};       ///< where the press went down, in points
    int32_t press_offset{};  ///< how far the list was moved up then, in points
};

/// The setup's controls and motion between layouts.
struct Session {
    Model model{};                  ///< what the steps show
    kit::CarouselModel carousel{};  ///< which card is selected, and the move
    kit::CarouselDrag drag{};       ///< a press on the carousel
    kit::Interaction interaction{}; ///< the pointer and the focus
    uint32_t pulse_ms{};            ///< how long Start playing has been pulsing
    bool focus_ready{};             ///< the focus has been placed for this step
    /// Milliseconds left before Continue presses itself. Negative means the
    /// countdown is not running. Zero means the moment has come.
    int32_t copy_continue_ms{-1};
    /// The copy was under way on this visit, so a clear check may count down.
    /// Coming back to a copy that already finished does not count down again.
    bool copy_continue_armed{false};
    /// The player did something, so the countdown stays off.
    bool copy_continue_stopped{false};
    ListScroll source_list{}; ///< the game step's places
    ListScroll copy_list{};   ///< the copying step's bundles
};

/// What the player asked the host to do.
enum class Ask : uint8_t {
    none,            ///< the picture may have changed; nothing was asked
    skip,            ///< Skip: the welcome is done
    back,            ///< Back
    advance,         ///< Next, or Start playing
    pick,            ///< the selected language card
    cancel_download, ///< Cancel download
    retry_download,  ///< Retry
    choose_on,       ///< Turn on online features
    choose_off,      ///< Not now
    locate,          ///< Locate another…
    pause_copy,      ///< Pause, or continue a paused copy
    cancel_copy,     ///< Cancel the copy and return to the game step
    quit,            ///< Quit, leaving the game uncopied
    gog_open,        ///< Download from GOG…, Open GOG again or Try again: open GOG's sign-in
    gog_paste,       ///< Paste address: read GOG's address from the clipboard
    gog_cancel,      ///< Cancel or Close on the GOG question
};

/// What a key, a pointer or a tick did.
struct Outcome {
    Ask ask{Ask::none}; ///< what the host should do
    int32_t index{-1};  ///< the card, when ask is pick
    bool redraw{};      ///< what the screen draws changed
    bool pass{};        ///< the screen did not take it
};

/// A laid-out step.
struct Scene {
    int32_t width{};                         ///< the window's width, in points
    int32_t height{};                        ///< the window's height, in points
    kit::DisplayList list{};                 ///< what is drawn, and the controls
    kit::Rect well_inset{};                  ///< where the sample Library is scaled; empty for none
    bool sample{};                           ///< the well draws the sample Library
    kit::Rect carousel{};                    ///< the carousel's band and pager
    kit::Rect previous{};                    ///< the previous arrow
    kit::Rect next_arrow{};                  ///< the next arrow
    std::vector<kit::CarouselSlot> places{}; ///< the cards that show
    kit::Rect coach_library{};               ///< the Library coach card
    kit::Rect coach_settings{};              ///< the Settings coach card
    kit::Rect title{};        ///< the large centred title; empty when the step has none
    std::string title_text{}; ///< the title's words
    /// the class the sample Library is laid out at
    kit::SizeClass sample_class{kit::SizeClass::compact};
    /// The step's list while it scrolls: its view, its scroll bar, and how
    /// far it is moved up. The view is empty when the whole list fits.
    kit::ScrollArea list_scroll{};
    /// The first item of the dim under a question over the step; 0 when no
    /// question is. The step's own parts, its title among them, are drawn
    /// first. The question's buttons are the list's only controls then.
    std::size_t over_from{};
    /// The question over the step, centred in the window; empty when none is.
    kit::Rect question{};
    /// The question's parts, in points from its top left corner, drawn there
    /// over the dim.
    kit::DisplayList question_list{};
};

/// The left footer button: Skip, or Quit when the host set Leave::quit.
inline constexpr kit::ControlId skip_id = 1;
/// Back.
inline constexpr kit::ControlId back_id = 2;
/// Next.
inline constexpr kit::ControlId next_id = 3;
/// The carousel's previous arrow.
inline constexpr kit::ControlId previous_id = 4;
/// The carousel's next arrow.
inline constexpr kit::ControlId next_card_id = 5;
/// Cancel download.
inline constexpr kit::ControlId cancel_id = 6;
/// Retry.
inline constexpr kit::ControlId retry_id = 7;
/// Turn on online features.
inline constexpr kit::ControlId on_id = 8;
/// Not now.
inline constexpr kit::ControlId off_id = 9;
/// Start playing.
inline constexpr kit::ControlId start_id = 10;
/// Locate another…
inline constexpr kit::ControlId locate_id = 11;
/// Choose another place, on the question a changed install asks.
inline constexpr kit::ControlId changed_choose_id = 12;
/// That question's Quit, or Skip when the host set Leave::skip.
inline constexpr kit::ControlId changed_leave_id = 13;
/// That question's text, which takes no press and which automation reads.
inline constexpr kit::ControlId changed_text_id = 14;
/// Download from GOG…, on the game step.
inline constexpr kit::ControlId gog_id = 15;
/// The GOG question's first button. Button n is gog_button_id + n.
inline constexpr kit::ControlId gog_button_id = 16;
/// The GOG question's text, which takes no press and which automation reads.
inline constexpr kit::ControlId gog_text_id = 19;
/// The first language card. Card n is card_id + n.
inline constexpr kit::ControlId card_id = 20;
/// The first source row. Source n is source_id + n.
inline constexpr kit::ControlId source_id = 40;

/// The most changed archives the question names one by one. With more, it
/// names one fewer and counts the rest, which the game's log names, so the
/// question fits the setup's smallest window in every language.
inline constexpr std::size_t changed_files_shown = 6;

/// The question the copying step asks when the place copied from holds an
/// archive of the base game or of an expansion at another size than Total
/// Annihilation 3.1c's.
///
/// It says the game's files there have been changed, as a mod or another
/// program changes them, names each archive with the base game or the
/// expansion it is from, its size there and the size it should be, says
/// that Open Annihilation needs a fresh install without mods to set up its
/// copy and that nothing was copied, and asks where to go next. Past
/// changed_files_shown archives the rest are counted in one line.
/// Choose another place, marked and in the accent, goes back to the game
/// step, and so do Escape and No; the walk's Quit or Skip is beside it. Its
/// words are in the language shown.
///
/// @param model the walk's leave word and the copy's changed archives
/// @param size_class the class the question is laid out at
/// @return the question; its buttons are Choose another place, then Quit or Skip
[[nodiscard]] kit::Question changed_question(const Model& model, kit::SizeClass size_class);

/// The question the game step asks while the game is downloaded from GOG.
///
/// While GOG's page is open in the game's sign-in window it tells the
/// player to sign in there, with Open GOG again and Cancel; while it is open
/// in the web browser it tells them to sign in there and copy the address
/// the page ends on, with Paste address, Open GOG again and Cancel; while the game signs in and downloads it shows how far, with
/// Cancel; after a failure it says why, with Try again and Close. Escape
/// answers Cancel or Close. Its words are in the language shown.
///
/// @param model the game step's GOG view
/// @param size_class the class the question is laid out at
/// @return the question; each button's id names the Ask it gives
[[nodiscard]] kit::Question gog_question(const Model& model, kit::SizeClass size_class);

/// Tells whether a download is running, so Next stays disabled.
///
/// @param model the model
/// @return true while a pack is downloading or being checked
[[nodiscard]] bool download_busy(const Model& model) noexcept;

/// Holds the carousel's count and selection inside the cards.
///
/// @param[in,out] session the session
void bind(Session& session) noexcept;

/// Lays one step out.
///
/// @param session the model and the carousel
/// @param frame the room, its class and its scale
/// @param fonts the fonts texts are measured in; null estimates them
/// @return the scene
[[nodiscard]] Scene
lay_out(const Session& session, const kit::Frame& frame, const kit::Fonts* fonts);

/// Advances the carousel's move, Start playing's pulse, and the copying
/// step's countdown.
///
/// The countdown starts only after a copy that was under way on this visit
/// has been checked and matches. It waits `copy_continue_wait_ms`, then
/// reaches zero. The host moves on when it sees zero.
///
/// @param[in,out] session the session
/// @param dt_ms how much time passed, in milliseconds
void tick(Session& session, uint32_t dt_ms) noexcept;

/// Stops the copying step's countdown. A countdown that is not running is
/// left as it is, so a press before the check does not block a later one.
///
/// @param[in,out] session the session
/// @return true when a countdown was running and is now stopped
bool stop_copy_continue(Session& session) noexcept;

/// Forgets the copying step's countdown, including one the player stopped,
/// and where the player scrolled its list, so the list follows the copy
/// again. A host calls it each time the copying step opens.
///
/// @param[in,out] session the session
void reset_copy_continue(Session& session) noexcept;

/// Places the focus on the step's first control, once per step. On the
/// language step a focus on a card follows the card the carousel turns to,
/// so the focus ring marks the chosen card and no other.
///
/// @param[in,out] session the session
/// @param list the scene's controls
void ensure_focus(Session& session, const kit::DisplayList& list);

/// Where a finger is.
enum class PointerKind : uint8_t {
    move, ///< the pointer moved
    down, ///< a press went down
    up,   ///< a press came up
};

/// Takes a pointer. On the game and copying steps a press on a list's scroll
/// bar, or on the list where no control takes it, drags the list while it
/// does not fit.
///
/// @param[in,out] session the session
/// @param scene the layout the press lands on
/// @param kind where the finger is
/// @param at the finger, in the screen's points
/// @param finger_reach how far a finger may lie from a control; 0 for a mouse
/// @param clicks how many presses landed as one; 2 or more is a double-click
/// @return what the setup did
[[nodiscard]] Outcome pointer(
    Session& session,
    const Scene& scene,
    PointerKind kind,
    kit::Point at,
    int32_t finger_reach,
    uint8_t clicks = 1
);

/// Takes a key. Escape does nothing on the language step, goes back on the
/// game step and the online step, and cancels a copy that is still running.
/// On the buttons step Escape and Enter finish the walk. While a list of the
/// game or copying step does not fit, Page Up, Page Down, Home and End
/// scroll it, and on the copying step Up and Down do too.
///
/// @param[in,out] session the session
/// @param scene the layout
/// @param pressed the key
/// @return what the setup did
[[nodiscard]] Outcome key(Session& session, const Scene& scene, kit::Key pressed);

/// Takes a turn of the wheel. On the language step a turn away from the
/// player moves to the previous card, and toward the player to the next.
/// One card does not move. On the game and copying steps a turn scrolls a
/// list that does not fit, list_step a notch. Any other step swallows the
/// turn.
///
/// @param[in,out] session the session
/// @param scene the layout
/// @param notches how far the wheel turned; positive is away from the player
/// @return what the setup did
[[nodiscard]] Outcome wheel(Session& session, const Scene& scene, float notches);

/// Words the sample Library looks up through the model's text hook: the Active
/// and Mods tabs, PLAYING, UNLOAD MOD and Built in, one a line.
///
/// The picture well draws the same Library. A hook that marks each text
/// changes these words; an empty hook leaves them in English.
///
/// @param model the sample's language and its text hook
/// @return the looked-up words
[[nodiscard]] std::string sample_words(const Model& model);

/// Draws a laid-out step. On the buttons step the dim leaves both buttons clear.
/// The sample Library is drawn into the picture well when the scene asks.
///
/// @param canvas where it is drawn
/// @param scene the layout
/// @param session the focus, the pulse and the sample's language
void draw(const kit::Canvas& canvas, const Scene& scene, const Session& session);

/// A coach card's width at a size class: 220, 260 or 300 points.
///
/// @param size_class the class
/// @return the width, in points
[[nodiscard]] int32_t coach_card_width(kit::SizeClass size_class) noexcept;

/// Where the two coach cards sit.
struct CoachCards {
    kit::Rect library{};  ///< the Library card
    kit::Rect settings{}; ///< the Settings card
};

/// Places the two coach cards so neither covers a button, the other card, or `blocked`.
///
/// Each card is tried on the right, then the left, then below, then above. The
/// Library card is placed first.
///
/// @param room the rectangle both cards must lie in
/// @param blocked a rectangle neither card may cover, such as the left panel; empty for none
/// @param library_button the Library button
/// @param settings_button the Settings button
/// @param card_width each card's width, in points
/// @param library_height the Library card's height, in points
/// @param settings_height the Settings card's height, in points
/// @return the two cards
[[nodiscard]] CoachCards place_coaches(
    const kit::Rect& room,
    const kit::Rect& blocked,
    const kit::Rect& library_button,
    const kit::Rect& settings_button,
    int32_t card_width,
    int32_t library_height,
    int32_t settings_height
) noexcept;

} // namespace oa::ui::first_run
