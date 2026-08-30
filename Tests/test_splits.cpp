#include "pch.h"
#include "../Core/Ghostty/Actions/Splits.h"
#include <vector>

using namespace core::ghostty::actions::splits;

namespace core::ghostty::actions::splits {
// gtest prints a scoped enum only through a PrintTo overload found
// by ADL; without one, a failing EXPECT_EQ on Axis cannot be
// reported.
inline void PrintTo(Axis axis, std::ostream* os) {
    *os << (axis == Axis::Horizontal ? "Horizontal" : "Vertical");
}
}  // namespace core::ghostty::actions::splits

namespace {

// A 2×2 grid of 100×100 panes:
//   0 | 1
//   --+--
//   2 | 3
std::vector<Rect> Grid2x2() {
    return {
        { 0,   0,   100, 100 },
        { 100, 0,   100, 100 },
        { 0,   100, 100, 100 },
        { 100, 100, 100, 100 },
    };
}

ghostty_action_resize_split_s Resize(ghostty_action_resize_split_direction_e dir, uint16_t amount) {
    ghostty_action_resize_split_s r{};
    r.direction = dir;
    r.amount = amount;
    return r;
}

}  // namespace

// ----- GOTO_SPLIT, spatial -----

TEST(SplitsTest, AdjacentPaneFindsTheAlignedNeighbour) {
    auto panes = Grid2x2();
    EXPECT_EQ(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_RIGHT), 1u);
    EXPECT_EQ(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_DOWN),  2u);
    EXPECT_EQ(AdjacentPane(panes, 3, GHOSTTY_GOTO_SPLIT_LEFT),  2u);
    EXPECT_EQ(AdjacentPane(panes, 3, GHOSTTY_GOTO_SPLIT_UP),    1u);
}

TEST(SplitsTest, AdjacentPaneIsNoneAtTheEdge) {
    auto panes = Grid2x2();
    EXPECT_FALSE(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_LEFT).has_value());
    EXPECT_FALSE(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_UP).has_value());
    EXPECT_FALSE(AdjacentPane(panes, 3, GHOSTTY_GOTO_SPLIT_RIGHT).has_value());
}

TEST(SplitsTest, AdjacentPanePrefersAlignedOverNearerDiagonal) {
    // Active on the left; to its right a tall pane whose centre is
    // far off-axis but which touches, and a narrow pane further away
    // that is aligned. The 2× perpendicular penalty makes the aligned
    // one win even though the tall one is closer in straight-line
    // distance.
    std::vector<Rect> panes = {
        { 0,   0,   100, 100 },   // active
        { 100, 0,   100, 400 },   // touching, centre 150px below
        { 220, 0,   100, 100 },   // 120px away, aligned
    };
    // scores: [1] 0 + 2*150 = 300, [2] 120 + 0 = 120
    EXPECT_EQ(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_RIGHT), 2u);
}

TEST(SplitsTest, AdjacentPaneAbsorbsBoundaryRounding) {
    // A neighbour whose edge overlaps the boundary by half a pixel
    // (float layout) still counts as "on that side".
    std::vector<Rect> panes = {
        { 0,     0, 100,   100 },
        { 99.5f, 0, 100.5f, 100 },
    };
    EXPECT_EQ(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_RIGHT), 1u);
    // More than a pixel of overlap is a pane beside us, not beyond.
    panes[1].x = 98.0f;
    EXPECT_FALSE(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_RIGHT).has_value());
}

TEST(SplitsTest, AdjacentPaneRejectsBadInput) {
    auto panes = Grid2x2();
    EXPECT_FALSE(AdjacentPane(panes, 4, GHOSTTY_GOTO_SPLIT_RIGHT).has_value());
    EXPECT_FALSE(AdjacentPane(panes, 0, GHOSTTY_GOTO_SPLIT_NEXT).has_value());
}

// ----- GOTO_SPLIT, cyclic -----

TEST(SplitsTest, CyclePaneWrapsBothWays) {
    EXPECT_EQ(CyclePane(0, 3, GHOSTTY_GOTO_SPLIT_NEXT), 1u);
    EXPECT_EQ(CyclePane(2, 3, GHOSTTY_GOTO_SPLIT_NEXT), 0u);
    EXPECT_EQ(CyclePane(0, 3, GHOSTTY_GOTO_SPLIT_PREVIOUS), 2u);
    EXPECT_EQ(CyclePane(1, 3, GHOSTTY_GOTO_SPLIT_PREVIOUS), 0u);
}

