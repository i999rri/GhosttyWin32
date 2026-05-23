#include "pch.h"
#include "../GhosttyWin32App/ImeBuffer.h"

// Unicode constants for Japanese characters used in tests
static const wchar_t A[] = L"\x3042";           // あ
static const wchar_t AI[] = L"\x3042\x3044";     // あい
static const wchar_t AIU[] = L"\x3042\x3044\x3046"; // あいう
static const wchar_t KA[] = L"\x304B";           // か
static const wchar_t KAKI[] = L"\x304B\x304D";   // かき
static const wchar_t AKA[] = L"\x3042\x304B";    // あか
static const wchar_t I[] = L"\x3044";            // い
static const wchar_t TOKYO[] = L"\x6771\x4EAC";  // 東京
static const wchar_t OSAKA[] = L"\x5927\x962A";  // 大阪
static const wchar_t DAI[] = L"\x5927";          // 大
static const wchar_t KYOTO[] = L"\x4EAC\x90FD";  // 京都

// --- Basic composition ---

TEST(ImeBufferTest, BasicComposition) {
    ImeBuffer buf;
    buf.compositionStarted();

    buf.applyTextUpdate(0, 0, A, 1);
    EXPECT_EQ(buf.text(), A);

    buf.applyTextUpdate(0, 1, AI, 2);
    EXPECT_EQ(buf.text(), AI);

    buf.applyTextUpdate(0, 2, AIU, 3);
    EXPECT_EQ(buf.text(), AIU);

    EXPECT_EQ(buf.selectionPosition(), 3);
    EXPECT_TRUE(buf.composing());
}

TEST(ImeBufferTest, CompositionConfirm) {
    ImeBuffer buf;
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, KAKI, 2);
    EXPECT_EQ(buf.text(), KAKI);

    buf.compositionCompleted();
    EXPECT_TRUE(buf.text().empty());
    EXPECT_EQ(buf.baseOffset(), 2);
    EXPECT_FALSE(buf.composing());
}

// --- Second composition after confirm (the bug we fixed) ---

TEST(ImeBufferTest, SecondCompositionAfterConfirm) {
    ImeBuffer buf;

    // First composition confirmed
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, AIU, 3);
    buf.compositionCompleted();
    EXPECT_EQ(buf.baseOffset(), 3);

    // Second composition: context sends range starting at 3 (accumulated)
    buf.compositionStarted();
    EXPECT_TRUE(buf.text().empty());

    buf.applyTextUpdate(3, 3, KA, 1);
    EXPECT_EQ(buf.text(), KA);

    buf.applyTextUpdate(3, 4, KAKI, 2);
    EXPECT_EQ(buf.text(), KAKI);

    EXPECT_EQ(buf.selectionPosition(), 5);  // 3 + 2
}

TEST(ImeBufferTest, ThirdComposition) {
    ImeBuffer buf;

    // First: "abc" (3 chars)
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, L"abc", 3);
    buf.compositionCompleted();
    EXPECT_EQ(buf.baseOffset(), 3);

    // Second: "de" (2 chars)
    buf.compositionStarted();
    buf.applyTextUpdate(3, 3, L"de", 2);
    buf.compositionCompleted();
    EXPECT_EQ(buf.baseOffset(), 5);

    // Third: context sends range starting at 5
    buf.compositionStarted();
    buf.applyTextUpdate(5, 5, L"f", 1);
    EXPECT_EQ(buf.text(), L"f");
    EXPECT_EQ(buf.selectionPosition(), 6);
}

// --- Backspace during composition ---

TEST(ImeBufferTest, BackspaceDuringComposition) {
    ImeBuffer buf;
    buf.compositionStarted();

    buf.applyTextUpdate(0, 0, AIU, 3);
    EXPECT_EQ(buf.text(), AIU);

    // Backspace: replace with shorter
    buf.applyTextUpdate(0, 3, AI, 2);
    EXPECT_EQ(buf.text(), AI);

    buf.applyTextUpdate(0, 2, A, 1);
    EXPECT_EQ(buf.text(), A);

    // Continue typing
    buf.applyTextUpdate(0, 1, AKA, 2);
    EXPECT_EQ(buf.text(), AKA);
}

TEST(ImeBufferTest, BackspaceThenNewComposition) {
    ImeBuffer buf;

    // Type and backspace all, then confirm empty
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, A, 1);
    buf.applyTextUpdate(0, 1, L"", 0);  // backspace all
    EXPECT_TRUE(buf.text().empty());

    buf.compositionCompleted();
    EXPECT_EQ(buf.baseOffset(), 0);  // nothing was committed

    // Second composition should work normally
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, I, 1);
    EXPECT_EQ(buf.text(), I);
}

// --- TextRequested padding ---

TEST(ImeBufferTest, PaddedText) {
    ImeBuffer buf;

    // First composition confirmed
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, L"abc", 3);
    buf.compositionCompleted();

    // Second composition in progress
    buf.compositionStarted();
    buf.applyTextUpdate(3, 3, L"de", 2);

    auto padded = buf.paddedText();
    EXPECT_EQ(padded.size(), 5u);  // 3 spaces + "de"
    EXPECT_EQ(padded, L"   de");
}

// --- Reset ---

TEST(ImeBufferTest, ResetClearsAll) {
    ImeBuffer buf;

    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, L"test", 4);
    buf.compositionCompleted();
    EXPECT_EQ(buf.baseOffset(), 4);

    buf.reset();
    EXPECT_TRUE(buf.text().empty());
    EXPECT_EQ(buf.baseOffset(), 0);
    EXPECT_FALSE(buf.composing());
    EXPECT_EQ(buf.selectionPosition(), 0);
}

// --- Edge cases ---

TEST(ImeBufferTest, ClampNegativeRange) {
    ImeBuffer buf;
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, L"ab", 2);

    buf.compositionCompleted();
    buf.compositionStarted();
    // Context sends range={0,0} but baseOffset=2, localStart = -2
    buf.applyTextUpdate(0, 0, L"x", 1);
    EXPECT_EQ(buf.text(), L"x");
}

TEST(ImeBufferTest, RangeExceedsBuffer) {
    ImeBuffer buf;
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, L"a", 1);

    // Range end exceeds buffer
    buf.applyTextUpdate(0, 5, L"b", 1);
    EXPECT_EQ(buf.text(), L"b");
}

TEST(ImeBufferTest, MultipleBackspaceAndRecompose) {
    ImeBuffer buf;

    // Type TOKYO, confirm, then type OSAKA, backspace, type KYOTO
    buf.compositionStarted();
    buf.applyTextUpdate(0, 0, TOKYO, 2);
    buf.compositionCompleted();
    EXPECT_EQ(buf.baseOffset(), 2);

    buf.compositionStarted();
    buf.applyTextUpdate(2, 2, OSAKA, 2);
    EXPECT_EQ(buf.text(), OSAKA);

    // Backspace to DAI
    buf.applyTextUpdate(2, 4, DAI, 1);
    EXPECT_EQ(buf.text(), DAI);

    // Change to KYOTO
    buf.applyTextUpdate(2, 3, KYOTO, 2);
    EXPECT_EQ(buf.text(), KYOTO);

    buf.compositionCompleted();
    EXPECT_EQ(buf.baseOffset(), 4);  // 2 + 2
}
