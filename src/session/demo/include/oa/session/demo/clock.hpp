// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The viewer's playback clock: how many ticks of a replay run in one frame,
// which of them is drawn, the speed actually reached, and which keyframes a
// jump can restore. It holds no match and changes no world.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::session::demo {

/// The speeds a viewer watches at. Skip is not one of them: seeking sets it
/// apart from the ladder + and - step through.
enum class PlaybackSpeed : uint8_t {
    half,      ///< 1/2 of the recorded pace
    normal,    ///< the recorded pace
    twice,     ///< 2x
    quadruple, ///< 4x
    octuple,   ///< 8x
    sixteen,   ///< 16x
};

/// A jump the command line or the viewer asked for.
struct ReplayJump {
    uint32_t from{};  ///< the tick that arms it; ignored when immediate
    uint32_t to{};    ///< the tick to land on
    bool immediate{}; ///< arm it on the next frame, whatever the play head
};

/// How many ticks one frame runs, and whether the frame draws.
struct FramePlan {
    uint32_t ticks{};       ///< ticks to run; 0 when the frame holds or skips
    bool draw_last{};       ///< draw the last of ticks; false when none run
    uint32_t undrawn{};     ///< ticks of the frame that are not drawn
    uint32_t asked_milli{}; ///< the asked speed, 1000 at 1x; 0 while skipping
    bool paced{};           ///< a finite speed owed ticks, so a short run is a lower speed
    bool skipping{};        ///< Skip: the caller runs what it can, and draws none of it
};

/// The speed a frame reached after it ran.
struct FrameResult {
    uint32_t asked_milli{};   ///< what the frame was asked, 1000 at 1x
    uint32_t reached_milli{}; ///< what it reached, in the same units
    bool reached_lower{};     ///< reached is below asked because ticks were left unrun
    uint32_t planned{};       ///< ticks the frame owed
    uint32_t ran{};           ///< ticks the frame ran
};

/// One stretch of the replay a keyframe can jump into.
struct ReadySpan {
    uint32_t begin{}; ///< first tick, included
    uint32_t end{};   ///< last tick, included
};

/// What the viewer and the Library read about the replay.
struct PlaybackState {
    uint32_t tick{};                            ///< the play head
    uint32_t length_known{};                    ///< the length known so far, in ticks
    uint32_t furthest{};                        ///< the furthest tick watched
    std::vector<ReadySpan> ready{};             ///< spans a jump can restore into
    std::optional<uint32_t> catch_up_percent{}; ///< set while a jump past the furthest runs
    uint32_t asked_milli{};                     ///< the asked speed, 1000 at 1x
    uint32_t reached_milli{};                   ///< the speed the last paced frame reached
    bool reached_lower{}; ///< the last paced frame fell short of the asked speed
    PlaybackSpeed speed{PlaybackSpeed::normal};
    bool paused{};
    bool skipping{};
    /// Empty while a jump can run. Otherwise one sentence for the player saying why it cannot.
    std::string unavailable{};
};

/// The most milliseconds one frame plans from. A longer stall does not burst, and the
/// speed reached is scaled by this cap over the time that actually passed.
inline constexpr uint32_t playback_stall_cap_ms = 250;

/// Folds one look at the world's pause bit into the pause a jump lands on.
///
/// Each skipped tick clears that bit. A Pause pressed during the jump sets it,
/// and the next look toggles the pause the landing puts back, then the caller
/// clears the bit. The next press sets the bit again and takes the pause off.
///
/// @param saved the pause the jump would land on
/// @param world_bit the world's pause bit at this look
/// @param clear_bit set when the bit was set and the caller must clear it
/// @return the pause the jump lands on
[[nodiscard]] inline bool fold_jump_pause(bool saved, bool world_bit, bool* clear_bit) noexcept {
    if (clear_bit != nullptr)
        *clear_bit = world_bit;
    if (!world_bit)
        return saved;
    return !saved;
}

/// How a seek moves: stay, restore a keyframe, or skip forward from here.
struct SeekPlan {
    enum class Kind : uint8_t { stay, restore, skip };
    Kind kind{Kind::stay};
    std::optional<uint32_t> restore_tick{}; ///< the keyframe to restore, when kind is restore
    bool past_furthest{};                   ///< the target is past the furthest point watched
};

/// The viewer's speeds as thousandths of 1x: 500 is 1/2x, 1000 is 1x, 16000 is 16x.
///
/// @param speed a ladder speed
/// @return its thousandths
[[nodiscard]] uint32_t playback_speed_milli(PlaybackSpeed speed) noexcept;

