#pragma once

#include <cwctype>
#include <optional>
#include <string_view>

namespace core::host {

// Scores how well a typed query matches a candidate string — the
// command palette's filtering rule. A match requires every query
// character to appear in the candidate in order (a subsequence,
// case-insensitive); the score prefers what a person typing an
// abbreviation means:
//
//   * consecutive characters beat scattered ones ("spl" hits
//     "Split" harder than "SuperPile")
//   * a character landing on a word start beats one mid-word
//     ("nt" reads as "New Tab", not "Winter")
//   * an earlier first hit breaks ties ("tab" prefers "Tab
//     Overview" over "New Tab")
//
// Pure and allocation-free so the rules are directly testable.
class FuzzyMatch {
public:
    // The score for query against candidate, or nullopt when query
    // is not a subsequence of candidate. An empty query matches
    // everything at score 0 (the palette shows the full list).
    // Scores only order candidates for the same query — they are
    // not comparable across queries.
    static std::optional<int> Score(std::wstring_view query,
                                    std::wstring_view candidate) noexcept {
        if (query.empty()) return 0;

        int score = 0;
        int streak = 0;
        std::size_t qi = 0;
        std::size_t firstHit = 0;
        bool prevWasSeparator = true;  // position 0 counts as a word start
        for (std::size_t ci = 0; ci < candidate.size() && qi < query.size(); ++ci) {
            const wchar_t c = candidate[ci];
            if (Lower(c) == Lower(query[qi])) {
                if (qi == 0) firstHit = ci;
                score += kHitScore + streak * kConsecutiveBonus;
                if (prevWasSeparator) score += kWordStartBonus;
                ++streak;
                ++qi;
            } else {
                streak = 0;
            }
            prevWasSeparator = IsSeparator(c);
        }
        if (qi < query.size()) return std::nullopt;

        return score - static_cast<int>(firstHit) * kLateStartPenalty;
    }

private:
    // Relative weights, not absolute meanings: a consecutive run
    // must be able to overtake a lucky single word-start hit, and
    // the late-start penalty is small so it only breaks ties
    // between otherwise equal matches.
    static constexpr int kHitScore = 4;
    static constexpr int kConsecutiveBonus = 3;
    static constexpr int kWordStartBonus = 6;
    static constexpr int kLateStartPenalty = 1;

    static wchar_t Lower(wchar_t c) noexcept {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    }
    static bool IsSeparator(wchar_t c) noexcept {
        return c == L' ' || c == L':' || c == L'-' || c == L'_' || c == L'.';
    }
};

}  // namespace core::host
