// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// registration_test.cpp - GoogleTest for word registration (Phase 10)
//
// Pins the word-registration C ABI (D-167/D-170) and the click commit during
// registration (D-116/D-173) with ctest. `sekka.h` is included directly
// (the discipline of `save_test.cpp:13-17`: never make a hand-written
// parallel copy of the C ABI declarations), which also lets a drift between
// `capi.rs` and this hand-written header surface as a build failure.
// `sekkacandidatelist.h` is included too (for the click test), and, exactly
// like `candidate_list_test.cpp`, `sekka.cpp` itself (which needs
// `Instance`/`InputContextManager`) is not linked.

#include "sekka.h"

#include "sekkacandidatelist.h"

#include <gtest/gtest.h>

#include <string>

// NewFunctionsAreNullSafe: the three C ABI functions this phase added
// (`sekka_context_is_registering` / `_get_registration_reading` /
// `_get_registration_prompt`) are safe to call with a NULL context.
TEST(SekkaRegistrationTest, NewFunctionsAreNullSafe) {
    EXPECT_EQ(sekka_context_is_registering(nullptr), 0);
    EXPECT_EQ(sekka_context_get_registration_reading(nullptr), nullptr);
    EXPECT_EQ(sekka_context_get_registration_prompt(nullptr), nullptr);
}

// D-170: Ctrl-R splits the display into the application's input position
// (just the typed reading) and the popup (the registration label, then the
// word being assembled) - confirmed here through the C ABI exactly as
// fcitx5-sekka calls it, without going through `sekka.cpp`.
TEST(SekkaRegistrationTest, CtrlRSplitsReadingPromptAndWord) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);

    int entered = sekka_context_process_key_event(ctx, 'r', 0x4, 0);
    EXPECT_EQ(entered, 1);
    EXPECT_EQ(sekka_context_is_registering(ctx), 1);

    char *reading = sekka_context_get_registration_reading(ctx);
    ASSERT_NE(reading, nullptr);
    EXPECT_STREQ(reading, "か");
    sekka_free_string(reading);

    char *prompt = sekka_context_get_registration_prompt(ctx);
    ASSERT_NE(prompt, nullptr);
    EXPECT_STREQ(prompt, "登録 ");
    sekka_free_string(prompt);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'i', 0, 0);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "Ki");
    sekka_free_string(preedit);

    char *output = sekka_context_poll_output(ctx);
    EXPECT_EQ(output, nullptr);

    sekka_context_free(ctx);
}

// D-116 / D-173: clicking a candidate inside the *inner* registration
// step's candidate window (the same call
// `SekkaCandidateList::selectAt`/`CandidateWord::select` makes) goes into
// the word being assembled - it never reaches the application, and
// registration stays active. A regression test for
// `sekka_context_confirm_candidate`'s delegation to `active_mut()`.
TEST(SekkaRegistrationTest, ClickingAnInnerCandidateGoesIntoTheWord) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);

    int entered = sekka_context_process_key_event(ctx, 'r', 0x4, 0);
    ASSERT_EQ(entered, 1);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'i', 0, 0);
    sekka_context_trigger(ctx); // stage き
    sekka_context_trigger(ctx); // enter the inner candidate window

    int calls = 0;
    fcitx::SekkaCandidateList list(ctx, [&calls] { ++calls; });
    ASSERT_GE(list.size(), 2);

    std::string secondText = list.candidate(1).text().toString();
    list.candidate(1).select(nullptr);

    char *output = sekka_context_poll_output(ctx);
    EXPECT_EQ(output, nullptr);
    EXPECT_EQ(sekka_context_is_registering(ctx), 1);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, secondText.c_str());
    sekka_free_string(preedit);

    EXPECT_EQ(sekka_context_get_candidate_count(ctx), 0);
    EXPECT_EQ(calls, 1);

    sekka_context_free(ctx);
}
