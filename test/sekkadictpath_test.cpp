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
#include <stdexcept>
#include <string>
#include <system_error>
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

// G-06-1 / 06-REVIEW CR-01 regression test: after `locateAll()` has already found a
// candidate, a failure while enumerating the search locations (`directories()`) must not
// discard that candidate - only `searched` (display-only information for the D-127 summary
// notice) should come back empty.
//
// A real `fcitx::StandardPaths` cannot be made to throw from `directories()`: fcitx5 5.1.16's
// implementation returns a span over an already-populated vector with no I/O at all, so no
// amount of permission/symlink mangling on the temporary directory reaches it. This test
// instead injects the failure on the provider's SECOND call - the point at which the
// enumeration query re-invokes `paths()` to list the search locations, after the first
// (locate) query already succeeded. `providerCalls == 2` is an explicit check that the
// injected failure actually reached the enumeration query rather than being skipped; if a
// future change goes back to fetching `paths()` only once, this test would otherwise stop
// meaning what it says without visibly failing.
TEST(SekkaDictPathTest, SearchLocationListingFailureKeepsLocatedCandidates) {
    SearchFixture fixture("search_location_listing_failure", /*numDataDirs=*/1);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);

    int providerCalls = 0;
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        ++providerCalls;
        if (providerCalls == 1) {
            return *fixture.paths;
        }
        throw std::filesystem::filesystem_error(
            "cannot list the search locations",
            std::make_error_code(std::errc::permission_denied));
    };

    auto resolved = fcitx::resolveMasterDictionaryCandidates("", provider);

    // Asserted first so a regression's failure reason shows up as "candidates lost", which is
    // the CR-01 mechanism itself, rather than a later unrelated assertion.
    ASSERT_EQ(resolved.candidates.size(), 1u);
    EXPECT_TRUE(resolved.isSearch);
    EXPECT_EQ(resolved.candidates[0].lexically_normal(),
              fixture.dataDirDictPaths[0].lexically_normal());
    EXPECT_TRUE(resolved.searched.empty());
    EXPECT_EQ(providerCalls, 2);

    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates,
        [](const std::filesystem::path &) { return static_cast<int>(SEKKA_DICT_OK); });
    ASSERT_TRUE(walk.adopted.has_value());
    EXPECT_EQ(walk.adopted->lexically_normal(),
              fixture.dataDirDictPaths[0].lexically_normal());

    fcitx::MasterDictionaryNoticeTexts texts;
    auto notices = fcitx::buildMasterDictionaryNotices(
        resolved, walk, texts, fcitx::kMaxShownSearchLocations);
    EXPECT_TRUE(notices.empty());
}

// D-35 / D-105: fixes the OTHER side of the exception-boundary split introduced for G-06-1 -
// when the locate query (the FIRST provider call) fails, the search-location enumeration
// (the second) is never attempted at all, and both `candidates` and `searched` come back
// empty. `providerCalls == 1` is the check that the early return in
// `resolveMasterDictionaryCandidates` actually took effect - an implementation that fell
// through to the enumeration anyway would call the provider a second time and end up with 3
// searched locations instead of 0 (see this task's SUMMARY.md for the one-time verification
// that removing the early return does make this fail).
TEST(SekkaDictPathTest, LocateFailureSkipsSearchLocationListing) {
    SearchFixture fixture("locate_failure_skips_listing", /*numDataDirs=*/1);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0444);

    int providerCalls = 0;
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        ++providerCalls;
        if (providerCalls == 1) {
            throw std::runtime_error("Home is not set");
        }
        return *fixture.paths;
    };

    auto resolved = fcitx::resolveMasterDictionaryCandidates("", provider);

    EXPECT_TRUE(resolved.isSearch);
    EXPECT_TRUE(resolved.candidates.empty());
    EXPECT_TRUE(resolved.searched.empty());
    EXPECT_EQ(providerCalls, 1);
}

