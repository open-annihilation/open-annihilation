// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The bytes of a keyframe: a playback session's state at one tick, written
// section by section into memory and read back in the same order. The layers
// of a playback session each write their own sections (the world and the
// match runtime, the network match, the playback's cursor); the session
// compresses the whole (oa/session/demo.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

namespace oa::sim::match_runtime::keyframe {

/// The longest name a section can have, in bytes.
inline constexpr std::size_t longest_section_name = 31;

/// The longest sentence a reader keeps of what went wrong, in bytes.
inline constexpr std::size_t longest_error = 127;

/// A run of bytes that grows as bytes are added, which a writer's regions are made of.
class Buffer {
  public:

    /// Makes room for bytes before they are added.
    ///
    /// @param bytes How many bytes the buffer is to hold; a smaller number changes nothing.
    void reserve(std::size_t bytes);

    /// Adds bytes at the end.
    ///
    /// @param data The bytes.
    /// @param size How many.
    void append(const void* data, std::size_t size);

    /// Returns the bytes held.
    ///
    /// @return The bytes, valid until the next append or clear.
    [[nodiscard]] std::span<uint8_t> bytes() noexcept { return {data_.get(), size_}; }

    /// Returns how many bytes are held.
    ///
    /// @return The count.
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// Drops the bytes held and keeps the room they used.
    void clear() noexcept { size_ = 0; }

    /// Returns the room reserved, which may be more than the bytes held.
    ///
    /// @return The room.
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  private:

    std::unique_ptr<uint8_t[]> data_;
    std::size_t size_{};
    std::size_t capacity_{};
};

/// What a keyframe leaves out on purpose, for a test.
///
/// The writer asks before each section whether to leave it out. A section left
/// out is marked in the stream, holds no bytes, and a restore leaves the state
/// it names as it is.
struct Omission {
    void* context{};
    /// Tells whether a section is left out.
    ///
    /// @param context The omission's context.
    /// @param section The section's name.
    /// @return True to leave the section out.
    bool (*omit)(void* context, std::string_view section){};
};

/// Writes a keyframe's sections.
///
/// A section is named and holds bytes. A fixed section is one whose size is the
/// same in every keyframe of a session (a table the game allocates once): the
/// session keeps the first keyframe's fixed bytes to store the others as the
/// difference from them. Every other section is varying.
class Writer {
  public:

    /// Starts a writer.
    ///
    /// @param omission Sections to leave out; none by default.
    explicit Writer(Omission omission = {}) : omission_(omission) {}

    /// Starts again for another keyframe, keeping the room the regions already have.
    ///
    /// @param omission Sections to leave out; none by default.
    void restart(Omission omission = {});

    /// The room the two regions hold.
    ///
    /// A take keeps this room for the next one, so it can be more than the bytes
    /// the last image wrote.
    ///
    /// @return The bytes reserved for the fixed and varying regions.
    [[nodiscard]] std::size_t room() const noexcept {
        return fixed_.capacity() + varying_.capacity();
    }

    /// Makes room for the regions' bytes before they are written.
    ///
    /// @param fixed_bytes The fixed sections' size, as the last keyframe's was.
    /// @param varying_bytes The size to expect of the others.
    void reserve(std::size_t fixed_bytes, std::size_t varying_bytes) {
        fixed_.reserve(fixed_bytes);
        varying_.reserve(varying_bytes);
    }

    /// Starts a named section; the bytes written until the next call belong to it.
    ///
    /// @param name The section's name, at most longest_section_name bytes.
    /// @param fixed Whether the section's size never changes in a session.
    void begin(std::string_view name, bool fixed = false);

    /// Writes bytes into the current section.
    ///
    /// Nothing is written while the section is left out.
    ///
    /// @param data The bytes.
    /// @param size How many.
    void bytes(const void* data, std::size_t size);

    /// Writes one trivially copyable value as its bytes.
    ///
    /// @tparam T The value's type.
    /// @param item The value.
    template <class T>
    void value(const T& item) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&item, sizeof item);
    }

    /// Writes a count and then that many trivially copyable values.
    ///
    /// @tparam T The values' type.
    /// @param items The values.
    template <class T>
    void list(std::span<const T> items) {
        static_assert(std::is_trivially_copyable_v<T>);
        value(static_cast<uint32_t>(items.size()));
        bytes(items.data(), items.size_bytes());
    }

    /// Ends the last section. Call it once, after the last byte, before the regions are read.
    void finish();

    /// Returns the fixed sections' bytes, in the order the sections were begun.
    ///
    /// @return The bytes; the caller may change them (the session stores them as a difference),
    ///         and they are valid until the next write.
    [[nodiscard]] std::span<uint8_t> fixed() noexcept { return fixed_.bytes(); }

    /// Returns the varying sections' bytes, with the marks that name every section.
    ///
    /// @return The bytes, valid until the next write.
    [[nodiscard]] std::span<uint8_t> varying() noexcept { return varying_.bytes(); }

    /// Returns how many sections were left out.
    ///
    /// @return The count.
    [[nodiscard]] uint32_t omitted() const noexcept { return omitted_; }

  private:

    static constexpr std::size_t no_mark = static_cast<std::size_t>(-1);

    Omission omission_;
    Buffer fixed_;
    Buffer varying_;
    bool omitting_{};
    bool current_fixed_{};
    uint32_t omitted_{};
    std::size_t length_at_{no_mark}; ///< where the open section's count goes in `varying_`
    std::size_t section_start_{};    ///< the size of the open section's region when it began
};

