// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Fetching the player's own Windows installer of Total Annihilation:
// Commander Pack from their GOG account. The player signs in on GOG's own
// page, in the game's sign-in window or in their web browser; the address
// that page ends on carries a code,
// which is exchanged for a sign-in token. The token asks GOG for the
// installer's address, and the installer is downloaded to a folder the
// caller names, resuming a part an earlier try left. Nothing in it is run:
// the setup copies the game out of it as it does from an installer the
// player downloaded themselves. The token is kept in memory only, for one
// download.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app::gog {

/// GOG's number for Total Annihilation: Commander Pack.
inline constexpr std::string_view commander_pack_product = "1207658880";

/// Tells whether this system can reach GOG, which needs https.
///
/// @return true when https can be fetched
[[nodiscard]] bool available() noexcept;

/// The page where the player signs in to GOG. It ends on an address that
/// starts with landing_address and carries the code login_code reads.
///
/// @return an https address on auth.gog.com
[[nodiscard]] std::string sign_in_address();

/// The start of the address GOG's sign-in page ends on.
///
/// @return an https address on embed.gog.com
[[nodiscard]] std::string_view landing_address() noexcept;

/// Reads the sign-in code out of what the player copied: the whole address
/// GOG's page ended on, or the code alone.
///
/// @param text the copied text; spaces and line ends around it are ignored
/// @return the code; nothing when the text holds none
[[nodiscard]] std::optional<std::string> login_code(std::string_view text);

/// Where a download is.
enum class Phase : uint8_t {
    idle,        ///< nothing has started
    signing_in,  ///< the code is being exchanged for a token
    finding,     ///< GOG is asked where the installer is
    downloading, ///< the installer's bytes are arriving
    done,        ///< the installer is whole, at Status::installer
    failed,      ///< it stopped; Status::failure says why
};

/// Why a download stopped.
enum class Failure : uint8_t {
    none,          ///< it has not failed
    sign_in,       ///< GOG did not accept the code, which is used once and soon expires
    not_owned,     ///< the account does not hold Total Annihilation: Commander Pack
    no_installer,  ///< GOG lists no Windows installer for it
    no_https,      ///< this system cannot fetch https
    network,       ///< GOG could not be reached, or the connection broke
    unexpected,    ///< GOG answered with something this reader does not know
    unknown_build, ///< the installer GOG sent is not the build the setup copies from
    disk,         ///< the file could not be written
    no_space,      ///< the disk has no room for the installer
    cancelled,     ///< the player cancelled it
};

/// How far a download has got. Copied out under the download's lock.
struct Status {
    Phase phase{Phase::idle};       ///< where it is
    Failure failure{Failure::none}; ///< why it stopped, when phase is failed
    uint64_t received{};            ///< bytes of the installer on disk so far
    uint64_t size{};                ///< the installer's size; 0 until GOG has said
    std::filesystem::path installer{}; ///< the whole installer, when phase is done
    std::string detail{};           ///< what went wrong, in English, for the log
};

/// The English sentence the player reads for a failure.
///
/// @param failure the failure
/// @return a sentence of the interface catalogue; empty for none
[[nodiscard]] std::string_view failure_text(Failure failure) noexcept;

/// One download from GOG, on its own thread. Destroying it cancels a download
/// still running and waits for its thread.
class Download {
  public:
    Download();
    ~Download();
    Download(const Download&) = delete;
    Download& operator=(const Download&) = delete;

    /// Starts signing in with the code and downloading the installer into
    /// `folder`, which is made when missing. The file keeps the name GOG
    /// publishes it under, and a `.part` name while it is incomplete; a whole
    /// file already there of the size GOG names is taken as it is. A download
    /// still running is cancelled first.
    ///
    /// @param code the code login_code read
    /// @param folder where the installer is written
    void start(std::string code, std::filesystem::path folder);

    /// Asks a running download to stop; its status turns failed with
    /// Failure::cancelled. The part stays, so a later try resumes it.
    void cancel();

    /// Returns how far the download has got.
    ///
    /// @return a copy of its status
    [[nodiscard]] Status status() const;

  private:
    struct State;
    std::shared_ptr<State> state_{};
};

} // namespace oa::app::gog
