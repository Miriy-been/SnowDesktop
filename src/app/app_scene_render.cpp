#include "app.h"
#include "../widget_visibility_rules.h"
#include "../widgets/collection_group_rules.h"

// Desktop scene composition: static cache, dynamic overlays and frame orchestration.

void DesktopApp::DrawStaticBackground(
    ID2D1DeviceContext* ctx,
    const RECT* updateRect,
    bool hiddenMode)
{
    auto intersectsUpdate =
        [&](RECT bounds, int overdraw = 0) {
        if (!updateRect)
            return true;
        if (overdraw > 0)
            InflateRect(
                &bounds, overdraw, overdraw);
        RECT intersection{};
        return IntersectRect(
            &intersection,
            &bounds,
            updateRect) != FALSE;
    };

    const bool suppressDesktopWidgetTargets = SuppressDesktopWidgetDragTargets();
    const bool popupOccludesPointer =
        IsPointOccludedByOpenPopup(lastMousePoint_);
    const POINT interactionMousePoint = lastMousePoint_;
    if (suppressDesktopWidgetTargets)
        lastMousePoint_ = { LONG_MIN, LONG_MIN };
    // 集合弹窗（含开/关动画期间）遮挡指针时，被遮挡元素不得渲染 hover
    // 反馈（图标高亮、组件 chrome、Dock 放大），但弹窗自身在动态层仍可用
    // lastMousePoint_ 绘制 item hover。
    if (popupOccludesPointer)
        lastMousePoint_ = { LONG_MIN, LONG_MIN };

    // Desktop icons
    const bool mouseOverWidget = IsPointOverWidgetChrome(lastMousePoint_);
    if (!hiddenMode)
    {
        for (auto& ooItem : items_oo_)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(ooItem.get());
            if (!icon) continue;
            DesktopItem* di = icon->GetDesktopItem();
            if (!di || IsRectEmptyRect(di->bounds)) continue;
            if (!intersectsUpdate(di->bounds, 8))
                continue;
            if (dragSession_.IsActive() && !dragSession_.Items().empty() &&
                dragSession_.IsMoveAction() && di->selected)
                continue;

            const bool desktopMarqueeActive =
                marqueeActive_ &&
                !marqueeDockFolderPopup_ &&
                marqueeWidgetIndex_ >= widgets_.size();
            const bool hovered = !desktopMarqueeActive && !mouseOverWidget &&
                PtInRect(&di->bounds, lastMousePoint_) != FALSE;
            const bool selected = di->selected && !desktopMarqueeActive;
            int state = selected ? 2 : (hovered ? 1 : 0);
            icon->Draw(ctx, di->bounds, state);
        }
    }

    // Widgets
    for (auto& widgetData : widgets_)
    {
        if (hiddenMode && !widgetData.keepWhenDesktopHidden)
            continue;
        // Dock-exclusive and grouped collections deliberately keep an empty
        // desktop rectangle while retaining a runtime WidgetContainer for
        // popup interaction. Never let an empty rectangle reach widget
        // drawing: rounded geometry APIs can turn it into a visible 1x1
        // artifact at the desktop origin.
        if (IsRectEmptyRect(widgetData.bounds))
            continue;
        const RECT widgetFrame =
            GetStandaloneWidgetFrameRect(widgetData);
        if (!intersectsUpdate(widgetFrame, 2))
            continue;
        if (widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize)
        {
            if (mouseDownWidgetIndex_ < widgets_.size() &&
                &widgetData == &widgets_[mouseDownWidgetIndex_])
                continue;
        }

        const bool popupOpen =
            popupWidgetIndex_ < widgets_.size() &&
            &widgetData == &widgets_[popupWidgetIndex_];
        const bool interactionPinned =
            !interactionPinnedWidgetId_.empty() &&
            widgetData.id == interactionPinnedWidgetId_;
        const bool interactionRetained =
            popupOpen || interactionPinned;
        if (!snowdesktop::widget_visibility_rules::ShouldRenderWidget(
                widgetData.showOnHoverOnly,
                dragSession_.IsActive(),
                dragDropController_.IsExternalDragActive(),
                widgetAction_ == WidgetAction::Move,
                widgetData.selected,
                interactionRetained,
                PtInRect(&widgetFrame, lastMousePoint_) != FALSE))
            continue;

        bool drawn = false;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgetData) continue;
            wc->DrawChrome(ctx, lastMousePoint_);
            drawn = true;
            break;
        }
        if (drawn) continue;

        for (auto& ooItem : items_oo_)
        {
            auto* widget = dynamic_cast<Widget*>(ooItem.get());
            if (!widget || widget->GetWidgetData() != &widgetData) continue;
            widget->Draw(ctx, widgetData.bounds, widgetData.selected ? 2 : 0);
            break;
        }
    }

    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(
            container.get());
        if (!dock ||
            (hiddenMode && !dockSettings_.keepWhenDesktopHidden) ||
            !snowdesktop::floating_dock_rules::
                ShouldRenderDesktopDock(
                    floatingDockDesktopCopySuppressed_,
                    dock ==
                        floatingDockContainer_))
            continue;
        RECT dockVisualBounds =
            dock->GetInteractiveBounds();
        const RECT titleBounds =
            dock->GetHoveredTitleBounds(
                lastMousePoint_);
        if (!IsRectEmptyRect(titleBounds))
            UnionRect(
                &dockVisualBounds,
                &dockVisualBounds,
                &titleBounds);
        if (!intersectsUpdate(
                dockVisualBounds, 4))
            continue;
        dock->DrawChrome(ctx, lastMousePoint_);
        dock->DrawContents(ctx);
    }

    if (suppressDesktopWidgetTargets || popupOccludesPointer)
        lastMousePoint_ = interactionMousePoint;
}

