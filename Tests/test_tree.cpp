#include "pch.h"
#include "../Core/Panes/Tree.h"
#include <vector>

// The tree without a single WinUI type: a pane's handle stays empty
// and its view null — the structural rules (replace, remove with
// sibling promotion, zoom invalidation) never look at either, which
// is exactly what lets them run here. Panes are told apart by id.

namespace core::panes {
// gtest prints a scoped enum only through a PrintTo overload found
// by ADL; without one, a failing EXPECT_EQ on these values
// cannot be reported.
inline void PrintTo(Layout l, std::ostream* os) {
    *os << (l.IsHorizontal() ? "Horizontal" : "Vertical");
}
inline void PrintTo(Direction d, std::ostream* os) {
    *os << (d.IsLeft() ? "Left" : d.IsRight() ? "Right" : d.IsUp() ? "Up" : "Down");
}
}  // namespace core::panes

using core::panes::Branch;
using core::panes::Direction;
using core::panes::Goto;
using core::panes::Layout;
using core::panes::Resize;
using core::panes::MakePaneBranch;
using core::panes::MakeSplitBranch;
using core::panes::Pane;
using core::panes::PaneId;
using core::panes::Split;
using core::panes::Tree;

namespace {

std::unique_ptr<Branch> Leaf(uint64_t id) {
    return MakePaneBranch(Pane{ nullptr, nullptr, PaneId{ id } });
}

// root = Split{ 1, Split{ 2, 3 } } — the smallest nested shape.
Tree Nested(Layout outer = Layout::Horizontal(),
            Layout inner = Layout::Vertical()) {
    auto sub = MakeSplitBranch(inner, 0.5, Leaf(2), Leaf(3));
    return Tree{ MakeSplitBranch(outer, 0.5, Leaf(1), std::move(sub)) };
}

std::vector<uint64_t> Ids(Tree& t) {
    std::vector<uint64_t> out;
    for (auto* p : t.Panes()) out.push_back(p->id.value);
    return out;
}

Pane* ById(Tree& t, uint64_t id) {
    return t.FindPaneBy([id](Pane const& p) { return p.id.value == id; });
}

// The split operations read the arranged rects the layout pass writes
// onto each leaf's wrapping Branch; the tests play the layout pass.
void Arrange(Tree& t, uint64_t id, float x, float y, float w, float h) {
    auto* branch = t.TryFindBranch(*ById(t, id));
    ASSERT_NE(branch, nullptr);
    branch->arrangedRect = { x, y, w, h };
}

// A 2x2 grid of 100x100 panes:
//   1 | 2
//   --+--
//   3 | 4
Tree Grid2x2() {
    auto left  = MakeSplitBranch(Layout::Vertical(), 0.5, Leaf(1), Leaf(3));
    auto right = MakeSplitBranch(Layout::Vertical(), 0.5, Leaf(2), Leaf(4));
    Tree t{ MakeSplitBranch(Layout::Horizontal(), 0.5,
                            std::move(left), std::move(right)) };
    Arrange(t, 1, 0,   0,   100, 100);
    Arrange(t, 2, 100, 0,   100, 100);
    Arrange(t, 3, 0,   100, 100, 100);
    Arrange(t, 4, 100, 100, 100, 100);
    return t;
}

Resize ResizeBy(ghostty_action_resize_split_direction_e dir,
                uint16_t amount) {
    ghostty_action_resize_split_s r{};
    r.direction = dir;
    r.amount = amount;
    auto typed = Resize::From(r);
    EXPECT_TRUE(typed.has_value());
    return *typed;
}

}  // namespace

// ----- walks -----

TEST(TreeTest, PanesListsLeavesDepthFirst) {
    auto t = Nested();
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 1, 2, 3 }));
}

TEST(TreeTest, TryFindBranchLocatesTheWrappingNode) {
    auto t = Nested();
    auto* pane = ById(t, 2);
    ASSERT_NE(pane, nullptr);
    auto* branch = t.TryFindBranch(*pane);
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(branch->TryGet<Pane>(), pane);
    // A pane that isn't in this tree comes back null.
    Pane stray{ nullptr, nullptr, PaneId{ 99 } };
    EXPECT_EQ(t.TryFindBranch(stray), nullptr);
}

TEST(TreeTest, NearestSplitAboveMatchesTheAxis) {
    auto t = Nested(Layout::Horizontal(), Layout::Vertical());
    auto* paneC = ById(t, 3);
    ASSERT_NE(paneC, nullptr);

    // 3's immediate parent splits vertically; the root horizontally.
    auto* vertical = t.NearestSplitAbove(*paneC, Layout::Vertical());
    ASSERT_NE(vertical, nullptr);
    EXPECT_EQ(vertical->TryGet<Split>()->layout, Layout::Vertical());

    auto* horizontal = t.NearestSplitAbove(*paneC, Layout::Horizontal());
    ASSERT_NE(horizontal, nullptr);
    EXPECT_EQ(horizontal, t.Root());

    // A lone pane has no split above it at all.
    Tree lone{ Leaf(1) };
    auto* only = ById(lone, 1);
    EXPECT_EQ(lone.NearestSplitAbove(*only, Layout::Horizontal()), nullptr);
}

// ----- ReplacePane (the NEW_SPLIT mutation) -----

TEST(TreeTest, ReplacePaneSwapsARootLeafForASubtree) {
    Tree t{ Leaf(1) };
    auto* pane = ById(t, 1);
    ASSERT_NE(pane, nullptr);

    auto sub = MakeSplitBranch(Layout::Horizontal(), 0.5, Leaf(1), Leaf(2));
    ASSERT_TRUE(t.ReplacePane(*pane, std::move(sub)));
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 1, 2 }));
    EXPECT_EQ(t.Root()->TryGet<Split>()->left->parent, t.Root());
}

TEST(TreeTest, ReplacePaneRewiresTheParentSlot) {
    auto t = Nested();
    auto* paneB = ById(t, 2);
    ASSERT_NE(paneB, nullptr);

    auto sub = MakeSplitBranch(Layout::Horizontal(), 0.5, Leaf(2), Leaf(4));
    ASSERT_TRUE(t.ReplacePane(*paneB, std::move(sub)));
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 1, 2, 4, 3 }));

    // The new subtree hangs where 2's wrapper was, with its parent set.
    auto* newB = ById(t, 2);
    auto* wrapping = t.TryFindBranch(*newB);
    ASSERT_NE(wrapping->parent, nullptr);
    EXPECT_NE(wrapping->parent->TryGet<Split>(), nullptr);
}

TEST(TreeTest, ReplacePaneOfAStrayPaneFails) {
    auto t = Nested();
    Pane stray{ nullptr, nullptr, PaneId{ 99 } };
    EXPECT_FALSE(t.ReplacePane(stray, Leaf(5)));
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 1, 2, 3 }));
}

// ----- RemovePane (the close mutation) -----

TEST(TreeTest, RemovingTheRootLeafEmptiesTheTree) {
    Tree t{ Leaf(1) };
    auto* pane = ById(t, 1);
    EXPECT_TRUE(t.RemovePane(*pane).IsRemovedRoot());
    EXPECT_FALSE(t.HasRoot());
}

TEST(TreeTest, RemovingOneOfTwoPromotesTheSiblingToRoot) {
    Tree t{ MakeSplitBranch(Layout::Horizontal(), 0.5, Leaf(1), Leaf(2)) };
    auto* paneA = ById(t, 1);
    EXPECT_TRUE(t.RemovePane(*paneA).IsCollapsed());
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 2 }));
    EXPECT_EQ(t.Root()->parent, nullptr);
    EXPECT_NE(t.Root()->TryGet<Pane>(), nullptr);
}

TEST(TreeTest, RemovingANestedPanePromotesTheSiblingIntoTheGrandparent) {
    auto t = Nested();
    auto* paneB = ById(t, 2);
    EXPECT_TRUE(t.RemovePane(*paneB).IsCollapsed());
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 1, 3 }));

    // 3 now hangs directly off the root split, parent rewired.
    auto* paneC = ById(t, 3);
    auto* wrapping = t.TryFindBranch(*paneC);
    EXPECT_EQ(wrapping->parent, t.Root());
}

TEST(TreeTest, RemovingAStrayPaneIsNotFound) {
    auto t = Nested();
    Pane stray{ nullptr, nullptr, PaneId{ 99 } };
    EXPECT_TRUE(t.RemovePane(stray).IsNotFound());
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 1, 2, 3 }));
}

// ----- zoom invalidation -----

