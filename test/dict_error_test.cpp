// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// dict_error_test.cpp - GoogleTest for the three kinds of dictionary error (D-64/SC6)
//
// Confirms through libsekka's C ABI (sekka_file_dict_new_with_error) that the C++ side can
// distinguish three cases: a path that does not exist, a file without read permission and a
// file with a broken header.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

// 02.1-REVIEW WR-04: the real header is included directly rather than hand-copied in
// parallel. That keeps `SekkaDictError`/`SEKKA_DICT_ERROR_*` and the FFI declarations
// (`sekka_file_dict_new_with_error` and friends) always in step with `capi.rs`/`sekka.h`, so
// a drifted value or signature becomes a compile error rather than a link error (the places
// subject to 02.1-RESEARCH.md Pitfall 1 drop from two to one).
#include "sekka.h"

namespace {

// Builds a unique path under the temporary test directory (never colliding with an existing
// file).
std::filesystem::path uniqueTempPath(const std::string &name) {
    auto dir = std::filesystem::temp_directory_path() /
               ("sekka_dict_error_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    return dir / name;
}

} // namespace

// Confirms that passing a path that does not exist returns nullptr, sets out_error to
// SEKKA_DICT_ERROR_NOT_FOUND, and creates neither a file nor a directory at that path
// (proof that sled's "silently create an empty DB" behaviour is gone).
TEST(SekkaDictErrorTest, NotFoundPath) {
    auto path = uniqueTempPath("does-not-exist.dict");
    ASSERT_FALSE(std::filesystem::exists(path));

    int outError = SEKKA_DICT_OK;
    auto *dict =
        sekka_file_dict_new_with_error(path.c_str(), "UTF-8", &outError);

    EXPECT_EQ(dict, nullptr);
    EXPECT_EQ(outError, SEKKA_DICT_ERROR_NOT_FOUND);
    EXPECT_FALSE(std::filesystem::exists(path));

    if (dict) {
        sekka_free_dictionary(dict);
    }
}

// Confirms that passing a file without read permission sets out_error to
// SEKKA_DICT_ERROR_UNREADABLE. Skipped when running as root, where a permission error cannot
// be reproduced.
TEST(SekkaDictErrorTest, UnreadableFile) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "a permission error cannot be reproduced when running as root";
    }

    auto path = uniqueTempPath("unreadable.dict");
    {
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "dummy";
    }
    ASSERT_EQ(::chmod(path.c_str(), 0), 0);

    int outError = SEKKA_DICT_OK;
    auto *dict =
        sekka_file_dict_new_with_error(path.c_str(), "UTF-8", &outError);

    EXPECT_EQ(dict, nullptr);
    EXPECT_EQ(outError, SEKKA_DICT_ERROR_UNREADABLE);

    if (dict) {
        sekka_free_dictionary(dict);
    }
    // Clean up (leaving it at chmod 0 would get in the way of cleaning the test directory).
    ::chmod(path.c_str(), 0644);
    std::filesystem::remove(path);
}

// Confirms that passing a file holding 64 bytes that do not match the magic byte sequence
// sets out_error to SEKKA_DICT_ERROR_CORRUPT.
TEST(SekkaDictErrorTest, CorruptHeader) {
    auto path = uniqueTempPath("corrupt.dict");
    {
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        std::vector<char> garbage(64, '\xAA');
        out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    int outError = SEKKA_DICT_OK;
    auto *dict =
        sekka_file_dict_new_with_error(path.c_str(), "UTF-8", &outError);

    EXPECT_EQ(dict, nullptr);
    EXPECT_EQ(outError, SEKKA_DICT_ERROR_CORRUPT);

    if (dict) {
        sekka_free_dictionary(dict);
    }
    std::filesystem::remove(path);
}