// ── Dynamic overlays (drag preview, dragged items, marquee, nav) ──

void DesktopApp::DrawDynamicOverlays(
    ID2D1DeviceContext* ctx,
    bool hiddenMode)
{
    auto beginPopupAnimationTransform =
        [&](const RECT& popup,
            D2D1_MATRIX_3X2_F& previousTransform) {
        const auto animation =
            popupAnimation_.GetVisual();
        if (!animation.visible ||
            animation.progress >= 1.0f)
            return false;

        ctx->GetTransform(&previousTransform);
        D2D1_POINT_2F origin =
            D2D1::Point2F(
                static_cast<float>(
                    popup.left + popup.right) * 0.5f,
                static_cast<float>(
                    popup.top + popup.bottom) * 0.5f);
        if (popupHasAnchor_)
        {
            origin.x = std::clamp(
                static_cast<float>(
                    popupAnchorPoint_.x),
                static_cast<float>(popup.left),
                static_cast<float>(popup.right));
            origin.y = std::clamp(
                static_cast<float>(
                    popupAnchorPoint_.y),
                static_cast<float>(popup.top),
                static_cast<float>(popup.bottom));
        }
        ctx->SetTransform(
            D2D1::Matrix3x2F::Scale(
                animation.scale,
                animation.scale,
                origin) *
            previousTransform);
        return true;
    };
    auto endPopupAnimationTransform =
        [&](bool applied,
            const D2D1_MATRIX_3X2_F&
                previousTransform) {
        if (!applied)
            return;
        ctx->SetTransform(previousTransform);
    };

    // Widget drag/resize preview
    if ((widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize) && mouseDownWidgetIndex_ < widgets_.size())
    {
        // 拖拽/缩放期间渲染 Lua 组件的真实内容：临时把组件几何移动到
        // 预览位置后按常规路径绘制（容器类组件内容缓存了绝对坐标，
        // 无法在预览位置重绘，仍走下方占位框）。
        if (widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::LuaScript)
        {
            RECT imageBounds = GetGridRect(
                gridPages_, widgetPreviewCell_, widgetPreviewSpan_);
            if (widgets_[mouseDownWidgetIndex_].freePosition ||
                generalSettings_.widgetFreePosition)
            {
                imageBounds.left   += widgetPreviewOffset_.x;
                imageBounds.right  += widgetPreviewOffset_.x;
                imageBounds.top    += widgetPreviewOffset_.y;
                imageBounds.bottom += widgetPreviewOffset_.y;
            }
            if (!IsRectEmptyRect(imageBounds))
            {
                DesktopWidget& dragged =
                    widgets_[mouseDownWidgetIndex_];
                const GridCell savedCell = dragged.gridCell;
                const GridSpan savedSpan = dragged.gridSpan;
                const RECT savedBounds = dragged.bounds;
                dragged.gridCell = widgetPreviewCell_;
                dragged.gridSpan = widgetPreviewSpan_;
                dragged.bounds = imageBounds;
                for (auto& ooItem : items_oo_)
                {
                    auto* widget = dynamic_cast<Widget*>(ooItem.get());
                    if (!widget || widget->GetWidgetData() != &dragged)
                        continue;
                    widget->Draw(ctx, imageBounds, 0);
                    break;
                }
                dragged.gridCell = savedCell;
                dragged.gridSpan = savedSpan;
                dragged.bounds = savedBounds;
            }
        }
        if (widgetAction_ == WidgetAction::Move &&
            widgetCollectionGroupTargetIndex_ <
                widgets_.size() &&
            (widgets_[widgetCollectionGroupTargetIndex_].type ==
                 DesktopWidgetType::CollectionGroup ||
             widgets_[widgetCollectionGroupTargetIndex_].type ==
                 DesktopWidgetType::FileGroup))
        {
            RECT target =
                widgets_[widgetCollectionGroupTargetIndex_].bounds;
            for (const auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<WidgetContainer*>(
                        container.get());
                if (group &&
                    group->GetWidgetData() ==
                        &widgets_[widgetCollectionGroupTargetIndex_])
                {
                    target = group->GetFrameRect();
                    break;
                }
            }
            const float cellScale =
                widgets_[widgetCollectionGroupTargetIndex_]
                    .cellScale;
            const int targetPadding =
                ScaleWidgetCu(3.0f, cellScale);
            InflateRect(
                &target, targetPadding, targetPadding);
            DrawD2DRoundedRectangle(
                ctx, target,
                static_cast<float>(
                    ScaleWidgetCu(10.0f, cellScale)),
                D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.14f),
                D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.92f),
                static_cast<float>(
                    ScaleWidgetCu(2.5f, cellScale)));
        }
        else if (widgetDockTarget_)
        {
            if (widgetDockTargetContainer_)
                widgetDockTargetContainer_->DrawInsertionPreview(
                    ctx, widgetDockInsertIndex_);
        }
        else if (widgets_[mouseDownWidgetIndex_].type != DesktopWidgetType::LuaScript)
        {
        GridCell cell = widgetPreviewCell_;
        GridSpan span = widgetPreviewSpan_;
        RECT previewBounds = GetGridRect(gridPages_, cell, span);
        if (widgets_[mouseDownWidgetIndex_].freePosition ||
                generalSettings_.widgetFreePosition)
        {
            previewBounds.left   += widgetPreviewOffset_.x;
            previewBounds.right  += widgetPreviewOffset_.x;
            previewBounds.top    += widgetPreviewOffset_.y;
            previewBounds.bottom += widgetPreviewOffset_.y;
        }

        // 容器类组件拖拽预览：整个组件（含内部子图标）随拖拽平移。
        // 不修改 bounds，改用 D2D 平移变换把 chrome+子项整体移到预览
        // 位置；关闭毛玻璃面板注册，避免拖拽时在旧位置残留面板。
        if (widgets_[mouseDownWidgetIndex_].type != DesktopWidgetType::LuaScript)
        {
            DesktopWidget& dragged = widgets_[mouseDownWidgetIndex_];
            const RECT original = dragged.bounds;
            const int deltaX = previewBounds.left - original.left;
            const int deltaY = previewBounds.top  - original.top;
            for (auto& container : containers_)
            {
                auto* wc = dynamic_cast<WidgetContainer*>(container.get());
                if (!wc || wc->GetWidgetData() != &dragged) continue;
                // 投射到预览选项：预览绘制、关闭毛玻璃面板注册
                snowdesktop::WidgetRenderOptions preOpts;
                preOpts.interactive = false;
                preOpts.registerBackdrop = false;
                auto* savedOpts = wc->GetRenderOptions();
                const bool savedSelected = dragged.selected;
                dragged.selected = false;   // 拖拽预览不显示选中态边框
                wc->SetRenderOptions(&preOpts);
                D2D1_MATRIX_3X2_F prevTx;
                ctx->GetTransform(&prevTx);
                ctx->SetTransform(prevTx *
                    D2D1::Matrix3x2F::Translation(
                        static_cast<float>(deltaX),
                        static_cast<float>(deltaY)));
                wc->DrawChrome(ctx, POINT{});
                ctx->SetTransform(prevTx);
                wc->SetRenderOptions(savedOpts);
                dragged.selected = savedSelected;
                break;
            }
        }
        }
    }

    // Drop previews are split around the popup draw. Desktop/widget targets
    // belong below foreground popups; only a popup's own target indicator is
    // allowed above its contents.
    auto drawDropPreviewLayer = [&](bool popupLayer) {
        Container* targetContainer =
            dragSession_.TargetContainer();
        Slot* targetSlot =
            dragSession_.TargetSlot();
        HitRegion targetRegion =
            dragSession_.TargetRegion();
        if (!(dragSession_.IsActive() ||
                dragDropController_.IsExternalDragActive()) ||
            !targetContainer ||
            (hiddenMode &&
                !IsRetainedContainer(targetContainer)) ||
            targetRegion == HitRegion::None)
            return;

        RECT clipViewport{};
        RECT popupTargetRect{};
        auto* wc = dynamic_cast<WidgetContainer*>(targetContainer);
        const DesktopWidget* openPopupWidget =
            GetOpenPopupWidget();
        const bool popupTarget = wc && openPopupWidget &&
            wc->GetWidgetData() == openPopupWidget &&
            targetSlot == popupDragTargetSlot_.get();
        const bool targetUsesPopupLayer =
            snowdesktop::popup_drag_rules::
                ResolveDropPreviewLayer(popupTarget) ==
            snowdesktop::popup_drag_rules::
                DropPreviewLayer::Popup;
        if (targetUsesPopupLayer != popupLayer)
            return;
        const bool groupEntryTarget =
            wc &&
            ((dragSession_.SourceList().
                    hasCollectionGroupEntries &&
                dynamic_cast<CollectionGroup*>(wc)) ||
             (dragSession_.SourceList().
                    UsesFileGroupSourceInsertion() &&
                dynamic_cast<FileGroup*>(wc)));
        if (wc && !groupEntryTarget)
        {
            RECT bodyRect = wc->GetBodyRect();
            if (popupTarget)
            {
                RECT popup =
                    GetCollectionPopupRect(
                        *openPopupWidget);
                popupTargetRect = popup;
                clipViewport =
                    snowdesktop::popup_drag_rules::
                        ExpandInsertionClipHorizontally(
                            GetCollectionPopupContentRect(
                                popup),
                            popup,
                            kCollectionPopupGapX / 2 + 2);
            }
            else
            {
                DesktopWidget* wd = wc->GetWidgetData();
                if (wd && wd->type == DesktopWidgetType::Collection && !wd->scrollContainerMode)
                {
                }
                else
                {
                    clipViewport = wc->GetContentViewportRect();
                    clipViewport.left = bodyRect.left;
                    clipViewport.right = bodyRect.right;
                }
            }
        }
        D2D1_MATRIX_3X2_F
            popupTargetPreviousTransform{};
        const bool popupTargetAnimationApplied =
            popupTarget &&
            beginPopupAnimationTransform(
                popupTargetRect,
                popupTargetPreviousTransform);
        bool clipped = false;
        if (!IsRectEmptyRect(clipViewport))
        {
            ctx->PushAxisAlignedClip(ToD2DRect(clipViewport), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            clipped = true;
        }
        if (targetRegion == HitRegion::Handoff && targetSlot)
        {
            RECT bounds = targetSlot->GetBounds();
            DrawD2DRoundedRectangle(ctx, bounds, 6.0f,
                D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.15f),
                D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.60f), 2.0f);
        }
        else
        {
            if (popupTarget && targetSlot &&
                (targetRegion == HitRegion::SortBefore ||
                 targetRegion == HitRegion::SortAfter))
            {
                targetSlot->DrawDropIndicator(ctx, targetRegion,
                    static_cast<float>(kCollectionPopupGapX) * 0.5f);
            }
            else
            {
                targetContainer->DrawDropPreview(ctx, targetSlot, targetRegion);
            }
        }
        if (clipped) ctx->PopAxisAlignedClip();
        endPopupAnimationTransform(
            popupTargetAnimationApplied,
            popupTargetPreviousTransform);
    };

    // Blue insertion bars and green handoff boxes for the desktop, widgets and
    // Dock are part of the background interaction layer.
    drawDropPreviewLayer(false);

    const bool popupBelongsToCurrentSurface =
        renderingFloatingDock_
            ? popupAnchoredToDock_ &&
                floatingDockVisible_
            : !(popupAnchoredToDock_ &&
                floatingDockDesktopCopySuppressed_);
    if (popupBelongsToCurrentSurface &&
        (!hiddenMode || IsOpenPopupRetained()) &&
        GetOpenPopupWidget())
    {
        const bool suppressPopupHover =
            SuppressDesktopWidgetDragTargets();
        const POINT interactionPoint =
            lastMousePoint_;
        if (suppressPopupHover)
            lastMousePoint_ = {
                LONG_MIN, LONG_MIN };
        DrawCollectionPopup(ctx);
        if (suppressPopupHover)
            lastMousePoint_ =
                interactionPoint;
    }

    // A target resolved inside the open popup must remain visible on top of
    // that popup, while every other target stays covered by it.
    drawDropPreviewLayer(true);

    if (!renderingFloatingDock_ &&
        !luaWidgetPanelRequest_.widgetId.empty())
    {
        bool renderLuaPanel = !hiddenMode;
        if (hiddenMode)
        {
            const auto source = std::find_if(
                widgets_.begin(), widgets_.end(),
                [&](const DesktopWidget& widget) {
                    return widget.id ==
                        luaWidgetPanelRequest_.widgetId;
                });
            renderLuaPanel = source != widgets_.end() &&
                source->keepWhenDesktopHidden;
        }
        if (renderLuaPanel)
            DrawLuaWidgetPanel(ctx);
    }

    // Dragged items at offset：跟随鼠标的半透明虚影（Windows 原生风格）。
    // 桌面图标拖拽不绘制落点占位框（见 DesktopGrid::DrawDropPreview），
    // 仅以虚影跟随鼠标，释放后吸附到最近网格空位。
    if (dragSession_.IsActive() && !dragSession_.Items().empty())
    {
        POINT current = dragSession_.CurrentPoint();
        const auto& dragItems = dragSession_.Items();
        for (size_t itemIndex = 0;
            itemIndex < dragItems.size(); ++itemIndex)
        {
            Item* item = dragItems[itemIndex];
            if (!item) continue;
            RECT bounds = item->GetBounds();
            if (IsRectEmptyRect(bounds)) continue;

            RECT draggedBounds = dragSession_.ResolveDraggedBounds(
                itemIndex, bounds, current);
            item->Draw(ctx, draggedBounds, 3);
        }
    }

    if (marqueeActive_)
    {
        if (!marqueeDockFolderPopup_ &&
            marqueeWidgetIndex_ >= widgets_.size())
        {
            for (auto& ooItem : items_oo_)
            {
                auto* icon = dynamic_cast<DesktopIcon*>(ooItem.get());
                if (!icon) continue;
                DesktopItem* item = icon->GetDesktopItem();
                if (!item || !item->selected || IsRectEmptyRect(item->bounds)) continue;
                icon->Draw(ctx, item->bounds, 2);
            }
        }
        if (marqueeDockFolderPopup_ ||
            marqueeWidgetIndex_ < widgets_.size())
        {
            RECT viewport = GetMarqueeViewportRect();
            const bool popupMarquee =
                marqueeDockFolderPopup_ ||
                marqueeWidgetIndex_ ==
                    popupWidgetIndex_;
            D2D1_MATRIX_3X2_F
                marqueePreviousTransform{};
            const bool marqueeAnimationApplied =
                popupMarquee &&
                beginPopupAnimationTransform(
                    popupRect_,
                    marqueePreviousTransform);
            ctx->PushAxisAlignedClip(ToD2DRect(viewport),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            DrawD2DFilledRectangle(ctx, marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
            ctx->PopAxisAlignedClip();
            endPopupAnimationTransform(
                marqueeAnimationApplied,
                marqueePreviousTransform);
        }
        else
        {
            DrawD2DFilledRectangle(ctx, marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
        }
    }

    if (!hiddenMode && !renderingFloatingDock_)
    {
        DrawPageNavButtons(ctx);
    }
}

void DesktopApp::RenderFrame(
    ID2D1DeviceContext* ctx,
    const RECT* updateRect,
    bool hiddenMode)
{
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    const bool widgetPreviewActive =
        widgetAction_ == WidgetAction::Move || widgetAction_ == WidgetAction::Resize;
    const bool desktopMarqueeActive =
        marqueeActive_ &&
        !marqueeDockFolderPopup_ &&
        marqueeWidgetIndex_ >= widgets_.size();
    if (!hiddenMode &&
        (dragSession_.IsActive() || widgetPreviewActive || desktopMarqueeActive))
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        UINT w = std::max<LONG>(1, client.right - client.left);
        UINT h = std::max<LONG>(1, client.bottom - client.top);

        bool cacheReady = dragRenderCache_.Ensure(d2dDevice_.Get(), D2D1_SIZE_U{ w, h },
            dragSession_.StaticSceneRevision(),
            [&](ID2D1DeviceContext* cacheCtx) {
                DrawStaticBackground(
                    cacheCtx, nullptr);
            });
        brushCache_.clear();
        brushCacheContext_ = ctx;
        if (cacheReady)
            dragRenderCache_.Draw(ctx);
        else
            DrawStaticBackground(
                ctx, updateRect);
        DrawDynamicOverlays(ctx);
        return;
    }

    // ── Normal path (not dragging) ────────────────────────────
    DrawStaticBackground(ctx, updateRect, hiddenMode);
    DrawDynamicOverlays(ctx, hiddenMode);
}