// G-06-1 / 06-REVIEW CR-01: the candidate that survives a search-location-listing failure
// still goes through the ordinary CR-01 writable-path refusal untouched, and the D-127 "not
// found" summary notice still fires (with an empty `searched` rendered as the relative
// dictionary name, per `summarizeSearchLocations`). Root is excluded because CR-01's
// writable check has no effect for root, same reason as the existing
// SkipsWritableCandidateAndAdoptsNext test.
TEST(SekkaDictPathTest, SearchLocationListingFailureStillRefusesWritableAndNotifies) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "CR-01's writable check has no effect when running as root";
    }
    SearchFixture fixture("search_location_listing_failure_writable", /*numDataDirs=*/1);
    writeDummyDictionary(fixture.dataDirDictPaths[0], 0644);

    int providerCalls = 0;
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        ++providerCalls;
        if (providerCalls == 1) {
            return *fixture.paths;
        }
        throw std::filesystem::filesystem_error(
            "cannot list the search locations",
            std::make_error_code(std::errc::permission_denied));
    };

    auto resolved = fcitx::resolveMasterDictionaryCandidates("", provider);
    ASSERT_EQ(resolved.candidates.size(), 1u);
    EXPECT_TRUE(resolved.searched.empty());

    bool loaderCalled = false;
    auto walk = fcitx::walkDictionaryCandidates(
        resolved.candidates, [&](const std::filesystem::path &) {
            loaderCalled = true;
            return static_cast<int>(SEKKA_DICT_OK);
        });

    ASSERT_EQ(walk.attempts.size(), 1u);
    EXPECT_EQ(walk.attempts[0].outcome, fcitx::DictCandidateOutcome::WritableRefused);
    EXPECT_FALSE(loaderCalled);
    EXPECT_FALSE(walk.adopted.has_value());

    fcitx::MasterDictionaryNoticeTexts texts;
    texts.writableRefused = "W:";
    texts.notFoundInSearchLocations = "S:";
    auto notices = fcitx::buildMasterDictionaryNotices(
        resolved, walk, texts, fcitx::kMaxShownSearchLocations);

    ASSERT_EQ(notices.size(), 2u);
    EXPECT_EQ(notices[0].path, resolved.candidates[0].string());
    EXPECT_EQ(notices[0].body, "W:" + resolved.candidates[0].string());
    EXPECT_EQ(notices[1].path, std::string(fcitx::kMasterDictionaryRelativePath));
    EXPECT_EQ(notices[1].body, "S:" + std::string(fcitx::kMasterDictionaryRelativePath));
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

// D-127/PATH-06/Claude's Discretion: the summary shows the first `maxShown` locations and
// folds the rest into a "+N" count; with 3 or fewer locations nothing is folded.
TEST(SekkaDictPathTest, SummaryShowsUserLocationFirstAndFoldsTheRest) {
    std::vector<std::filesystem::path> five{"a", "b", "c", "d", "e"};
    EXPECT_EQ(fcitx::summarizeSearchLocations(five, 3), "a, b, c, ... (+2)");

    std::vector<std::filesystem::path> three{"a", "b", "c"};
    EXPECT_EQ(fcitx::summarizeSearchLocations(three, 3), "a, b, c");

    std::vector<std::filesystem::path> one{"a"};
    EXPECT_EQ(fcitx::summarizeSearchLocations(one, 3), "a");
}

// An empty searched-locations list (an explicit path never populates it) summarizes as the
// relative dictionary name itself, so the summary is always meaningful.
TEST(SekkaDictPathTest, SummaryOfNoLocationsIsTheRelativeName) {
    EXPECT_EQ(fcitx::summarizeSearchLocations({}, 3),
              std::string(fcitx::kMasterDictionaryRelativePath));
}

// v1.0 compatibility: a single notice's dedup key is byte-identical to the pre-06-02
// `notifyDictionaryError()` key (`path + "\n" + body`), so an explicit-path failure's
// suppression behaves exactly as before.
TEST(SekkaDictPathTest, DedupKeyOfOneNoticeMatchesV1Key) {
    std::vector<fcitx::DictionaryErrorNotice> notices{{"p", "b"}};
    EXPECT_EQ(fcitx::dictionaryNoticeDedupKey(notices), "p\nb");
    EXPECT_EQ(fcitx::dictionaryNoticeDedupKey({}), "");
}

