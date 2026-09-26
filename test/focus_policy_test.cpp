// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// focus_policy_test.cpp - GoogleTest for the D-14/D-15 decision function
//
// Verifies commitPendingOnDeactivate() directly, without mocking an InputContext.

#include "focuspolicy.h"

#include <gtest/gtest.h>

// The core of D-14. On a real focus loss (which could move to another application),
// uncommitted text is not committed. This is the line that prevents a recurrence of
// G-01.1-4 (the upstream fcitx5 bug where commitStringDelegate ignores its IC argument and
// lands the text in another application).
TEST(SekkaFocusPolicyTest, FocusOutDoesNotCommitPending) {
    EXPECT_FALSE(
        fcitx::commitPendingOnDeactivate(fcitx::EventType::InputContextFocusOut));
}

// The core of D-15. On an input method switch within the same application (a trigger key, a
// group switch and a capability change all arrive as this EventType; RESEARCH.md Pattern 2)
// the destination does not change, so uncommitted text is committed.
TEST(SekkaFocusPolicyTest, SwitchInputMethodCommitsPending) {
    EXPECT_TRUE(fcitx::commitPendingOnDeactivate(
        fcitx::EventType::InputContextSwitchInputMethod));
}

// Pins the safe-side fallback for an unexpected EventType (RESEARCH.md
// `## Security Domain` V5 Input Validation). Two kinds that are not expected to reach
// deactivate are passed, and both must fall to the do-not-commit side.
TEST(SekkaFocusPolicyTest, UnknownEventTypeDoesNotCommitPending) {
    EXPECT_FALSE(
        fcitx::commitPendingOnDeactivate(fcitx::EventType::InputContextFocusIn));
    EXPECT_FALSE(
        fcitx::commitPendingOnDeactivate(fcitx::EventType::InputContextKeyEvent));
}
