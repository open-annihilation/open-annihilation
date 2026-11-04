// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Who sees each unfinished feature. The release constant is a parameter of
// the decision, so one binary covers a constant that is off and one that is
// on. The number a completed login stores is decided here; the status widget
// test is what stores and clears it.
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/screens/login.hpp"

#include <cstdio>
#include <cstring>

namespace {

using namespace oa::boneyards;

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void check_shown(
    ReleaseFeature feature, int32_t status, int32_t released, int32_t beta, bool expect
) {
    const bool shown = feature_visible(feature, status, released, beta);
    if (shown == expect)
        return;
    std::fprintf(
        stderr,
        "FAIL feature %d status %d released %d beta %d shown %d\n",
        static_cast<int>(feature),
        status,
        released,
        beta,
        shown ? 1 : 0
    );
    ++g_failures;
}

// QuickPlay, Locator and Matchmaking: status 1 to 5, released 0 or 1.
// BetaLoginUX does not change those three. The registration form follows
// its constant and BetaLoginUX, not the live account status.
void test_visibility_matrix() {
    const ReleaseFeature consoles[] = {
        ReleaseFeature::quickplay, ReleaseFeature::locator, ReleaseFeature::matchmaking
    };
    for (const ReleaseFeature feature : consoles) {
        for (int32_t status = 1; status <= 5; ++status) {
            for (int32_t released = 0; released <= 1; ++released) {
                const bool expect = released != 0 || account_sees_unfinished(status);
                check_shown(feature, status, released, 0, expect);
                check_shown(feature, status, released, 1, expect);
            }
        }
        CHECK(gated_feature_shown(feature, 0) == (release_flag(feature) != 0));
        CHECK(gated_feature_shown(feature, 1));
    }

    for (int32_t status = 1; status <= 5; ++status) {
        check_shown(ReleaseFeature::registration_form, status, 0, 0, false);
        check_shown(ReleaseFeature::registration_form, status, 1, 0, true);
        const int32_t beta = account_sees_unfinished(status) ? 1 : 0;
        check_shown(ReleaseFeature::registration_form, status, 0, beta, beta != 0);
    }
    for (int32_t released = 0; released <= 1; ++released) {
        for (int32_t beta = 0; beta <= 1; ++beta)
            check_shown(
                ReleaseFeature::registration_form, 1, released, beta, released != 0 || beta != 0
            );
    }

    CHECK(!account_sees_unfinished(0));
    CHECK(!account_sees_unfinished(kAccountStatusActive));
    CHECK(!account_sees_unfinished(2));
    CHECK(!account_sees_unfinished(3));
    CHECK(account_sees_unfinished(kAccountStatusAdmin));
    CHECK(account_sees_unfinished(kAccountStatusBetaTester));
    CHECK(
        release_flag(ReleaseFeature::quickplay) == 0 || release_flag(ReleaseFeature::quickplay) == 1
    );
    CHECK(release_flag(ReleaseFeature::locator) == 0 || release_flag(ReleaseFeature::locator) == 1);
    CHECK(
        release_flag(ReleaseFeature::matchmaking) == 0 ||
        release_flag(ReleaseFeature::matchmaking) == 1
    );
    CHECK(
        release_flag(ReleaseFeature::registration_form) == 0 ||
        release_flag(ReleaseFeature::registration_form) == 1
    );
}

// REGISTER offers the in-game form when BetaLoginUX is set, and also when
// the form's own constant is on. The web page is not this decision.
void test_registration_access() {
    const BoneyardsService service = boneyards_offline_service(nullptr);
    screens::LoginDialog dialog;
    screens::login_init(&dialog, &service, nullptr, nullptr);
    screens::login_set_registration_access(&dialog, 0);
    CHECK(dialog.registration_form == release_flag(ReleaseFeature::registration_form));
    screens::login_set_registration_access(&dialog, 1);
    CHECK(dialog.registration_form == 1);
}

// A completed Admin or Beta Tester login stores 1. Any other status stores 0.
void test_preference_for_status() {
    CHECK(std::strcmp(kBetaLoginUxKey, "BetaLoginUX") == 0);
    CHECK(beta_login_ux_for_status(0) == 0);
    CHECK(beta_login_ux_for_status(kAccountStatusActive) == 0);
    CHECK(beta_login_ux_for_status(2) == 0);
    CHECK(beta_login_ux_for_status(3) == 0);
    CHECK(beta_login_ux_for_status(kAccountStatusAdmin) == 1);
    CHECK(beta_login_ux_for_status(kAccountStatusBetaTester) == 1);
}

} // namespace

int main() {
    test_visibility_matrix();
    test_registration_access();
    test_preference_for_status();
    if (g_failures != 0) {
        std::fprintf(stderr, "release: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("release: ok");
    return 0;
}
