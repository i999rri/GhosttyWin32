#include "pch.h"
#include "SplitPanel.h"
#if __has_include("SplitPanel.g.cpp")
#include "SplitPanel.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation {

void SplitPanel::SetRoot(std::unique_ptr<Pane> root) {
    m_root = std::move(root);
    SyncChildrenFromTree();
    InvalidateMeasure();
    InvalidateArrange();
}

bool SplitPanel::ReplaceLeaf(Pane* leaf, std::unique_ptr<Pane> newSubtree) {
    if (!leaf || !newSubtree || !m_root) return false;

    // Root replacement: defer to SetRoot so the same children-sync /
    // invalidate path runs.
    if (m_root.get() == leaf) {
        SetRoot(std::move(newSubtree));
        return true;
    }

    // Non-root: parent owns leaf via unique_ptr; rewrite that pointer.
    auto* parent = leaf->Parent();
    if (!parent) return false;
    if (!parent->ReplaceChild(leaf, std::move(newSubtree))) return false;

    SyncChildrenFromTree();
    InvalidateMeasure();
    InvalidateArrange();
    return true;
}

SplitPanel::RemovalResult SplitPanel::RemoveLeaf(Pane* leaf) {
    if (!leaf || !m_root) return RemovalResult::NotFound;

    if (m_root.get() == leaf) {
        // Root removal — tree becomes empty. Caller decides the
        // surrounding-tab action.
        SetRoot(nullptr);
        return RemovalResult::RemovedRoot;
    }

    auto* parent = leaf->Parent();
    if (!parent) return RemovalResult::NotFound;

    // Identify the surviving sibling (the parent's other child) and
    // detach it from the parent so its unique_ptr survives the parent
    // destruction triggered below.
    Pane* siblingRaw = (parent->First() == leaf) ? parent->Second()
                                                  : parent->First();
    if (!siblingRaw) return RemovalResult::NotFound;
    auto sibling = parent->DetachChild(siblingRaw);
    if (!sibling) return RemovalResult::NotFound;

    // Replace `parent` in its slot with the sibling subtree. The
    // parent's unique_ptr is overwritten, which destroys the parent
    // node and (transitively) the doomed leaf. The sibling subtree's
    // contents are unaffected because we already detached it.
    auto* grandparent = parent->Parent();
    if (!grandparent) {
        // parent was root.
        SetRoot(std::move(sibling));
    } else {
        if (!grandparent->ReplaceChild(parent, std::move(sibling))) {
            return RemovalResult::NotFound;  // shouldn't happen, but fail closed
        }
        SyncChildrenFromTree();
        InvalidateMeasure();
        InvalidateArrange();
    }
    return RemovalResult::Collapsed;
}

void SplitPanel::SetZoomed(Pane* leaf) {
    m_zoomedLeaf = leaf;
    UpdateChildVisibility();
    InvalidateMeasure();
    InvalidateArrange();
}

void SplitPanel::UpdateChildVisibility() {
    using namespace winrt::Microsoft::UI::Xaml;
    // No zoom in effect — every child stays visible. Walk Children()
    // directly so this also recovers visibility for elements that
    // were previously hidden by an earlier zoom.
    if (!m_zoomedLeaf) {
        for (auto&& child : Children()) {
            if (auto el = child.try_as<UIElement>()) {
                el.Visibility(Visibility::Visible);
            }
        }
        return;
    }
    // Zoom active — only the zoomed leaf's content stays visible.
    // Comparing UIElement projections by identity works because each
    // element appears at most once in Children().
    auto zoomElement = m_zoomedLeaf->Content();
    for (auto&& child : Children()) {
        if (auto el = child.try_as<UIElement>()) {
            el.Visibility(el == zoomElement ? Visibility::Visible : Visibility::Collapsed);
        }
    }
}

