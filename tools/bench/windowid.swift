// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// windowid PID: prints the window number of the largest window process PID
// shows on screen, or 0 when it shows none, for screencapture -l (run.sh).
import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2, let pid = Int32(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: windowid PID\n".data(using: .utf8)!)
    exit(2)
}
let windows =
    CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
var largest = (number: 0, area: 0.0)
for window in windows where (window[kCGWindowOwnerPID as String] as? Int32) == pid {
    guard let bounds = window[kCGWindowBounds as String] as? [String: Double],
        let number = window[kCGWindowNumber as String] as? Int
    else { continue }
    let area = (bounds["Width"] ?? 0) * (bounds["Height"] ?? 0)
    if area > largest.area { largest = (number, area) }
}
print(largest.number)
