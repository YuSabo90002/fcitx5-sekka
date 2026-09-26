// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkacandidatelist.h - header of the Sekka candidate list implementation
#ifndef SEKKA_CANDIDATE_LIST_H
#define SEKKA_CANDIDATE_LIST_H

#include <fcitx/candidatelist.h>

#include <functional>
#include <string>
#include <vector>

// Forward declarations of the libsekka C ABI
extern "C" {
struct SekkaContextFfi;
int sekka_context_get_candidate_count(SekkaContextFfi *ctx);
int sekka_context_get_candidates(SekkaContextFfi *ctx, char **candidates,
                                  int max_count, int offset);
void sekka_context_select_candidate(SekkaContextFfi *ctx, int index);
void sekka_context_confirm_candidate(SekkaContextFfi *ctx);
int sekka_context_get_candidate_index(SekkaContextFfi *ctx);
void sekka_free_candidate_list(char **candidates, int count);
}

namespace fcitx {

/// Sekka candidate list
///
/// Implements fcitx5's CandidateList, CursorMovableCandidateList and
/// PageableCandidateList. It fetches candidates from libsekka's SekkaContextFfi and
/// displays them.
class SekkaCandidateList : public CandidateList,
                           public CursorMovableCandidateList,
                           public PageableCandidateList {
public:
    /// Callback invoked after a click commit (D-116).
    ///
    /// `select()` is a const member, and the candidate list itself can be replaced
    /// through the callback (via `InputPanel::setCandidateList()`), so rather than
    /// giving this class a direct dependency on `SekkaState` (which owns the committed
    /// output and the preedit update), the caller (`SekkaState::updatePreedit()`)
    /// injects a function object. That lets this file avoid including `sekka.h` (the
    /// definitions of `SekkaState`/`SekkaEngine`) altogether, so ctest can unit-test it
    /// without linking `sekka.cpp` (which needs `Instance`/`InputContextManager`).
    using SelectCallback = std::function<void()>;

    explicit SekkaCandidateList(SekkaContextFfi *ctx,
                                SelectCallback onSelect = {});

    // CandidateList
    const Text &label(int idx) const override;
    const CandidateWord &candidate(int idx) const override;
    int size() const override;
    int cursorIndex() const override;
    CandidateLayoutHint layoutHint() const override;

    // CursorMovableCandidateList
    void prevCandidate() override;
    void nextCandidate() override;

    // PageableCandidateList
    bool hasPrev() const override;
    bool hasNext() const override;
    void prev() override;
    void next() override;

    bool usedNextBefore() const override { return false; }
    int totalPages() const override;
    int currentPage() const override;
    void setPage(int page) override;

private:
    /// Loads the candidates of the current page
    void loadCandidates();

    /// Performs a click commit. `absoluteIndex` is the absolute position among all
    /// candidates (not the position within the page).
    void selectAt(int absoluteIndex);

    SekkaContextFfi *ctx_;
    int totalSize_;
    int cursorIndex_;
    int pageStart_;
    static constexpr int pageSize_ = 10;
    SelectCallback onSelect_;

    /// Wrapper around a candidate word
    class SekkaCandidateWord : public CandidateWord {
    public:
        SekkaCandidateWord(const std::string &text, SekkaCandidateList *owner,
                           int absoluteIndex);
        void select(InputContext *ic) const override;

    private:
        SekkaCandidateList *owner_;
        int absoluteIndex_;
    };

    std::vector<Text> labels_;
    std::vector<std::unique_ptr<SekkaCandidateWord>> candidates_;
};

} // namespace fcitx

#endif // SEKKA_CANDIDATE_LIST_H
