// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// guide_test.cpp - GoogleTest for the real-time guide (Phase 16, GUIDE-01 to GUIDE-03)
//
// 1. `sekka.h` is included directly (the discipline of `save_test.cpp` and
//    `registration_test.cpp`: never keep a hand-made parallel copy of the C ABI
//    declarations), so a drift between the hand-written declarations and `capi.rs`
//    surfaces as a build failure.
// 2. D-233 (ctest includes the C++ side of the guide): the functions that
//    `SekkaState` calls to show the guide are called here as they are, not copied
//    into the tests. `fcitx::guideAuxUpText` builds the auxUp text that
//    `updatePreedit()` shows, and `fcitx::guideTimerDelayUsec` turns the remaining
//    milliseconds into the wait of the one-shot timer that clears the guide.
// 3. What ctest does NOT cover, on purpose: the call that puts the text into the
//    input panel (`setAuxUp`), the one-shot timer of the event loop together with
//    its callback, and the lifetime of the input context. All three need an fcitx
//    `Instance`, and `sekka.cpp` is not linked into `sekka_test` for the same reason
//    as in `candidate_list_test.cpp` and `registration_test.cpp`. They are pinned by
//    the isolated-fcitx5 E2E instead: the content of auxUp by
//    `e2e/test_realtime_guide.py` (every test there), and the clearing at the 30th
//    second without any key press by the 31-second test of plan 16-03 in the same
//    file.

#include "sekka.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace {

// Types ASCII text as plain key presses (keysym = character code, no modifiers).
void typeAscii(SekkaContextFfi *ctx, const char *text) {
    for (const char *p = text; *p != '\0'; ++p) {
        sekka_context_process_key_event(ctx, static_cast<uint32_t>(*p), 0, 0);
    }
}

// Raw read of the C ABI: the guide as the library returns it (never NULL for a valid
// context), released exactly once.
std::string takeGuide(SekkaContextFfi *ctx) {
    char *guide = sekka_context_get_guide(ctx);
    EXPECT_NE(guide, nullptr);
    std::string text = guide ? guide : "";
    if (guide) {
        sekka_free_string(guide);
    }
    return text;
}

// The auxUp text through the very function `updatePreedit()` uses.
fcitx::Text auxUpOf(SekkaContextFfi *ctx) {
    return fcitx::guideAuxUpText(sekka_context_get_guide(ctx));
}

constexpr uint32_t kCtrl = 0x4;

} // namespace

// === The C ABI contract ===

// NULL is answered with NULL / 0 and never crashes.
TEST(SekkaGuideTest, NewFunctionsAreNullSafe) {
    EXPECT_EQ(sekka_context_get_guide(nullptr), nullptr);
    EXPECT_EQ(sekka_context_guide_remaining_ms(nullptr), 0);
}

// GUIDE-02: nothing is shown and no time is left until the first trigger.
TEST(SekkaGuideTest, GuideIsOffBeforeTheFirstTrigger) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    typeAscii(ctx, "Nihongo");
    EXPECT_EQ(takeGuide(ctx), "");
    EXPECT_EQ(sekka_context_guide_remaining_ms(ctx), 0);

    sekka_context_free(ctx);
}

// GUIDE-01: after the trigger (a pass-through on an empty buffer) the guide is the
// first candidate in brackets.
TEST(SekkaGuideTest, GuideIsBracketedAfterTheTrigger) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    EXPECT_EQ(sekka_context_trigger(ctx), 0);
    typeAscii(ctx, "Nihongo");
    const std::string guide = takeGuide(ctx);
    ASSERT_GT(guide.size(), size_t{2});
    EXPECT_EQ(guide.front(), '[');
    EXPECT_EQ(guide.back(), ']');

    sekka_context_free(ctx);
}

// GUIDE-02: right after the trigger the window has just about 30 seconds left.
TEST(SekkaGuideTest, RemainingTimeIsWithinThirtySecondsAfterTheTrigger) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_trigger(ctx);
    const int left = sekka_context_guide_remaining_ms(ctx);
    EXPECT_GT(left, 29000);
    EXPECT_LE(left, 30001);

    sekka_context_free(ctx);
}

// GUIDE-03: Ctrl-G and the reset close the window; the next trigger opens it again.
TEST(SekkaGuideTest, CtrlGAndResetCloseTheWindow) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_trigger(ctx);
    EXPECT_EQ(sekka_context_process_key_event(ctx, 'G', kCtrl, 0), 0);
    EXPECT_EQ(sekka_context_guide_remaining_ms(ctx), 0);

    sekka_context_trigger(ctx);
    EXPECT_GT(sekka_context_guide_remaining_ms(ctx), 0);
    sekka_context_reset(ctx);
    EXPECT_EQ(sekka_context_guide_remaining_ms(ctx), 0);

    sekka_context_free(ctx);
}

// === The C++ side: the functions SekkaState calls ===

// GUIDE-02 / T-16-01-03: NULL from the C ABI shows nothing and does not crash, and
// an empty guide (before the first trigger) is an empty Text, so `updatePreedit()`
// leaves auxUp alone.
TEST(SekkaGuideTest, AuxUpTextIsEmptyForNullAndBeforeTheFirstTrigger) {
    EXPECT_TRUE(fcitx::guideAuxUpText(nullptr).empty());

    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);
    typeAscii(ctx, "Ka");
    EXPECT_TRUE(auxUpOf(ctx).empty());
    sekka_context_free(ctx);
}

