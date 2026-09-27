// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// basic_test.cpp - basic Sekka tests
//
// Basic behaviour verification through libsekka's C ABI

#include <gtest/gtest.h>

extern "C" {
struct SekkaContextFfi;
SekkaContextFfi *sekka_context_new(void);
void sekka_context_free(SekkaContextFfi *ctx);
void sekka_context_reset(SekkaContextFfi *ctx);
int sekka_context_process_key_event(SekkaContextFfi *ctx, uint32_t keysym,
                                     uint32_t modifiers, int is_release);
int sekka_context_trigger(SekkaContextFfi *ctx);
char *sekka_context_get_preedit(SekkaContextFfi *ctx);
char *sekka_context_poll_output(SekkaContextFfi *ctx);
int sekka_context_get_candidate_count(SekkaContextFfi *ctx);
int sekka_context_get_candidate_index(SekkaContextFfi *ctx);
int sekka_context_take_forward_key(SekkaContextFfi *ctx);
void sekka_free_string(char *str);
const char *sekka_get_version(void);
}

// Creating and freeing a context
TEST(SekkaBasicTest, CreateAndFreeContext) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);
    sekka_context_free(ctx);
}

// Getting the version string
TEST(SekkaBasicTest, GetVersion) {
    const char *version = sekka_get_version();
    ASSERT_NE(version, nullptr);
    EXPECT_GT(strlen(version), 0);
}

// Safe operations on a NULL pointer
TEST(SekkaBasicTest, NullPointerSafety) {
    sekka_context_free(nullptr);
    sekka_context_reset(nullptr);
    EXPECT_EQ(sekka_context_process_key_event(nullptr, 0, 0, 0), 0);
    EXPECT_EQ(sekka_context_get_preedit(nullptr), nullptr);
    EXPECT_EQ(sekka_context_poll_output(nullptr), nullptr);
    EXPECT_EQ(sekka_context_get_candidate_index(nullptr), -1);
    EXPECT_EQ(sekka_context_take_forward_key(nullptr), 0);
    sekka_free_string(nullptr);
}

// Romaji input and preedit updates
TEST(SekkaBasicTest, RomajiInput) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    // Type 'a'.
    int consumed = sekka_context_process_key_event(ctx, 'a', 0, 0);
    EXPECT_EQ(consumed, 1);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "a");
    sekka_free_string(preedit);

    sekka_context_free(ctx);
}

// Key releases are ignored
TEST(SekkaBasicTest, KeyReleaseIgnored) {
    auto *ctx = sekka_context_new();
    int consumed = sekka_context_process_key_event(ctx, 'a', 0, 1);
    EXPECT_EQ(consumed, 0);
    sekka_context_free(ctx);
}

// Reset clears the preedit
TEST(SekkaBasicTest, ResetClearsPreedit) {
    auto *ctx = sekka_context_new();
    sekka_context_process_key_event(ctx, 'k', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);

    sekka_context_reset(ctx);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "");
    sekka_free_string(preedit);

    sekka_context_free(ctx);
}

// Ctrl-J places the first candidate in the preedit (no candidate window and no
// committed_output either; revised D-08)
TEST(SekkaOperationTest, CtrlJStagesFirstCandidateInPreedit) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    int consumed = sekka_context_trigger(ctx);
    EXPECT_EQ(consumed, 1);

    char *output = sekka_context_poll_output(ctx);
    EXPECT_EQ(output, nullptr);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "か");
    sekka_free_string(preedit);

    EXPECT_EQ(sekka_context_get_candidate_count(ctx), 0);
    EXPECT_EQ(sekka_context_take_forward_key(ctx), 0);

    sekka_context_free(ctx);
}

// A second Ctrl-J in the commit display state enters reselection mode, and the selection
// index and the preedit match libsekka's state. No delete request occurs at all
// (D-06/revised D-08/stage 1 of D-18)
TEST(SekkaOperationTest, CtrlJRightAfterStagingEntersReselect) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);
    // Ctrl-J only places it in the preedit and does not commit (revised D-08).
    char *staged = sekka_context_poll_output(ctx);
    EXPECT_EQ(staged, nullptr);

    int consumed = sekka_context_trigger(ctx);
    EXPECT_EQ(consumed, 1);
    EXPECT_GE(sekka_context_get_candidate_count(ctx), 2);
    EXPECT_EQ(sekka_context_get_candidate_index(ctx), 0);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "か");
    sekka_free_string(preedit);

    char *noOutput = sekka_context_poll_output(ctx);
    EXPECT_EQ(noOutput, nullptr);

    // Move to the next candidate (fcitx5 normalizes Ctrl + letter to the uppercase keysym).
    consumed = sekka_context_process_key_event(ctx, 'J', 0x4, 0);
    EXPECT_EQ(consumed, 1);
    EXPECT_EQ(sekka_context_get_candidate_index(ctx), 1);

    preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "カ");
    sekka_free_string(preedit);

    // Enter only returns to the commit display state and does not commit yet (revised D-08).
    consumed = sekka_context_process_key_event(ctx, 0xFF0D, 0, 0);
    EXPECT_EQ(consumed, 1);
    EXPECT_EQ(sekka_context_get_candidate_count(ctx), 0);

    char *output = sekka_context_poll_output(ctx);
    EXPECT_EQ(output, nullptr);
    preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "カ");
    sekka_free_string(preedit);

    sekka_context_free(ctx);
}