TEST(TreeTest, MutationsClearAZoomOnTheTouchedPane) {
    auto t = Nested();
    auto* paneB = ById(t, 2);
    t.SetZoomed(paneB);
    ASSERT_EQ(t.Zoomed(), paneB);

    EXPECT_TRUE(t.RemovePane(*paneB).IsCollapsed());
    EXPECT_EQ(t.Zoomed(), nullptr);

    auto* paneC = ById(t, 3);
    t.SetZoomed(paneC);
    t.SetRoot(Leaf(7));
    EXPECT_EQ(t.Zoomed(), nullptr);
}

// ----- GOTO_SPLIT, spatial -----

TEST(TreeTest, GotoTargetFindsTheAlignedNeighbour) {
    auto t = Grid2x2();
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Right()), ById(t, 2));
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Down()),  ById(t, 3));
    EXPECT_EQ(t.GotoTarget(*ById(t, 4), Goto::Left()),  ById(t, 3));
    EXPECT_EQ(t.GotoTarget(*ById(t, 4), Goto::Up()),    ById(t, 2));
}

TEST(TreeTest, GotoTargetIsNoneAtTheEdge) {
    auto t = Grid2x2();
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Left()),  nullptr);
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Up()),    nullptr);
    EXPECT_EQ(t.GotoTarget(*ById(t, 4), Goto::Right()), nullptr);
}

TEST(TreeTest, GotoTargetPrefersAlignedOverNearerDiagonal) {
    // Active on the left; to its right a tall pane whose centre is
    // far off-axis but which touches, and a narrow pane further away
    // that is aligned. The 2x perpendicular penalty makes the aligned
    // one win even though the tall one is closer in straight-line
    // distance.
    auto sub = MakeSplitBranch(Layout::Horizontal(), 0.5, Leaf(2), Leaf(3));
    Tree t{ MakeSplitBranch(Layout::Horizontal(), 0.5,
                            Leaf(1), std::move(sub)) };
    Arrange(t, 1, 0,   0, 100, 100);   // active
    Arrange(t, 2, 100, 0, 100, 400);   // touching, centre 150px below
    Arrange(t, 3, 220, 0, 100, 100);   // 120px away, aligned
    // scores: [2] 0 + 2*150 = 300, [3] 120 + 0 = 120
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Right()), ById(t, 3));
}

TEST(TreeTest, GotoTargetAbsorbsBoundaryRounding) {
    // A neighbour whose edge overlaps the boundary by half a pixel
    // (float layout) still counts as "on that side".
    Tree t{ MakeSplitBranch(Layout::Horizontal(), 0.5, Leaf(1), Leaf(2)) };
    Arrange(t, 1, 0,     0, 100,    100);
    Arrange(t, 2, 99.5f, 0, 100.5f, 100);
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Right()), ById(t, 2));
    // More than a pixel of overlap is a pane beside us, not beyond.
    Arrange(t, 2, 98.0f, 0, 100.5f, 100);
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Right()), nullptr);
}

// ----- GOTO_SPLIT, cyclic -----

TEST(TreeTest, GotoTargetCyclesDepthFirstWrappingBothWays) {
    auto t = Nested();   // depth-first order 1, 2, 3
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Next()),     ById(t, 2));
    EXPECT_EQ(t.GotoTarget(*ById(t, 3), Goto::Next()),     ById(t, 1));
    EXPECT_EQ(t.GotoTarget(*ById(t, 1), Goto::Previous()), ById(t, 3));
    EXPECT_EQ(t.GotoTarget(*ById(t, 2), Goto::Previous()), ById(t, 1));
}

TEST(TreeTest, GotoTargetRejectsBadInput) {
    auto t = Nested();
    // A pane that isn't in this tree.
    Pane stray{ nullptr, nullptr, PaneId{ 99 } };
    EXPECT_EQ(t.GotoTarget(stray, Goto::Next()), nullptr);
    // A lone pane has nowhere to go.
    Tree lone{ Leaf(1) };
    EXPECT_EQ(lone.GotoTarget(*ById(lone, 1), Goto::Next()), nullptr);
}

// ----- NEW_SPLIT -----

