// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Who sees each unfinished feature. QuickPlay, Locator and Matchmaking are
// on screen when that feature's release constant is set, or when the
// account status is Admin (4) or Beta Tester (5); every other account keeps
// the dust cover. The in-game registration form is on screen when its
// release constant is set or the BetaLoginUX setting is set. A finished
// feature is left on screen for every account and its constant is removed.
#pragma once

#include <cstdint>

#ifndef OA_BONEYARDS_QUICKPLAY_RELEASED
#define OA_BONEYARDS_QUICKPLAY_RELEASED 0
#endif
#ifndef OA_BONEYARDS_LOCATOR_RELEASED
#define OA_BONEYARDS_LOCATOR_RELEASED 0
#endif
#ifndef OA_BONEYARDS_MATCHMAKING_RELEASED
#define OA_BONEYARDS_MATCHMAKING_RELEASED 0
#endif
#ifndef OA_BONEYARDS_REGISTRATION_FORM_RELEASED
#define OA_BONEYARDS_REGISTRATION_FORM_RELEASED 0
#endif

namespace oa::boneyards {

// Account status codes a dossier reports. Admin and Beta Tester see the
// unfinished consoles; every other code is an ordinary account.
inline constexpr int32_t kAccountStatusActive = 1;
inline constexpr int32_t kAccountStatusAdmin = 4;
inline constexpr int32_t kAccountStatusBetaTester = 5;

// Unfinished features, each with its own release constant.
enum class ReleaseFeature : int32_t {
    quickplay = 0,
    locator = 1,
    matchmaking = 2,
    registration_form = 3,
};

// BetaLoginUX in the Boneyards settings, beside Username and UnitLimit.
inline constexpr const char* kBetaLoginUxKey = "BetaLoginUX";

/// Tells whether an account status sees unfinished consoles.
///
/// Admin (4) and Beta Tester (5) do. Every other status, including none, does
/// not.
///
/// @param account_status dossier USER_STATUS
/// @return true for Admin and Beta Tester
inline bool account_sees_unfinished(int32_t account_status) {
    return account_status == kAccountStatusAdmin || account_status == kAccountStatusBetaTester;
}

/// Returns the release constant of one feature, 1 when that feature is on for
/// every account.
///
/// The constants are the build's OA_BONEYARDS_*_RELEASED options. A feature
/// that has shipped has no constant; this returns 0 for a feature the build
/// does not know.
///
/// @param feature unfinished feature
/// @return 1 when the feature is released, otherwise 0
inline int32_t release_flag(ReleaseFeature feature) {
    switch (feature) {
    case ReleaseFeature::quickplay:
        return OA_BONEYARDS_QUICKPLAY_RELEASED;
    case ReleaseFeature::locator:
        return OA_BONEYARDS_LOCATOR_RELEASED;
    case ReleaseFeature::matchmaking:
        return OA_BONEYARDS_MATCHMAKING_RELEASED;
    case ReleaseFeature::registration_form:
        return OA_BONEYARDS_REGISTRATION_FORM_RELEASED;
    }
    return 0;
}

/// Tells whether the player sees a feature instead of its dust cover or its
/// absence.
///
/// QuickPlay, Locator and Matchmaking are shown when `released` is set or
/// `account_status` is Admin or Beta Tester. The registration form is shown
/// when `released` is set or `beta_login_ux` is set, and does not read
/// `account_status`.
///
/// @param feature unfinished feature
/// @param account_status dossier USER_STATUS
/// @param released that feature's release constant, 0 or 1
/// @param beta_login_ux stored BetaLoginUX preference, 0 or 1
/// @return true when the feature is on screen
inline bool feature_visible(
    ReleaseFeature feature, int32_t account_status, int32_t released, int32_t beta_login_ux
) {
    if (released != 0)
        return true;
    if (feature == ReleaseFeature::registration_form)
        return beta_login_ux != 0;
    return account_sees_unfinished(account_status);
}

/// Tells whether QuickPlay, Locator or Matchmaking is on screen for this
/// session.
///
/// `privileged` is the session bit set for a dossier status of Admin or Beta
/// Tester. A released feature is on screen for every account. The
/// registration form does not use this: it follows its release constant and
/// BetaLoginUX.
///
/// @param feature QuickPlay, Locator or Matchmaking
/// @param privileged 1 when the account is Admin or Beta Tester
/// @return true when that console is on screen
inline bool gated_feature_shown(ReleaseFeature feature, uint8_t privileged) {
    const int32_t status = privileged != 0 ? kAccountStatusAdmin : kAccountStatusActive;
    return feature_visible(feature, status, release_flag(feature), 0);
}

/// Returns the BetaLoginUX number a completed login stores.
///
/// Admin and Beta Tester store 1. Every other status stores 0. A login
/// that does not complete stores nothing; this is only the number.
///
/// @param account_status dossier USER_STATUS of the login that completed
/// @return 1 for Admin and Beta Tester, otherwise 0
inline int32_t beta_login_ux_for_status(int32_t account_status) {
    return account_sees_unfinished(account_status) ? 1 : 0;
}

} // namespace oa::boneyards
