#pragma once

#include <Panes/Branch.h>
#include <Panes/Goto.h>
#include <optional>
#include <Panes/RemoveResult.h>
#include <Panes/Resize.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace core::panes {

// Model layer for one tab's split arrangement. Pure C++ — a pane
// carries its control as an IInspectable handle plus an IPaneView,
// never the App's concrete type — so the tree mutations and walks
// are unit-testable without WinUI (test_tree.cpp). SplitPanel owns a
// Tree and syncs its Children collection after each mutation.
class Tree {
public:
    Tree() = default;
    explicit Tree(std::unique_ptr<Branch> root) noexcept
        : m_root(std::move(root)) {}

    Tree(Tree const&) = delete;
    Tree& operator=(Tree const&) = delete;
    Tree(Tree&&) = default;
    Tree& operator=(Tree&&) = default;

    bool HasRoot() const noexcept { return static_cast<bool>(m_root); }
    Branch*       Root()       noexcept { return m_root.get(); }
    Branch const* Root() const noexcept { return m_root.get(); }

    void SetRoot(std::unique_ptr<Branch> root) noexcept {
        if (root) root->parent = nullptr;
        m_root = std::move(root);
        // The old zoomed pointer targeted a node we just dropped.
        m_zoomed = nullptr;
    }

    bool AnyPaneMatches(std::function<bool(Pane const&)> const& pred) const {
        return HasRoot() && m_root->AnyPaneMatches(pred);
    }
    void ForEachPane(std::function<void(Pane&)> const& visitor) {
        if (HasRoot()) m_root->ForEachPane(visitor);
    }
    // Const overloads go through Root() so the receiver is Branch
    // const* — otherwise MSVC sees both Branch overloads as viable
    // (mutable receiver + argument that matches either) and flags
    // C2666. The mutable overloads work off m_root directly.
    void ForEachPane(std::function<void(Pane const&)> const& visitor) const {
        if (auto const* root = Root()) root->ForEachPane(visitor);
    }
    Pane* FindPaneBy(std::function<bool(Pane const&)> const& pred) {
        return HasRoot() ? m_root->FindPaneBy(pred) : nullptr;
    }
    Pane const* FindPaneBy(std::function<bool(Pane const&)> const& pred) const {
        auto const* root = Root();
        return root ? root->FindPaneBy(pred) : nullptr;
    }

    // Both structural mutations start by locating the wrapping Branch
    // or bailing — collapsed here so the two failure modes (empty
    // tree, pane not present) share one nullable return.
    Branch* TryFindBranch(Pane const& pane) noexcept {
        return HasRoot() ? m_root->FindBranchOfPane(pane) : nullptr;
    }

    // Every pane in depth-first order — the order GOTO_SPLIT
    // PREVIOUS / NEXT cycles through.
    std::vector<Pane*> Panes() {
        std::vector<Pane*> out;
        ForEachPane([&out](Pane& p) { out.push_back(&p); });
        return out;
    }

    // Every pane's wrapping Branch, in the same depth-first order —
    // the pane and the arranged rect the layout pass gave it,
    // together. The spatial rules read both, so walking this list
    // needs no per-pane TryFindBranch re-search.
    std::vector<Branch*> PaneBranches() {
        std::vector<Branch*> out;
        auto walk = [&out](auto&& self, Branch& branch) -> void {
            if (branch.Is<Pane>()) {
                out.push_back(&branch);
                return;
            }
            if (auto* split = branch.TryGet<Split>()) {
                split->ForEachChild([&](Branch& child) { self(self, child); });
            }
        };
        if (HasRoot()) walk(walk, *m_root);
        return out;
    }

    // The nearest Split above `pane` with the given layout — the one
    // RESIZE_SPLIT moves for an arrow across that axis. Null when no
    // ancestor has it (a lone pane, or only splits the other way).
    Branch* NearestSplitAbove(Pane const& pane, Layout layout) noexcept {
        Branch* node = TryFindBranch(pane);
        while (node && node->parent) {
            node = node->parent;
            if (auto* split = node->TryGet<Split>(); split && split->layout == layout) {
                return node;
            }
        }
        return nullptr;
    }

