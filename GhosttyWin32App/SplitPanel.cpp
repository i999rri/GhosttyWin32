#include "pch.h"
#include "SplitPanel.h"
#if __has_include("SplitPanel.g.cpp")
#include "SplitPanel.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation {

namespace {

// Walks `node` and accumulates the union of every leaf's desired size.
// Stacked dimensions add, perpendicular dimensions take the max — so a
// horizontal split's width is `first + second` and its height is
// `max(first, second)`. Mirrored for vertical splits.
//
// Called from MeasureOverride so the framework knows how much room
// SplitPanel wants. Available size constrains each leaf so the
// terminal surface receives a Measure with the right cap (avoids the
// leaf reporting a content size that ignores the host's available
// area).
Windows::Foundation::Size MeasureNode(Pane& node, Windows::Foundation::Size available) {
    if (node.IsLeaf()) {
        if (auto element = node.Content()) {
            element.Measure(available);
            return element.DesiredSize();
        }
        return {0, 0};
    }

    auto* first = node.First();
    auto* second = node.Second();
    if (!first && !second) return {0, 0};
    if (!first) return MeasureNode(*second, available);
    if (!second) return MeasureNode(*first, available);

    Windows::Foundation::Size firstAvail = available;
    Windows::Foundation::Size secondAvail = available;
    if (node.Orientation() == SplitOrientation::Horizontal) {
        firstAvail.Width = static_cast<float>(available.Width * node.Ratio());
        secondAvail.Width = static_cast<float>(available.Width * (1.0 - node.Ratio()));
    } else {
        firstAvail.Height = static_cast<float>(available.Height * node.Ratio());
        secondAvail.Height = static_cast<float>(available.Height * (1.0 - node.Ratio()));
    }

    auto a = MeasureNode(*first, firstAvail);
    auto b = MeasureNode(*second, secondAvail);

    if (node.Orientation() == SplitOrientation::Horizontal) {
        return { a.Width + b.Width, std::max(a.Height, b.Height) };
    }
    return { std::max(a.Width, b.Width), a.Height + b.Height };
}

}  // namespace

void SplitPanel::SetRoot(std::unique_ptr<Pane> root) {
    m_root = std::move(root);
    SyncChildrenFromTree();
    InvalidateMeasure();
    InvalidateArrange();
}

void SplitPanel::SyncChildrenFromTree() {
    Children().Clear();
    if (m_root) AppendLeavesToChildren(*m_root);
}

void SplitPanel::AppendLeavesToChildren(Pane& node) {
    if (node.IsLeaf()) {
        if (auto element = node.Content()) {
            Children().Append(element);
        }
        return;
    }
    if (auto* f = node.First()) AppendLeavesToChildren(*f);
    if (auto* s = node.Second()) AppendLeavesToChildren(*s);
}

Windows::Foundation::Size SplitPanel::MeasureOverride(Windows::Foundation::Size availableSize) {
    if (!m_root) return {0, 0};
    return MeasureNode(*m_root, availableSize);
}

Windows::Foundation::Size SplitPanel::ArrangeOverride(Windows::Foundation::Size finalSize) {
    if (m_root) {
        ArrangeNode(*m_root, Windows::Foundation::Rect{0, 0, finalSize.Width, finalSize.Height});
    }
    return finalSize;
}

void SplitPanel::ArrangeNode(Pane& node, Windows::Foundation::Rect rect) {
    if (node.IsLeaf()) {
        if (auto element = node.Content()) {
            element.Arrange(rect);
        }
        return;
    }

    auto* first = node.First();
    auto* second = node.Second();
    if (!first && !second) return;
    if (!first) { ArrangeNode(*second, rect); return; }
    if (!second) { ArrangeNode(*first, rect); return; }

    if (node.Orientation() == SplitOrientation::Horizontal) {
        float w = rect.Width * static_cast<float>(node.Ratio());
        Windows::Foundation::Rect firstRect{ rect.X, rect.Y, w, rect.Height };
        Windows::Foundation::Rect secondRect{ rect.X + w, rect.Y, rect.Width - w, rect.Height };
        ArrangeNode(*first, firstRect);
        ArrangeNode(*second, secondRect);
    } else {
        float h = rect.Height * static_cast<float>(node.Ratio());
        Windows::Foundation::Rect firstRect{ rect.X, rect.Y, rect.Width, h };
        Windows::Foundation::Rect secondRect{ rect.X, rect.Y + h, rect.Width, rect.Height - h };
        ArrangeNode(*first, firstRect);
        ArrangeNode(*second, secondRect);
    }
}

}  // namespace winrt::GhosttyWin32::implementation
