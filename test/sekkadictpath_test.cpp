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

// Builds a `fcitx::StandardPaths` instance (and the env vars it reads) rooted at a fresh
// temporary directory, with `numDataDirs` `XDG_DATA_DIRS` prefixes. Never touches the real
// user's `~/.local/share/fcitx5` or the real fcitx5 install (06-RESEARCH.md Pitfall 5).
// Exposes the expected dictionary path under each slot so tests can place/inspect files
// without recomputing the layout.
struct SearchFixture {
    std::filesystem::path root;
    std::filesystem::path userDictPath;
    std::vector<std::filesystem::path> dataDirDictPaths;
    std::filesystem::path builtinDictPath;
    std::unique_ptr<ScopedEnvVar> home;
    std::unique_ptr<ScopedEnvVar> xdgDataHome;
    std::unique_ptr<ScopedEnvVar> xdgConfigHome;
    std::unique_ptr<ScopedEnvVar> xdgDataDirs;
    std::unique_ptr<ScopedEnvVar> fcitxDataHome;
    std::unique_ptr<ScopedEnvVar> fcitxDataDirs;
    std::unique_ptr<fcitx::StandardPaths> paths;
    fcitx::StandardPathsProvider provider;

    explicit SearchFixture(const std::string &testName, int numDataDirs = 1) {
        root = testRoot(testName);
        auto homeDir = root / "home";
        auto builtinDir = root / "builtin/share/fcitx5";
        std::filesystem::create_directories(builtinDir);

        std::string dataDirsValue;
        for (int i = 0; i < numDataDirs; ++i) {
            auto shareDir = root / ("prefix" + std::to_string(i + 1)) / "share";
            dataDirDictPaths.push_back(shareDir / "fcitx5" /
                                        fcitx::kMasterDictionaryRelativePath);
            if (!dataDirsValue.empty()) {
                dataDirsValue += ":";
            }
            dataDirsValue += shareDir.string();
        }

        home = std::make_unique<ScopedEnvVar>("HOME", homeDir.string());
        xdgDataHome = std::make_unique<ScopedEnvVar>(
            "XDG_DATA_HOME", (homeDir / ".local/share").string());
        xdgConfigHome = std::make_unique<ScopedEnvVar>(
            "XDG_CONFIG_HOME", (homeDir / ".config").string());
        xdgDataDirs = std::make_unique<ScopedEnvVar>("XDG_DATA_DIRS", dataDirsValue);
        fcitxDataHome = std::make_unique<ScopedEnvVar>("FCITX_DATA_HOME", std::nullopt);
        fcitxDataDirs = std::make_unique<ScopedEnvVar>("FCITX_DATA_DIRS", std::nullopt);

        userDictPath =
            homeDir / ".local/share/fcitx5" / fcitx::kMasterDictionaryRelativePath;
        builtinDictPath = builtinDir / fcitx::kMasterDictionaryRelativePath;

        paths = std::make_unique<fcitx::StandardPaths>(
            "fcitx5",
            std::unordered_map<std::string, std::vector<std::filesystem::path>>{
                {"pkgdatadir", {builtinDir}}},
            fcitx::StandardPathsOptions{});
        provider = [this]() -> const fcitx::StandardPaths & { return *paths; };
    }

    ~SearchFixture() { std::filesystem::remove_all(root); }
};

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

// PATH-01: with dictionaries at all four slots (user, two XDG_DATA_DIRS prefixes, built-in),
// the candidate order is exactly [user, prefix1, prefix2, built-in] and the walk adopts the
// first (user) candidate.
TEST(SekkaDictPathTest, SearchOrderIsUserThenDataDirsThenBuiltIn) {
    SearchFixture fixture("search_order", /*numDataDirs=*/2);

    writeDummyDictionary(fixture.userDictPath, 0444);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);
    writeDummyDictionary(fixture.dataDirDictPaths[1], 0444);
    writeDummyDictionary(fixture.builtinDictPath, 0444);

    auto resolved =
        fcitx::resolveMasterDictionaryCandidates("", fixture.provider);

    ASSERT_EQ(resolved.candidates.size(), 4u);
    EXPECT_EQ(resolved.candidates[0].lexically_normal(),
              fixture.userDictPath.lexically_normal());
    EXPECT_EQ(resolved.candidates[1].lexically_normal(),
              fixture.dataDirDictPaths[0].lexically_normal());
    EXPECT_EQ(resolved.candidates[2].lexically_normal(),
              fixture.dataDirDictPaths[1].lexically_normal());
    EXPECT_EQ(resolved.candidates[3].lexically_normal(),
              fixture.builtinDictPath.lexically_normal());

    int loadCount = 0;
    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates, [&](const std::filesystem::path &) {
            ++loadCount;
            return static_cast<int>(SEKKA_DICT_OK);
        });

    ASSERT_TRUE(walk.adopted.has_value());
    EXPECT_EQ(walk.adopted->lexically_normal(), fixture.userDictPath.lexically_normal());
    EXPECT_EQ(loadCount, 1);
}

