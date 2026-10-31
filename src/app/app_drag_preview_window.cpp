#include "app.h"
#include "../drag_visual_rules.h"

// Compact top-level DComp surface used only for the custom drag ghost. The
// desktop, floating Dock and popup surfaces keep rendering drop guidance, but
// no longer submit a second copy of the dragged item on every pointer move.

bool DesktopApp::CreateDragPreviewWindow()
{
    if (dragPreviewHwnd_ && IsWindow(dragPreviewHwnd_))
        return true;

    dragPreviewHwnd_ = CreateWindowExW(
        snowdesktop::drag_visual_rules::kPreviewWindowExStyle,
        kDragPreviewWindowClassName,
        L"SnowDesktop drag preview",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!dragPreviewHwnd_)
        return false;
    dragPreviewWindowBoundsValid_ = false;

    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(
        dragPreviewHwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions, sizeof(disableTransitions));
    return true;
}

void DesktopApp::ResetDragPreviewCompositionResources()
{
    if (dragPreviewDcompVisual_)
        dragPreviewDcompVisual_->SetContent(nullptr);
    dragPreviewDcompSurface_.Reset();
    dragPreviewCompWidth_ = 0;
    dragPreviewCompHeight_ = 0;
    dragPreviewItemIndices_.clear();
    dragPreviewItemBounds_.clear();
    dragPreviewRenderRevision_ = 0;
    dragPreviewContentBounds_ = {};
    dragPreviewGeometryWidth_ = 0;
    dragPreviewGeometryHeight_ = 0;
    dragPreviewWindowBoundsValid_ = false;
    DragPreviewZOrderLost();
}

void DesktopApp::HideDragPreviewWindow()
{
    if (dragPreviewHwnd_ &&
        IsWindow(dragPreviewHwnd_) &&
        IsWindowVisible(dragPreviewHwnd_))
    {
        ShowWindow(dragPreviewHwnd_, SW_HIDE);
    }
    // 隐藏后的首次放置必须重新建立顶层 z 序，不能沿用会话中的登记值。
    DragPreviewZOrderLost();
}

void DesktopApp::MarkDragPreviewZOrderApplied(
    bool zOrderWritten,
    HWND insertAfter)
{
    if (!zOrderWritten)
        return;
    dragPreviewZOrderApplied_ = true;
    dragPreviewZOrderAfter_ = insertAfter;
}

void DesktopApp::DragPreviewZOrderLost()
{
    dragPreviewZOrderApplied_ = false;
    dragPreviewZOrderAfter_ = HWND_TOPMOST;
}

