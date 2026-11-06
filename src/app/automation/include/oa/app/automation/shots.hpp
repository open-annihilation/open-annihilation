// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One picture of the game, for the MCP server: the frame it presented, or
// the one it composed, whole or a region, scaled down, as a PNG. A presented
// picture is started at a pump and encoded at the presented stage of that
// frame. The surface stays in the buffer until stop.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oa::ui::frontend_renderer {
struct Surface;
}

namespace oa::app {
struct AutomationHost;
struct CheckHost;
} // namespace oa::app

namespace oa::app::automation {

/// What a picture request asks for.
struct ShotRequest {
    bool presented{true};   ///< the presented frame; false is the composed one
    bool window_space{};    ///< a presented region is in the window's pixels
    bool has_region{};      ///< region_x and the rest name a region
    int32_t region_x{};     ///< the region's left column
    int32_t region_y{};     ///< the region's top row
    int32_t region_w{};     ///< the region's width
    int32_t region_h{};     ///< the region's height
    int scale_percent{100}; ///< the picture's size, 1 to 100 percent
    int32_t max_width{};    ///< the widest result, in pixels; 0 leaves the width
};

/// How many stages a picture waits for the frame. The same number as
/// presented_frame_wait_limit in frames.hpp; shots.cpp checks they match.
inline constexpr uint32_t shot_frame_wait_limit = 60;

/// Which stage of the frame is asking the buffer to move on.
enum class ShotStep : uint8_t {
    pump,      ///< start a presented capture; a composed one only waits
    presented, ///< encode, once the frame is in hand
};

/// The picture being taken, and the PNG once it is ready.
///
/// Stop a capture before destroying the buffer, or before starting another
/// into a different buffer: the game copies into this buffer's surface.
class ShotBuffer {
  public:

    /// An empty buffer, not capturing.
    ShotBuffer();

    /// Frees the picture. A capture still running must be stopped first.
    ~ShotBuffer();

    /// Takes over another buffer's picture; that buffer is then empty.
    ShotBuffer(ShotBuffer&&) noexcept;

    /// Takes over another buffer's picture; that buffer is then empty.
    ///
    /// @return this buffer
    ShotBuffer& operator=(ShotBuffer&&) noexcept;
    ShotBuffer(const ShotBuffer&) = delete;
    ShotBuffer& operator=(const ShotBuffer&) = delete;

    /// Tells whether a picture is still being waited for.
    ///
    /// @return true while one is open
    [[nodiscard]] bool waiting() const noexcept;

    /// Tells whether the PNG is ready.
    ///
    /// @return true when encode finished
    [[nodiscard]] bool ready() const noexcept;

    /// Tells whether the picture failed.
    ///
    /// @return true when it will not finish
    [[nodiscard]] bool failed() const noexcept;

    /// Returns why the picture failed.
    ///
    /// @return the reason; empty when it has not failed
    [[nodiscard]] const std::string& failure() const;

    /// Returns the PNG bytes.
    ///
    /// @return the file; empty until the picture is ready
    [[nodiscard]] const std::vector<uint8_t>& png() const;

    /// Returns the PNG's width.
    ///
    /// @return the width in pixels
    [[nodiscard]] int32_t width() const noexcept;

    /// Returns the PNG's height.
    ///
    /// @return the height in pixels
    [[nodiscard]] int32_t height() const noexcept;

    /// Returns where the pixels came from.
    ///
    /// @return presented or composed
    [[nodiscard]] const std::string& source() const;

    /// How many presented stages this picture has waited.
    ///
    /// @return the count
    [[nodiscard]] uint32_t waited() const noexcept;

    /// Stops a presented capture. Does nothing when this buffer is not the
    /// one the game is copying into.
    ///
    /// @param automation the automation host
    void stop(const AutomationHost& automation);

    /// Stops a capture and forgets the picture, so another can be taken.
    ///
    /// @param automation the automation host
    void abandon(const AutomationHost& automation);

  private:

    struct Impl;
    std::unique_ptr<Impl> impl_;
    /// Ends the picture with an error (shots.cpp).
    ///
    /// @param picture the picture
    /// @param automation the automation host
    /// @param why the error
    friend void fail_shot(Impl& picture, const AutomationHost& automation, std::string why);

    /// Encodes a captured frame into the picture (shots.cpp).
    ///
    /// @param picture the picture
    /// @param ask the request
    /// @param automation the automation host
    /// @param check the check host
    /// @param frame the captured frame
    /// @param source_name the frame's source
    friend void encode_shot(
        Impl& picture,
        const ShotRequest& ask,
        const AutomationHost& automation,
        const CheckHost& check,
        const oa::ui::frontend_renderer::Surface& frame,
        std::string source_name
    );
    /// Moves a picture on by one step (shots.cpp).
    ///
    /// @param buffer the picture's buffer
    /// @param ask the request
    /// @param check the check host
    /// @param automation the automation host
    /// @param step the step
    friend void advance_shot(
        ShotBuffer& buffer,
        const ShotRequest& ask,
        const CheckHost& check,
        const AutomationHost& automation,
        ShotStep step
    );
};

/// Moves a picture on by one stage.
///
/// At the pump, a presented picture asks the game to copy the frame it is
/// about to show. At the presented stage the pixels are encoded, or the
/// wait continues until presented_frame_wait_limit stages have passed.
/// A composed picture reads the frame the game last composed, at the
/// presented stage. A region of a presented picture in the game's space
/// covers every presented pixel that shows part of those canvas pixels.
///
/// @param[in,out] buffer the picture
/// @param ask what was asked for; the same request at every stage
/// @param check the check host
/// @param automation the automation host
/// @param step the stage being served
void advance_shot(
    ShotBuffer& buffer,
    const ShotRequest& ask,
    const CheckHost& check,
    const AutomationHost& automation,
    ShotStep step
);

} // namespace oa::app::automation