// CR-01/D-124: a writable user-side candidate is refused without ever calling the loader on
// it, and the walk moves on to adopt the next (read-only) candidate.
TEST(SekkaDictPathTest, SkipsWritableCandidateAndAdoptsNext) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "CR-01's writable check has no effect when running as root";
    }
    SearchFixture fixture("skips_writable");

    writeDummyDictionary(fixture.userDictPath, 0644);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);

    auto resolved =
        fcitx::resolveMasterDictionaryCandidates("", fixture.provider);
    ASSERT_EQ(resolved.candidates.size(), 2u);

    std::vector<std::filesystem::path> loadedPaths;
    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates, [&](const std::filesystem::path &path) {
            loadedPaths.push_back(path);
            return static_cast<int>(SEKKA_DICT_OK);
        });

    ASSERT_EQ(walk.attempts.size(), 2u);
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::WritableRefused);
    EXPECT_EQ(walk.attempts[1].outcome, fcitx::DictCandidateOutcome::Loaded);
    ASSERT_TRUE(walk.adopted.has_value());
    EXPECT_EQ(walk.adopted->lexically_normal(),
              fixture.dataDirDictPaths[0].lexically_normal());
    // CR-01: the refused (writable) candidate must never reach the loader.
    for (const auto &loaded : loadedPaths) {
        EXPECT_NE(loaded.lexically_normal(), fixture.userDictPath.lexically_normal());
    }
    ASSERT_EQ(loadedPaths.size(), 1u);
}

// PATH-03: a read-only user-side dictionary is preferred over the system one, since it is
// tried first and adopted immediately.
TEST(SekkaDictPathTest, PrefersReadOnlyUserDictionaryOverSystem) {
    SearchFixture fixture("prefers_readonly_user");

    writeDummyDictionary(fixture.userDictPath, 0444);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);

    auto resolved =
        fcitx::resolveMasterDictionaryCandidates("", fixture.provider);
    ASSERT_EQ(resolved.candidates.size(), 2u);

    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates,
        [](const std::filesystem::path &) { return static_cast<int>(SEKKA_DICT_OK); });

    ASSERT_TRUE(walk.adopted.has_value());
    EXPECT_EQ(walk.adopted->lexically_normal(), fixture.userDictPath.lexically_normal());
    ASSERT_EQ(walk.attempts.size(), 1u);
}

// D-125: a corrupt (real libsekka `SEKKA_DICT_ERROR_CORRUPT`) user-side candidate is skipped
// and the walk adopts the next candidate.
TEST(SekkaDictPathTest, SkipsCorruptCandidateAndAdoptsNext) {
    SearchFixture fixture("skips_corrupt");

    writeDummyDictionary(fixture.userDictPath, 0444);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);

    auto resolved =
        fcitx::resolveMasterDictionaryCandidates("", fixture.provider);
    ASSERT_EQ(resolved.candidates.size(), 2u);

    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates, [&](const std::filesystem::path &path) {
            if (path.lexically_normal() == fixture.userDictPath.lexically_normal()) {
                int outError = SEKKA_DICT_OK;
                auto *dict =
                    sekka_file_dict_new_with_error(path.c_str(), "UTF-8", &outError);
                if (dict) {
                    sekka_free_dictionary(dict);
                }
                return outError;
            }
            return static_cast<int>(SEKKA_DICT_OK);
        });

    ASSERT_EQ(walk.attempts.size(), 2u);
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::Corrupt);
    EXPECT_EQ(walk.attempts[1].outcome, fcitx::DictCandidateOutcome::Loaded);
    ASSERT_TRUE(walk.adopted.has_value());
    EXPECT_EQ(walk.adopted->lexically_normal(),
              fixture.dataDirDictPaths[0].lexically_normal());
}

// D-125: an unreadable (mode 0000, real libsekka `SEKKA_DICT_ERROR_UNREADABLE`) user-side
// candidate is skipped and the walk adopts the next candidate.
TEST(SekkaDictPathTest, SkipsUnreadableCandidateAndAdoptsNext) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "a permission error cannot be reproduced when running as root";
    }
    SearchFixture fixture("skips_unreadable");

    writeDummyDictionary(fixture.userDictPath, 0000);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);

    auto resolved =
        fcitx::resolveMasterDictionaryCandidates("", fixture.provider);
    ASSERT_EQ(resolved.candidates.size(), 2u);

    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates, [&](const std::filesystem::path &path) {
            if (path.lexically_normal() == fixture.userDictPath.lexically_normal()) {
                int outError = SEKKA_DICT_OK;
                auto *dict =
                    sekka_file_dict_new_with_error(path.c_str(), "UTF-8", &outError);
                if (dict) {
                    sekka_free_dictionary(dict);
                }
                return outError;
            }
            return static_cast<int>(SEKKA_DICT_OK);
        });

    ASSERT_EQ(walk.attempts.size(), 2u);
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::Unreadable);
    EXPECT_EQ(walk.attempts[1].outcome, fcitx::DictCandidateOutcome::Loaded);
    ASSERT_TRUE(walk.adopted.has_value());
    EXPECT_EQ(walk.adopted->lexically_normal(),
              fixture.dataDirDictPaths[0].lexically_normal());

    // Restore write permission so the fixture destructor's remove_all can clean up.
    ::chmod(fixture.userDictPath.c_str(), 0644);
}