TEST(TreeTest, DirectionFromMapsTheGhosttyArrow) {
    using D = Direction;
    EXPECT_EQ(D::From(GHOSTTY_SPLIT_DIRECTION_RIGHT), D::Right());
    EXPECT_EQ(D::From(GHOSTTY_SPLIT_DIRECTION_LEFT),  D::Left());
    EXPECT_EQ(D::From(GHOSTTY_SPLIT_DIRECTION_DOWN),  D::Down());
    EXPECT_EQ(D::From(GHOSTTY_SPLIT_DIRECTION_UP),    D::Up());
    EXPECT_TRUE(D::Left().IsLeft());
    EXPECT_TRUE(D::Right().IsRight());
    EXPECT_TRUE(D::Up().IsUp());
    EXPECT_TRUE(D::Down().IsDown());
}

namespace {

// Splitting source pane 1 toward `direction`, growing new pane 2 —
// returns the (layout, left id, right id) the factory produced.
struct SplitShape {
    Layout layout;
    uint64_t left;
    uint64_t right;
};

SplitShape ShapeOfSplitToward(Direction direction) {
    auto b = MakeSplitBranch(Leaf(1), direction, Leaf(2));
    auto* split = b->TryGet<Split>();
    EXPECT_NE(split, nullptr);
    return { split->layout,
             split->left->TryGet<Pane>()->id.value,
             split->right->TryGet<Pane>()->id.value };
}

}  // namespace

TEST(TreeTest, MakeSplitBranchGrowsTheNewPaneWhereTheArrowPoints) {
    using D = Direction;
    // source = 1, new pane = 2; left is the left / top slot.
    {
        auto s = ShapeOfSplitToward(D::Right());
        EXPECT_EQ(s.layout, Layout::Horizontal());
        EXPECT_EQ(s.left,  1u);   // the source keeps its side
        EXPECT_EQ(s.right, 2u);   // the new pane grows to the right
    }
    {
        auto s = ShapeOfSplitToward(D::Left());
        EXPECT_EQ(s.layout, Layout::Horizontal());
        EXPECT_EQ(s.left,  2u);
        EXPECT_EQ(s.right, 1u);
    }
    {
        auto s = ShapeOfSplitToward(D::Down());
        EXPECT_EQ(s.layout, Layout::Vertical());
        EXPECT_EQ(s.left,  1u);   // the source stays on top
        EXPECT_EQ(s.right, 2u);   // the new pane grows below
    }
    {
        auto s = ShapeOfSplitToward(D::Up());
        EXPECT_EQ(s.layout, Layout::Vertical());
        EXPECT_EQ(s.left,  2u);
        EXPECT_EQ(s.right, 1u);
    }
}

TEST(TreeTest, DirectionAnswersItsLayout) {
    using D = Direction;
    // Left/Right split side by side; Up/Down stack.
    EXPECT_EQ(D::Right().Layout(), Layout::Horizontal());
    EXPECT_EQ(D::Left().Layout(),  Layout::Horizontal());
    EXPECT_EQ(D::Down().Layout(),  Layout::Vertical());
    EXPECT_EQ(D::Up().Layout(),    Layout::Vertical());
}

// ----- RESIZE_SPLIT -----

namespace {

// A lone split whose arranged extent the test controls directly.
Tree SplitWithExtent(Layout layout, float extent) {
    Tree t{ MakeSplitBranch(layout, 0.5, Leaf(1), Leaf(2)) };
    t.Root()->arrangedRect = layout == Layout::Horizontal()
        ? winrt::Windows::Foundation::Rect{ 0, 0, extent, 100 }
        : winrt::Windows::Foundation::Rect{ 0, 0, 100, extent };
    return t;
}

double RatioOf(Tree& t) {
    return t.Root()->TryGet<Split>()->ratio;
}

}  // namespace

TEST(TreeTest, ResizeSplitMovesTheBoundaryTheArrowsWay) {
    // 1000px wide split, 0px divider: 100px is a tenth.
    {
        auto t = SplitWithExtent(Layout::Horizontal(), 1000);
        EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_RIGHT, 100), 0));
        EXPECT_DOUBLE_EQ(RatioOf(t), 0.6);
    }
    {
        auto t = SplitWithExtent(Layout::Horizontal(), 1000);
        EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_LEFT, 100), 0));
        EXPECT_DOUBLE_EQ(RatioOf(t), 0.4);
    }
    {
        auto t = SplitWithExtent(Layout::Vertical(), 1000);
        EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_DOWN, 100), 0));
        EXPECT_DOUBLE_EQ(RatioOf(t), 0.6);
    }
    {
        auto t = SplitWithExtent(Layout::Vertical(), 1000);
        EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_UP, 100), 0));
        EXPECT_DOUBLE_EQ(RatioOf(t), 0.4);
    }
}