/// The ladder + and - step through, one step at a time, stopping at either end.
///
/// @param speed the speed now
/// @param delta positive to go faster, negative to go slower; 0 stays
/// @return the speed after the step
[[nodiscard]] PlaybackSpeed step_playback_speed(PlaybackSpeed speed, int delta) noexcept;

/// Whether unit sounds are thinned and speech is muted at this speed.
///
/// Above 4x. Skip counts as above, through PlaybackClock::skipping.
///
/// @param speed a ladder speed
/// @return true at 8x and 16x
[[nodiscard]] bool playback_speed_thins_sound(PlaybackSpeed speed) noexcept;

/// A sentence for the speed button: "16x", "1/2x", or "16x · 9x now".
///
/// The multiplication sign and the one-half sign are the ones the bar shows.
/// A speed of 500 thousandths is one half; any other is the nearest whole
/// number of times. Skip is the word Skip.
///
/// @param asked_milli the asked speed, 1000 at 1x; ignored while skipping
/// @param reached_milli the speed reached, in the same units
/// @param reached_lower whether the frame fell short of the asked speed
/// @param skipping whether the replay is skipping
/// @return the sentence
[[nodiscard]] std::string playback_speed_line(
    uint32_t asked_milli, uint32_t reached_milli, bool reached_lower, bool skipping
);

/// How far a catch-up has got, as a whole percent.
///
/// @param origin the tick the catch-up started at
/// @param play_tick the tick it has reached
/// @param target the tick it is going to
/// @return 0 at the origin through 100 at the target
[[nodiscard]] uint32_t
catch_up_percent(uint32_t origin, uint32_t play_tick, uint32_t target) noexcept;

/// Whether a keyframe is taken at this tick: the start, and every ten seconds.
///
/// @param play_tick the tick just reached
/// @return true at tick 0 and on the near interval
[[nodiscard]] bool keyframe_due(uint32_t play_tick) noexcept;

/// Whether a keyframe sits on a minute of game time.
///
/// @param play_tick the keyframe's tick
/// @return true when the tick is a multiple of the far interval
[[nodiscard]] bool keyframe_sparse(uint32_t play_tick) noexcept;

/// Whether a keyframe stays once the play head has moved on.
///
/// The start stays. Within two minutes of the play head every ten-second
/// keyframe stays. Further away, only the ones on a minute stay.
///
/// @param play_tick the keyframe's tick
/// @param head the play head
/// @return true when the keyframe is kept
[[nodiscard]] bool keyframe_kept(uint32_t play_tick, uint32_t head) noexcept;

/// One of each unit sound in a second, for a replay watched above 4x.
///
/// The clock is the caller's milliseconds, so a test can step it.
class SoundGate {
  public:

    /// Allows a sound, or refuses it when this name played within the last second.
    ///
    /// @param name the sound's name
    /// @param now_ms the caller's clock, in milliseconds
    /// @return true when the sound plays
    [[nodiscard]] bool allow(std::string_view name, uint32_t now_ms);

    /// Forgets every name, so the next of each plays.
    void clear() noexcept;

  private:

    struct Heard {
        std::string name;
        uint32_t at{};
    };

    std::vector<Heard> heard_{};
};

/// The keyframes a replay holds, and which of them a jump restores.
///
/// The bytes are what the caller says each keyframe takes, plus the base.
/// Past the budget, the keyframe furthest from the play head goes first, and
/// of two equally far the dense one goes before the sparse one.
class KeyframeReel {
  public:

    /// A reel under the computer's keyframe budget.
    KeyframeReel() noexcept;

    /// Sets the bytes the base keyframe takes. They count toward the budget and are never dropped.
    ///
    /// @param bytes the base's size
    void set_base_bytes(std::size_t bytes) noexcept;

    /// Sets the most the keyframes, the base and one take's working memory may take.
    ///
    /// @param bytes the budget; the computer's when the caller leaves it
    void set_budget(std::size_t bytes) noexcept;

    /// The budget set_budget last set.
    ///
    /// @return that many bytes
    [[nodiscard]] std::size_t budget() const noexcept;

    /// The bytes the base takes.
    ///
    /// @return that many bytes
    [[nodiscard]] std::size_t base_bytes() const noexcept;

    /// Counts one take's working memory toward the budget, once, not once per keyframe.
    ///
    /// @param bytes the compress buffer and about one compress_bound of the writer's image;
    /// 0 when the session holds none
    void set_working_bytes(std::size_t bytes) noexcept;