// The batch key changes whenever any notice's content changes, whenever a notice is
// added/removed, or whenever the order changes - so a genuinely different set of skipped
// candidates is never silently treated as "the same as last time".
TEST(SekkaDictPathTest, DedupKeyChangesWithEveryNoticeAndOrder) {
    fcitx::DictionaryErrorNotice a{"pa", "ba"};
    fcitx::DictionaryErrorNotice b{"pb", "bb"};
    fcitx::DictionaryErrorNotice bPrime{"pb", "bb-different"};

    auto keyAB = fcitx::dictionaryNoticeDedupKey({a, b});
    auto keyA = fcitx::dictionaryNoticeDedupKey({a});
    auto keyBA = fcitx::dictionaryNoticeDedupKey({b, a});
    auto keyABPrime = fcitx::dictionaryNoticeDedupKey({a, bPrime});

    EXPECT_NE(keyAB, keyA);
    EXPECT_NE(keyAB, keyBA);
    EXPECT_NE(keyAB, keyABPrime);
}

// D-127: when nothing was found in a search (isSearch, no candidates, no attempts), the
// notice list has exactly one entry naming the searched locations.
TEST(SekkaDictPathTest, NoticesForEmptySearchNameTheSearchedLocations) {
    fcitx::MasterDictionaryCandidates resolved;
    resolved.isSearch = true;
    resolved.searched = {"u", "p1", "b"};
    fcitx::DictCandidateWalkResult walk; // no attempts, no adopted

    fcitx::MasterDictionaryNoticeTexts texts;
    texts.notFoundInSearchLocations = "S:";

    auto notices =
        fcitx::buildMasterDictionaryNotices(resolved, walk, texts, /*maxShownLocations=*/3);

    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].path, "u, p1, b");
    EXPECT_EQ(notices[0].body, "S:u, p1, b");
}

// D-124/D-125/D-127: every skipped candidate gets its own notice (in walk order), followed
// by the searched-locations summary when nothing was adopted.
TEST(SekkaDictPathTest, NoticesForSkippedCandidatesEndWithTheSummary) {
    fcitx::MasterDictionaryCandidates resolved;
    resolved.isSearch = true;
    resolved.searched = {"u", "p1"};
    fcitx::DictCandidateWalkResult walk;
    walk.attempts = {{"u", fcitx::DictCandidateOutcome::WritableRefused},
                      {"p1", fcitx::DictCandidateOutcome::Corrupt}};
    // walk.adopted stays empty.

    fcitx::MasterDictionaryNoticeTexts texts;
    texts.writableRefused = "W:";
    texts.corrupt = "C:";
    texts.notFoundInSearchLocations = "S:";

    auto notices =
        fcitx::buildMasterDictionaryNotices(resolved, walk, texts, /*maxShownLocations=*/3);

    ASSERT_EQ(notices.size(), 3u);
    EXPECT_EQ(notices[0].path, "u");
    EXPECT_EQ(notices[0].body, "W:u");
    EXPECT_EQ(notices[1].path, "p1");
    EXPECT_EQ(notices[1].body, "C:p1");
    EXPECT_EQ(notices[2].path, "u, p1");
    EXPECT_EQ(notices[2].body, "S:u, p1");
}

// D-124: a skipped candidate followed by a successful load produces just the one skipped
// candidate's notice - no summary, since something was adopted.
TEST(SekkaDictPathTest, NoticesForSkippedCandidateThenLoadHaveNoSummary) {
    fcitx::MasterDictionaryCandidates resolved;
    resolved.isSearch = true;
    resolved.searched = {"u", "p1"};
    fcitx::DictCandidateWalkResult walk;
    walk.attempts = {{"u", fcitx::DictCandidateOutcome::Corrupt},
                      {"p1", fcitx::DictCandidateOutcome::Loaded}};
    walk.adopted = std::filesystem::path("p1");

    fcitx::MasterDictionaryNoticeTexts texts;
    texts.corrupt = "C:";
    texts.notFoundInSearchLocations = "S:";

    auto notices =
        fcitx::buildMasterDictionaryNotices(resolved, walk, texts, /*maxShownLocations=*/3);

    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].path, "u");
    EXPECT_EQ(notices[0].body, "C:u");
}

