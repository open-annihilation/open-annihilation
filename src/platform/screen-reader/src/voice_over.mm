// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether VoiceOver is running (screen_reader.cpp). The system call is
// Objective-C, so it lives in this file and the rest of the bridge does not.

#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

namespace oa::platform::screen_reader {

/// Reports whether VoiceOver is on.
///
/// @return true while VoiceOver is running
bool voice_over_running() {
#if TARGET_OS_IPHONE
    return UIAccessibilityIsVoiceOverRunning();
#else
    @autoreleasepool {
        return NSWorkspace.sharedWorkspace.isVoiceOverEnabled;
    }
#endif
}

} // namespace oa::platform::screen_reader