// Escape during reselection returns to the preedit of the commit display state (the
// original first candidate) (the meaning changed in D-19; it does not commit)
TEST(SekkaOperationTest, EscapeInReselectRestoresStagedPreedit) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);
    char *staged = sekka_context_poll_output(ctx);
    EXPECT_EQ(staged, nullptr);

    sekka_context_trigger(ctx); // enter reselection
    sekka_context_process_key_event(ctx, 'j', 0x4, 0); // next candidate

    int consumed = sekka_context_process_key_event(ctx, 0xFF1B, 0, 0);
    EXPECT_EQ(consumed, 1);

    char *output = sekka_context_poll_output(ctx);
    EXPECT_EQ(output, nullptr);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "か");
    sekka_free_string(preedit);

    sekka_context_free(ctx);
}

// A word placed in the preedit by Ctrl-J is committed exactly once by the next key
// (anything but Ctrl-J), and that key is forwarded to the application too. End-to-end
// evidence from libsekka's state machine to the C ABI (revised D-08/D-10/D-12)
TEST(SekkaOperationTest, StagedWordCommitsOnNextKeyAndForwards) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);
    char *staged = sekka_context_poll_output(ctx);
    EXPECT_EQ(staged, nullptr);

    int consumed = sekka_context_process_key_event(ctx, 0xFF0D, 0, 0);
    EXPECT_EQ(consumed, 1);

    char *output = sekka_context_poll_output(ctx);
    ASSERT_NE(output, nullptr);
    EXPECT_STREQ(output, "か");
    sekka_free_string(output);

    EXPECT_EQ(sekka_context_take_forward_key(ctx), 1);

    sekka_context_free(ctx);
}

// A non-character key during input commits the romaji as it is and forwards it
// (D-03). Space was used here before D-158 (Phase 9) made it a printable key
// that appends instead of forwarding, so Tab (0xFF09) is used instead.
TEST(SekkaOperationTest, NonCharKeyCommitsRomajiAndForwards) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'h', 0, 0);
    sekka_context_process_key_event(ctx, 'i', 0, 0);
    int consumed = sekka_context_process_key_event(ctx, 0xFF09, 0, 0);
    EXPECT_EQ(consumed, 1);

    char *output = sekka_context_poll_output(ctx);
    ASSERT_NE(output, nullptr);
    EXPECT_STREQ(output, "hi");
    sekka_free_string(output);

    EXPECT_EQ(sekka_context_take_forward_key(ctx), 1);
    EXPECT_EQ(sekka_context_take_forward_key(ctx), 0);

    sekka_context_free(ctx);
}

// A lone modifier key is not consumed and does not change the preedit
TEST(SekkaOperationTest, ModifierOnlyKeyIsIgnored) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'k', 0, 0);
    int consumed = sekka_context_process_key_event(ctx, 0xFFE1, 0, 0);
    EXPECT_EQ(consumed, 0);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "k");
    sekka_free_string(preedit);

    sekka_context_free(ctx);
}

// An other character key during reselection confirms with the selected candidate and continues input (D-09)
TEST(SekkaOperationTest, OtherKeyInReselectConfirmsAndContinues) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);
    // Ctrl-J only places it in the preedit and does not commit (revised D-08).
    char *staged = sekka_context_poll_output(ctx);
    EXPECT_EQ(staged, nullptr);

    sekka_context_trigger(ctx); // enter reselection

    int consumed = sekka_context_process_key_event(ctx, 'k', 0, 0);
    EXPECT_EQ(consumed, 1);

    char *output = sekka_context_poll_output(ctx);
    ASSERT_NE(output, nullptr);
    EXPECT_STREQ(output, "か");
    sekka_free_string(output);

    char *preedit = sekka_context_get_preedit(ctx);
    ASSERT_NE(preedit, nullptr);
    EXPECT_STREQ(preedit, "k");
    sekka_free_string(preedit);

    EXPECT_EQ(sekka_context_take_forward_key(ctx), 0);

    sekka_context_free(ctx);
}

// A non-character key during reselection confirms with the selected candidate and is then forwarded (D-09)
TEST(SekkaOperationTest, NonCharKeyInReselectConfirmsAndForwards) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'K', 0, 0);
    sekka_context_process_key_event(ctx, 'a', 0, 0);
    sekka_context_trigger(ctx);
    // Ctrl-J only places it in the preedit and does not commit (revised D-08).
    char *staged = sekka_context_poll_output(ctx);
    EXPECT_EQ(staged, nullptr);

    sekka_context_trigger(ctx); // enter reselection

    int consumed = sekka_context_process_key_event(ctx, 0xFF53, 0, 0);
    EXPECT_EQ(consumed, 1);

    char *output = sekka_context_poll_output(ctx);
    ASSERT_NE(output, nullptr);
    EXPECT_STREQ(output, "か");
    sekka_free_string(output);

    EXPECT_EQ(sekka_context_take_forward_key(ctx), 1);

    sekka_context_free(ctx);
}

// A key with Alt is treated as a non-character key
TEST(SekkaOperationTest, AltKeyIsNonCharKey) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    sekka_context_process_key_event(ctx, 'k', 0, 0);
    int consumed = sekka_context_process_key_event(ctx, 'f', 0x8, 0);
    EXPECT_EQ(consumed, 1);

    char *output = sekka_context_poll_output(ctx);
    ASSERT_NE(output, nullptr);
    EXPECT_STREQ(output, "k");
    sekka_free_string(output);

    EXPECT_EQ(sekka_context_take_forward_key(ctx), 1);

    sekka_context_free(ctx);
}
