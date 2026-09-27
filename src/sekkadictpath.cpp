// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkadictpath.cpp - master/user dictionary path resolution through fcitx::StandardPaths
#include "sekkadictpath.h"

#include "sekka.h"

#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpaths.h>

#include <exception>
#include <unistd.h>

namespace fcitx {

MasterDictionaryCandidates
resolveMasterDictionaryCandidates(const std::string &configuredPath,
                                   const StandardPathsProvider &paths) {
    MasterDictionaryCandidates result;

    if (!configuredPath.empty()) {
        // D-126 / PATH-04: an explicit path is the only candidate, and `paths` is never
        // called. Falling back to search on failure would silently replace a user's
        // (possibly mistyped) explicit choice with a different dictionary.
        result.candidates.push_back(configuredPath);
        result.isSearch = false;
        return result;
    }

    result.isSearch = true;
    try {
        const StandardPaths &sp = paths();
        result.candidates =
            sp.locateAll(StandardPathsType::PkgData, kMasterDictionaryRelativePath);
        for (const auto &dir : sp.directories(StandardPathsType::PkgData)) {
            result.searched.push_back(dir / kMasterDictionaryRelativePath);
        }
    } catch (const std::exception &e) {
        // D-35 / D-105: the `StandardPaths` constructor throws
        // `std::runtime_error("Home is not set")` when neither `HOME` nor `XDG_DATA_HOME`
        // is set, and the existence checks behind `directories()`/`locateAll()` may raise a
        // `std::filesystem::filesystem_error`. Catching `std::exception` covers both; the
        // failure never crosses the addon boundary, and search simply yields zero
        // candidates instead of taking the process down.
        FCITX_WARN() << "cannot resolve the master dictionary search locations: "
                     << e.what();
        result.candidates.clear();
        result.searched.clear();
    }
    return result;
}

DictCandidateOutcome dictCandidateOutcomeFromError(int sekkaDictError) {
    switch (sekkaDictError) {
    case SEKKA_DICT_OK:
        return DictCandidateOutcome::Loaded;
    case SEKKA_DICT_ERROR_NOT_FOUND:
        return DictCandidateOutcome::NotFound;
    case SEKKA_DICT_ERROR_UNREADABLE:
        return DictCandidateOutcome::Unreadable;
    case SEKKA_DICT_ERROR_CORRUPT:
        return DictCandidateOutcome::Corrupt;
    default:
        // SEKKA_DICT_ERROR_INVALID_ARG, SEKKA_DICT_ERROR_OTHER, or an unrecognized value.
        return DictCandidateOutcome::Other;
    }
}

DictCandidateWalkResult
walkDictionaryCandidates(const std::vector<std::filesystem::path> &candidates,
                          const DictCandidateLoader &tryLoad) {
    DictCandidateWalkResult result;
    for (const auto &candidate : candidates) {
        // 02.1-REVIEW CR-01: the master dictionary is mmapped, and rewriting the file while
        // it is mapped takes the whole fcitx5 process down with SIGBUS (which
        // catch_unwind cannot catch). A search candidate can be an arbitrary
        // user-writable location, so at the very least a path writable by the running
        // user is refused here (writable by the real UID of the running process = that
        // user could rewrite it while mapped and induce SIGBUS). The candidate is skipped
        // without ever being opened or mapped.
        if (::access(candidate.c_str(), W_OK) == 0) {
            result.attempts.push_back({candidate, DictCandidateOutcome::WritableRefused});
            continue;
        }

        int sekkaDictError = tryLoad(candidate);
        auto outcome = dictCandidateOutcomeFromError(sekkaDictError);
        result.attempts.push_back({candidate, outcome});
        if (outcome == DictCandidateOutcome::Loaded) {
            result.adopted = candidate;
            break;
        }
        // D-125: any other outcome (not found, unreadable, corrupt, other) is skipped and
        // the next candidate is tried. An explicit path has exactly one candidate, so
        // there is no next one to fall back to (D-126 / PATH-04).
    }
    return result;
}

// RED-phase stub (06-02 Task 2): always returns an empty string, deliberately wrong so the
// new SekkaDictPathTest cases fail on real assertions before the real implementation lands.
std::string summarizeSearchLocations(const std::vector<std::filesystem::path> & /*searched*/,
                                      std::size_t /*maxShown*/) {
    return "";
}

// RED-phase stub (06-02 Task 2): always returns an empty string.
std::string dictionaryNoticeDedupKey(const std::vector<DictionaryErrorNotice> & /*notices*/) {
    return "";
}

// RED-phase stub (06-02 Task 2): always returns an empty list.
std::vector<DictionaryErrorNotice>
buildMasterDictionaryNotices(const MasterDictionaryCandidates & /*resolved*/,
                              const DictCandidateWalkResult & /*walk*/,
                              const MasterDictionaryNoticeTexts & /*texts*/,
                              std::size_t /*maxShownLocations*/) {
    return {};
}

} // namespace fcitx
