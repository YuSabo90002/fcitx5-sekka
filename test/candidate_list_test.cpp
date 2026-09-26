// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// candidate_list_test.cpp - unit tests for SekkaCandidateList (D-115/D-116)
//
// Pins the empty candidate labels (D-115) and the click commit (D-116,
// select() -> selectAt()) with ctest. libsekka's C ABI is taken in by including `sekka.h`
// directly (the discipline of `save_test.cpp:13-17`: never make a hand-written parallel
// copy). `sekkacandidatelist.h` itself does not include `sekka.h`, so this file can be
// tested on its own without linking `sekka.cpp` (which needs
// `Instance`/`InputContextManager`).

#include "sekkacandidatelist.h"

#include "sekka.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using fcitx::SekkaCandidateList;

// Builds the reselection state (the same dictionary-free procedure as
// basic_test.cpp:118-140). 'K' -> 'a' -> Ctrl-J (commit display state) -> Ctrl-J
// (reselection) is already measured by the existing tests to give at least 2 candidates
// with 「カ」 second.
SekkaContextFfi *EnterReselectState() {
    auto *ctx = sekka_context_new();
    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);
    sekka_context_trigger(ctx);
    return ctx;
}

} // namespace

// D-115: every candidate label in the reselection state is an empty string, so the UI does
// not suggest a numeric selection operation that does not exist.
TEST(SekkaCandidateListTest, LabelsAreEmpty) {
    auto *ctx = EnterReselectState();
    SekkaCandidateList list(ctx);
    ASSERT_GE(list.size(), 2);
    for (int idx = 0; idx < list.size(); ++idx) {
        EXPECT_TRUE(list.label(idx).toString().empty())
            << "idx=" << idx << " label=\"" << list.label(idx).toString()
            << "\"";
    }
    sekka_context_free(ctx);
}

// labels_ and candidates_ keep the same number of elements (label(idx)/candidate(idx)
// throw for no idx below size()).
TEST(SekkaCandidateListTest, LabelCountMatchesCandidateCount) {
    auto *ctx = EnterReselectState();
    SekkaCandidateList list(ctx);
    ASSERT_GE(list.size(), 1);
    for (int idx = 0; idx < list.size(); ++idx) {
        EXPECT_NO_THROW({
            list.label(idx);
            list.candidate(idx);
        });
    }
    sekka_context_free(ctx);
}

// The point of D-116: clicking the second candidate commits the second one (not the
// first). A regression test for the absolute-index wiring.
TEST(SekkaCandidateListTest, SelectCommitsTheClickedCandidateNotTheFirst) {
    auto *ctx = EnterReselectState();
    int calls = 0;
    SekkaCandidateList list(ctx, [&calls] { ++calls; });
    ASSERT_GE(list.size(), 2);

    std::string secondText = list.candidate(1).text().toString();
    list.candidate(1).select(nullptr);

    char *output = sekka_context_poll_output(ctx);
    ASSERT_NE(output, nullptr);
    EXPECT_STREQ(output, secondText.c_str());
    sekka_free_string(output);

    EXPECT_EQ(sekka_context_get_candidate_count(ctx), 0);
    EXPECT_EQ(calls, 1);

    sekka_context_free(ctx);
}

// Constructing it without a callback does not crash and the libsekka-side commit still happens.
TEST(SekkaCandidateListTest, SelectWithoutCallbackDoesNotCrash) {
    auto *ctx = EnterReselectState();
    SekkaCandidateList list(ctx);
    ASSERT_GE(list.size(), 1);

    list.candidate(0).select(nullptr);

    char *output = sekka_context_poll_output(ctx);
    ASSERT_NE(output, nullptr);
    sekka_free_string(output);

    sekka_context_free(ctx);
}

// Regression for the existing paging behaviour (with the candidates fitting on one page).
TEST(SekkaCandidateListTest, PagingApiUnchanged) {
    auto *ctx = EnterReselectState();
    SekkaCandidateList list(ctx);
    ASSERT_LT(list.size(), 10) << "this test assumes the candidates fit on one page";

    EXPECT_FALSE(list.hasPrev());
    EXPECT_FALSE(list.hasNext());
    EXPECT_EQ(list.currentPage(), 0);
    EXPECT_EQ(list.totalPages(), 1);

    list.nextCandidate();
    EXPECT_EQ(list.cursorIndex(), 1);

    sekka_context_free(ctx);
}
