#include "pch.h"
#include "Input/PressAction.h"

#include <string>

using core::input::PressAction;

namespace {

// Which handler a Match ran, by name.
std::string Taken(PressAction const& action) {
    return action.Match(
        [](PressAction::Ignore) { return std::string("ignore"); },
        [](PressAction::CopySelection) { return std::string("copy"); },
        [](PressAction::RelayToTerminal) { return std::string("relay"); });
}

}  // namespace

TEST(PressActionTest, MatchRunsTheHandlerForItsCase) {
    EXPECT_EQ(Taken(PressAction::Ignore{}), "ignore");
    EXPECT_EQ(Taken(PressAction::CopySelection{}), "copy");
    EXPECT_EQ(Taken(PressAction::RelayToTerminal{}), "relay");
}

TEST(PressActionTest, HandlersAreToldApartByTypeNotOrder) {
    PressAction const action = PressAction::CopySelection{};
    auto taken = action.Match(
        [](PressAction::RelayToTerminal) { return 3; },
        [](PressAction::Ignore) { return 1; },
        [](PressAction::CopySelection) { return 2; });
    EXPECT_EQ(taken, 2);
}