void SplitPanel::EqualizeAll() {
    // m_splitters already holds one entry per internal node, so reuse
    // it instead of re-walking the tree. The vector is rebuilt on
    // every SyncChildrenFromTree, so it's always in sync with the
    // current tree shape.
    if (m_splitters.empty()) return;
    for (auto const& entry : m_splitters) {
        if (entry.node) entry.node->SetRatio(0.5);
    }
    InvalidateMeasure();
    InvalidateArrange();
}

void SplitPanel::SyncChildrenFromTree() {
    Children().Clear();
    m_splitters.clear();
    m_draggingNode = nullptr;
    // Any tree shape change invalidates a previously-stored zoom
    // pointer (the leaf may have moved, been wrapped in a split, or
    // gone away entirely). Clearing here is safer than auditing every
    // call site for whether the zoomed leaf survived.
    m_zoomedLeaf = nullptr;
    if (m_root) AppendNodeToChildren(*m_root);
}

void SplitPanel::AppendNodeToChildren(Pane& node) {
    if (node.IsLeaf()) {
        if (auto element = node.Content()) {
            Children().Append(element);
        }
        return;
    }
    // Walk first → splitter → second. The visual order matches the
    // layout order, and putting the splitter between the children in
    // the Children() collection means it gets painted on top of the
    // junction so the dragable strip is always reachable for input.
    if (auto* f = node.First()) AppendNodeToChildren(*f);
    auto splitter = MakeSplitter(&node);
    Children().Append(splitter);
    m_splitters.push_back({ splitter, &node });
    if (auto* s = node.Second()) AppendNodeToChildren(*s);
}

Microsoft::UI::Xaml::Controls::Border SplitPanel::MakeSplitter(Pane* node) {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::UI::Xaml::Input;
    using namespace winrt::Microsoft::UI::Xaml::Media;

    Border border{};
    // Semi-transparent gray, visible on both light- and dark-themed
    // terminal backgrounds without dominating. Refined theming can
    // come later; the priority here is "the user can see it and grab
    // it" rather than "it matches the palette".
    border.Background(SolidColorBrush(winrt::Windows::UI::Color{ 96, 128, 128, 128 }));

    // No resize cursor for now — ProtectedCursor is protected on
    // UIElement and Border is sealed, so we can't set the per-element
    // cursor from outside without subclassing. A follow-up can swap
    // this Border for a custom UserControl-based splitter that
    // exposes the cursor setup; until then the strip is visible
    // enough to grab without the cursor hint.

    // Wire pointer events. `node` is captured by raw pointer; this is
    // safe because Borders are recreated on every SyncChildrenFromTree,
    // so a stale `node` would only exist on a stale Border that's
    // already been removed from Children() (and whose events therefore
    // can't fire).
    //
    // `this` is also captured raw — the SplitPanel owns the Border via
    // Children(), so the impl lives at least as long as any event the
    // Border can fire.
    border.PointerPressed([this, node](winrt::Windows::Foundation::IInspectable const& sender,
                                       PointerRoutedEventArgs const& args) {
        if (auto el = sender.try_as<UIElement>()) {
            OnSplitterPointerPressed(el, node, args);
        }
    });
    border.PointerMoved([this, node](winrt::Windows::Foundation::IInspectable const&,
                                     PointerRoutedEventArgs const& args) {
        OnSplitterPointerMoved(node, args);
    });
    border.PointerReleased([this](winrt::Windows::Foundation::IInspectable const& sender,
                                  PointerRoutedEventArgs const& args) {
        if (auto el = sender.try_as<UIElement>()) {
            OnSplitterPointerReleased(el, args);
        }
    });
    border.PointerCaptureLost([this](winrt::Windows::Foundation::IInspectable const& sender,
                                     PointerRoutedEventArgs const& args) {
        if (auto el = sender.try_as<UIElement>()) {
            OnSplitterPointerReleased(el, args);
        }
    });

    return border;
}