void DesktopApp::ApplyDragPreviewLayerPolicy()
{
    if (!dragPreviewHwnd_ ||
        !IsWindow(dragPreviewHwnd_) ||
        !IsWindowVisible(dragPreviewHwnd_))
        return;
    const HWND visibleHint = hintHwnd_ && IsWindowVisible(hintHwnd_) ? hintHwnd_ : nullptr;
    const auto policy = snowdesktop::drag_visual_rules::ResolvePreviewWindowZOrderPolicy(true, visibleHint);
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    // Raise the pair together above a newly opened Dock popup. A fixed order
    // keeps the DComp ghost from alternating over the layered hint on refresh.
    HDWP batch = BeginDeferWindowPos(visibleHint ? 2 : 1);
    if (batch && visibleHint) batch = DeferWindowPos(batch, visibleHint, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (batch) batch = DeferWindowPos(batch, dragPreviewHwnd_, policy.insertAfter, 0, 0, 0, 0, flags);
    if (batch && EndDeferWindowPos(batch))
        MarkDragPreviewZOrderApplied(true, policy.insertAfter);
    else
        DragPreviewZOrderLost();
}

void DesktopApp::DestroyDragPreviewWindow()
{
    HideDragPreviewWindow();
    ResetDragPreviewCompositionResources();
    if (dragPreviewDcompTarget_)
        dragPreviewDcompTarget_->SetRoot(nullptr);
    dragPreviewDcompVisual_.Reset();
    dragPreviewDcompTarget_.Reset();
    if (dragPreviewHwnd_ && IsWindow(dragPreviewHwnd_))
        DestroyWindow(dragPreviewHwnd_);
    dragPreviewHwnd_ = nullptr;
    dragPreviewWindowBounds_ = {};
    dragPreviewWindowBoundsValid_ = false;
}

HRESULT DesktopApp::CreateOrResizeDragPreviewCompositionSurface(
    UINT width, UINT height)
{
    if (!dcompDevice_ || !dragPreviewHwnd_ ||
        !IsWindow(dragPreviewHwnd_))
        return E_UNEXPECTED;

    if (!dragPreviewDcompTarget_)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(
            dragPreviewHwnd_, FALSE,
            &dragPreviewDcompTarget_);
        if (FAILED(hr))
            return hr;
    }
    if (!dragPreviewDcompVisual_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &dragPreviewDcompVisual_);
        if (FAILED(hr) || !dragPreviewDcompVisual_)
            return FAILED(hr) ? hr : E_FAIL;
        hr = dragPreviewDcompTarget_->SetRoot(
            dragPreviewDcompVisual_.Get());
        if (FAILED(hr))
            return hr;
    }
    // 最近一次绘制请求的几何；surface 实际尺寸可能更大（见下方复用规则）。
    dragPreviewGeometryWidth_ = width;
    dragPreviewGeometryHeight_ = height;
    const bool surfaceReused =
        dragPreviewDcompSurface_ &&
        snowdesktop::drag_visual_rules::
            ShouldReusePreviewSurface(
                dragPreviewCompWidth_, dragPreviewCompHeight_,
                width, height);
    if (surfaceReused)
        return S_OK;

    // surface 只增长不收缩：窗口尺寸只在几何真正增长时变化，拖拽中不再
    // 逐像素重建。内容仍按窗口大小绘制，旧尺寸的残余区域由调用方清除。
    const UINT surfaceWidth =
        dragPreviewDcompSurface_ && dragPreviewCompWidth_ >= width
        ? dragPreviewCompWidth_ : width;
    const UINT surfaceHeight =
        dragPreviewDcompSurface_ && dragPreviewCompHeight_ >= height
        ? dragPreviewCompHeight_ : height;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(
        surfaceWidth, surfaceHeight,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr) || !surface)
        return FAILED(hr) ? hr : E_FAIL;
    hr = dragPreviewDcompVisual_->SetContent(surface.Get());
    if (FAILED(hr))
        return hr;

    dragPreviewDcompSurface_ = std::move(surface);
    dragPreviewCompWidth_ = surfaceWidth;
    dragPreviewCompHeight_ = surfaceHeight;
    dragPreviewRenderRevision_ = 0;
    return S_OK;
}

