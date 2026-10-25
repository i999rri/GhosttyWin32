#include "pch.h"
#include "../Core/Wsl/SessionExit.h"

using core::wsl::SessionExit;

namespace {

// ghostty's own default for abnormal-command-exit-runtime.
constexpr uint32_t kAbnormalMs = 250;

}  // namespace

TEST(WslSessionExitTest, CarriesTheCodeWhateverItIs) {
    // The code is the shim's answer, so it travels untouched. No value
    // is read for meaning, which is why none of them appear below.
    EXPECT_EQ(SessionExit(0, 9'000).Code(), 0u);
    EXPECT_EQ(SessionExit(3, 9'000).Code(), 3u);
    EXPECT_EQ(SessionExit(4294967295u, 130).Code(), 4294967295u);
}

TEST(WslSessionExitTest, FailedBeforeUseWhenTheSessionNeverStarted) {
    // The shapes seen while the bridge was young: the helper could not
    // exec what it was asked for, and wsl.exe refused the line before
    // the distribution came up at all (130 ms, measured).
    EXPECT_TRUE(SessionExit(127, 20).FailedBeforeUse(kAbnormalMs));
    EXPECT_TRUE(SessionExit(4294967295u, 130).FailedBeforeUse(kAbnormalMs));
    EXPECT_TRUE(SessionExit(1, kAbnormalMs).FailedBeforeUse(kAbnormalMs));
}

TEST(WslSessionExitTest, NotFailedBeforeUseAfterASessionThatWasUsed) {
    // `exit 3` is non-zero and ends a session someone worked in, which
    // is why the code alone cannot decide this.
    EXPECT_FALSE(SessionExit(3, 9'000).FailedBeforeUse(kAbnormalMs));
    EXPECT_FALSE(SessionExit(127, kAbnormalMs + 1).FailedBeforeUse(kAbnormalMs));
}

TEST(WslSessionExitTest, NotFailedBeforeUseWhenTheSessionSucceeded) {
    // Zero is zero however briefly it lived, matching the gate ghostty
    // puts in front of its own abnormal-exit handling.
    EXPECT_FALSE(SessionExit(0, 0).FailedBeforeUse(kAbnormalMs));
    EXPECT_FALSE(SessionExit(0, 20).FailedBeforeUse(kAbnormalMs));
    EXPECT_FALSE(SessionExit(0, 60'000).FailedBeforeUse(kAbnormalMs));
}

TEST(WslSessionExitTest, FollowsTheConfiguredWindow) {
    // The threshold is a setting, so the same exit falls either side of
    // it depending on what the config says.
    const SessionExit ended{ 1, 1'000 };
    EXPECT_FALSE(ended.FailedBeforeUse(250));
    EXPECT_TRUE(ended.FailedBeforeUse(2'500));
}