Microsoft::UI::Xaml::Controls::Border SplitPanel::SplitterForNode(Pane const* node) const {
    for (auto const& entry : m_splitters) {
        if (entry.node == node) return entry.element;
    }
    return nullptr;
}

Windows::Foundation::Size SplitPanel::MeasureOverride(Windows::Foundation::Size availableSize) {
    if (!m_root) return { 0, 0 };
    // Zoom path: only the zoomed leaf participates in layout. The
    // others are Visibility=Collapsed so Panel's base class skips
    // them entirely — we don't need to Measure them.
    if (m_zoomedLeaf && m_zoomedLeaf->IsLeaf()) {
        if (auto element = m_zoomedLeaf->Content()) {
            element.Measure(availableSize);
            return element.DesiredSize();
        }
        return { 0, 0 };
    }
    auto result = MeasureNode(*m_root, availableSize);
    // Every Splitter must also be measured before Arrange — XAML's
    // contract is "every child gets Measure before Arrange or the
    // framework panics". They don't influence the panel's desired
    // size, just have to be visited.
    for (auto const& entry : m_splitters) {
        if (entry.element) {
            entry.element.Measure({ static_cast<float>(kSplitterThickness),
                                    static_cast<float>(kSplitterThickness) });
        }
    }
    return result;
}

Windows::Foundation::Size SplitPanel::MeasureNode(Pane& node, Windows::Foundation::Size available) {
    if (node.IsLeaf()) {
        if (auto element = node.Content()) {
            element.Measure(available);
            return element.DesiredSize();
        }
        return { 0, 0 };
    }

    auto* first = node.First();
    auto* second = node.Second();
    if (!first && !second) return { 0, 0 };
    if (!first)  return MeasureNode(*second, available);
    if (!second) return MeasureNode(*first, available);

    // Reserve room for the splitter strip on the split axis so the
    // children don't ask for space the splitter will end up taking.
    auto firstAvail  = available;
    auto secondAvail = available;
    float thickness  = static_cast<float>(kSplitterThickness);
    if (node.Orientation() == SplitOrientation::Horizontal) {
        float useable = std::max(0.0f, available.Width - thickness);
        firstAvail.Width  = static_cast<float>(useable * node.Ratio());
        secondAvail.Width = static_cast<float>(useable * (1.0 - node.Ratio()));
    } else {
        float useable = std::max(0.0f, available.Height - thickness);
        firstAvail.Height  = static_cast<float>(useable * node.Ratio());
        secondAvail.Height = static_cast<float>(useable * (1.0 - node.Ratio()));
    }

    auto a = MeasureNode(*first, firstAvail);
    auto b = MeasureNode(*second, secondAvail);

    if (node.Orientation() == SplitOrientation::Horizontal) {
        return { a.Width + b.Width + thickness, std::max(a.Height, b.Height) };
    }
    return { std::max(a.Width, b.Width), a.Height + b.Height + thickness };
}

Windows::Foundation::Size SplitPanel::ArrangeOverride(Windows::Foundation::Size finalSize) {
    if (!m_root) return finalSize;
    Windows::Foundation::Rect fullRect{ 0, 0, finalSize.Width, finalSize.Height };
    // Zoom: only the zoomed leaf is arranged. Others are Collapsed so
    // their ActualSize / SizeChanged don't fire; the SwapChainPanel
    // they own keeps whatever swap chain it had bound and resumes
    // when unzoomed.
    if (m_zoomedLeaf && m_zoomedLeaf->IsLeaf()) {
        m_zoomedLeaf->SetArrangedRect(fullRect);
        if (auto element = m_zoomedLeaf->Content()) {
            element.Arrange(fullRect);
        }
        return finalSize;
    }
    ArrangeNode(*m_root, fullRect);
    return finalSize;
}

void SplitPanel::ArrangeNode(Pane& node, Windows::Foundation::Rect rect) {
    // Cache the arranged rect on the node so drag-resize and
    // direction-based pane navigation can recover it without walking
    // the tree from the root each time.
    node.SetArrangedRect(rect);

    if (node.IsLeaf()) {
        if (auto element = node.Content()) {
            element.Arrange(rect);
        }
        return;
    }

    auto* first = node.First();
    auto* second = node.Second();
    if (!first && !second) return;
    if (!first)  { ArrangeNode(*second, rect); return; }
    if (!second) { ArrangeNode(*first, rect); return; }

    float thickness = static_cast<float>(kSplitterThickness);

    if (node.Orientation() == SplitOrientation::Horizontal) {
        float useable = std::max(0.0f, rect.Width - thickness);
        float firstW  = useable * static_cast<float>(node.Ratio());
        float secondW = useable - firstW;
        Windows::Foundation::Rect firstRect{ rect.X, rect.Y, firstW, rect.Height };
        Windows::Foundation::Rect splitRect{ rect.X + firstW, rect.Y, thickness, rect.Height };
        Windows::Foundation::Rect secondRect{ rect.X + firstW + thickness, rect.Y, secondW, rect.Height };
        ArrangeNode(*first, firstRect);
        if (auto sp = SplitterForNode(&node)) sp.Arrange(splitRect);
        ArrangeNode(*second, secondRect);
    } else {
        float useable = std::max(0.0f, rect.Height - thickness);
        float firstH  = useable * static_cast<float>(node.Ratio());
        float secondH = useable - firstH;
        Windows::Foundation::Rect firstRect{ rect.X, rect.Y, rect.Width, firstH };
        Windows::Foundation::Rect splitRect{ rect.X, rect.Y + firstH, rect.Width, thickness };
        Windows::Foundation::Rect secondRect{ rect.X, rect.Y + firstH + thickness, rect.Width, secondH };
        ArrangeNode(*first, firstRect);
        if (auto sp = SplitterForNode(&node)) sp.Arrange(splitRect);
        ArrangeNode(*second, secondRect);
    }
}

void SplitPanel::OnSplitterPointerPressed(Microsoft::UI::Xaml::UIElement const& splitter,
                                          Pane* node,
                                          Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
{
    if (!node) return;
    if (!splitter.CapturePointer(args.Pointer())) return;
    m_draggingNode = node;
    args.Handled(true);
}

void SplitPanel::OnSplitterPointerMoved(Pane* node,
                                        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
{
    // Only consume moves that belong to the active drag — without
    // this check, a stray PointerMoved on a non-pressed splitter
    // (e.g. hover) would mutate the ratio.
    if (!m_draggingNode || m_draggingNode != node) return;

    // Position in SplitPanel local coordinates so it can be compared
    // against the parent split's arranged rect (also expressed in
    // SplitPanel coordinates).
    auto point = args.GetCurrentPoint(*this).Position();

    // The parent split occupies the same rect ArrangeNode last
    // assigned to it, cached on the Pane itself.
    auto rect = node->ArrangedRect();
    double newRatio = node->Ratio();
    float thickness = static_cast<float>(kSplitterThickness);

    if (node->Orientation() == SplitOrientation::Horizontal) {
        float useable = std::max(1.0f, rect.Width - thickness);
        newRatio = (point.X - rect.X) / useable;
    } else {
        float useable = std::max(1.0f, rect.Height - thickness);
        newRatio = (point.Y - rect.Y) / useable;
    }
    node->SetRatio(newRatio);  // clamped to [0.05, 0.95] inside Pane
    InvalidateMeasure();
    InvalidateArrange();
    args.Handled(true);
}

void SplitPanel::OnSplitterPointerReleased(Microsoft::UI::Xaml::UIElement const& splitter,
                                           Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
{
    splitter.ReleasePointerCapture(args.Pointer());
    m_draggingNode = nullptr;
    args.Handled(true);
}

}  // namespace winrt::GhosttyWin32::implementation