// D-124: when every candidate is writable (and thus refused), nothing is adopted.
TEST(SekkaDictPathTest, AllCandidatesSkippedAdoptsNothing) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "CR-01's writable check has no effect when running as root";
    }
    SearchFixture fixture("all_skipped");

    writeDummyDictionary(fixture.userDictPath, 0644);

    auto resolved =
        fcitx::resolveMasterDictionaryCandidates("", fixture.provider);
    ASSERT_EQ(resolved.candidates.size(), 1u);

    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates,
        [](const std::filesystem::path &) { return static_cast<int>(SEKKA_DICT_OK); });

    ASSERT_EQ(walk.attempts.size(), 1u);
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::WritableRefused);
    EXPECT_FALSE(walk.adopted.has_value());
}

// D-127/PATH-06: with nothing installed anywhere, there are no candidates, but every
// searched location is reported, with the user-side location listed first.
TEST(SekkaDictPathTest, NothingInstalledYieldsNoCandidatesButListsSearchedLocations) {
    SearchFixture fixture("nothing_installed");

    auto resolved =
        fcitx::resolveMasterDictionaryCandidates("", fixture.provider);

    EXPECT_TRUE(resolved.isSearch);
    EXPECT_TRUE(resolved.candidates.empty());
    ASSERT_EQ(resolved.searched.size(), 3u);
    EXPECT_EQ(resolved.searched[0].lexically_normal(),
              fixture.userDictPath.lexically_normal());

    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates,
        [](const std::filesystem::path &) { return static_cast<int>(SEKKA_DICT_OK); });
    EXPECT_TRUE(walk.attempts.empty());
    EXPECT_FALSE(walk.adopted.has_value());
}

// D-126: an explicit path that fails to load never falls back to search, even when a
// perfectly usable search candidate exists in the environment.
TEST(SekkaDictPathTest, ExplicitPathFailureDoesNotFallBackToSearch) {
    SearchFixture fixture("explicit_failure_no_fallback");

    // A usable candidate exists in the environment, but must never be consulted.
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);

    auto explicitPath = fixture.root / "explicit-corrupt.db";
    writeDummyDictionary(explicitPath, 0444);

    bool providerCalled = false;
    fcitx::StandardPathsProvider countingProvider =
        [&]() -> const fcitx::StandardPaths & {
        providerCalled = true;
        return *fixture.paths;
    };

    auto resolved = fcitx::resolveMasterDictionaryCandidates(explicitPath.string(),
                                                              countingProvider);

    EXPECT_FALSE(resolved.isSearch);
    ASSERT_EQ(resolved.candidates.size(), 1u);
    EXPECT_EQ(resolved.candidates[0], explicitPath);
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
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::Corrupt);
    EXPECT_FALSE(walk.adopted.has_value());
}

// D-35/D-105: a `StandardPaths` provider that throws (e.g. "Home is not set") yields no
// candidates and no searched locations instead of letting the exception escape.
TEST(SekkaDictPathTest, ProviderExceptionYieldsNoCandidates) {
    fcitx::StandardPathsProvider provider = []() -> const fcitx::StandardPaths & {
        throw std::runtime_error("Home is not set");
    };

    auto resolved = fcitx::resolveMasterDictionaryCandidates("", provider);

    EXPECT_TRUE(resolved.isSearch);
    EXPECT_TRUE(resolved.candidates.empty());
    EXPECT_TRUE(resolved.searched.empty());
}

// Confirms every `SekkaDictError` value (including unrecognized ones) maps to the expected
// `DictCandidateOutcome`.
TEST(SekkaDictPathTest, OutcomeFromEveryDictError) {
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(SEKKA_DICT_OK),
              fcitx::DictCandidateOutcome::Loaded);
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(SEKKA_DICT_ERROR_INVALID_ARG),
              fcitx::DictCandidateOutcome::Other);
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(SEKKA_DICT_ERROR_NOT_FOUND),
              fcitx::DictCandidateOutcome::NotFound);
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(SEKKA_DICT_ERROR_UNREADABLE),
              fcitx::DictCandidateOutcome::Unreadable);
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(SEKKA_DICT_ERROR_CORRUPT),
              fcitx::DictCandidateOutcome::Corrupt);
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(SEKKA_DICT_ERROR_OTHER),
              fcitx::DictCandidateOutcome::Other);
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(99), fcitx::DictCandidateOutcome::Other);
    EXPECT_EQ(fcitx::dictCandidateOutcomeFromError(-1), fcitx::DictCandidateOutcome::Other);
}