    /// The working memory set_working_bytes last set.
    ///
    /// @return that many bytes
    [[nodiscard]] std::size_t working_bytes() const noexcept;

    /// Forgets one keyframe. A tick that is not held changes nothing.
    ///
    /// @param play_tick the tick
    void forget(uint32_t play_tick);

    /// Remembers a keyframe. A tick already held is replaced.
    ///
    /// @param play_tick the keyframe's tick
    /// @param bytes what the keyframe takes of memory
    void add(uint32_t play_tick, std::size_t bytes);

    /// Drops keyframes the schedule no longer keeps at this play head.
    ///
    /// @param head the play head
    /// @return the ticks that were dropped
    [[nodiscard]] std::vector<uint32_t> thin(uint32_t head);

    /// Drops keyframes until the reel fits the budget.
    ///
    /// @param head the play head the distance is measured from
    /// @return the ticks that were dropped
    [[nodiscard]] std::vector<uint32_t> evict(uint32_t head);

    /// The nearest keyframe at or before a tick.
    ///
    /// @param play_tick the tick
    /// @return that keyframe's tick, or nothing when none is early enough
    [[nodiscard]] std::optional<uint32_t> nearest_at_or_before(uint32_t play_tick) const;

    /// The next keyframe strictly after a tick.
    ///
    /// @param play_tick the tick
    /// @return that keyframe's tick, or nothing when none is later
    [[nodiscard]] std::optional<uint32_t> next_after(uint32_t play_tick) const;

    /// How a jump to a target moves, from the keyframes held.
    ///
    /// A target within what was watched restores the nearest keyframe at or
    /// before it. A target past the furthest point watched restores the latest
    /// keyframe still ahead of the play head, when there is one, and then
    /// skips. With no keyframe to restore, a jump back stays and a jump
    /// forward skips from here.
    ///
    /// @param current the play head
    /// @param target the tick to land on
    /// @param furthest the furthest tick watched
    /// @return the plan
    [[nodiscard]] SeekPlan plan_seek(uint32_t current, uint32_t target, uint32_t furthest) const;

    /// The spans the held keyframes cover up to the furthest tick watched.
    ///
    /// @param furthest the furthest tick watched
    /// @return each keyframe's span, the last one ending on furthest
    [[nodiscard]] std::vector<ReadySpan> ready_spans(uint32_t furthest) const;

    /// Whether a tick is held.
    ///
    /// @param play_tick the tick
    /// @return true when add kept it and nothing has dropped it
    [[nodiscard]] bool contains(uint32_t play_tick) const noexcept;

    /// How many keyframes are held.
    ///
    /// @return the count
    [[nodiscard]] std::size_t size() const noexcept;

    /// The bytes the base, the keyframes and one take's working memory take.
    ///
    /// @return the total
    [[nodiscard]] std::size_t total_bytes() const noexcept;

  private:

    struct Slot {
        uint32_t tick{};
        std::size_t bytes{};
        bool sparse{};
    };

    std::vector<Slot> slots_{};
    std::size_t base_bytes_{};
    std::size_t working_bytes_{};
    std::size_t budget_{};
};

/// The viewer's clock for one replay.
///
/// The recorded speed paces it and the viewer's speed multiplies that. A
/// recorded speed change never writes the viewer's speed back. Pause and a
/// hold leave the fractional tick where it is, so the replay does not catch
/// the held time up.
class PlaybackClock {
  public:

    /// The viewer's speed. Skip is not a ladder speed; set_skipping sets it.
    ///
    /// @return the ladder speed
    [[nodiscard]] PlaybackSpeed speed() const noexcept;

    /// Sets the viewer's speed. It does not clear Skip.
    ///
    /// @param speed the ladder speed
    void set_speed(PlaybackSpeed speed) noexcept;

    /// Steps the viewer's speed one place up or down the ladder.
    ///
    /// @param delta positive to go faster, negative to go slower
    void step_speed(int delta) noexcept;

    /// The recorded game's speed, kept when the value is 0 or left unchanged.
    ///
    /// The clock starts at the game's normal speed, 10.
    ///
    /// @param recorded_speed the recorded speed; 0 keeps the last one
    void set_recorded_speed(uint32_t recorded_speed) noexcept;

    /// Pause holds the clock: frames plan no ticks and the fraction stays.
    void pause() noexcept;

    /// Clears pause. The next frame runs at the viewer's speed.
    void play() noexcept;

    /// Whether pause is holding the clock.
    ///
    /// @return true while paused
    [[nodiscard]] bool paused() const noexcept;

