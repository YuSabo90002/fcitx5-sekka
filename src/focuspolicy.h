// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// focuspolicy.h - pure function deciding what happens to uncommitted text on focus loss
#ifndef FOCUSPOLICY_H
#define FOCUSPOLICY_H

#include <fcitx/event.h>

namespace fcitx {

/// Decides, from the `InputContextEvent::type()` passed to
/// `SekkaEngine::deactivate`, whether uncommitted text may be committed.
///
/// D-14: on a real focus loss (`EventType::InputContextFocusOut`) the destination
/// could be another application, so this returns false (do not commit). As long as
/// Sekka never calls `commitString`, there is no path that can hit the upstream
/// fcitx5 bug where `WaylandIMInputContextV2::commitStringDelegate` ignores its IC
/// argument and writes to the seat-shared object (G-01.1-4).
///
/// D-15: on an input method switch within the same application
/// (`EventType::InputContextSwitchInputMethod`, which is how a trigger key, a group
/// switch and a capability change all arrive), the destination is the same
/// application, so this returns true (do commit).
///
/// The implementation is an **allow list** of known values rather than a negative
/// condition ("not FocusOut"). That is a deliberate design choice, to satisfy the
/// requirement that "every unexpected `EventType` falls back to the safe side (do
/// not commit)" (RESEARCH.md `## Security Domain` V5 Input Validation). A negative
/// condition could let an unknown kind become true by mistake, whereas an allow list
/// drops unknown kinds to false automatically.
inline bool commitPendingOnDeactivate(EventType type) {
    return type == EventType::InputContextSwitchInputMethod;
}

} // namespace fcitx

#endif // FOCUSPOLICY_H