    // GOTO_SPLIT: the pane `target` points at from `from` — the
    // best-scoring pane on that side for a spatial arrow (the rule
    // is SpatialScore below), the depth-first neighbour (wrapping
    // at either end) for Previous / Next — or null when there is
    // none (a lone pane, or nothing on that side).
    Pane* GotoTarget(Pane const& from, Goto target) {
        auto branches = PaneBranches();
        if (branches.size() <= 1) return nullptr;   // nowhere to go
        auto it = std::find_if(branches.begin(), branches.end(),
            [&from](Branch* b) { return b->TryGet<Pane>() == &from; });
        if (it == branches.end()) return nullptr;
        const size_t index = static_cast<size_t>(std::distance(branches.begin(), it));

        if (target.IsNext()) {
            return branches[(index + 1) % branches.size()]->TryGet<Pane>();
        }
        if (target.IsPrevious()) {
            return branches[index == 0 ? branches.size() - 1 : index - 1]->TryGet<Pane>();
        }

        const auto fromRect = branches[index]->arrangedRect;
        Pane* best = nullptr;
        double bestScore = std::numeric_limits<double>::max();
        for (size_t i = 0; i < branches.size(); ++i) {
            if (i == index) continue;
            const auto score = SpatialScore(target, fromRect, branches[i]->arrangedRect);
            if (score && *score < bestScore) {
                bestScore = *score;
                best = branches[i]->TryGet<Pane>();
            }
        }
        return best;
    }

    // RESIZE_SPLIT: move the boundary of the nearest split with the
    // request's layout by its signed amount, regardless of which
    // side of the split `pane` is on. The ratio is clamped so
    // neither child can vanish. `splitterThickness` is what the
    // divider takes of the split's arranged extent. Returns false
    // when no split with that layout exists (a lone pane, or only
    // splits the other way).
    bool ResizeSplit(Pane const& pane, Resize resize,
                     float splitterThickness) noexcept {
        Branch* node = NearestSplitAbove(pane, resize.Layout());
        if (!node) return false;
        auto* split = node->TryGet<Split>();
        if (!split) return false;

        const float extent = resize.Layout().IsHorizontal()
            ? node->arrangedRect.Width
            : node->arrangedRect.Height;
        const float usable = std::max(kMinUsableExtentPx, extent - splitterThickness);
        const double delta = static_cast<double>(resize.SignedAmount()) / usable;
        split->ratio = ClampSplitRatio(split->ratio + delta);
        return true;
    }

    // Preserves `target`'s identity by rewiring its wrapping Branch's
    // owning unique_ptr slot (in the parent Split, or m_root if it
    // was the root). Returns false only if the pane isn't in this
    // tree; a null newSubtree is a caller bug (asserted).
    bool ReplacePane(Pane const& target, std::unique_ptr<Branch> newSubtree) noexcept {
        // Null newSubtree would leave the tree in a shape we don't
        // model — RemovePane exists for the "make it empty here" case.
        // Assert so the caller bug is caught in debug instead of
        // silently returning false alongside the real not-found case.
        assert(newSubtree && "ReplacePane requires non-null newSubtree");
        Branch* wrapping = TryFindBranch(target);
        if (!wrapping) return false;

        if (wrapping == m_root.get()) {
            newSubtree->parent = nullptr;
            m_root = std::move(newSubtree);
            if (m_zoomed == &target) m_zoomed = nullptr;
            return true;
        }

        Branch* parent = wrapping->parent;
        if (!parent) return false;
        auto* parentSplit = parent->TryGet<Split>();
        if (!parentSplit) return false;

        newSubtree->parent = parent;
        if (parentSplit->left.get() == wrapping) {
            parentSplit->left = std::move(newSubtree);
        } else if (parentSplit->right.get() == wrapping) {
            parentSplit->right = std::move(newSubtree);
        } else {
            return false;
        }
        if (m_zoomed == &target) m_zoomed = nullptr;
        return true;
    }

