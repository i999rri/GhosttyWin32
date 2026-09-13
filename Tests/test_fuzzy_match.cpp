#include "pch.h"
#include "../Core/Host/FuzzyMatch.h"

using core::host::FuzzyMatch;

TEST(FuzzyMatchTest, EmptyQueryMatchesEverythingAtZero) {
    EXPECT_EQ(FuzzyMatch::Score(L"", L"New Tab"), 0);
    EXPECT_EQ(FuzzyMatch::Score(L"", L""), 0);
}

TEST(FuzzyMatchTest, RejectsWhenNotASubsequence) {
    EXPECT_FALSE(FuzzyMatch::Score(L"xyz", L"New Tab").has_value());
    EXPECT_FALSE(FuzzyMatch::Score(L"tabb", L"New Tab").has_value());
    // Order matters: characters must appear in query order.
    EXPECT_FALSE(FuzzyMatch::Score(L"bat", L"New Tab").has_value());
}

TEST(FuzzyMatchTest, MatchesCaseInsensitively) {
    auto lower = FuzzyMatch::Score(L"split", L"New Split: Right");
    auto upper = FuzzyMatch::Score(L"SPLIT", L"New Split: Right");
    ASSERT_TRUE(lower.has_value());
    EXPECT_EQ(lower, upper);
}

TEST(FuzzyMatchTest, ConsecutiveRunBeatsScatteredHits) {
    auto run = FuzzyMatch::Score(L"spl", L"Split");
    auto scattered = FuzzyMatch::Score(L"spl", L"SuperPile");
    ASSERT_TRUE(run.has_value());
    ASSERT_TRUE(scattered.has_value());
    EXPECT_GT(*run, *scattered);
}

TEST(FuzzyMatchTest, WordStartsBeatMidWordHits) {
    // "nt" typed as an initialism should read as New Tab.
    auto initials = FuzzyMatch::Score(L"nt", L"New Tab");
    auto midword = FuzzyMatch::Score(L"nt", L"Winter");
    ASSERT_TRUE(initials.has_value());
    ASSERT_TRUE(midword.has_value());
    EXPECT_GT(*initials, *midword);
}

TEST(FuzzyMatchTest, EarlierStartBreaksTies) {
    auto atStart = FuzzyMatch::Score(L"tab", L"Tab Overview");
    auto later = FuzzyMatch::Score(L"tab", L"New Tab");
    ASSERT_TRUE(atStart.has_value());
    ASSERT_TRUE(later.has_value());
    EXPECT_GT(*atStart, *later);
}
