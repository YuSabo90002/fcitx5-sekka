// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkadictpath_test.cpp - GoogleTest for PATH-01/02/04 and D-124..D-126, verified against a
// real `fcitx::StandardPaths` instance rooted at a temporary directory. The global()
// singleton is never touched (06-RESEARCH.md Pitfall 5).

#include <gtest/gtest.h>

#include "sekka.h"
#include "sekkadictpath.h"

#include <fcitx-utils/standardpaths.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

// Sets an environment variable for the lifetime of the object and restores the previous
// value (or absence of one) on destruction. `sekka_test` runs each TEST in its own process
// under ctest's `gtest_discover_tests` (06-RESEARCH.md Pitfall 5), but this guards against
// leaking env vars between tests when `sekka_test` is invoked directly without ctest too.
class ScopedEnvVar {
public:
    ScopedEnvVar(const char *name, std::optional<std::string> value) : name_(name) {
        const char *existing = std::getenv(name);
        if (existing) {
            previous_ = std::string(existing);
        }
        if (value) {
            ::setenv(name, value->c_str(), 1);
        } else {
            ::unsetenv(name);
        }
    }

    ScopedEnvVar(const ScopedEnvVar &) = delete;
    ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;

    ~ScopedEnvVar() {
        if (previous_) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

// Builds (recreating if necessary) a unique temporary directory for one test.
std::filesystem::path testRoot(const std::string &testName) {
    auto root = std::filesystem::temp_directory_path() /
                ("sekka_dictpath_test_" + std::to_string(::getpid()) + "_" + testName);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

// Writes a 64-byte dummy dictionary file at `path` (creating its parent directory) and sets
// its permission bits to `mode`.
void writeDummyDictionary(const std::filesystem::path &path, mode_t mode) {
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        std::vector<char> data(64, '\xAA');
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
}

} // namespace

// PATH-01/02/D-124: with `/usr/share` absent from the picture entirely and the master
// dictionary present only under one `XDG_DATA_DIRS` prefix, a real `fcitx::StandardPaths`
// instance (rooted at a temporary directory, never the real user's `~/.local/share/fcitx5`
// or the real fcitx5 install) resolves candidates that include that prefix and exclude
// everything else, and the walk adopts it.
TEST(SekkaDictPathTest, SearchFindsDictionaryOnlyUnderXdgDataDirsPrefix) {
    auto root = testRoot("search_xdg_data_dirs_prefix");

    ScopedEnvVar home("HOME", (root / "home").string());
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", (root / "home/.local/share").string());
    ScopedEnvVar xdgConfigHome("XDG_CONFIG_HOME", (root / "home/.config").string());
    ScopedEnvVar xdgDataDirs("XDG_DATA_DIRS", (root / "prefix/share").string());
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::nullopt);
    ScopedEnvVar fcitxDataDirs("FCITX_DATA_DIRS", std::nullopt);

    // The only place the dictionary actually exists: under the XDG_DATA_DIRS prefix.
    auto dictPath = root / "prefix/share/fcitx5/sekka/master-dict.db";
    writeDummyDictionary(dictPath, 0444);

    // The built-in (compiled-in) pkgdatadir has no "sekka" subdirectory at all, so it never
    // becomes a candidate - only a searched location.
    auto builtinDir = root / "builtin/share/fcitx5";
    std::filesystem::create_directories(builtinDir);

    auto paths = std::make_unique<fcitx::StandardPaths>(
        "fcitx5",
        std::unordered_map<std::string, std::vector<std::filesystem::path>>{
            {"pkgdatadir", {builtinDir}}},
        fcitx::StandardPathsOptions{});
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        return *paths;
    };

    auto resolved = fcitx::resolveMasterDictionaryCandidates("", provider);

    EXPECT_TRUE(resolved.isSearch);
    ASSERT_EQ(resolved.candidates.size(), 1u);
    EXPECT_EQ(resolved.candidates[0].lexically_normal(), dictPath.lexically_normal());

    ASSERT_EQ(resolved.searched.size(), 3u);
    for (const auto &searchedPath : resolved.searched) {
        EXPECT_EQ(searchedPath.filename(), "master-dict.db");
        EXPECT_FALSE(searchedPath.string().starts_with("/usr/share"));
    }
    for (const auto &candidatePath : resolved.candidates) {
        EXPECT_FALSE(candidatePath.string().starts_with("/usr/share"));
    }

    int loadCount = 0;
    std::vector<std::filesystem::path> loadedPaths;
    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates, [&](const std::filesystem::path &path) {
            ++loadCount;
            loadedPaths.push_back(path);
            return static_cast<int>(SEKKA_DICT_OK);
        });

    ASSERT_TRUE(walk.adopted.has_value());
    EXPECT_EQ(walk.adopted->lexically_normal(), dictPath.lexically_normal());
    EXPECT_EQ(loadCount, 1);
    ASSERT_EQ(walk.attempts.size(), 1u);
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::Loaded);

    std::filesystem::remove_all(root);
}

// PATH-04/D-126: an explicit `DictionaryPath` is the only candidate. `StandardPaths` is
// never consulted, and a failure to load that one candidate does not fall back to search.
TEST(SekkaDictPathTest, ExplicitPathIsTheOnlyCandidateAndSearchIsNotRun) {
    auto root = testRoot("explicit_path_only_candidate");

    ScopedEnvVar home("HOME", (root / "home").string());
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", (root / "home/.local/share").string());
    ScopedEnvVar xdgConfigHome("XDG_CONFIG_HOME", (root / "home/.config").string());
    ScopedEnvVar xdgDataDirs("XDG_DATA_DIRS", (root / "prefix/share").string());
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::nullopt);
    ScopedEnvVar fcitxDataDirs("FCITX_DATA_DIRS", std::nullopt);

    bool providerCalled = false;
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        providerCalled = true;
        static fcitx::StandardPaths sp(
            "fcitx5", std::unordered_map<std::string, std::vector<std::filesystem::path>>{},
            fcitx::StandardPathsOptions{});
        return sp;
    };

    auto explicitPath = root / "missing/explicit.db";
    auto resolved =
        fcitx::resolveMasterDictionaryCandidates(explicitPath.string(), provider);

    EXPECT_FALSE(resolved.isSearch);
    ASSERT_EQ(resolved.candidates.size(), 1u);
    EXPECT_EQ(resolved.candidates[0], explicitPath);
    EXPECT_TRUE(resolved.searched.empty());
    EXPECT_FALSE(providerCalled);

    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates, [](const std::filesystem::path &path) {
            int outError = SEKKA_DICT_OK;
            auto *dict = sekka_file_dict_new_with_error(path.c_str(), "UTF-8", &outError);
            if (dict) {
                sekka_free_dictionary(dict);
            }
            return outError;
        });

    ASSERT_EQ(walk.attempts.size(), 1u);
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::NotFound);
    EXPECT_FALSE(walk.adopted.has_value());

    std::filesystem::remove_all(root);
}