TEST(TreeTest, ResizeSplitAccountsForTheDivider) {
    // The divider is not pane space: 100px of a 1001px split with a
    // 1px divider is a tenth of the usable 1000.
    auto t = SplitWithExtent(Layout::Horizontal(), 1001);
    EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_RIGHT, 100), 1));
    EXPECT_DOUBLE_EQ(RatioOf(t), 0.6);
}

TEST(TreeTest, ResizeSplitNeverLetsAChildVanish) {
    {
        auto t = SplitWithExtent(Layout::Horizontal(), 1000);
        t.Root()->TryGet<Split>()->ratio = 0.9;
        EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_RIGHT, 500), 0));
        EXPECT_DOUBLE_EQ(RatioOf(t), 0.95);
    }
    {
        auto t = SplitWithExtent(Layout::Horizontal(), 1000);
        t.Root()->TryGet<Split>()->ratio = 0.1;
        EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_LEFT, 500), 0));
        EXPECT_DOUBLE_EQ(RatioOf(t), 0.05);
    }
    {
        // A degenerate extent does not divide by zero.
        auto t = SplitWithExtent(Layout::Horizontal(), 0);
        EXPECT_TRUE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_RIGHT, 1), 1));
        EXPECT_DOUBLE_EQ(RatioOf(t), 0.95);
    }
}

TEST(TreeTest, ResizeSplitIsFalseWithoutASplitOnThatAxis) {
    // The tree splits horizontally only -- an UP/DOWN arrow crosses a
    // vertical split, and there is none.
    auto t = SplitWithExtent(Layout::Horizontal(), 1000);
    EXPECT_FALSE(t.ResizeSplit(*ById(t, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_UP, 100), 0));
    Tree lone{ Leaf(1) };
    EXPECT_FALSE(lone.ResizeSplit(*ById(lone, 1), ResizeBy(GHOSTTY_RESIZE_SPLIT_RIGHT, 100), 0));
}

// ----- the resize request at the boundary -----

TEST(TreeTest, ResizeFromCarriesTheArrowInLayoutAndSign) {
    // LEFT / RIGHT cross a vertical boundary — a horizontal split —
    // and the sign is the way the boundary moves (+right / +down,
    // the first child grows).
    auto right = ResizeBy(GHOSTTY_RESIZE_SPLIT_RIGHT, 100);
    EXPECT_EQ(right.Layout(), Layout::Horizontal());
    EXPECT_EQ(right.SignedAmount(), 100);
    auto left = ResizeBy(GHOSTTY_RESIZE_SPLIT_LEFT, 100);
    EXPECT_EQ(left.Layout(), Layout::Horizontal());
    EXPECT_EQ(left.SignedAmount(), -100);
    auto down = ResizeBy(GHOSTTY_RESIZE_SPLIT_DOWN, 100);
    EXPECT_EQ(down.Layout(), Layout::Vertical());
    EXPECT_EQ(down.SignedAmount(), 100);
    auto up = ResizeBy(GHOSTTY_RESIZE_SPLIT_UP, 100);
    EXPECT_EQ(up.Layout(), Layout::Vertical());
    EXPECT_EQ(up.SignedAmount(), -100);
}

TEST(TreeTest, GotoFromMapsTheGhosttyTarget) {
    using G = Goto;
    EXPECT_EQ(G::From(GHOSTTY_GOTO_SPLIT_PREVIOUS), G::Previous());
    EXPECT_EQ(G::From(GHOSTTY_GOTO_SPLIT_NEXT),     G::Next());
    EXPECT_EQ(G::From(GHOSTTY_GOTO_SPLIT_LEFT),     G::Left());
    EXPECT_EQ(G::From(GHOSTTY_GOTO_SPLIT_RIGHT),    G::Right());
    EXPECT_EQ(G::From(GHOSTTY_GOTO_SPLIT_UP),       G::Up());
    EXPECT_EQ(G::From(GHOSTTY_GOTO_SPLIT_DOWN),     G::Down());
}
