#include "pch.h"
#include "../GhosttyWin32App/Encoding.h"

TEST(EncodingTest, Utf16ToUtf8Ascii) {
    auto result = Encoding::toUtf8(L"hello", 5);
    EXPECT_EQ(result, "hello");
}

TEST(EncodingTest, Utf16ToUtf8Japanese) {
    // U+3042 U+3044 U+3046 = 9 UTF-8 bytes
    const wchar_t input[] = L"\x3042\x3044\x3046";
    auto result = Encoding::toUtf8(input, 3);
    EXPECT_EQ(result.size(), 9u);
}

TEST(EncodingTest, Utf16ToUtf8WString) {
    std::wstring input = L"test";
    auto result = Encoding::toUtf8(input);
    EXPECT_EQ(result, "test");
}

TEST(EncodingTest, Utf16ToUtf8Empty) {
    auto result = Encoding::toUtf8(L"", 0);
    EXPECT_TRUE(result.empty());
}

TEST(EncodingTest, Utf16ToUtf8Null) {
    auto result = Encoding::toUtf8(nullptr, 0);
    EXPECT_TRUE(result.empty());
}

TEST(EncodingTest, Utf8ToUtf16Ascii) {
    auto result = Encoding::toUtf16("hello");
    EXPECT_EQ(result, L"hello");
}

TEST(EncodingTest, Utf8ToUtf16Japanese) {
    // UTF-8 for U+3042 U+3044 U+3046
    const char utf8[] = "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86";
    auto result = Encoding::toUtf16(utf8, 9);
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 0x3042);
    EXPECT_EQ(result[1], 0x3044);
    EXPECT_EQ(result[2], 0x3046);
}

TEST(EncodingTest, Utf8ToUtf16NullTerminated) {
    auto result = Encoding::toUtf16("abc");
    EXPECT_EQ(result, L"abc");
    EXPECT_EQ(result.size(), 3u);
}

TEST(EncodingTest, Utf8ToUtf16Null) {
    auto result = Encoding::toUtf16(nullptr);
    EXPECT_TRUE(result.empty());
}
