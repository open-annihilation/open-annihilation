// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// macOS: the file manager's trash. A failure leaves the file where it was.

#include "oa/platform/trash.hpp"

#include "oa/platform/files.hpp"

#import <Foundation/Foundation.h>

#include <filesystem>
#include <string>

namespace oa::platform {

bool system_has_trash() noexcept {
    return true;
}

TrashMove move_to_trash(const std::filesystem::path& file) {
    TrashMove out;
    @autoreleasepool {
        const std::string spelling = path_to_utf8(file);
        if (spelling.empty()) {
            out.error = "the file is not there";
            return out;
        }
        NSString* text = [NSString stringWithUTF8String:spelling.c_str()];
        if (text == nil) {
            out.error = "the file's path cannot be written";
            return out;
        }
        NSURL* url = [NSURL fileURLWithPath:text isDirectory:NO];
        NSURL* kept = nil;
        NSError* failure = nil;
        const BOOL moved = [[NSFileManager defaultManager] trashItemAtURL:url
                                                         resultingItemURL:&kept
                                                                    error:&failure];
        if (!moved) {
            if (failure != nil && failure.localizedDescription != nil) {
                const char* reason = failure.localizedDescription.UTF8String;
                if (reason != nullptr)
                    out.error = reason;
            }
            if (out.error.empty())
                out.error = "the trash did not take the file";
            return out;
        }
        out.moved = true;
        out.place = TrashPlace::trash;
        if (kept != nil && kept.path != nil) {
            const char* kept_text = kept.path.UTF8String;
            if (kept_text != nullptr)
                out.kept_at = kept_text;
        }
    }
    return out;
}

} // namespace oa::platform
