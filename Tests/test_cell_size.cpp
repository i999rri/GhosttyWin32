#include "pch.h"
#include "../Core/Ghostty/Actions/Tags/CellSize.h"

using core::ghostty::actions::tags::CellSize;

namespace {

ghostty_action_cell_size_s Cell(unsigned w, unsigned h) {
    ghostty_action_cell_size_s c{};
    c.width = w;
    c.height = h;
    return c;
}

}  // namespace

// ----- the rounding rule -----

TEST(CellSizeTest, SnapRoundsTheDeltaToWholeCells) {
    // Growing the right edge from 100 by 25 with 12px cells: 25/12 =
    // 2.08 → 2 cells → 124.
    EXPECT_EQ(CellSize::Snap(100, 125, 12, +1), 124);
    // 31/12 = 2.58 → 3 cells → 136.
    EXPECT_EQ(CellSize::Snap(100, 131, 12, +1), 136);
}

TEST(CellSizeTest, SnapIsSymmetricWhenShrinking) {
    // The delta is negative when the user shrinks the window; it must
    // round the same way as growing (lround, not truncation toward
    // zero, which biased the negative side).
    EXPECT_EQ(CellSize::Snap(100, 75, 12, +1), 76);    // -25 → -2 cells
    EXPECT_EQ(CellSize::Snap(100, 69, 12, +1), 64);    // -31 → -3 cells
}

TEST(CellSizeTest, SnapHonoursTheEdgeDirection) {
    // A left edge grows away from its anchor toward negative x:
    // dragging from 100 to 75 is a +25 delta in the edge's own
    // direction → 2 cells → 76.
    EXPECT_EQ(CellSize::Snap(100, 75, 12, -1), 76);
    EXPECT_EQ(CellSize::Snap(100, 131, 12, -1), 136);   // shrinking from the left
}

TEST(CellSizeTest, SnapWithNoStepLeavesTheEdgeAlone) {
    EXPECT_EQ(CellSize::Snap(100, 117, 0, +1), 117);
    EXPECT_EQ(CellSize::Snap(100, 117, -5, +1), 117);
}

// ----- value + gate -----

TEST(CellSizeTest, DoesNotSnapUntilMeasuredAndEnabled) {
    CellSize c;
    EXPECT_FALSE(c.Snaps());
    c.Apply(Cell(12, 31), /*stepResize=*/false);
    EXPECT_FALSE(c.Snaps());   // measured, gate off
    c.SetEnabled(true);
    EXPECT_TRUE(c.Snaps());    // config reload flipped the gate
}

TEST(CellSizeTest, IgnoresAnUnmeasuredReport) {
    CellSize c;
    c.Apply(Cell(12, 31), true);
    c.Apply(Cell(0, 31), true);   // nothing measured yet on one axis
    EXPECT_EQ(c.Value().width, 12u);
    EXPECT_EQ(c.Value().height, 31u);
    EXPECT_TRUE(c.Snaps());
}

TEST(CellSizeTest, AxisHelpersUseWidthAndHeight) {
    CellSize c;
    c.Apply(Cell(12, 31), true);
    EXPECT_EQ(c.SnapHorizontal(0, 25, +1), 24);   // 12px cells
    EXPECT_EQ(c.SnapVertical(0, 50, +1), 62);     // 31px cells: 50/31 = 1.6 → 2
}

TEST(CellSizeTest, IsAPlainValue) {
    CellSize a;
    a.Apply(Cell(12, 31), true);
    CellSize b = a;   // NativeWindow keeps its own copy this way
    EXPECT_TRUE(b.Snaps());
    EXPECT_EQ(b.Value().width, 12u);
}