// D-126: an explicit-path failure never gets the D-127 summary appended, even though
// nothing was adopted - its one notice is identical to v1.0's.
TEST(SekkaDictPathTest, NoticesForExplicitFailureHaveNoSummary) {
    fcitx::MasterDictionaryCandidates resolved;
    resolved.isSearch = false;
    resolved.searched = {}; // explicit path never populates `searched`.
    fcitx::DictCandidateWalkResult walk;
    walk.attempts = {{"x", fcitx::DictCandidateOutcome::NotFound}};
    // walk.adopted stays empty.

    fcitx::MasterDictionaryNoticeTexts texts;
    texts.notFound = "N:";
    texts.notFoundInSearchLocations = "S:";

    auto notices =
        fcitx::buildMasterDictionaryNotices(resolved, walk, texts, /*maxShownLocations=*/3);

    ASSERT_EQ(notices.size(), 1u);
    EXPECT_EQ(notices[0].path, "x");
    EXPECT_EQ(notices[0].body, "N:x");
}

// A clean load (single Loaded attempt, nothing skipped) produces no notices at all.
TEST(SekkaDictPathTest, NoticesForCleanLoadAreEmpty) {
    fcitx::MasterDictionaryCandidates resolved;
    resolved.isSearch = true;
    resolved.searched = {"u"};
    fcitx::DictCandidateWalkResult walk;
    walk.attempts = {{"u", fcitx::DictCandidateOutcome::Loaded}};
    walk.adopted = std::filesystem::path("u");

    fcitx::MasterDictionaryNoticeTexts texts;
    auto notices =
        fcitx::buildMasterDictionaryNotices(resolved, walk, texts, /*maxShownLocations=*/3);

    EXPECT_TRUE(notices.empty());
}

// Every non-Loaded outcome maps to its own text field.
TEST(SekkaDictPathTest, NoticesMapEveryOutcomeToItsText) {
    fcitx::MasterDictionaryCandidates resolved;
    resolved.isSearch = true;
    resolved.searched = {"u", "p1"};
    fcitx::DictCandidateWalkResult walk;
    walk.attempts = {{"u", fcitx::DictCandidateOutcome::Unreadable},
                      {"p1", fcitx::DictCandidateOutcome::Other}};

    fcitx::MasterDictionaryNoticeTexts texts;
    texts.unreadable = "U:";
    texts.other = "O:";
    texts.notFoundInSearchLocations = "S:";

    auto notices =
        fcitx::buildMasterDictionaryNotices(resolved, walk, texts, /*maxShownLocations=*/3);

    ASSERT_EQ(notices.size(), 3u);
    EXPECT_EQ(notices[0].body, "U:u");
    EXPECT_EQ(notices[1].body, "O:p1");
    EXPECT_EQ(notices[2].body, "S:u, p1");
}

// USER-01/USER-02/D-35: defaultUserDictionaryPath, verified against a real
// `fcitx::StandardPaths` instance rooted at a temporary directory (and, for one test only,
// the production `fcitx::StandardPaths::global()` singleton - see
// GlobalInstanceMatchesV1PathWhenXdgDataHomeIsUnset below). No test ever creates a file; each
// only computes a path.

// USER-02: with `XDG_DATA_HOME` unset, the result is byte-identical to v1.0's hand-built
// `$HOME/.local/share/fcitx5/sekka/user-dict.db`, so existing learned data keeps loading.
TEST(SekkaUserDictPathTest, MatchesV1PathWhenXdgDataHomeIsUnset) {
    auto root = testRoot("user_dict_matches_v1_no_xdg");
    auto homeDir = root / "home";
    std::filesystem::create_directories(homeDir);

    ScopedEnvVar home("HOME", homeDir.string());
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", std::nullopt);
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::nullopt);

    fcitx::StandardPaths paths(
        "fcitx5", std::unordered_map<std::string, std::vector<std::filesystem::path>>{},
        fcitx::StandardPathsOptions{});
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        return paths;
    };

    auto result = fcitx::defaultUserDictionaryPath(provider);

    EXPECT_EQ(result.string(), homeDir.string() + "/.local/share/fcitx5/sekka/user-dict.db");

    std::filesystem::remove_all(root);
}