// GUIDE-01 / D-232: the auxUp text is the library's string as one plain segment.
// Without a dictionary the first candidate of `Ka` is the hiragana `か` (pinned by
// `CtrlJRightAfterStagingEntersReselect` in basic_test.cpp).
TEST(SekkaGuideTest, AuxUpTextIsTheGuideAsOnePlainSegment) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    EXPECT_EQ(sekka_context_trigger(ctx), 0);
    typeAscii(ctx, "Ka");
    EXPECT_EQ(takeGuide(ctx), "[か]");

    const fcitx::Text aux = auxUpOf(ctx);
    EXPECT_EQ(aux.size(), size_t{1});
    EXPECT_EQ(aux.stringAt(0), "[か]");
    EXPECT_EQ(aux.toString(), "[か]");
    EXPECT_TRUE(aux.formatAt(0) == fcitx::TextFormatFlag::NoFlag);

    sekka_context_free(ctx);
}

// D-232 / D-231: the auxUp text is empty wherever the guide is hidden or stopped.
TEST(SekkaGuideTest, AuxUpTextIsEmptyWhereverTheGuideIsHiddenOrStopped) {
    // (a) The commit display and reselection.
    {
        auto *ctx = sekka_context_new();
        ASSERT_NE(ctx, nullptr);
        sekka_context_trigger(ctx);
        typeAscii(ctx, "Ka");
        EXPECT_FALSE(auxUpOf(ctx).empty());
        sekka_context_trigger(ctx);
        EXPECT_TRUE(auxUpOf(ctx).empty());
        sekka_context_trigger(ctx);
        EXPECT_GE(sekka_context_get_candidate_count(ctx), 2);
        EXPECT_TRUE(auxUpOf(ctx).empty());
        sekka_context_free(ctx);
    }
    // (b) Word registration.
    {
        auto *ctx = sekka_context_new();
        ASSERT_NE(ctx, nullptr);
        sekka_context_trigger(ctx);
        typeAscii(ctx, "Ka");
        sekka_context_trigger(ctx);
        EXPECT_EQ(sekka_context_process_key_event(ctx, 'r', kCtrl, 0), 1);
        EXPECT_EQ(sekka_context_is_registering(ctx), 1);
        EXPECT_TRUE(auxUpOf(ctx).empty());
        typeAscii(ctx, "Ki");
        EXPECT_TRUE(auxUpOf(ctx).empty());
        sekka_context_free(ctx);
    }
    // (c) After Ctrl-G with romaji (committed and forwarded, D-03).
    {
        auto *ctx = sekka_context_new();
        ASSERT_NE(ctx, nullptr);
        sekka_context_trigger(ctx);
        typeAscii(ctx, "Ka");
        EXPECT_EQ(sekka_context_process_key_event(ctx, 'G', kCtrl, 0), 1);
        typeAscii(ctx, "Ka");
        EXPECT_TRUE(auxUpOf(ctx).empty());
        sekka_context_free(ctx);
    }
    // (d) After the reset.
    {
        auto *ctx = sekka_context_new();
        ASSERT_NE(ctx, nullptr);
        sekka_context_trigger(ctx);
        typeAscii(ctx, "Ka");
        sekka_context_reset(ctx);
        typeAscii(ctx, "Ka");
        EXPECT_TRUE(auxUpOf(ctx).empty());
        sekka_context_free(ctx);
    }
}

// D-229 / T-16-02-02: the timer wait is the remaining milliseconds times 1000,
// 0 when the window is closed, and cannot overflow.
TEST(SekkaGuideTest, TimerDelayIsZeroWhenClosedAndMicrosecondsWhenOpen) {
    EXPECT_EQ(fcitx::guideTimerDelayUsec(0), uint64_t{0});
    EXPECT_EQ(fcitx::guideTimerDelayUsec(-1), uint64_t{0});
    EXPECT_EQ(fcitx::guideTimerDelayUsec(std::numeric_limits<int>::min()),
              uint64_t{0});
    EXPECT_EQ(fcitx::guideTimerDelayUsec(1), uint64_t{1000});
    EXPECT_EQ(fcitx::guideTimerDelayUsec(30001), uint64_t{30001000});
    EXPECT_EQ(fcitx::guideTimerDelayUsec(std::numeric_limits<int>::max()),
              static_cast<uint64_t>(std::numeric_limits<int>::max()) * 1000);
    EXPECT_EQ(fcitx::guideTimerDelayUsec(sekka_context_guide_remaining_ms(nullptr)),
              uint64_t{0});

    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(fcitx::guideTimerDelayUsec(sekka_context_guide_remaining_ms(ctx)),
              uint64_t{0});
    sekka_context_trigger(ctx);
    const uint64_t open =
        fcitx::guideTimerDelayUsec(sekka_context_guide_remaining_ms(ctx));
    EXPECT_GT(open, uint64_t{29000000});
    EXPECT_LE(open, uint64_t{30001000});
    EXPECT_EQ(sekka_context_process_key_event(ctx, 'G', kCtrl, 0), 0);
    EXPECT_EQ(fcitx::guideTimerDelayUsec(sekka_context_guide_remaining_ms(ctx)),
              uint64_t{0});
    sekka_context_free(ctx);
}