TEST(SplitsTest, CyclePaneLeavesOtherDirectionsAlone) {
    EXPECT_EQ(CyclePane(1, 3, GHOSTTY_GOTO_SPLIT_LEFT), 1u);
    EXPECT_EQ(CyclePane(5, 3, GHOSTTY_GOTO_SPLIT_NEXT), 5u);   // out of range
    EXPECT_EQ(CyclePane(0, 0, GHOSTTY_GOTO_SPLIT_NEXT), 0u);
}

// ----- NEW_SPLIT -----

TEST(SplitsTest, PlaceSplitMapsDirectionToAxisAndOrder) {
    auto r = PlaceSplit(GHOSTTY_SPLIT_DIRECTION_RIGHT);
    ASSERT_TRUE(r); EXPECT_EQ(r->axis, Axis::Horizontal); EXPECT_FALSE(r->newFirst);
    auto l = PlaceSplit(GHOSTTY_SPLIT_DIRECTION_LEFT);
    ASSERT_TRUE(l); EXPECT_EQ(l->axis, Axis::Horizontal); EXPECT_TRUE(l->newFirst);
    auto d = PlaceSplit(GHOSTTY_SPLIT_DIRECTION_DOWN);
    ASSERT_TRUE(d); EXPECT_EQ(d->axis, Axis::Vertical); EXPECT_FALSE(d->newFirst);
    auto u = PlaceSplit(GHOSTTY_SPLIT_DIRECTION_UP);
    ASSERT_TRUE(u); EXPECT_EQ(u->axis, Axis::Vertical); EXPECT_TRUE(u->newFirst);
}

TEST(SplitsTest, HalfAlongHalvesOnlyTheSplitAxis) {
    auto h = HalfAlong({ 800, 600 }, Axis::Horizontal);
    EXPECT_EQ(h.width, 400u); EXPECT_EQ(h.height, 600u);
    auto v = HalfAlong({ 800, 600 }, Axis::Vertical);
    EXPECT_EQ(v.width, 800u); EXPECT_EQ(v.height, 300u);
}

// ----- RESIZE_SPLIT -----

TEST(SplitsTest, ResizeAxisIsTheOneTheArrowCrosses) {
    EXPECT_EQ(ResizeAxis(GHOSTTY_RESIZE_SPLIT_LEFT),  Axis::Horizontal);
    EXPECT_EQ(ResizeAxis(GHOSTTY_RESIZE_SPLIT_RIGHT), Axis::Horizontal);
    EXPECT_EQ(ResizeAxis(GHOSTTY_RESIZE_SPLIT_UP),    Axis::Vertical);
    EXPECT_EQ(ResizeAxis(GHOSTTY_RESIZE_SPLIT_DOWN),  Axis::Vertical);
}

TEST(SplitsTest, ResizedRatioMovesTheBoundaryTheArrowsWay) {
    // 1000px wide split, 0px divider: 100px is a tenth.
    EXPECT_DOUBLE_EQ(ResizedRatio(0.5, Resize(GHOSTTY_RESIZE_SPLIT_RIGHT, 100), 1000, 0), 0.6);
    EXPECT_DOUBLE_EQ(ResizedRatio(0.5, Resize(GHOSTTY_RESIZE_SPLIT_LEFT,  100), 1000, 0), 0.4);
    EXPECT_DOUBLE_EQ(ResizedRatio(0.5, Resize(GHOSTTY_RESIZE_SPLIT_DOWN,  100), 1000, 0), 0.6);
    EXPECT_DOUBLE_EQ(ResizedRatio(0.5, Resize(GHOSTTY_RESIZE_SPLIT_UP,    100), 1000, 0), 0.4);
}

TEST(SplitsTest, ResizedRatioAccountsForTheDivider) {
    // The divider is not pane space: 100px of a 1001px split with a
    // 1px divider is a tenth of the usable 1000.
    EXPECT_DOUBLE_EQ(ResizedRatio(0.5, Resize(GHOSTTY_RESIZE_SPLIT_RIGHT, 100), 1001, 1), 0.6);
}

TEST(SplitsTest, ResizedRatioNeverLetsAChildVanish) {
    EXPECT_DOUBLE_EQ(ResizedRatio(0.9, Resize(GHOSTTY_RESIZE_SPLIT_RIGHT, 500), 1000, 0), 0.95);
    EXPECT_DOUBLE_EQ(ResizedRatio(0.1, Resize(GHOSTTY_RESIZE_SPLIT_LEFT,  500), 1000, 0), 0.05);
    // A degenerate extent does not divide by zero.
    EXPECT_DOUBLE_EQ(ResizedRatio(0.5, Resize(GHOSTTY_RESIZE_SPLIT_RIGHT, 1), 0, 1), 0.95);
}