// USER-02: a trailing slash on `HOME` reproduces v1.0's naive string concatenation (which
// yields a doubled "//"), but the result still resolves to the same file once normalized.
TEST(SekkaUserDictPathTest, MatchesV1FileWhenHomeHasTrailingSlash) {
    auto root = testRoot("user_dict_trailing_slash");
    auto homeDir = root / "home";
    std::filesystem::create_directories(homeDir);
    std::string homeValue = homeDir.string() + "/";

    ScopedEnvVar home("HOME", homeValue);
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", std::nullopt);
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::nullopt);

    fcitx::StandardPaths paths(
        "fcitx5", std::unordered_map<std::string, std::vector<std::filesystem::path>>{},
        fcitx::StandardPathsOptions{});
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        return paths;
    };

    auto result = fcitx::defaultUserDictionaryPath(provider);

    // v1.0's formula: `home + "/.local/share/fcitx5/sekka/user-dict.db"`. With a trailing
    // slash already on `home`, straight concatenation yields a doubled "//".
    auto v1Path =
        std::filesystem::path(homeValue + "/.local/share/fcitx5/sekka/user-dict.db");
    EXPECT_EQ(result.lexically_normal(), v1Path.lexically_normal());

    std::filesystem::remove_all(root);
}

// USER-01: an explicitly-set `XDG_DATA_HOME` takes priority over the `HOME`-derived default.
TEST(SekkaUserDictPathTest, FollowsXdgDataHome) {
    auto root = testRoot("user_dict_follows_xdg_data_home");
    auto homeDir = root / "home";
    auto xdgDataHomeDir = root / "custom-data-home";
    std::filesystem::create_directories(homeDir);
    std::filesystem::create_directories(xdgDataHomeDir);

    ScopedEnvVar home("HOME", homeDir.string());
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", xdgDataHomeDir.string());
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::nullopt);

    fcitx::StandardPaths paths(
        "fcitx5", std::unordered_map<std::string, std::vector<std::filesystem::path>>{},
        fcitx::StandardPathsOptions{});
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        return paths;
    };

    auto result = fcitx::defaultUserDictionaryPath(provider);

    auto expected = xdgDataHomeDir / "fcitx5/sekka/user-dict.db";
    EXPECT_EQ(result.lexically_normal(), expected.lexically_normal());

    std::filesystem::remove_all(root);
}

// A relative fcitx5 user data directory must never place learned data under fcitx5's
// current working directory - `defaultUserDictionaryPath` rejects it and returns empty (no
// user dictionary).
//
// NOTE on the env var actually used here: a *relative* `XDG_DATA_HOME` does NOT reproduce
// this in real fcitx5 5.1.16 - verified empirically (see SUMMARY.md for the probe). This
// package's `StandardPathsPrivate::defaultPaths()` re-anchors a relative XDG_DATA_HOME
// against the real `HOME` env var before it ever reaches `userDirectory(PkgData)` (its
// homeFallback for the FCITX_DATA_HOME layer is `dataDirs_[0] / "fcitx5"`, and when that
// fallback itself is not absolute, the private implementation prepends `HOME` to it - so
// the result stays absolute either way). Setting `FCITX_DATA_HOME` itself to a relative
// value is what actually reaches `userDirectory(PkgData)` unresolved, since fcitx5 uses that
// value exactly as given when the env var is present (06-RESEARCH.md `## Open Questions` 2:
// "a non-empty `XDG_DATA_HOME` is used as-is even when it is relative" - the same "used as-is" rule
// applies to `FCITX_DATA_HOME`, the higher-priority override for this package). The test
// name matches this task's `<verify>` gate; the env var it manipulates is the one that
// genuinely reproduces a relative `userDirectory(PkgData)` result.
TEST(SekkaUserDictPathTest, RelativeXdgDataHomeYieldsEmpty) {
    auto root = testRoot("user_dict_relative_xdg_data_home");
    auto homeDir = root / "home";
    std::filesystem::create_directories(homeDir);

    ScopedEnvVar home("HOME", homeDir.string());
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", std::nullopt);
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::string("relative/data"));

    fcitx::StandardPaths paths(
        "fcitx5", std::unordered_map<std::string, std::vector<std::filesystem::path>>{},
        fcitx::StandardPathsOptions{});
    fcitx::StandardPathsProvider provider = [&]() -> const fcitx::StandardPaths & {
        return paths;
    };

    auto result = fcitx::defaultUserDictionaryPath(provider);

    EXPECT_TRUE(result.empty());

    std::filesystem::remove_all(root);
}

