#include "pch.h"
#include "Terminal/ImeSession.h"
#include <optional>
#include <string>
#include <vector>

using winrt::GhosttyWin32::implementation::EditContextHandlers;
using winrt::GhosttyWin32::implementation::IEditContextEvents;
using winrt::GhosttyWin32::implementation::ImeSession;

namespace {

// Keeps whatever the session installs, so a test can fire the events
// by hand without a CoreTextEditContext.
struct FakeEvents : IEditContextEvents {
    EditContextHandlers handlers;
    int sets = 0;
    void SetHandlers(EditContextHandlers h) override {
        handlers = std::move(h);
        ++sets;
    }
};

struct Recorder {
    std::vector<std::string> preedits;
    std::vector<std::string> commits;
    void Attach(ImeSession& s) {
        s.SetOnPreedit([this](std::string const& t) { preedits.push_back(t); });
        s.SetOnCommit([this](std::string const& t) { commits.push_back(t); });
    }
};

const wchar_t A[] = L"\x3042";            // あ
const char A_UTF8[] = "\xE3\x81\x82";

}  // namespace

TEST(ImeSessionTest, InstallsAllSevenHandlers) {
    FakeEvents events;
    ImeSession session(events);
    EXPECT_EQ(events.sets, 1);
    EXPECT_TRUE(events.handlers.textRequested);
    EXPECT_TRUE(events.handlers.selectionRequested);
    EXPECT_TRUE(events.handlers.textUpdating);
    EXPECT_TRUE(events.handlers.compositionStarted);
    EXPECT_TRUE(events.handlers.compositionCompleted);
    EXPECT_TRUE(events.handlers.layoutRequested);
    EXPECT_TRUE(events.handlers.focusRemoved);
}

TEST(ImeSessionTest, CompositionUpdatesReachPreedit) {
    FakeEvents events;
    ImeSession session(events);
    Recorder rec; rec.Attach(session);

    events.handlers.compositionStarted();
    events.handlers.textUpdating(0, 0, winrt::hstring(A));

    EXPECT_TRUE(session.Composing());
    ASSERT_EQ(rec.preedits.size(), 1u);
    EXPECT_EQ(rec.preedits[0], A_UTF8);
    EXPECT_TRUE(rec.commits.empty());
}

TEST(ImeSessionTest, CommitReachesOnCommitOnce) {
    FakeEvents events;
    ImeSession session(events);
    Recorder rec; rec.Attach(session);

    events.handlers.compositionStarted();
    events.handlers.textUpdating(0, 0, winrt::hstring(A));
    events.handlers.compositionCompleted();

    ASSERT_EQ(rec.commits.size(), 1u);
    EXPECT_EQ(rec.commits[0], A_UTF8);
    EXPECT_FALSE(session.Composing());
}

TEST(ImeSessionTest, TextOutsideCompositionIsNotPreedit) {
    FakeEvents events;
    ImeSession session(events);
    Recorder rec; rec.Attach(session);

    events.handlers.textUpdating(0, 0, winrt::hstring(A));   // no compositionStarted

    EXPECT_FALSE(session.Composing());
    EXPECT_TRUE(rec.preedits.empty());
}

TEST(ImeSessionTest, FocusRemovedMidCompositionClearsPreedit) {
    FakeEvents events;
    ImeSession session(events);
    Recorder rec; rec.Attach(session);

    events.handlers.compositionStarted();
    events.handlers.textUpdating(0, 0, winrt::hstring(A));
    events.handlers.focusRemoved();

    EXPECT_FALSE(session.Composing());
    ASSERT_EQ(rec.preedits.size(), 2u);
    EXPECT_EQ(rec.preedits.back(), "");   // empty = clear
    EXPECT_TRUE(rec.commits.empty());
}

TEST(ImeSessionTest, FocusRemovedOutsideCompositionIsQuiet) {
    FakeEvents events;
    ImeSession session(events);
    Recorder rec; rec.Attach(session);

    events.handlers.focusRemoved();

    EXPECT_TRUE(rec.preedits.empty());
    EXPECT_TRUE(rec.commits.empty());
}

TEST(ImeSessionTest, TextRequestedReturnsTheCompositionText) {
    FakeEvents events;
    ImeSession session(events);

    events.handlers.compositionStarted();
    events.handlers.textUpdating(0, 0, winrt::hstring(A));

    EXPECT_EQ(std::wstring(events.handlers.textRequested()), std::wstring(A));
    EXPECT_EQ(events.handlers.selectionRequested(), 1);
}

TEST(ImeSessionTest, CaretRectPassesThroughWhenSupplied) {
    FakeEvents events;
    ImeSession session(events);

    EXPECT_FALSE(events.handlers.layoutRequested().has_value());

    session.SetCaretRect([]() -> std::optional<winrt::Windows::Foundation::Rect> {
        return winrt::Windows::Foundation::Rect{ 10.f, 20.f, 30.f, 40.f };
    });
    auto rect = events.handlers.layoutRequested();
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->X, 10.f);
    EXPECT_EQ(rect->Height, 40.f);
}

TEST(ImeSessionTest, ResetForgetsTheComposition) {
    FakeEvents events;
    ImeSession session(events);

    events.handlers.compositionStarted();
    events.handlers.textUpdating(0, 0, winrt::hstring(A));
    session.Reset();

    EXPECT_FALSE(session.Composing());
    EXPECT_EQ(std::wstring(events.handlers.textRequested()), std::wstring());
}

TEST(ImeSessionTest, DestructorRemovesItsHandlers) {
    FakeEvents events;
    {
        ImeSession session(events);
        EXPECT_TRUE(events.handlers.textUpdating);
    }
    EXPECT_EQ(events.sets, 2);
    EXPECT_FALSE(events.handlers.textUpdating);
    EXPECT_FALSE(events.handlers.focusRemoved);
}