bool DesktopApp::RenderDragPreviewCompositionFrame(
    const RECT& desktopBounds)
{
    if (dragPreviewCompositionPaintInProgress_)
        return false;
    dragPreviewCompositionPaintInProgress_ = true;
    struct PaintScope final
    {
        bool& active;
        ~PaintScope() { active = false; }
    } paintScope{ dragPreviewCompositionPaintInProgress_ };

    const UINT width = static_cast<UINT>(std::max<LONG>(
        1, desktopBounds.right - desktopBounds.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(
        1, desktopBounds.bottom - desktopBounds.top));
    const bool surfaceMissing = !dragPreviewDcompSurface_;
    HRESULT hr = CreateOrResizeDragPreviewCompositionSurface(
        width, height);
    if (FAILED(hr) || !dragPreviewDcompSurface_)
    {
        ResetDragPreviewCompositionResources();
        return false;
    }

    // surface 只增长不收缩。内容按窗口大小（desktopBounds）绘制；
    // surface 本地坐标等于世界坐标（平移只扣除 BeginDraw 更新偏移），
    // 因此复用更宽的 surface 时先整体清空，既覆盖当前窗口区域，也清掉
    // 旧尺寸的残余像素，避免透明虚影残留上一帧的放大图标。
    const RECT drawBounds{ 0, 0,
        static_cast<LONG>(dragPreviewCompWidth_),
        static_cast<LONG>(dragPreviewCompHeight_) };
    const RECT* drawUpdate = surfaceMissing ? nullptr : &drawBounds;

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = dragPreviewDcompSurface_->BeginDraw(
        drawUpdate, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        ResetDragPreviewCompositionResources();
        return false;
    }

    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(updateOffset.x - desktopBounds.left),
        static_cast<float>(updateOffset.y - desktopBounds.top)));
    context->SetAntialiasMode(
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->Clear(D2D1::ColorF(0, 0, 0, 0));

    brushCache_.clear();
    brushCacheContext_ = context.Get();
    const auto& dragItems = dragSession_.Items();
    const std::size_t previewItemCount = std::min(
        dragPreviewItemIndices_.size(),
        dragPreviewItemBounds_.size());
    for (std::size_t previewIndex = 0;
         previewIndex < previewItemCount; ++previewIndex)
    {
        const std::size_t itemIndex =
            dragPreviewItemIndices_[previewIndex];
        if (itemIndex >= dragItems.size())
            continue;
        Item* item = dragItems[itemIndex];
        if (!item)
            continue;
        const RECT destination = dragPreviewItemBounds_[previewIndex];
        auto* icon = dynamic_cast<DesktopIcon*>(item);
        const auto* data = icon ? icon->GetDesktopItem() : nullptr;
        if (data && data->largeIcon)
        {
            const RECT source = dragSession_.ResolveDraggedBounds(itemIndex, item->GetBounds(), dragSession_.CurrentPoint());
            DesktopWidget geometry; geometry.bounds = source; geometry.gridCell = data->gridCell;
            const RECT frame = GetStandaloneWidgetFrameRect(geometry);
            const float sx = static_cast<float>(destination.right - destination.left) / std::max(1L, frame.right - frame.left);
            const float sy = static_cast<float>(destination.bottom - destination.top) / std::max(1L, frame.bottom - frame.top);
            context->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(frame.left), -static_cast<float>(frame.top)) *
                D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(
                    static_cast<float>(destination.left + updateOffset.x - desktopBounds.left),
                    static_cast<float>(destination.top + updateOffset.y - desktopBounds.top)));
            item->Draw(context.Get(), source, 3);
            context->SetTransform(D2D1::Matrix3x2F::Translation(
                static_cast<float>(updateOffset.x - desktopBounds.left),
                static_cast<float>(updateOffset.y - desktopBounds.top)));
        }
        else item->Draw(context.Get(), destination, 3);
    }
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = dragPreviewDcompSurface_->EndDraw();
    if (FAILED(hr))
    {
        ResetDragPreviewCompositionResources();
        return false;
    }
    if (!CommitCompositionAnimationFrame())
    {
        ResetDragPreviewCompositionResources();
        return false;
    }
    dragPreviewContentBounds_ = desktopBounds;
    dragPreviewRenderRevision_ =
        dragSession_.StaticSceneRevision();
    return true;
}