/// Reads a keyframe's sections back, in the order they were written.
class Reader {
  public:

    /// Starts a reader over the two regions a writer made.
    ///
    /// @param fixed The fixed sections' bytes.
    /// @param varying The varying sections' bytes.
    Reader(std::span<const uint8_t> fixed, std::span<const uint8_t> varying)
        : fixed_(fixed), varying_(varying) {}

    /// Starts the next section, which must be the one named.
    ///
    /// @param name The section's name.
    /// @param fixed Whether the section is fixed, as the writer began it.
    /// @return True when the section holds bytes to read; false when it was left out (the state
    ///         it names is to stay as it is) or when the stream does not hold this section, which
    ///         `ok` then reports.
    [[nodiscard]] bool begin(std::string_view name, bool fixed = false);

    /// Reads bytes from the current section.
    ///
    /// @param[out] data Where the bytes go.
    /// @param size How many.
    /// @return False, with `ok` false, when the section holds fewer bytes.
    bool bytes(void* data, std::size_t size);

    /// Reads one trivially copyable value.
    ///
    /// @tparam T The value's type.
    /// @param[out] item The value.
    /// @return False when the section holds fewer bytes.
    template <class T>
    bool value(T* item) {
        static_assert(std::is_trivially_copyable_v<T>);
        return bytes(item, sizeof *item);
    }

    /// Reads the count a writer's list wrote, so that the caller makes room and reads the values.
    ///
    /// @param[out] count How many values follow.
    /// @param most The most values the list may hold; a larger count is a fault.
    /// @return False when the count cannot be read or is larger than `most`.
    bool list_count(uint32_t* count, std::size_t most) {
        if (!value(count))
            return false;
        return *count <= most || fail("a list is longer than its table");
    }

    /// Tells whether every read so far found what it expected.
    ///
    /// @return False after the first fault.
    [[nodiscard]] bool ok() const noexcept { return error_[0] == '\0'; }

    /// Returns the first fault.
    ///
    /// @return The sentence; empty while `ok`.
    [[nodiscard]] const char* error() const noexcept { return error_.data(); }

    /// Records a fault, when none was recorded.
    ///
    /// @param what The sentence.
    /// @param detail A name the sentence is about, added after a colon; none by default.
    /// @return False, so a caller can return it.
    bool fail(std::string_view what, std::string_view detail = {});

    /// Ends the last section and checks that both regions were read to their end.
    ///
    /// @return False, with `ok` false, when a section was left unread or bytes remain.
    bool finish();

  private:

    /// Checks that the section just read was read to its end.
    bool close_section();

    std::span<const uint8_t> fixed_;
    std::span<const uint8_t> varying_;
    std::size_t fixed_at_{};
    std::size_t varying_at_{};
    bool current_fixed_{};
    bool section_open_{};
    std::size_t section_start_{};
    std::size_t section_size_{};
    std::array<char, longest_section_name + 1> section_name_{};
    std::array<char, longest_error + 1> error_{};
};

/// What a playback session's match holds that a keyframe has to carry.
///
/// Counts the match's own state, so that a test or a measurement can say what
/// a keyframe of a session held: which structures a playback session uses and
/// how many of each exist.
struct Census {
    uint32_t live_units{};           ///< unit slots that hold a unit
    uint32_t units_with_instances{}; ///< live units that have a model instance
    uint32_t units_with_scripts{};   ///< live units that run a script
    uint32_t ground_runtimes{};      ///< movement objects of ground units
    uint32_t mirrored_ground{};      ///< of them, the ones another machine steers
    uint32_t mirrored_routes{};      ///< mirrored ground units holding a route
    uint32_t air_drivers_local{};    ///< flying units steered here
    uint32_t air_drivers_mirrored{}; ///< flying units another machine steers
    uint32_t orders{};               ///< orders the match owns
    uint32_t unit_order_queues{};    ///< unit slots whose order queue is not empty
    uint32_t wrecks{};               ///< corpses placed
    uint32_t squads{};               ///< squads of units
    uint32_t rule_state_tables{};    ///< rule-state tables the match keeps
    uint32_t effect_emitters{};      ///< emitters of the effect layers
    uint32_t effect_particles{};     ///< live particles of the effect pools
    uint32_t effect_explosions{};    ///< explosion records in use
    uint32_t effect_debris{};        ///< debris pieces in use
    uint32_t effect_fragments{};     ///< shatter fragments in use
    uint32_t seen_units{};           ///< units on the players' seen lists
    uint32_t radar_units{};          ///< units on the players' radar lists
    uint32_t contact_players{};      ///< players whose radar and sonar picture is kept
    uint32_t scenario_conditions{};  ///< victory and defeat conditions a scenario registered
    uint32_t remembered_sights{};    ///< stamps of the viewer's dead units
    uint32_t movement_classes{};     ///< movement classes that have a map
    uint32_t placed_features{};      ///< features standing on the map
    bool structure_gifts{};          ///< the structure-gift rule keeps state
    bool computer_state{};           ///< the built-in computer player keeps state
    bool seat_state{};               ///< a brain keeps state
};

} // namespace oa::sim::match_runtime::keyframe
