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
        auto panes = Panes();
        if (panes.size() <= 1) return nullptr;   // nowhere to go
        auto it = std::find(panes.begin(), panes.end(), &from);
        if (it == panes.end()) return nullptr;
        const size_t index = static_cast<size_t>(std::distance(panes.begin(), it));

        if (target.IsNext()) {
            return panes[(index + 1) % panes.size()];
        }
        if (target.IsPrevious()) {
            return panes[index == 0 ? panes.size() - 1 : index - 1];
        }

        const auto fromRect = TryFindBranch(from)->arrangedRect;
        Pane* best = nullptr;
        double bestScore = std::numeric_limits<double>::max();
        for (size_t i = 0; i < panes.size(); ++i) {
            if (i == index) continue;
            Branch* branch = TryFindBranch(*panes[i]);
            if (!branch) continue;
            const auto score = SpatialScore(target, fromRect, branch->arrangedRect);
            if (score && *score < bestScore) {
                bestScore = *score;
                best = panes[i];
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
        const float usable = std::max(1.0f, extent - splitterThickness);
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
    // Whether `candidate` lies wholly on the `target` side of
    // `from`, and if so its rank as a neighbour — nullopt means
    // "not on that side at all". A candidate qualifies when its
    // whole extent is past `from`'s edge, with 1px of slack to
    // absorb float rounding on a shared boundary. The rank is
    //   distance along the arrow + 2 × off-axis offset of the centres
    // and lower is better: the 2× penalty keeps focus moves
    // predictable when an off-axis pane is technically closer in
    // straight-line distance than the aligned neighbour.
    static std::optional<double> SpatialScore(
        Goto target,
        winrt::Windows::Foundation::Rect const& from,
        winrt::Windows::Foundation::Rect const& candidate) noexcept
    {
        const auto right   = [](winrt::Windows::Foundation::Rect const& r) { return r.X + r.Width; };
        const auto bottom  = [](winrt::Windows::Foundation::Rect const& r) { return r.Y + r.Height; };
        const auto centerX = [](winrt::Windows::Foundation::Rect const& r) { return r.X + r.Width * 0.5f; };
        const auto centerY = [](winrt::Windows::Foundation::Rect const& r) { return r.Y + r.Height * 0.5f; };

        double primary = 0.0, perpendicular = 0.0;
        if (target.IsLeft()) {
            if (right(candidate) > from.X + 1.0f) return std::nullopt;
            primary = from.X - right(candidate);
            perpendicular = std::abs(centerY(candidate) - centerY(from));
        } else if (target.IsRight()) {
            if (candidate.X < right(from) - 1.0f) return std::nullopt;
            primary = candidate.X - right(from);
            perpendicular = std::abs(centerY(candidate) - centerY(from));
        } else if (target.IsUp()) {
            if (bottom(candidate) > from.Y + 1.0f) return std::nullopt;
            primary = from.Y - bottom(candidate);
            perpendicular = std::abs(centerX(candidate) - centerX(from));
        } else {   // Down — GotoTarget routed Previous / Next already
            if (candidate.Y < bottom(from) - 1.0f) return std::nullopt;
            primary = candidate.Y - bottom(from);
            perpendicular = std::abs(centerX(candidate) - centerX(from));
        }
        return primary + 2.0 * perpendicular;
    }

    std::unique_ptr<Branch> m_root;
    // Non-owning pointer into m_root's subtree. Cleared whenever the
    // pointed-at pane could go away (SetRoot, ReplacePane, RemovePane).
    Pane const* m_zoomed{ nullptr };
};

}  // namespace core::panes