void DesktopApp::SyncDragPreviewWindow()
{
    const auto& dragItems = dragSession_.Items();
    if (!snowdesktop::drag_visual_rules::ShouldShowPreview(
            dragSession_.IsActive(),
            dragSession_.IsVisualVisible(),
            !dragItems.empty()))
    {
        HideDragPreviewWindow();
        return;
    }

    const POINT current = dragSession_.CurrentPoint();
    std::size_t validItemCount = 0;
    std::size_t firstValidIndex = dragItems.size();
    RECT firstValidBounds{};
    std::size_t primaryItemIndex = dragItems.size();
    RECT primaryBounds{};
    for (std::size_t itemIndex = 0;
         itemIndex < dragItems.size(); ++itemIndex)
    {
        Item* item = dragItems[itemIndex];
        if (!item)
            continue;
        const RECT itemBounds = item->GetBounds();
        if (IsRectEmptyRect(itemBounds))
            continue;
        const RECT draggedBounds =
            dragSession_.ResolveDraggedBounds(
                itemIndex, itemBounds, current);
        ++validItemCount;
        if (firstValidIndex == dragItems.size())
        {
            firstValidIndex = itemIndex;
            firstValidBounds = draggedBounds;
        }
        if (primaryItemIndex == dragItems.size() &&
            PtInRect(&draggedBounds, current))
        {
            primaryItemIndex = itemIndex;
            primaryBounds = draggedBounds;
        }
    }
    if (validItemCount == 0)
    {
        HideDragPreviewWindow();
        return;
    }
    if (primaryItemIndex == dragItems.size())
    {
        primaryItemIndex = firstValidIndex;
        primaryBounds = firstValidBounds;
    }

    const auto compactLargeIcon = [&](std::size_t index, RECT bounds) {
        auto* icon = dynamic_cast<DesktopIcon*>(dragItems[index]);
        const auto* data = icon ? icon->GetDesktopItem() : nullptr;
        if (!data || !data->largeIcon) return bounds;
        DesktopWidget geometry; geometry.gridCell = data->gridCell; geometry.bounds = bounds;
        const RECT sourceFrame = GetStandaloneWidgetFrameRect(geometry);
        geometry.bounds = GetGridRect(gridPages_, data->gridCell, {1, 1});
        const RECT cellFrame = GetStandaloneWidgetFrameRect(geometry);
        return snowdesktop::drag_visual_rules::FitPreviewInCell(sourceFrame,
            {cellFrame.right - cellFrame.left, cellFrame.bottom - cellFrame.top}, current);
    };
    firstValidBounds = compactLargeIcon(firstValidIndex, firstValidBounds);
    primaryBounds = compactLargeIcon(primaryItemIndex, primaryBounds);

    dragPreviewItemIndices_.clear();
    dragPreviewItemBounds_.clear();
    dragPreviewItemIndices_.reserve(
        snowdesktop::drag_visual_rules::
            kMaximumStackedPreviewItems);
    dragPreviewItemBounds_.reserve(
        snowdesktop::drag_visual_rules::
            kMaximumStackedPreviewItems);
    if (!snowdesktop::drag_visual_rules::ShouldCompactPreview(
            validItemCount))
    {
        // 散开模式：每个被拖图标按自身偏移跟随鼠标（Windows 原生风格），
        // 不叠成一摞。
        for (std::size_t itemIndex = 0;
             itemIndex < dragItems.size();
             ++itemIndex)
        {
            Item* item = dragItems[itemIndex];
            if (!item)
                continue;
            const RECT itemBounds = item->GetBounds();
            if (IsRectEmptyRect(itemBounds))
                continue;
            dragPreviewItemIndices_.push_back(itemIndex);
            dragPreviewItemBounds_.push_back(
                compactLargeIcon(itemIndex,
                    dragSession_.ResolveDraggedBounds(
                        itemIndex, itemBounds, current)));
        }
    }
    else
    {
        constexpr std::size_t kMaximumSecondaryItems =
            snowdesktop::drag_visual_rules::
                kMaximumStackedPreviewItems - 1;
        std::array<std::size_t,
            kMaximumSecondaryItems> secondaryIndices{};
        std::array<RECT,
            kMaximumSecondaryItems> secondaryBounds{};
        std::size_t secondaryCount = 0;
        for (std::size_t itemIndex = 0;
             itemIndex < dragItems.size() &&
             secondaryCount < kMaximumSecondaryItems;
             ++itemIndex)
        {
            Item* item = dragItems[itemIndex];
            if (!item || itemIndex == primaryItemIndex)
                continue;
            const RECT itemBounds = item->GetBounds();
            if (IsRectEmptyRect(itemBounds))
                continue;
            secondaryIndices[secondaryCount] = itemIndex;
            secondaryBounds[secondaryCount] =
                compactLargeIcon(itemIndex, dragSession_.ResolveDraggedBounds(
                    itemIndex, itemBounds, current));
            ++secondaryCount;
        }

        for (std::size_t stackIndex = 0;
             stackIndex < secondaryCount;
             ++stackIndex)
        {
            const RECT sourceBounds =
                secondaryBounds[stackIndex];
            const LONG depth = static_cast<LONG>(
                secondaryCount - stackIndex);
            const LONG left = primaryBounds.left -
                depth * snowdesktop::drag_visual_rules::
                    kStackedPreviewOffset;
            const LONG top = primaryBounds.top -
                depth * snowdesktop::drag_visual_rules::
                    kStackedPreviewOffset;
            dragPreviewItemIndices_.push_back(
                secondaryIndices[stackIndex]);
            dragPreviewItemBounds_.push_back({
                left,
                top,
                left + sourceBounds.right - sourceBounds.left,
                top + sourceBounds.bottom - sourceBounds.top });
        }
        dragPreviewItemIndices_.push_back(primaryItemIndex);
        dragPreviewItemBounds_.push_back(primaryBounds);
    }

    RECT contentBounds = dragPreviewItemBounds_.front();
    for (std::size_t index = 1;
         index < dragPreviewItemBounds_.size(); ++index)
    {
        const RECT& itemBounds =
            dragPreviewItemBounds_[index];
        contentBounds.left = std::min(
            contentBounds.left, itemBounds.left);
        contentBounds.top = std::min(
            contentBounds.top, itemBounds.top);
        contentBounds.right = std::max(
            contentBounds.right, itemBounds.right);
        contentBounds.bottom = std::max(
            contentBounds.bottom, itemBounds.bottom);
    }

    // Preserve icon shadows and Dock bounce overdraw without allocating a
    // desktop-sized transparent target.
    constexpr LONG kOverdraw = 8;
    InflateRect(&contentBounds, kOverdraw, kOverdraw);
    if (!CreateDragPreviewWindow())
        return;

    const UINT width = static_cast<UINT>(std::max<LONG>(
        1, contentBounds.right - contentBounds.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(
        1, contentBounds.bottom - contentBounds.top));
    POINT screenOrigin{ contentBounds.left, contentBounds.top };
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !ClientToScreen(hwnd_, &screenOrigin))
    {
        HideDragPreviewWindow();
        return;
    }

    const RECT requestedWindowBounds{
        screenOrigin.x,
        screenOrigin.y,
        screenOrigin.x + static_cast<LONG>(width),
        screenOrigin.y + static_cast<LONG>(height),
    };
    const bool previewWasVisible =
        IsWindowVisible(dragPreviewHwnd_) != FALSE;
    const bool geometryChanged =
        !dragPreviewDcompSurface_ ||
        width != dragPreviewGeometryWidth_ ||
        height != dragPreviewGeometryHeight_;
    const bool needsRender =
        geometryChanged ||
        dragPreviewRenderRevision_ !=
            dragSession_.StaticSceneRevision();
    // 高速移动时每个采样点都会放置一次窗口：位置必须跟手；但窗口尺寸
    // 只在需要重建 surface 时才提交。虚影窗口长度会随内容宽度变化，长度
    // 变化不改变左上角——逐像素改窗口尺寸与 surface 会带来同步的
    // WM_WINDOWPOSCHANGING/CHANGED 与全部被拖图标重绘，放大丢帧。
    const bool needsPlacement =
        snowdesktop::drag_visual_rules::
            NeedsPreviewWindowPlacement(
                previewWasVisible,
                dragPreviewWindowBoundsValid_,
                dragPreviewWindowBounds_,
                requestedWindowBounds);
    if (needsPlacement)
    {
        const bool firstPlacement =
            !previewWasVisible || !dragPreviewWindowBoundsValid_;
        const bool includeSize =
            snowdesktop::drag_visual_rules::
                PreviewPlacementIncludesSize(
                    firstPlacement, needsRender);
        // z 序在已登记且前置窗口不变时保持，用 SWP_NOZORDER 跳过同步
        // 的 z 序重排。
        const auto placementZOrder =
            snowdesktop::drag_visual_rules::
                ResolvePreviewPlacementZOrder(
                    previewWasVisible,
                    dragPreviewZOrderApplied_,
                    dragPreviewZOrderAfter_,
                    hintHwnd_ && IsWindowVisible(hintHwnd_)
                        ? hintHwnd_ : nullptr);
        const UINT windowWidth =
            snowdesktop::drag_visual_rules::PreviewWindowDimension(
                includeSize, width, dragPreviewWindowBoundsValid_
                    ? static_cast<UINT>(dragPreviewWindowBounds_.right -
                        dragPreviewWindowBounds_.left)
                    : width);
        const UINT windowHeight =
            snowdesktop::drag_visual_rules::PreviewWindowDimension(
                includeSize, height, dragPreviewWindowBoundsValid_
                    ? static_cast<UINT>(dragPreviewWindowBounds_.bottom -
                        dragPreviewWindowBounds_.top)
                    : height);
        const UINT placementFlags = placementZOrder.flags |
            (includeSize ? 0u : static_cast<UINT>(SWP_NOSIZE));
        if (!SetWindowPos(
                dragPreviewHwnd_, placementZOrder.insertAfter,
                screenOrigin.x, screenOrigin.y,
                includeSize ? static_cast<int>(windowWidth) : 0,
                includeSize ? static_cast<int>(windowHeight) : 0,
                placementFlags))
        {
            dragPreviewWindowBoundsValid_ = false;
            DragPreviewZOrderLost();
            HideDragPreviewWindow();
            return;
        }
        dragPreviewWindowBounds_ =
            snowdesktop::drag_visual_rules::
                RegisteredPreviewWindowBounds(
                    requestedWindowBounds,
                    windowWidth, windowHeight, includeSize);
        dragPreviewWindowBoundsValid_ = true;
        MarkDragPreviewZOrderApplied(
            (placementFlags & SWP_NOZORDER) == 0,
            placementZOrder.insertAfter);
    }
    else if (previewWasVisible)
    {
        // 位置与尺寸均未变化：本采样点无需任何窗口调用。
    }
    else
    {
        // A Dock popup can enter the topmost band while the pointer is held
        // still for dwell activation. Reassert the ghost even without a
        // geometry update, while keeping the hint above the ghost.
        ApplyDragPreviewLayerPolicy();
    }

    // Native input stays captured and external-window classification already
    // resolves below this presentation-only HWND. While OLE owns routing the
    // custom preview is hidden before DoDragDrop starts and remains hidden
    // until that nested loop has fully unwound. A moving one-pixel window
    // region is therefore unnecessary; avoiding SetWindowRgn here also avoids
    // the synchronous WM_WINDOWPOSCHANGING/WM_WINDOWPOSCHANGED pair it emits
    // for every pointer pixel.

    if (needsRender &&
        !RenderDragPreviewCompositionFrame(contentBounds))
    {
        HideDragPreviewWindow();
        return;
    }
    if (needsRender)
    {
        // Submit the compact surface before any desktop/Dock feedback paint.
        // The outer pointer message may still have larger work to do, but the
        // compositor can start presenting the ghost immediately.
        FlushPendingCompositionCommit();
    }
    if (!previewWasVisible)
    {
        // 首次显示：建立顶层带并显示窗口，z 序视为已登记，后续逐像素
        // 放置即可用 SWP_NOZORDER 跳过同步的 z 序重排。
        const auto policy = snowdesktop::drag_visual_rules::ResolvePreviewWindowZOrderPolicy(false,
            hintHwnd_ && IsWindowVisible(hintHwnd_) ? hintHwnd_ : nullptr);
        SetWindowPos(dragPreviewHwnd_, policy.insertAfter, 0, 0, 0, 0,
            policy.flags | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        MarkDragPreviewZOrderApplied(true, policy.insertAfter);
    }
}

bool DesktopApp::IsDragPresentationOnlyWindow(HWND window) const
{
    if (!window) return false;
    if (window == dragPreviewHwnd_ ||
        window == hintHwnd_ ||
        desktopBackdropCompositor_.IsBackdropWindow(window) ||
        IsPersistentDockBackdropWindow(window) ||
        collectionPopupBackdropCompositor_.IsBackdropWindow(window) ||
        quickNavBackdropCompositor_.IsBackdropWindow(window) ||
        (dockWindowTransition_ &&
            dockWindowTransition_->IsPresentationWindow(window)))
        return true;

    const DWORD extendedStyle = static_cast<DWORD>(
        GetWindowLongPtrW(window, GWL_EXSTYLE));
    return snowdesktop::drag_visual_rules::
        IsLayeredTransparentPresentationWindow(extendedStyle);
}

HWND DesktopApp::ResolveWindowBelowDragPreviewAt(
    POINT screenPoint) const
{
    HWND hit = WindowFromPoint(screenPoint);
    if (!dragPreviewHwnd_ || !IsWindow(dragPreviewHwnd_) ||
        (hit != dragPreviewHwnd_ &&
         GetAncestor(hit, GA_ROOT) != dragPreviewHwnd_))
        return hit;

    for (HWND candidate = GetWindow(
             dragPreviewHwnd_, GW_HWNDNEXT);
         candidate;
         candidate = GetWindow(candidate, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(candidate) ||
            !IsWindowEnabled(candidate))
            continue;
        RECT windowRect{};
        if (!GetWindowRect(candidate, &windowRect) ||
            !PtInRect(&windowRect, screenPoint))
            continue;

        const bool presentationOnly =
            IsDragPresentationOnlyWindow(candidate);
        DWORD cloaked = 0;
        const bool isCloaked =
            SUCCEEDED(DwmGetWindowAttribute(
                candidate, DWMWA_CLOAKED,
                &cloaked, sizeof(cloaked))) &&
            cloaked != 0;
        if (snowdesktop::drag_visual_rules::
                ShouldSkipPreviewFallbackCandidate(
                    true, true, isCloaked,
                    presentationOnly))
            continue;

        const POINT localPoint{
            screenPoint.x - windowRect.left,
            screenPoint.y - windowRect.top };
        RECT regionBounds{};
        const int regionType =
            GetWindowRgnBox(candidate, &regionBounds);
        if (!snowdesktop::drag_visual_rules::
                PreviewFallbackRegionContainsPoint(
                    regionType,
                    PtInRect(&regionBounds, localPoint) != FALSE))
            continue;
        if (snowdesktop::drag_visual_rules::
                PreviewFallbackNeedsExactRegionCheck(regionType))
        {
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            if (!region)
                return nullptr;
            const int exactRegionType =
                GetWindowRgn(candidate, region);
            const bool pointInRegion =
                PtInRegion(region,
                    localPoint.x, localPoint.y) != FALSE;
            DeleteObject(region);
            if (!snowdesktop::drag_visual_rules::
                    PreviewFallbackRegionContainsPoint(
                        exactRegionType, pointInRegion))
                continue;
        }
        return candidate;
    }
    return nullptr;
}

LRESULT DesktopApp::HandleDragPreviewMessage(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
