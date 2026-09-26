// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkacandidatelist.cpp - Sekka candidate list implementation
#include "sekkacandidatelist.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace fcitx {

// === SekkaCandidateWord implementation ===

SekkaCandidateList::SekkaCandidateWord::SekkaCandidateWord(
    const std::string &text, SekkaCandidateList *owner, int absoluteIndex)
    : owner_(owner), absoluteIndex_(absoluteIndex) {
    setText(Text(text));
}

void SekkaCandidateList::SekkaCandidateWord::select(
    InputContext * /*ic*/) const {
    if (owner_) {
        owner_->selectAt(absoluteIndex_);
    }
}

// === SekkaCandidateList implementation ===

SekkaCandidateList::SekkaCandidateList(SekkaContextFfi *ctx,
                                       SelectCallback onSelect)
    : ctx_(ctx), totalSize_(0), cursorIndex_(-1), pageStart_(0),
      onSelect_(std::move(onSelect)) {
    if (ctx_) {
        totalSize_ = sekka_context_get_candidate_count(ctx_);
        // Line the highlight and the page up with the selection index of reselection (D-08).
        cursorIndex_ = sekka_context_get_candidate_index(ctx_);
        if (cursorIndex_ >= 0) {
            pageStart_ = (cursorIndex_ / pageSize_) * pageSize_;
        }
    }
    setPageable(this);
    setCursorMovable(this);
    loadCandidates();
}

void SekkaCandidateList::loadCandidates() {
    labels_.clear();
    candidates_.clear();

    if (!ctx_ || totalSize_ == 0) {
        return;
    }

    int count = std::min(pageSize_, totalSize_ - pageStart_);
    if (count <= 0) {
        return;
    }

    // D-115: during reselection a digit key is not a numeric selection but the upstream
    // behaviour of "commit the displayed candidate and insert the digit" (the "other key"
    // handling of D-09; numeric selection is not implemented). Leaving numeric labels on
    // would have the UI suggest an operation that does not exist, so the labels are empty.
    // Symbol labels (・ and friends) are not used either, since they would still suggest
    // "listed = selectable"; an empty string removes the suggestion entirely. The selected
    // position is shown by the cursorIndex() highlight rather than by a label, and that is
    // left unchanged.
    for (int i = 0; i < count; ++i) {
        labels_.emplace_back(Text(""));
    }

    // Fetch the candidates from libsekka.
    std::vector<char *> rawCandidates(count, nullptr);
    int fetched = sekka_context_get_candidates(ctx_, rawCandidates.data(),
                                                count, pageStart_);

    for (int i = 0; i < fetched; ++i) {
        std::string text;
        if (rawCandidates[i]) {
            text = rawCandidates[i];
        }
        candidates_.emplace_back(
            std::make_unique<SekkaCandidateWord>(text, this, pageStart_ + i));
    }

    // Pad the remainder with empty candidates.
    for (int i = fetched; i < count; ++i) {
        candidates_.emplace_back(
            std::make_unique<SekkaCandidateWord>("", this, pageStart_ + i));
    }

    // Free the strings.
    sekka_free_candidate_list(rawCandidates.data(), fetched);
}

void SekkaCandidateList::selectAt(int absoluteIndex) {
    if (!ctx_) {
        return;
    }
    // sekka_context_select_candidate takes an absolute index
    // (libsekka/src/capi.rs:836-849 compares it against get_candidates().len()).
    // Passing the position within the page would select a different candidate from the
    // second page on (T-05-03-03).
    // It returns without doing anything when state != Selecting, so a click arriving
    // outside reselection is harmless: confirm_candidate merely calls finalize_staged
    // (see the comment at capi.rs:825-839).
    sekka_context_select_candidate(ctx_, absoluteIndex);
    sekka_context_confirm_candidate(ctx_);
    // The callback is always invoked exactly once, at the very end, and nothing touches
    // this object's own members after it returns. The callback can replace the candidate
    // list itself through InputPanel::setCandidateList(). fcitx5's
    // InputPanel::candidateList() returns a std::shared_ptr<CandidateList>
    // (5.1.16 inputpanel.h:67), so the object stays alive while the UI holds it, but the
    // code is written not to rely on that discipline (T-05-03-02).
    if (onSelect_) {
        onSelect_();
    }
}

const Text &SekkaCandidateList::label(int idx) const {
    return labels_.at(idx);
}

const CandidateWord &SekkaCandidateList::candidate(int idx) const {
    return *candidates_.at(idx);
}

int SekkaCandidateList::size() const {
    return static_cast<int>(candidates_.size());
}

int SekkaCandidateList::cursorIndex() const {
    if (cursorIndex_ < pageStart_ ||
        cursorIndex_ >= pageStart_ + size()) {
        return -1;
    }
    return cursorIndex_ - pageStart_;
}

CandidateLayoutHint SekkaCandidateList::layoutHint() const {
    return CandidateLayoutHint::Vertical;
}

void SekkaCandidateList::prevCandidate() {
    if (cursorIndex_ > 0) {
        cursorIndex_--;
        if (cursorIndex_ < pageStart_) {
            pageStart_ = std::max(0, pageStart_ - pageSize_);
            loadCandidates();
        }
    }
}

void SekkaCandidateList::nextCandidate() {
    if (cursorIndex_ < totalSize_ - 1) {
        cursorIndex_++;
        if (cursorIndex_ >= pageStart_ + pageSize_) {
            pageStart_ += pageSize_;
            loadCandidates();
        }
    }
}

bool SekkaCandidateList::hasPrev() const { return pageStart_ > 0; }

bool SekkaCandidateList::hasNext() const {
    return pageStart_ + pageSize_ < totalSize_;
}

void SekkaCandidateList::prev() {
    if (hasPrev()) {
        pageStart_ -= pageSize_;
        loadCandidates();
    }
}

void SekkaCandidateList::next() {
    if (hasNext()) {
        pageStart_ += pageSize_;
        loadCandidates();
    }
}

int SekkaCandidateList::totalPages() const {
    if (totalSize_ == 0) {
        return 0;
    }
    return (totalSize_ + pageSize_ - 1) / pageSize_;
}

int SekkaCandidateList::currentPage() const {
    return pageStart_ / pageSize_;
}

void SekkaCandidateList::setPage(int page) {
    int newStart = page * pageSize_;
    if (newStart >= 0 && newStart < totalSize_) {
        pageStart_ = newStart;
        loadCandidates();
    }
}

} // namespace fcitx