    RemoveResult RemovePane(Pane const& target) noexcept {
        Branch* wrapping = TryFindBranch(target);
        if (!wrapping) return RemoveResult::NotFound();

        if (wrapping == m_root.get()) {
            m_root.reset();
            m_zoomed = nullptr;
            return RemoveResult::RemovedRoot();
        }

        Branch* parent = wrapping->parent;
        if (!parent) return RemoveResult::NotFound();
        auto* parentSplit = parent->TryGet<Split>();
        if (!parentSplit) return RemoveResult::NotFound();

        // Detach the sibling first so its unique_ptr survives when
        // the parent Split is overwritten below.
        std::unique_ptr<Branch> sibling;
        if (parentSplit->left.get() == wrapping) {
            sibling = std::move(parentSplit->right);
        } else if (parentSplit->right.get() == wrapping) {
            sibling = std::move(parentSplit->left);
        } else {
            return RemoveResult::NotFound();
        }
        if (!sibling) return RemoveResult::NotFound();

        // Promote the sibling into the grandparent's slot (or the
        // root if the collapsing Split was the root).
        Branch* grand = parent->parent;
        if (!grand) {
            sibling->parent = nullptr;
            m_root = std::move(sibling);
        } else {
            auto* grandSplit = grand->TryGet<Split>();
            if (!grandSplit) return RemoveResult::NotFound();
            sibling->parent = grand;
            if (grandSplit->left.get() == parent) {
                grandSplit->left = std::move(sibling);
            } else if (grandSplit->right.get() == parent) {
                grandSplit->right = std::move(sibling);
            } else {
                return RemoveResult::NotFound();
            }
        }
        if (m_zoomed == &target) m_zoomed = nullptr;
        return RemoveResult::Collapsed();
    }

    // toggle_split_zoom support: SplitPanel consults Zoomed() in its
    // arrange pass and gives the zoomed pane the whole area.
    Pane const* Zoomed() const noexcept { return m_zoomed; }
    void SetZoomed(Pane const* pane) noexcept { m_zoomed = pane; }
    void ClearZoomed() noexcept { m_zoomed = nullptr; }

private:
    // Panes sharing an edge can overlap by a rounding hair — the
    // layout pass writes arranged rects as floats — so "past the
    // boundary" allows this much slack.
    static constexpr float kSharedBoundarySlackPx = 1.0f;

    // The weight of the centres' off-axis offset in the spatial
    // score: heavy enough that the aligned pane beats a diagonal
    // one that is closer in straight-line distance.
    static constexpr double kOffAxisPenalty = 2.0;

    // A degenerate arranged extent must not divide the resize
    // amount by zero.
    static constexpr float kMinUsableExtentPx = 1.0f;

    // A rect projected onto `target`'s axis, oriented so the arrow
    // points toward +: `begin` / `end` bound the rect along the
    // arrow, `center` is its middle on the other axis. Negating the
    // coordinates for LEFT / UP is what lets one score formula
    // serve all four arrows.
    struct ArrowSpan {
        float begin;
        float end;
        float center;
    };

    static ArrowSpan ProjectOntoArrow(
        Goto target,
        winrt::Windows::Foundation::Rect const& r) noexcept
    {
        if (target.IsRight()) return { r.X, r.X + r.Width, r.Y + r.Height * 0.5f };
        if (target.IsLeft())  return { -(r.X + r.Width), -r.X, r.Y + r.Height * 0.5f };
        if (target.IsDown())  return { r.Y, r.Y + r.Height, r.X + r.Width * 0.5f };
        return /* Up */       { -(r.Y + r.Height), -r.Y, r.X + r.Width * 0.5f };
    }

    // Whether `candidate` lies wholly on the `target` side of
    // `from`, and if so its rank as a neighbour — nullopt means
    // "not on that side at all". The rank is
    //   gap along the arrow + kOffAxisPenalty × centre offset
    // and lower is better.
    static std::optional<double> SpatialScore(
        Goto target,
        winrt::Windows::Foundation::Rect const& from,
        winrt::Windows::Foundation::Rect const& candidate) noexcept
    {
        const ArrowSpan f = ProjectOntoArrow(target, from);
        const ArrowSpan c = ProjectOntoArrow(target, candidate);

        // Qualify: the candidate begins at or beyond from's end
        // along the arrow.
        if (c.begin < f.end - kSharedBoundarySlackPx) return std::nullopt;

        const double primary       = c.begin - f.end;
        const double perpendicular = std::abs(c.center - f.center);
        return primary + kOffAxisPenalty * perpendicular;
    }

    std::unique_ptr<Branch> m_root;
    // Non-owning pointer into m_root's subtree. Cleared whenever the
    // pointed-at pane could go away (SetRoot, ReplacePane, RemovePane).
    Pane const* m_zoomed{ nullptr };
};

}  // namespace core::panes