    /// Skip runs undrawn, as fast as the caller can, and does not move the fraction.
    ///
    /// @param skipping true while a seek is skipping
    void set_skipping(bool skipping) noexcept;

    /// Whether the clock is skipping.
    ///
    /// @return true while skipping
    [[nodiscard]] bool skipping() const noexcept;

    /// Whether unit sounds are thinned to one of each a second and speech is muted.
    ///
    /// @return true above 4x and while skipping
    [[nodiscard]] bool thins_sound() const noexcept;

    /// Allows a unit sound, thinning it while thins_sound is set.
    ///
    /// @param name the sound's name
    /// @param now_ms the caller's clock, in milliseconds
    /// @return true when the sound plays
    [[nodiscard]] bool allows_unit_sound(std::string_view name, uint32_t now_ms);

    /// Plans one frame from the milliseconds since the previous one.
    ///
    /// At the normal recorded speed a second owes 15 ticks at one half, 30 at
    /// 1x, then 60, 120, 240 and 480, however the second is split across frames.
    /// Only the last tick is drawn. A stall longer than playback_stall_cap_ms
    /// plans only that cap. A held, paused or skipping frame owes nothing and
    /// leaves the fractional tick alone, and does not become the cadence the
    /// next frame is compared with.
    ///
    /// @param elapsed_ms milliseconds since the previous frame; 0 plans nothing
    /// @param hold true when a menu holds the clock
    /// @return the plan
    [[nodiscard]] FramePlan begin_frame(uint32_t elapsed_ms, bool hold) noexcept;

    /// Records how many of the planned ticks ran.
    ///
    /// When a paced frame runs fewer than it owed, the speed reached is the
    /// asked speed times the share that ran. A frame at least twice as long as
    /// the previous playing frame, or a stall past the cap, lowers it again by
    /// that ratio, even when every planned tick ran. The unrun ticks are not
    /// owed later. A held, paused or skipping frame is not a shortfall and
    /// leaves the speed the last paced frame reached.
    ///
    /// @param ran ticks the frame ran
    /// @return the speed the frame reached
    [[nodiscard]] FrameResult note_frame(uint32_t ran) noexcept;

    /// Notes the play head, and remembers it when it is the furthest watched.
    ///
    /// @param play_tick the tick just reached
    void note_tick(uint32_t play_tick) noexcept;

    /// Raises the length known so far. A smaller value changes nothing.
    ///
    /// @param play_tick a tick the recording is known to reach
    void note_length(uint32_t play_tick) noexcept;

    /// The furthest tick watched.
    ///
    /// @return that tick
    [[nodiscard]] uint32_t furthest() const noexcept;

    /// Sets or clears the catch-up percent the state reports.
    ///
    /// @param percent the percent, or nothing when no catch-up is running
    void set_catch_up(std::optional<uint32_t> percent) noexcept;

    /// Sets the sentence that says why a jump cannot run. Empty clears it.
    ///
    /// @param sentence the sentence for the player, or empty while jumping works
    void set_unavailable(std::string sentence);

    /// The sentence the speed button shows for the last frame.
    ///
    /// @return playback_speed_line for the clock's last result
    [[nodiscard]] std::string speed_line() const;

    /// The state a viewer reads. The tick is the caller's play head.
    ///
    /// @param play_tick the play head
    /// @return the state
    [[nodiscard]] PlaybackState state(uint32_t play_tick) const;

    /// The keyframes the clock's reel holds.
    ///
    /// @return the reel
    [[nodiscard]] KeyframeReel& reel() noexcept;

    /// The keyframes the clock's reel holds.
    ///
    /// @return the reel
    [[nodiscard]] const KeyframeReel& reel() const noexcept;

  private:

    PlaybackSpeed speed_{PlaybackSpeed::normal};
    bool skipping_{};
    bool paused_{};
    uint32_t recorded_speed_{10};
    uint64_t fraction_{}; ///< millionths of a tick still owed
    uint32_t asked_milli_{1000};
    uint32_t reached_milli_{1000};
    bool reached_lower_{};
    uint32_t last_planned_{};
    bool last_paced_{};
    uint32_t last_elapsed_{};     ///< the capped milliseconds the last paced frame planned from
    uint32_t last_raw_elapsed_{}; ///< the milliseconds that frame was given, before the cap
    std::string unavailable_{};
    uint32_t furthest_{};
    uint32_t length_known_{};
    std::optional<uint32_t> catch_up_{};
    KeyframeReel reel_{};
    SoundGate sounds_{};
};

} // namespace oa::session::demo
