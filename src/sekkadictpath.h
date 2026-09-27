// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkadictpath.h - master/user dictionary path resolution through fcitx::StandardPaths
#ifndef SEKKA_DICT_PATH_H
#define SEKKA_DICT_PATH_H

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fcitx {

// Forward declaration only - this header never includes <fcitx-utils/standardpaths.h>.
// `StandardPaths::global()` is a process-wide singleton that reads HOME/XDG_*/FCITX_DATA_*
// only once per process (06-RESEARCH.md Pitfall 5), so production code must pass that
// singleton while tests must pass a local instance rooted at a temporary directory. Keeping
// this header free of the fcitx-utils dependency is what lets `resolveMasterDictionaryCandidates`
// take either one through the same `StandardPathsProvider` seam.
class StandardPaths;

/// Supplies a `StandardPaths` instance to resolve against.
///
/// Production code passes `&StandardPaths::global`; tests pass a lambda returning a local
/// instance constructed with a temporary directory as its built-in path, so the real
/// singleton (and the user's real `~/.local/share/fcitx5`) is never touched by a test
/// (06-RESEARCH.md Pitfall 5).
using StandardPathsProvider = std::function<const StandardPaths &()>;

/// Relative path of the master dictionary under a `StandardPathsType::PkgData` directory.
inline constexpr char kMasterDictionaryRelativePath[] = "sekka/master-dict.db";

/// The result of resolving where the master dictionary might be.
struct MasterDictionaryCandidates {
    /// Candidate paths, to be tried in this order.
    std::vector<std::filesystem::path> candidates;
    /// Every location that was looked at. Populated only in search mode; stays empty when
    /// `isSearch` is false, since an explicit path never touches `StandardPaths` at all.
    std::vector<std::filesystem::path> searched;
    /// True when the configured path was empty and this is a search result rather than an
    /// explicit path.
    bool isSearch = false;
};

/// Resolves the master dictionary candidates.
///
/// When `configuredPath` is non-empty, `candidates` is exactly that one path, `searched`
/// stays empty and `paths` is never called at all (D-126 / PATH-04): an explicit path must
/// never silently fall back to search on failure, since that would hide a typo or a
/// deliberately-chosen path from the user.
///
/// When `configuredPath` is empty, `candidates` is every path returned by
/// `paths().locateAll(StandardPathsType::PkgData, kMasterDictionaryRelativePath)`, in the
/// same order (D-124): fcitx5's own single-result search functions (`locate()`) only check
/// existence and never check writability, so Sekka must judge each candidate for itself
/// (CR-01) rather than simply accept the first one fcitx5 itself would have picked
/// (06-RESEARCH.md Pitfall 2). `searched` lists every directory `paths()` would have looked
/// in (present or not), for use in a future "not found" summary (D-127, 06-02).
///
/// If constructing/querying `paths()` throws - `std::runtime_error("Home is not set")` when
/// neither `HOME` nor `XDG_DATA_HOME` is set, or a `std::filesystem::filesystem_error` from
/// the existence checks - the exception is caught here and an empty candidate/search list is
/// returned instead of letting it escape the addon boundary and take the whole process down
/// (D-35 / D-105).
MasterDictionaryCandidates
resolveMasterDictionaryCandidates(const std::string &configuredPath,
                                   const StandardPathsProvider &paths);

/// The outcome of trying to load one dictionary candidate.
enum class DictCandidateOutcome {
    Loaded,
    WritableRefused,
    NotFound,
    Unreadable,
    Corrupt,
    Other,
};

/// Maps a `SekkaDictError` value (declared in `sekka.h`) to a `DictCandidateOutcome`.
DictCandidateOutcome dictCandidateOutcomeFromError(int sekkaDictError);

/// One candidate that was tried during the walk, and what happened to it.
struct DictCandidateAttempt {
    std::filesystem::path path;
    DictCandidateOutcome outcome;
};

/// The result of walking every candidate in order.
struct DictCandidateWalkResult {
    /// Every attempt, in the order tried.
    std::vector<DictCandidateAttempt> attempts;
    /// The first candidate that reached `Loaded`, if any.
    std::optional<std::filesystem::path> adopted;
};

/// Attempts to load one candidate path. Returns a `SekkaDictError` value (`SEKKA_DICT_OK` on
/// success). Declared as a plain `std::function` rather than binding directly to `sekka.h`'s
/// FFI declarations so this header stays free of any dependency beyond std.
using DictCandidateLoader = std::function<int(const std::filesystem::path &)>;

/// Walks `candidates` in order and stops at the first one that loads successfully.
///
/// For each candidate, CR-01 is judged first: `::access(path, W_OK) == 0` means the
/// candidate is writable by the running user, so it is recorded as `WritableRefused` and
/// `tryLoad` is never called for it. A writable dictionary file can be rewritten while
/// libsekka has it mmapped, which crashes the whole fcitx5 process with SIGBUS -
/// `catch_unwind` cannot catch that. Otherwise `tryLoad` is called and its result classified
/// with `dictCandidateOutcomeFromError`; `Loaded` stops the walk and becomes `adopted`, any
/// other outcome moves on to the next candidate (D-125). An explicit path has exactly one
/// candidate, so there is no next one to fall back to (D-126 / PATH-04).
DictCandidateWalkResult
walkDictionaryCandidates(const std::vector<std::filesystem::path> &candidates,
                          const DictCandidateLoader &tryLoad);

} // namespace fcitx

#endif // SEKKA_DICT_PATH_H
