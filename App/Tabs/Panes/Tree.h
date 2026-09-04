#pragma once

#include "Tabs/Panes/Branch.h"
#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace winrt::GhosttyWin32::implementation {

// Model layer for one tab's split arrangement. Pure C++ so the tree
// mutations and walks are unit-testable without WinUI. SplitPanel
// owns a Tree and syncs its Children collection after each mutation.
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

    // The nearest Split above `pane` that divides along `axis` — the
    // one RESIZE_SPLIT moves for an arrow across that axis. Null when
    // no ancestor does (a lone pane, or only splits the other way).
    Branch* NearestSplitAbove(Pane const& pane, Split::Direction axis) noexcept {
        Branch* node = TryFindBranch(pane);
        while (node && node->parent) {
            node = node->parent;
            if (auto* split = node->TryGet<Split>(); split && split->direction == axis) {
                return node;
            }
        }
        return nullptr;
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

    // Callers key UI reactions off which case fired: last-pane closes
    // the whole tab, split-collapse just relayouts, not-found is a
    // stale close event. Predicate methods so call sites read as
    // `result.IsCollapsed()` rather than `result == …::Collapsed`.
    class RemoveResult {
    public:
        static constexpr RemoveResult NotFound()    noexcept { return { Kind::NotFound }; }
        static constexpr RemoveResult Collapsed()   noexcept { return { Kind::Collapsed }; }
        static constexpr RemoveResult RemovedRoot() noexcept { return { Kind::RemovedRoot }; }

        constexpr bool IsNotFound()    const noexcept { return m_kind == Kind::NotFound; }
        constexpr bool IsCollapsed()   const noexcept { return m_kind == Kind::Collapsed; }
        constexpr bool IsRemovedRoot() const noexcept { return m_kind == Kind::RemovedRoot; }

    private:
        enum class Kind { NotFound, Collapsed, RemovedRoot };
        constexpr RemoveResult(Kind k) noexcept : m_kind(k) {}
        Kind m_kind;
    };

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
    std::unique_ptr<Branch> m_root;
    // Non-owning pointer into m_root's subtree. Cleared whenever the
    // pointed-at pane could go away (SetRoot, ReplacePane, RemovePane).
    Pane const* m_zoomed{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation
