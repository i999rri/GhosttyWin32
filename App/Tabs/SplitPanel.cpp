#include "pch.h"
#include "Tabs/SplitPanel.h"
#include <algorithm>
#if __has_include("SplitPanel.g.cpp")
#include "SplitPanel.g.cpp"

namespace {

// The pane's control as the UIElement it is, for the layout side.
// The handle is an IInspectable so the tree never names XAML types;
// this panel is where it becomes an element again. Null-safe: an
// empty pane yields an empty element, exactly like the old direct
// control read.
winrt::Microsoft::UI::Xaml::UIElement ElementOf(
    winrt::GhosttyWin32::implementation::Pane const& p)
{
    return p.handle
        ? p.handle.try_as<winrt::Microsoft::UI::Xaml::UIElement>()
        : nullptr;
}

}  // namespace
#endif

namespace winrt::GhosttyWin32::implementation {

void SplitPanel::SetRoot(std::unique_ptr<Branch> root) {
    m_tree.SetRoot(std::move(root));
    SyncChildrenFromTree();
    InvalidateMeasure();
    InvalidateArrange();
}

bool SplitPanel::ReplacePane(Pane const& pane, std::unique_ptr<Branch> newSubtree) {
    if (!m_tree.ReplacePane(pane, std::move(newSubtree))) return false;
    SyncChildrenFromTree();
    InvalidateMeasure();
    InvalidateArrange();
    return true;
}

Tree::RemoveResult SplitPanel::RemovePane(Pane const& pane) {
    auto result = m_tree.RemovePane(pane);
    if (!result.IsNotFound()) {
        SyncChildrenFromTree();
        InvalidateMeasure();
        InvalidateArrange();
    }
    return result;
}

void SplitPanel::SetZoomed(Pane const* pane) {
    m_tree.SetZoomed(pane);
    UpdateChildVisibility();
    InvalidateMeasure();
    InvalidateArrange();
}

void SplitPanel::UpdateChildVisibility() {
    using namespace winrt::Microsoft::UI::Xaml;
    auto const* zoomed = m_tree.Zoomed();
    // No zoom — every child stays visible. Walk Children() directly so
    // this also recovers visibility for elements previously hidden by
    // an earlier zoom.
    if (!zoomed) {
        for (auto&& child : Children()) {
            if (auto el = child.try_as<UIElement>()) {
                el.Visibility(Visibility::Visible);
            }
        }
        return;
    }
    // Zoom active — only the zoomed pane's content stays visible.
    UIElement zoomElement = ElementOf(*zoomed);
    for (auto&& child : Children()) {
        if (auto el = child.try_as<UIElement>()) {
            el.Visibility(el == zoomElement ? Visibility::Visible : Visibility::Collapsed);
        }
    }
}

void SplitPanel::SetDividerColor(winrt::Windows::UI::Color color) noexcept {
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::UI::Xaml::Media;
    m_dividerBrush = SolidColorBrush(color);
    for (auto const& entry : m_splitters) {
        if (auto border = entry.element.try_as<Border>()) {
            border.Background(m_dividerBrush);
        }
    }
}

void SplitPanel::EqualizeAll() {
    // m_splitters already holds one entry per Split node — reuse
    // instead of re-walking the tree.
    if (m_splitters.empty()) return;
    for (auto const& entry : m_splitters) {
        if (entry.branch) {
            if (auto* split = entry.branch->TryGet<Split>()) {
                split->ratio = 0.5;
            }
        }
    }
    InvalidateMeasure();
    InvalidateArrange();
}

namespace {

namespace splits = core::ghostty::actions::splits;

Split::Direction ToTree(splits::Axis axis) noexcept {
    return axis == splits::Axis::Horizontal
        ? Split::Direction::Horizontal
        : Split::Direction::Vertical;
}

splits::Rect ToSplits(winrt::Windows::Foundation::Rect const& r) noexcept {
    return { r.X, r.Y, r.Width, r.Height };
}

}  // namespace

Pane* SplitPanel::SplitPane(Pane const& source,
                            splits::Placement placement,
                            std::unique_ptr<Branch> fresh) {
    if (!fresh) return nullptr;
    // The pane inside `fresh` keeps its address when the branch is
    // moved into the subtree (unique_ptr hands over the same object),
    // so this stays valid past ReplacePane.
    Pane* created = fresh->TryGet<Pane>();
    // Copy out before ReplacePane destroys the Branch wrapping
    // `source`: the new wrapper needs its own reference to the
    // control and the same PaneId so close_surface_cb still routes.
    auto sourceWrapper = MakePaneBranch(source);
    auto subtree = placement.newFirst
        ? MakeSplitBranch(ToTree(placement.axis), 0.5, std::move(fresh), std::move(sourceWrapper))
        : MakeSplitBranch(ToTree(placement.axis), 0.5, std::move(sourceWrapper), std::move(fresh));
    if (!ReplacePane(source, std::move(subtree))) return nullptr;
    return created;
}

Pane* SplitPanel::PaneToward(Pane const& from, ghostty_action_goto_split_e direction) {
    auto panes = m_tree.Panes();
    if (panes.size() <= 1) return nullptr;   // nothing to navigate to
    auto it = std::find(panes.begin(), panes.end(), &from);
    if (it == panes.end()) return nullptr;
    const size_t index = static_cast<size_t>(std::distance(panes.begin(), it));

    if (direction == GHOSTTY_GOTO_SPLIT_PREVIOUS || direction == GHOSTTY_GOTO_SPLIT_NEXT) {
        return panes[splits::CyclePane(index, panes.size(), direction)];
    }
    // The spatial rule works on the arranged rects, which live on the
    // wrapping Branch rather than the Pane.
    std::vector<splits::Rect> rects;
    rects.reserve(panes.size());
    for (auto* pane : panes) {
        auto* branch = m_tree.TryFindBranch(*pane);
        rects.push_back(branch ? ToSplits(branch->arrangedRect) : splits::Rect{});
    }
    auto target = splits::AdjacentPane(rects, index, direction);
    return target ? panes[*target] : nullptr;
}

bool SplitPanel::ResizeSplit(Pane const& pane, ghostty_action_resize_split_s resize) {
    const auto axis = splits::ResizeAxis(resize.direction);
    Branch* node = m_tree.NearestSplitAbove(pane, ToTree(axis));
    if (!node) return false;
    auto* split = node->TryGet<Split>();
    if (!split) return false;
    const auto rect = node->arrangedRect;
    const float extent = (axis == splits::Axis::Horizontal) ? rect.Width : rect.Height;
    split->ratio = splits::ResizedRatio(split->ratio, resize, extent,
                                        static_cast<float>(kSplitterThickness));
    InvalidateMeasure();
    InvalidateArrange();
    return true;
}

bool SplitPanel::ToggleZoom(Pane const& pane) {
    if (m_tree.Zoomed()) {
        SetZoomed(nullptr);
        return false;
    }
    auto* root = m_tree.Root();
    if (root && root->TryGet<Pane>() == &pane) return false;   // lone pane
    SetZoomed(&pane);
    return true;
}

void SplitPanel::SyncChildrenFromTree() {
    Children().Clear();
    m_splitters.clear();
    m_draggingBranch = nullptr;
    // Any tree shape change invalidates a stored zoom pointer.
    m_tree.ClearZoomed();
    if (auto* root = m_tree.Root()) AppendBranchToChildren(*root);
    // Re-evaluate Visibility after rebuilding Children (previous zoom
    // could have left Visibility=Collapsed on now-unrelated elements).
    UpdateChildVisibility();
}

void SplitPanel::AppendBranchToChildren(Branch& branch) {
    if (auto* pane = branch.TryGet<Pane>()) {
        if (auto element = ElementOf(*pane)) {
            Children().Append(element);
        }
        return;
    }
    auto* split = branch.TryGet<Split>();
    if (!split) return;
    // Walk left → splitter → right. Placing the splitter between the
    // children in Children() means it paints on top of the junction so
    // the drag strip is always reachable for input.
    if (split->left)  AppendBranchToChildren(*split->left);
    auto splitter = MakeSplitter(&branch);
    Children().Append(splitter);
    m_splitters.push_back({ splitter, &branch });
    if (split->right) AppendBranchToChildren(*split->right);
}

Microsoft::UI::Xaml::Controls::Border SplitPanel::MakeSplitter(Branch* splitBranch) {
    using namespace winrt::Microsoft::UI::Xaml;
    using namespace winrt::Microsoft::UI::Xaml::Controls;
    using namespace winrt::Microsoft::UI::Xaml::Input;
    using namespace winrt::Microsoft::UI::Xaml::Media;

    Border border{};
    if (m_dividerBrush) {
        border.Background(m_dividerBrush);
    } else {
        border.Background(SolidColorBrush(winrt::Windows::UI::Color{ 96, 128, 128, 128 }));
    }

    border.PointerPressed([this, splitBranch](winrt::Windows::Foundation::IInspectable const& sender,
                                              PointerRoutedEventArgs const& args) {
        if (auto el = sender.try_as<UIElement>()) {
            OnSplitterPointerPressed(el, splitBranch, args);
        }
    });
    border.PointerMoved([this, splitBranch](winrt::Windows::Foundation::IInspectable const&,
                                            PointerRoutedEventArgs const& args) {
        OnSplitterPointerMoved(splitBranch, args);
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

Microsoft::UI::Xaml::Controls::Border SplitPanel::SplitterForBranch(Branch const* splitBranch) const {
    for (auto const& entry : m_splitters) {
        if (entry.branch == splitBranch) return entry.element;
    }
    return nullptr;
}

Windows::Foundation::Size SplitPanel::MeasureOverride(Windows::Foundation::Size availableSize) {
    auto* root = m_tree.Root();
    if (!root) return { 0, 0 };
    // Zoom path: only the zoomed pane participates in layout. Others
    // are Visibility=Collapsed so Panel's base class skips them.
    if (auto const* zoomed = m_tree.Zoomed()) {
        if (auto element = ElementOf(*zoomed)) {
            element.Measure(availableSize);
            return element.DesiredSize();
        }
        return { 0, 0 };
    }
    auto result = MeasureBranch(*root, availableSize);
    // Every splitter must be measured before Arrange — XAML's contract
    // is "every child gets Measure or the framework panics".
    for (auto const& entry : m_splitters) {
        if (entry.element) {
            entry.element.Measure({ static_cast<float>(kSplitterThickness),
                                    static_cast<float>(kSplitterThickness) });
        }
    }
    return result;
}

Windows::Foundation::Size SplitPanel::MeasureBranch(Branch& branch, Windows::Foundation::Size available) {
    if (auto* pane = branch.TryGet<Pane>()) {
        if (auto element = ElementOf(*pane)) {
            element.Measure(available);
            return element.DesiredSize();
        }
        return { 0, 0 };
    }

    auto* split = branch.TryGet<Split>();
    if (!split) return { 0, 0 };
    auto* first  = split->left.get();
    auto* second = split->right.get();
    if (!first && !second) return { 0, 0 };
    if (!first)  return MeasureBranch(*second, available);
    if (!second) return MeasureBranch(*first, available);

    auto firstAvail  = available;
    auto secondAvail = available;
    float thickness  = static_cast<float>(kSplitterThickness);
    if (split->IsHorizontal()) {
        float useable = std::max(0.0f, available.Width - thickness);
        firstAvail.Width  = static_cast<float>(useable * split->ratio);
        secondAvail.Width = static_cast<float>(useable * (1.0 - split->ratio));
    } else {
        float useable = std::max(0.0f, available.Height - thickness);
        firstAvail.Height  = static_cast<float>(useable * split->ratio);
        secondAvail.Height = static_cast<float>(useable * (1.0 - split->ratio));
    }

    auto a = MeasureBranch(*first, firstAvail);
    auto b = MeasureBranch(*second, secondAvail);

    if (split->IsHorizontal()) {
        return { a.Width + b.Width + thickness, std::max(a.Height, b.Height) };
    }
    return { std::max(a.Width, b.Width), a.Height + b.Height + thickness };
}

Windows::Foundation::Size SplitPanel::ArrangeOverride(Windows::Foundation::Size finalSize) {
    auto* root = m_tree.Root();
    if (!root) return finalSize;
    Windows::Foundation::Rect fullRect{ 0, 0, finalSize.Width, finalSize.Height };
    if (auto const* zoomed = m_tree.Zoomed()) {
        // Zoom: only the zoomed pane is arranged. Cache the rect on
        // its wrapping Branch so downstream code (drag-resize,
        // GOTO_SPLIT direction navigation) can still recover it.
        if (auto* zoomedBranch = root->FindBranchOfPane(*zoomed)) {
            zoomedBranch->arrangedRect = fullRect;
        }
        if (auto element = ElementOf(*zoomed)) {
            element.Arrange(fullRect);
        }
        return finalSize;
    }
    ArrangeBranch(*root, fullRect);
    return finalSize;
}

void SplitPanel::ArrangeBranch(Branch& branch, Windows::Foundation::Rect rect) {
    branch.arrangedRect = rect;

    if (auto* pane = branch.TryGet<Pane>()) {
        if (auto element = ElementOf(*pane)) {
            element.Arrange(rect);
        }
        return;
    }

    auto* split = branch.TryGet<Split>();
    if (!split) return;
    auto* first  = split->left.get();
    auto* second = split->right.get();
    if (!first && !second) return;
    if (!first)  { ArrangeBranch(*second, rect); return; }
    if (!second) { ArrangeBranch(*first, rect); return; }

    float thickness = static_cast<float>(kSplitterThickness);

    if (split->IsHorizontal()) {
        float useable = std::max(0.0f, rect.Width - thickness);
        float firstW  = useable * static_cast<float>(split->ratio);
        float secondW = useable - firstW;
        Windows::Foundation::Rect firstRect{ rect.X, rect.Y, firstW, rect.Height };
        Windows::Foundation::Rect splitRect{ rect.X + firstW, rect.Y, thickness, rect.Height };
        Windows::Foundation::Rect secondRect{ rect.X + firstW + thickness, rect.Y, secondW, rect.Height };
        ArrangeBranch(*first, firstRect);
        if (auto sp = SplitterForBranch(&branch)) sp.Arrange(splitRect);
        ArrangeBranch(*second, secondRect);
    } else {
        float useable = std::max(0.0f, rect.Height - thickness);
        float firstH  = useable * static_cast<float>(split->ratio);
        float secondH = useable - firstH;
        Windows::Foundation::Rect firstRect{ rect.X, rect.Y, rect.Width, firstH };
        Windows::Foundation::Rect splitRect{ rect.X, rect.Y + firstH, rect.Width, thickness };
        Windows::Foundation::Rect secondRect{ rect.X, rect.Y + firstH + thickness, rect.Width, secondH };
        ArrangeBranch(*first, firstRect);
        if (auto sp = SplitterForBranch(&branch)) sp.Arrange(splitRect);
        ArrangeBranch(*second, secondRect);
    }
}

void SplitPanel::OnSplitterPointerPressed(Microsoft::UI::Xaml::UIElement const& splitter,
                                          Branch* splitBranch,
                                          Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
{
    if (!splitBranch) return;
    if (!splitter.CapturePointer(args.Pointer())) return;
    m_draggingBranch = splitBranch;
    args.Handled(true);
}

void SplitPanel::OnSplitterPointerMoved(Branch* splitBranch,
                                        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
{
    if (!m_draggingBranch || m_draggingBranch != splitBranch) return;
    auto* split = splitBranch ? splitBranch->TryGet<Split>() : nullptr;
    if (!split) return;

    auto self = get_strong().as<winrt::Microsoft::UI::Xaml::UIElement>();
    auto point = args.GetCurrentPoint(self).Position();

    auto rect = splitBranch->arrangedRect;
    double newRatio = split->ratio;
    float thickness = static_cast<float>(kSplitterThickness);

    if (split->IsHorizontal()) {
        float useable = std::max(1.0f, rect.Width - thickness);
        newRatio = (point.X - rect.X) / useable;
    } else {
        float useable = std::max(1.0f, rect.Height - thickness);
        newRatio = (point.Y - rect.Y) / useable;
    }
    split->ratio = ClampSplitRatio(newRatio);
    InvalidateMeasure();
    InvalidateArrange();
    args.Handled(true);
}

void SplitPanel::OnSplitterPointerReleased(Microsoft::UI::Xaml::UIElement const& splitter,
                                           Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
{
    splitter.ReleasePointerCapture(args.Pointer());
    m_draggingBranch = nullptr;
    args.Handled(true);
}

}  // namespace winrt::GhosttyWin32::implementation
