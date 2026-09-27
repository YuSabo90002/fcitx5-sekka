// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkadictpath.h - master/user dictionary path resolution through fcitx::StandardPaths
#ifndef SEKKA_DICT_PATH_H
#define SEKKA_DICT_PATH_H

#include <cstddef>
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

/// Relative path of the user dictionary under a `StandardPathsType::PkgData` directory.
inline constexpr char kUserDictionaryRelativePath[] = "sekka/user-dict.db";

/// USER-01/USER-02: resolves the default user dictionary path as
/// `paths().userDirectory(StandardPathsType::PkgData) / kUserDictionaryRelativePath`.
///
/// When `XDG_DATA_HOME` is set, this follows it (USER-01) - the same rule the master
/// dictionary search already uses. When it is unset, `userDirectory(PkgData)` falls back to
/// `$HOME/.local/share/fcitx5`, so the result is byte-identical to v1.0's hand-built
/// `$HOME/.local/share/fcitx5/sekka/user-dict.db` and existing learned data keeps loading
/// (USER-02).
///
/// Returns an empty path - no exception, no crash - instead of a real path in three cases,
/// matching v1.0's "no `HOME` means no user dictionary" behavior (D-35):
/// - constructing or querying `paths()` throws (e.g. `std::runtime_error("Home is not
///   set")` when neither `HOME` nor any `XDG_*_HOME`/`FCITX_*_HOME` variable is set);
/// - `userDirectory(PkgData)` returns an empty path;
/// - `userDirectory(PkgData)` returns a path that is not absolute (a relative
///   `XDG_DATA_HOME` must never place learned data under fcitx5's current working
///   directory).
std::filesystem::path defaultUserDictionaryPath(const StandardPathsProvider &paths);

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

/// Maximum number of searched locations shown in a user-facing notification body (D-127).
/// The rest are folded into a "+N" count; the full list still goes to the log
/// (`FCITX_WARN`) with no limit. NixOS's `XDG_DATA_DIRS` can list a dozen `/nix/store`
/// paths, and a notification listing all of them would be unreadable (06-CONTEXT.md
/// Claude's Discretion).
inline constexpr std::size_t kMaxShownSearchLocations = 3;

/// Renders `searched` as a short, human-readable summary: the first `maxShown` locations
/// joined by ", ", followed by ", ... (+N)" for the rest when there are more than
/// `maxShown`. An empty `searched` (e.g. an explicit path, which never populates it) is
/// rendered as the relative dictionary name itself (`kMasterDictionaryRelativePath`), so a
/// summary is always meaningful even with nothing to list. `maxShown == 0` is treated as 1
/// (there is always at least one location worth naming - the user-side directory is always
/// first per `resolveMasterDictionaryCandidates`).
std::string summarizeSearchLocations(const std::vector<std::filesystem::path> &searched,
                                      std::size_t maxShown);

/// One notification about a dictionary load problem, ready to hand to the notifications
/// addon (or to a log line).
struct DictionaryErrorNotice {
    /// The dictionary path this notice is about, or - for the D-127 "not found in the
    /// search locations" notice - the searched-locations summary. Used only as the first
    /// half of the dedup key; the displayed text lives entirely in `body`.
    std::string path;
    /// The full notification/log text (a prefix from `MasterDictionaryNoticeTexts`,
    /// followed by `path`).
    std::string body;
};

/// Computes the deduplication key for a batch of notices from one `openDictionaries()`
/// call. One notice's key is `path + "\n" + body` - identical to v1.0's
/// `notifyDictionaryError()` key, so the single-notice case (an explicit path failure)
/// dedups exactly as it did before this plan. Multiple notices are joined with "\n\n", so
/// the key changes if any notice's path/body changes, if a notice is added or removed, or
/// if their order changes. An empty batch's key is "".
std::string dictionaryNoticeDedupKey(const std::vector<DictionaryErrorNotice> &notices);

/// The notification/log text prefixes for each non-`Loaded` outcome, plus the D-127
/// "not found in the search locations" prefix. Each field is a prefix; the candidate path
/// (or, for `notFoundInSearchLocations`, the searched-locations summary) is appended to it
/// by `buildMasterDictionaryNotices`. Callers build one instance with translated
/// (`_()`-wrapped) text for user-facing notifications, and a second with the untranslated
/// v1.0 English text for the log (`FCITX_WARN`), so this struct itself has no i18n
/// dependency.
struct MasterDictionaryNoticeTexts {
    std::string writableRefused;
    std::string notFound;
    std::string unreadable;
    std::string corrupt;
    std::string other;
    /// D-127 / PATH-06: prefix for the "master dictionary not found in the search
    /// locations" summary notice.
    std::string notFoundInSearchLocations;
};

/// Builds the ordered list of notices for one `openDictionaries()` call's master
/// dictionary domain: one notice per non-`Loaded` attempt (in walk order, using the
/// `texts` field matching its outcome), followed - only when `resolved.isSearch` is true
/// and nothing was adopted - by exactly one D-127 notice whose `path` is
/// `summarizeSearchLocations(resolved.searched, maxShownLocations)` and whose `body` is
/// `texts.notFoundInSearchLocations` followed by that same summary. An explicit-path
/// failure (`resolved.isSearch == false`) never gets this trailing summary notice (D-126):
/// its one notice is identical to v1.0's. A search that DID adopt a candidate also never
/// gets it, even though earlier candidates may have been skipped along the way.
std::vector<DictionaryErrorNotice>
buildMasterDictionaryNotices(const MasterDictionaryCandidates &resolved,
                              const DictCandidateWalkResult &walk,
                              const MasterDictionaryNoticeTexts &texts,
                              std::size_t maxShownLocations);

} // namespace fcitx

#endif // SEKKA_DICT_PATH_H
