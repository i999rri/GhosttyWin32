#include "pch.h"
#include "../Core/Panes/Tree.h"
#include <vector>

// The tree without a single WinUI type: a pane's handle stays empty
// and its view null — the structural rules (replace, remove with
// sibling promotion, zoom invalidation) never look at either, which
// is exactly what lets them run here. Panes are told apart by id.

using core::panes::Branch;
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
Tree Nested(Split::Direction outer = Split::Direction::Horizontal,
            Split::Direction inner = Split::Direction::Vertical) {
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
    auto t = Nested(Split::Direction::Horizontal, Split::Direction::Vertical);
    auto* paneC = ById(t, 3);
    ASSERT_NE(paneC, nullptr);

    // 3's immediate parent splits vertically; the root horizontally.
    auto* vertical = t.NearestSplitAbove(*paneC, Split::Direction::Vertical);
    ASSERT_NE(vertical, nullptr);
    EXPECT_EQ(vertical->TryGet<Split>()->direction, Split::Direction::Vertical);

    auto* horizontal = t.NearestSplitAbove(*paneC, Split::Direction::Horizontal);
    ASSERT_NE(horizontal, nullptr);
    EXPECT_EQ(horizontal, t.Root());

    // A lone pane has no split above it at all.
    Tree lone{ Leaf(1) };
    auto* only = ById(lone, 1);
    EXPECT_EQ(lone.NearestSplitAbove(*only, Split::Direction::Horizontal), nullptr);
}

// ----- ReplacePane (the NEW_SPLIT mutation) -----

TEST(TreeTest, ReplacePaneSwapsARootLeafForASubtree) {
    Tree t{ Leaf(1) };
    auto* pane = ById(t, 1);
    ASSERT_NE(pane, nullptr);

    auto sub = MakeSplitBranch(Split::Direction::Horizontal, 0.5, Leaf(1), Leaf(2));
    ASSERT_TRUE(t.ReplacePane(*pane, std::move(sub)));
    EXPECT_EQ(Ids(t), (std::vector<uint64_t>{ 1, 2 }));
    EXPECT_EQ(t.Root()->TryGet<Split>()->left->parent, t.Root());
}

TEST(TreeTest, ReplacePaneRewiresTheParentSlot) {
    auto t = Nested();
    auto* paneB = ById(t, 2);
    ASSERT_NE(paneB, nullptr);

    auto sub = MakeSplitBranch(Split::Direction::Horizontal, 0.5, Leaf(2), Leaf(4));
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
    Tree t{ MakeSplitBranch(Split::Direction::Horizontal, 0.5, Leaf(1), Leaf(2)) };
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
