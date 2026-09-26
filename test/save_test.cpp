// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// save_test.cpp - GoogleTest for the two C ABI save functions (D-102/02.1-REVIEW WR-04)
//
// D-102: confirms from the C++ side that the two C ABI save functions
// (`sekka_context_save_dictionaries` / `sekka_dictionary_save`) really honour the contract
// of 0 = success and non-zero = error.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

// 02.1-REVIEW WR-04: the real header is included directly rather than hand-copied in
// parallel. That keeps the C ABI declarations (`sekka_dictionary_save` and friends) always
// in step with `capi.rs`/`sekka.h`, so a drifted signature becomes a compile error rather
// than a link error (02.1-RESEARCH.md Pitfall 1).
#include "sekka.h"

namespace {

// Builds a unique path under the temporary test directory (never colliding with an existing
// file).
std::filesystem::path uniqueTempPath(const std::string &name) {
    auto dir = std::filesystem::temp_directory_path() /
               ("sekka_save_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir / name;
}

} // namespace

// sekka_dictionary_save(nullptr) returns non-zero.
TEST(SekkaSaveTest, DictionarySaveNullReturnsNonZero) {
    EXPECT_NE(sekka_dictionary_save(nullptr), 0);
}

// sekka_dictionary_save on a dictionary handle created by sekka_user_dict_new returns
// zero.
TEST(SekkaSaveTest, DictionarySaveUserDictReturnsZero) {
    auto path = uniqueTempPath("user-dict-save.db");
    auto *dict = sekka_user_dict_new(path.c_str(), "UTF-8");
    ASSERT_NE(dict, nullptr);

    EXPECT_EQ(sekka_dictionary_save(dict), 0);

    sekka_free_dictionary(dict);
}

// sekka_context_save_dictionaries(nullptr) returns non-zero.
TEST(SekkaSaveTest, ContextSaveNullReturnsNonZero) {
    EXPECT_NE(sekka_context_save_dictionaries(nullptr), 0);
}

// sekka_context_save_dictionaries on a context from sekka_context_new() with no dictionary
// loaded returns zero.
TEST(SekkaSaveTest, ContextSaveEmptyContextReturnsZero) {
    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    EXPECT_EQ(sekka_context_save_dictionaries(ctx), 0);

    sekka_context_free(ctx);
}

// sekka_context_save_dictionaries on a context with a user dictionary loaded returns zero.
// Freeing goes context first with sekka_context_free and then sekka_free_dictionary (drop
// the Arc clone first, then the last reference - the same order the "D-98(a) fixing the
// ownership graph" section of RESEARCH settled on).
TEST(SekkaSaveTest, ContextSaveWithUserDictReturnsZero) {
    auto path = uniqueTempPath("user-dict-context-save.db");
    auto *dict = sekka_user_dict_new(path.c_str(), "UTF-8");
    ASSERT_NE(dict, nullptr);

    auto *ctx = sekka_context_new();
    ASSERT_NE(ctx, nullptr);

    SekkaDictionaryFfi *dicts[] = {dict};
    sekka_context_set_dictionaries(ctx, dicts, 1);

    EXPECT_EQ(sekka_context_save_dictionaries(ctx), 0);

    sekka_context_free(ctx);
    sekka_free_dictionary(dict);
}