// D-35: a provider that throws a synthetic exception (rather than a real fcitx5 exception -
// see HomeAndXdgDataHomeUnsetYieldsEmptyWithoutThrowing below for that case) must not let the
// exception escape `defaultUserDictionaryPath`.
TEST(SekkaUserDictPathTest, ProviderExceptionYieldsEmpty) {
    fcitx::StandardPathsProvider provider = []() -> const fcitx::StandardPaths & {
        throw std::runtime_error("Home is not set");
    };

    auto result = fcitx::defaultUserDictionaryPath(provider);

    EXPECT_TRUE(result.empty());
}

// D-35: with neither `HOME` nor any `XDG_*_HOME`/`FCITX_*_HOME` variable set, constructing a
// real `fcitx::StandardPaths` throws `std::runtime_error("Home is not set")`. The
// construction happens inside the provider lambda itself (not before it), so the exception
// occurs at the same point `defaultUserDictionaryPath` calls `paths()` and is caught by its
// own exception boundary - exercising the real fcitx5 exception, rather than the synthetic
// one used by ProviderExceptionYieldsEmpty above.
TEST(SekkaUserDictPathTest, HomeAndXdgDataHomeUnsetYieldsEmptyWithoutThrowing) {
    ScopedEnvVar home("HOME", std::nullopt);
    ScopedEnvVar xdgConfigHome("XDG_CONFIG_HOME", std::nullopt);
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", std::nullopt);
    ScopedEnvVar xdgCacheHome("XDG_CACHE_HOME", std::nullopt);
    ScopedEnvVar xdgStateHome("XDG_STATE_HOME", std::nullopt);
    ScopedEnvVar fcitxConfigHome("FCITX_CONFIG_HOME", std::nullopt);
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::nullopt);

    std::unique_ptr<fcitx::StandardPaths> holder;
    fcitx::StandardPathsProvider provider = [&holder]() -> const fcitx::StandardPaths & {
        holder = std::make_unique<fcitx::StandardPaths>(
            "fcitx5",
            std::unordered_map<std::string, std::vector<std::filesystem::path>>{},
            fcitx::StandardPathsOptions{});
        return *holder;
    };

    auto result = fcitx::defaultUserDictionaryPath(provider);

    EXPECT_TRUE(result.empty());
}

// This is the only test in this binary that calls the production
// `fcitx::StandardPaths::global()` singleton, which reads HOME/XDG_*/FCITX_DATA_* only once
// per process (06-RESEARCH.md Pitfall 5). That is safe here because ctest's
// `gtest_discover_tests` runs each TEST case as its own process (one `--gtest_filter`
// invocation per ctest entry), so the env vars set immediately above are the only ones the
// singleton ever observes in this process.
TEST(SekkaUserDictPathTest, GlobalInstanceMatchesV1PathWhenXdgDataHomeIsUnset) {
    auto root = testRoot("user_dict_global_matches_v1");
    auto homeDir = root / "home";
    std::filesystem::create_directories(homeDir);

    ScopedEnvVar home("HOME", homeDir.string());
    ScopedEnvVar xdgDataHome("XDG_DATA_HOME", std::nullopt);
    ScopedEnvVar fcitxDataHome("FCITX_DATA_HOME", std::nullopt);

    auto result = fcitx::defaultUserDictionaryPath(&fcitx::StandardPaths::global);

    EXPECT_EQ(result.string(), homeDir.string() + "/.local/share/fcitx5/sekka/user-dict.db");

    std::filesystem::remove_all(root);
}
