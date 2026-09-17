#include "app.h"
#include "../page_navigation_rules.h"
#include "../page_dots_geometry.h"

namespace pageDots = snowdesktop::page_dots;

// Desktop page-navigation layout and rendering.

const GridPage* DesktopApp::GetPageNavigationGridPage() const
{
    if (gridPages_.empty()) return nullptr;

    const GridPage* targetPage = nullptr;
    for (const auto& page : gridPages_)
    {
        if (!lastMonitorPageId_.empty() && page.id == lastMonitorPageId_)
        {
            targetPage = &page;
            break;
        }
    }
    if (!targetPage)
    {
        // 回退到渲染顺序的末屏（双锚点下可能不是 bounds.left 最右者）
        std::vector<size_t> order = BuildMonitorRenderOrder();
        if (!order.empty()) targetPage = &gridPages_[order.back()];
    }
    if (!targetPage) targetPage = &gridPages_.back();
    return targetPage;
}

void DesktopApp::GetNavHotEdgeRects(
    RECT& outPrev, RECT& outNext) const
{
    outPrev = {};
    outNext = {};
    const GridPage* targetPage = GetPageNavigationGridPage();
    if (!targetPage) return;
    snowdesktop::page_navigation_rules::BuildHotEdgeRects(
        targetPage->bounds, targetPage->dpiX,
        outPrev, outNext);
}

RECT DesktopApp::GetPageNavHotEdgeHintBounds(
    int side, POINT point) const
{
    const GridPage* page = GetPageNavigationGridPage();
    if (!page || (side != -1 && side != 1)) return {};

    const float scale = std::max(
        1.0f, static_cast<float>(page->dpiX) /
            static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    const LONG width = static_cast<LONG>(360.0f * scale);
    const LONG height = static_cast<LONG>(44.0f * scale);
    const LONG gap = static_cast<LONG>(8.0f * scale);
    RECT previousEdge{};
    RECT nextEdge{};
    GetNavHotEdgeRects(previousEdge, nextEdge);
    const RECT edge = side < 0 ? previousEdge : nextEdge;

    LONG top = point.y - height / 2;
    top = std::clamp(
        top,
        page->workArea.top,
        std::max(page->workArea.top,
            page->workArea.bottom - height));
    LONG left = side < 0
        ? edge.right + gap
        : edge.left - gap - width;
    left = std::clamp(
        left,
        page->workArea.left,
        std::max(page->workArea.left,
            page->workArea.right - width));
    return MakeRect(left, top, left + width, top + height);
}

void DesktopApp::SetPageNavHotEdgeHover(int side)
{
    if (side != -1 && side != 1)
        side = 0;
    if (navHoverSide_ == side &&
        navHotEdgeHover_ == (side != 0))
        return;

    if (navHotEdgeHintToken_)
        uiAnimationScheduler_.Cancel(navHotEdgeHintToken_);
    navHotEdgeHintToken_ = 0;
    navHoverSide_ = side;
    navHotEdgeHover_ = side != 0;
    navHotEdgeHintVisible_ = false;
    if (side == 0) return;

    navHotEdgeHintToken_ =
        uiAnimationScheduler_.ScheduleOnce(
            snowdesktop::page_navigation_rules::
                kHotEdgeHintDelayMs,
            [this, side](snowdesktop::UiScheduleToken token) {
                if (navHotEdgeHintToken_ != token)
                    return;
                navHotEdgeHintToken_ = 0;
                if (!navHotEdgeHover_ ||
                    navHoverSide_ != side)
                    return;
                navHotEdgeHintVisible_ = true;
                RECT dirty = GetPageNavHotEdgeHintBounds(
                    side, lastMousePoint_);
                InflateRect(&dirty, 4, 4);
                if (!PresentDesktopForegroundComposition(dirty) &&
                    hwnd_ && IsWindow(hwnd_))
                {
                    InvalidateRect(hwnd_, &dirty, FALSE);
                    PresentDesktopPointerUpdate();
                }
            });
}

void DesktopApp::RefreshPageNavHotEdgeHoverAt(POINT point)
{
    lastMousePoint_ = point;
    int side = 0;
    // 屏幕边缘热区只在拖拽（组件/图标/外部拖放）时生效；
    // 日常悬停不再触发翻页提示，避免误触。
    const bool dragging =
        widgetAction_ == WidgetAction::Move ||
        dragSession_.IsActive() ||
        dragDropController_.IsTransportActive();
    if (dragging && !desktopIconsHidden_ &&
        (MaxPageOffset() > 0 || pageOffset_ > 0))
    {
        RECT previousEdge{};
        RECT nextEdge{};
        GetNavHotEdgeRects(previousEdge, nextEdge);
        const auto target = snowdesktop::page_navigation_rules::
            HitTestPointerTarget(point, previousEdge, nextEdge);
        const int direction = snowdesktop::page_navigation_rules::
            PointerTargetDirection(target);
        if ((direction == -1 && pageOffset_ > 0) ||
            (direction == 1 && pageOffset_ < MaxPageOffset()))
            side = direction;
    }
    SetPageNavHotEdgeHover(side);
}

void DesktopApp::DrawPageNavHotEdgeHint(
    ID2D1DeviceContext* ctx)
{
    if (!ctx) return;
    const bool dragging =
        widgetAction_ == WidgetAction::Move ||
        dragSession_.IsActive() ||
        dragDropController_.IsTransportActive();
    const bool hovering =
        navHotEdgeHover_ &&
        (navHoverSide_ == -1 || navHoverSide_ == 1);
    if (!dragging && !hovering) return;

    const auto rails = snowdesktop::page_navigation_rules::
        ResolveHotEdgeRailVisibility(
            dragging,
            hovering ? navHoverSide_ : 0,
            pageOffset_ > 0,
            pageOffset_ < MaxPageOffset());
    if (!rails.previous && !rails.next) return;

    const GridPage* page = GetPageNavigationGridPage();
    if (!page) return;
    const float scale = std::max(
        1.0f, static_cast<float>(page->dpiX) /
            static_cast<float>(USER_DEFAULT_SCREEN_DPI));

    RECT previousEdge{};
    RECT nextEdge{};
    GetNavHotEdgeRects(previousEdge, nextEdge);

    ComPtr<ID2D1SolidColorBrush> accentBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.18f, 0.45f, 0.90f, 0.72f),
        &accentBrush);
    if (accentBrush)
    {
        if (rails.previous)
            ctx->FillRectangle(
                ToD2DRect(previousEdge), accentBrush.Get());
        if (rails.next)
            ctx->FillRectangle(
                ToD2DRect(nextEdge), accentBrush.Get());
    }

    const bool hoveredDirectionVisible = hovering &&
        (navHoverSide_ < 0 ? rails.previous : rails.next);
    if (!hoveredDirectionVisible ||
        (!dragging && !navHotEdgeHintVisible_))
        return;

    const UINT modifiers = navHoverSide_ < 0
        ? generalSettings_.pageNavigationPreviousModifiers
        : generalSettings_.pageNavigationNextModifiers;
    const UINT virtualKey = navHoverSide_ < 0
        ? generalSettings_.pageNavigationPreviousVirtualKey
        : generalSettings_.pageNavigationNextVirtualKey;
    std::wstring message;
    if (dragging)
    {
        message = _LW("app.navigation.edge_drag_dwell");
    }
    else if (generalSettings_.pageNavigationKeyboardEnabled &&
        virtualKey != 0)
    {
        NavigationSettings displaySettings;
        displaySettings.modifiers = modifiers;
        displaySettings.virtualKey = virtualKey;
        const std::wstring shortcut =
            FormatNavigationHotkey(displaySettings);
        message = _LFW(
            navHoverSide_ < 0
                ? "app.navigation.edge_previous_with_key"
                : "app.navigation.edge_next_with_key",
            shortcut);
    }
    else
    {
        message = _LW(navHoverSide_ < 0
            ? "app.navigation.edge_previous"
            : "app.navigation.edge_next");
    }
    message.insert(0, navHoverSide_ < 0
        ? L"\u25C0  " : L"\u25B6  ");

    const RECT hint = GetPageNavHotEdgeHintBounds(
        navHoverSide_, lastMousePoint_);
    if (IsRectEmptyRect(hint)) return;
    const D2D1_RECT_F hintRect = ToD2DRect(hint);

    ComPtr<ID2D1SolidColorBrush> backgroundBrush;
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f),
        &backgroundBrush);
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f),
        &borderBrush);
    ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.08f, 0.12f, 0.18f, 0.92f),
        &textBrush);
    const float radius = 8.0f * scale;
    if (backgroundBrush)
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(hintRect, radius, radius),
            backgroundBrush.Get());
    if (borderBrush)
        ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(hintRect, radius, radius),
            borderBrush.Get(), std::max(1.0f, scale));

    if (!dwriteFactory_ || !textBrush) return;
    ComPtr<IDWriteTextFormat> format;
    dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f * scale, L"", &format);
    if (!format) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    D2D1_RECT_F textRect = hintRect;
    textRect.left += 14.0f * scale;
    textRect.right -= 14.0f * scale;
    ctx->DrawTextW(
        message.c_str(), static_cast<UINT32>(message.size()),
        format.Get(), textRect, textBrush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

/**
 * @brief 计算常驻圆点分页指示器的布局信息（末屏定位 + 几何）。
 *
 * 与 GetPageNavigationGridPage 使用一致的末屏解析策略，绘制与命中测试共用，
 * 避免两处几何漂移。位置/大小/间距支持 GeneralSettings 自定义。
 *
 * @return 布局信息；总页数不足 2 或桌面隐藏时 valid=false。
 */
PageDotsLayoutInfo DesktopApp::ComputePageDotsLayout() const
{
    PageDotsLayoutInfo info;
    if (desktopIconsHidden_) return info;
    if (gridPages_.empty()) return info;

    const int totalPages = MaxPageOffset() + 1;
    if (totalPages <= 1) return info;

    // 末屏定位策略与 GetPageNavigationGridPage 一致
    const GridPage* targetPage = GetPageNavigationGridPage();
    if (!targetPage) return info;

    const RECT workArea = targetPage->workArea;
    if (IsRectEmptyRect(workArea)) return info;

    // 间距/大小可自定义（读取通用设置，未加载时用默认值）
    const float activeRadius = generalSettings_.pageDotsActiveRadius;
    const float inactiveRadius = std::max(2.0f, activeRadius * 0.6f);
    const float dotGap = generalSettings_.pageDotsGap;
    const pageDots::PageDotsGeometry geom =
        pageDots::ComputePageDotsGeometry(
            totalPages, activeRadius, inactiveRadius, dotGap);

    // 位置可自定义：水平（左/中/右）+ 垂直（底部/顶部）+ 边距
    const float margin = generalSettings_.pageDotsMargin;
    const float workL = static_cast<float>(workArea.left);
    const float workR = static_cast<float>(workArea.right);
    const float workT = static_cast<float>(workArea.top);
    const float workB = static_cast<float>(workArea.bottom);
    float capsuleCenterX = 0.0f;
    switch (generalSettings_.pageDotsHorizontal)
    {
    case 1: // 左侧
        capsuleCenterX = workL + margin + geom.capsuleWidth * 0.5f;
        break;
    case 2: // 右侧
        capsuleCenterX = workR - margin - geom.capsuleWidth * 0.5f;
        break;
    default: // 居中
        capsuleCenterX = (workL + workR) * 0.5f;
        break;
    }
    float capsuleTop = 0.0f;
    if (generalSettings_.pageDotsVertical == 1)
        capsuleTop = workT + margin; // 顶部
    else
        capsuleTop = workB - margin - geom.capsuleHeight; // 底部

    float capsuleLeft = capsuleCenterX - geom.capsuleWidth * 0.5f;
    // 防止边距设置导致胶囊越出工作区
    capsuleLeft = std::clamp(
        capsuleLeft, workL + 4.0f,
        std::max(workL + 4.0f, workR - geom.capsuleWidth - 4.0f));
    capsuleTop = std::clamp(
        capsuleTop, workT + 4.0f,
        std::max(workT + 4.0f, workB - geom.capsuleHeight - 4.0f));

    info.valid = true;
    info.totalPages = totalPages;
    info.capsuleLeft = capsuleLeft;
    info.capsuleTop = capsuleTop;
    info.capsuleHeight = geom.capsuleHeight;
    info.firstDotCenterX = capsuleLeft + geom.firstDotCenterX;
    info.dotsCenterY = capsuleTop + geom.capsuleHeight * 0.5f;
    info.uniformSpacing = geom.uniformSpacing;
    info.activeRadius = geom.activeRadius;
    info.inactiveRadius = geom.inactiveRadius;
    info.capsule = MakeRect(
        static_cast<LONG>(std::floor(capsuleLeft)),
        static_cast<LONG>(std::floor(capsuleTop)),
        static_cast<LONG>(std::ceil(capsuleLeft + geom.capsuleWidth)),
        static_cast<LONG>(std::ceil(capsuleTop + geom.capsuleHeight)));
    return info;
}

/**
 * @brief 仅重绘圆点指示器区域（含悬停页面名气泡），避免整屏刷新。
 */
void DesktopApp::InvalidatePageDotsIndicator()
{
    const PageDotsLayoutInfo info = ComputePageDotsLayout();
    if (!info.valid) return;
    RECT dirty = info.capsule;
    const bool hintVisible = ComputeCollectHint().valid;
    if (generalSettings_.pageDotsShowName || hintVisible)
    {
        // 气泡/轻提示可能出现在胶囊上方或下方，预留一个胶囊高度的扩展区。
        const LONG band = static_cast<LONG>(info.capsuleHeight) + 64;
        if (generalSettings_.pageDotsVertical == 1)
            dirty.bottom += band;
        else
            dirty.top -= band;
    }
    InflateRect(&dirty, 8, 8);
    if (hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(hwnd_, &dirty, FALSE);
        PresentDesktopPointerUpdate();
    }
}

/**
 * @brief 命中测试常驻圆点分页指示器。
 * @param clientPt client 坐标
 * @return 命中页索引（0-based），未命中返回 -1
 */
int DesktopApp::HitTestPageDotsIndicator(POINT clientPt) const
{
    const PageDotsLayoutInfo info = ComputePageDotsLayout();
    if (!info.valid) return -1;

    // 命中容差：比最大圆点半径再放大 4px，方便点按
    const float hitRadius = std::max(
        info.activeRadius, info.inactiveRadius) + 4.0f;
    for (int i = 0; i < info.totalPages; ++i)
    {
        const float centerX = info.firstDotCenterX
            + info.uniformSpacing * static_cast<float>(i);
        const float dx = static_cast<float>(clientPt.x) - centerX;
        const float dy = static_cast<float>(clientPt.y) - info.dotsCenterY;
        if (dx * dx + dy * dy <= hitRadius * hitRadius)
            return i;
    }
    return -1;
}

/**
 * @brief 绘制常驻圆点分页指示器（移动端风格，末屏定位）。
 *
 * 几何与配色：
 * - 仅在桌面可见且总页数 >= 2 时绘制
 * - 位置/大小/间距/颜色由 GeneralSettings 自定义
 * - 鼠标悬停的圆点放大 + 提亮，提示可点击跳转；可选显示页面名气泡
 *
 * @param ctx D2D 设备上下文
 */
void DesktopApp::DrawPageDotsIndicator(ID2D1DeviceContext* ctx)
{
    if (!ctx) return;
    const PageDotsLayoutInfo info = ComputePageDotsLayout();
    if (!info.valid) return;

    const int currentPage = pageOffset_;
    const int hoveredDot = HitTestPageDotsIndicator(lastMousePoint_);

    // 主题色（背景胶囊沿用全局外观）
    const PersonalizationSettings& p = CurrentPersonalization();
    const bool darkTheme = p.contentTheme == 0;

    // 圆点颜色：0=跟随主题 1=白色 2=黑色 3=蓝色 4=自定义
    float dotR = 0.0f, dotG = 0.0f, dotB = 0.0f;
    switch (generalSettings_.pageDotsColorMode)
    {
    case 1:
        dotR = 1.0f; dotG = 1.0f; dotB = 1.0f;
        break;
    case 2:
        dotR = 0.10f; dotG = 0.12f; dotB = 0.16f;
        break;
    case 3:
        dotR = 0.39f; dotG = 0.66f; dotB = 1.0f;
        break;
    case 4:
        dotR = static_cast<float>(generalSettings_.pageDotsColorR) / 255.0f;
        dotG = static_cast<float>(generalSettings_.pageDotsColorG) / 255.0f;
        dotB = static_cast<float>(generalSettings_.pageDotsColorB) / 255.0f;
        break;
    default:
        dotR = darkTheme ? 1.0f : 0.10f;
        dotG = darkTheme ? 1.0f : 0.12f;
        dotB = darkTheme ? 1.0f : 0.16f;
        break;
    }

    // 背景胶囊（hover 时略提亮，提示可交互）
    // 胶囊使用专门的配色而不跟随全局组件透明度，保证在任何壁纸上可辨识。
    const bool capsuleHovered = hoveredDot >= 0;
    float capsuleBgR = 0.0f, capsuleBgG = 0.0f, capsuleBgB = 0.0f;
    float capsuleBgAlpha = 0.0f;
    float capsuleBorderR = 0.0f, capsuleBorderG = 0.0f, capsuleBorderB = 0.0f;
    float capsuleBorderAlpha = 0.0f;
    if (darkTheme)
    {
        capsuleBgR = 0.05f; capsuleBgG = 0.06f; capsuleBgB = 0.08f;
        capsuleBgAlpha = 0.55f;
        capsuleBorderR = 1.0f; capsuleBorderG = 1.0f; capsuleBorderB = 1.0f;
        capsuleBorderAlpha = 0.35f;
    }
    else
    {
        capsuleBgR = 0.97f; capsuleBgG = 0.98f; capsuleBgB = 1.0f;
        capsuleBgAlpha = 0.60f;
        capsuleBorderR = 0.0f; capsuleBorderG = 0.0f; capsuleBorderB = 0.0f;
        capsuleBorderAlpha = 0.30f;
    }
    // hover 视觉强化：填充略提亮、边框更清晰，提示可交互。
    if (capsuleHovered)
    {
        capsuleBgAlpha = std::min(1.0f, capsuleBgAlpha * 1.25f);
        capsuleBorderAlpha = std::min(1.0f, capsuleBorderAlpha * 1.7f);
    }
    {
        ComPtr<ID2D1SolidColorBrush> bg;
        ctx->CreateSolidColorBrush(
            D2D1::ColorF(capsuleBgR, capsuleBgG, capsuleBgB,
                capsuleBgAlpha),
            &bg);
        if (bg) ctx->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    static_cast<float>(info.capsule.left),
                    static_cast<float>(info.capsule.top),
                    static_cast<float>(info.capsule.right),
                    static_cast<float>(info.capsule.bottom)),
                info.capsuleHeight * 0.5f, info.capsuleHeight * 0.5f),
            bg.Get());

        ComPtr<ID2D1SolidColorBrush> border;
        ctx->CreateSolidColorBrush(
            D2D1::ColorF(capsuleBorderR, capsuleBorderG, capsuleBorderB,
                capsuleBorderAlpha),
            &border);
        if (border) ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    static_cast<float>(info.capsule.left),
                    static_cast<float>(info.capsule.top),
                    static_cast<float>(info.capsule.right),
                    static_cast<float>(info.capsule.bottom)),
                info.capsuleHeight * 0.5f, info.capsuleHeight * 0.5f),
            border.Get(), 1.0f);
    }

    for (int i = 0; i < info.totalPages; ++i)
    {
        const bool isActive = (i == currentPage);
        const bool isHovered = (i == hoveredDot);
        float radius = isActive ? info.activeRadius : info.inactiveRadius;
        if (isHovered) radius = info.activeRadius + 1.5f;
        const float alpha = isActive ? 1.0f
            : (isHovered ? 0.90f : 0.50f);
        const float centerX = info.firstDotCenterX
            + info.uniformSpacing * static_cast<float>(i);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        ctx->CreateSolidColorBrush(
            D2D1::ColorF(dotR, dotG, dotB, alpha), &dotBrush);
        if (!dotBrush) continue;

        ctx->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(centerX, info.dotsCenterY),
                radius, radius),
            dotBrush.Get());

        // 当前页强调：在 active 圆点外加一圈浅色光环，强化当前页可辨识度
        if (isActive)
        {
            ComPtr<ID2D1SolidColorBrush> haloBrush;
            ctx->CreateSolidColorBrush(
                D2D1::ColorF(dotR, dotG, dotB, 0.35f), &haloBrush);
            if (haloBrush)
            {
                ctx->DrawEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(centerX, info.dotsCenterY),
                        radius + 2.5f, radius + 2.5f),
                    haloBrush.Get(), 1.5f);
            }
        }
    }

    // 收录轻提示：有待归位文件时显示在圆点胶囊旁（hover 名字气泡会覆盖它）。
    DrawCollectHint(ctx);

    // hover 提示：显示悬停圆点对应页面的名字
    if (hoveredDot >= 0 && generalSettings_.pageDotsShowName)
    {
        const std::wstring name = GetPageDisplayName(hoveredDot);
        auto* dwrite = GetDWriteFactory();
        if (dwrite && !name.empty())
        {
            ComPtr<IDWriteTextFormat> fmt;
            if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &fmt)) &&
                fmt)
            {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                float textW = 0.0f;
                float textH = 0.0f;
                ComPtr<IDWriteTextLayout> measure;
                if (SUCCEEDED(dwrite->CreateTextLayout(name.c_str(),
                        static_cast<UINT32>(name.size()), fmt.Get(),
                        800.0f, 60.0f, &measure)) &&
                    measure)
                {
                    DWRITE_TEXT_METRICS metrics{};
                    if (SUCCEEDED(measure->GetMetrics(&metrics)))
                    {
                        textW = metrics.width;
                        textH = metrics.height;
                    }
                }

                constexpr float kPadX = 10.0f;
                constexpr float kPadY = 5.0f;
                const float tooltipW = textW + kPadX * 2.0f;
                const float tooltipH = textH + kPadY * 2.0f;
                const float hoverCenterX = info.firstDotCenterX
                    + info.uniformSpacing
                        * static_cast<float>(hoveredDot);
                const float tooltipLeft = hoverCenterX - tooltipW * 0.5f;
                // 圆点在顶部时气泡放到下方，否则放到上方，避免顶部气泡超出屏幕。
                const float tooltipTop =
                    (generalSettings_.pageDotsVertical == 1)
                        ? info.capsuleTop + info.capsuleHeight + 8.0f
                        : info.capsuleTop - tooltipH - 8.0f;
                const RECT tooltipRect = MakeRect(
                    static_cast<LONG>(std::floor(tooltipLeft)),
                    static_cast<LONG>(std::floor(tooltipTop)),
                    static_cast<LONG>(std::ceil(tooltipLeft + tooltipW)),
                    static_cast<LONG>(std::ceil(tooltipTop + tooltipH)));

                DrawD2DRoundedRectangle(
                    ctx, tooltipRect, 6.0f,
                    D2D1::ColorF(0.05f, 0.05f, 0.08f, 0.90f),
                    D2D1::ColorF(0.4f, 0.6f, 1.0f, 0.40f),
                    1.0f);
                DrawD2DText(ctx, name, tooltipRect, fmt.Get(),
                    D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.95f));
            }
        }
    }
}

/**
 * @brief 汇总当前显示页上已收录分类散落在其它页面的可归位文件。
 *
 * 只统计顶层桌面项，且只针对“当前正在显示的那一页”作为归属页的占用：
 * 轻提示画在圆点胶囊旁，语义是「有文件该归到本页」，因此在其它页面上
 * 不显示，避免任何一页都弹提示而没有针对性。
 */
DesktopApp::CollectHintInfo DesktopApp::ComputeCollectHint() const
{
    CollectHintInfo info;
    if (desktopIconsHidden_ || savedPageIds_.size() < 2) return info;
    const GridPage* hostPage = GetPageNavigationGridPage();
    if (!hostPage || hostPage->id.empty()) return info;
    for (const auto& [categoryId, ownerPageId] : categoryClaims_)
    {
        if (categoryId.empty() || ownerPageId.empty()) continue;
        if (ownerPageId != hostPage->id) continue;
        int count = 0;
        for (const auto& item : items_)
        {
            if (item.name.empty() || item.gridCell.pageId.empty())
                continue;
            if (item.gridCell.pageId == kDockPageId ||
                item.gridCell.pageId == ownerPageId)
                continue;
            if (IsItemInAnyWidget(item)) continue;
            if (!IsCategoryMatchForItem(item, categoryId)) continue;
            ++count;
        }
        if (count > 0)
        {
            info.pending.emplace_back(categoryId, ownerPageId);
            info.totalPending += count;
        }
    }
    info.valid = info.totalPending > 0;
    return info;
}

/**
 * @brief 命中测试收录轻提示胶囊（区域由最近一次绘制记录）。
 *
 * 先做 O(1) 区域排除，仅在指针落入胶囊时才复核提示仍然有效，
 * 避免桌面隐藏或翻页布局变化后残留的旧区域被误点，也避免每次
 * 鼠标移动都做一次全量分类统计。
 */
bool DesktopApp::HitTestCollectHint(POINT clientPt) const
{
    if (collectHintRect_.right <= collectHintRect_.left ||
        collectHintRect_.bottom <= collectHintRect_.top)
        return false;
    if (!PtInRect(&collectHintRect_, clientPt)) return false;
    return ComputeCollectHint().valid;
}

/**
 * @brief 绘制收录轻提示胶囊（圆点指示器旁，hover 提亮）。
 *
 * 位置跟随圆点配置：圆点在顶部时显示在下方，否则显示在上方；
 * 悬停页面名气泡在其后绘制，会短暂覆盖本提示。
 */
void DesktopApp::DrawCollectHint(ID2D1DeviceContext* ctx)
{
    collectHintRect_ = {};
    if (!ctx) return;
    const CollectHintInfo hint = ComputeCollectHint();
    if (!hint.valid) return;
    const PageDotsLayoutInfo info = ComputePageDotsLayout();
    if (!info.valid) return;

    const std::wstring text = _LFW("app.collect.hint",
        std::to_wstring(hint.totalPending));
    auto* dwrite = GetDWriteFactory();
    if (!dwrite) return;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"", &fmt)) || !fmt)
        return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    float textWidth = 0.0f;
    float textHeight = 0.0f;
    ComPtr<IDWriteTextLayout> measure;
    if (SUCCEEDED(dwrite->CreateTextLayout(text.c_str(),
            static_cast<UINT32>(text.size()), fmt.Get(),
            800.0f, 60.0f, &measure)) && measure)
    {
        DWRITE_TEXT_METRICS textMetrics{};
        if (SUCCEEDED(measure->GetMetrics(&textMetrics)))
        {
            textWidth = textMetrics.width;
            textHeight = textMetrics.height;
        }
    }
    if (textWidth <= 0.0f || textHeight <= 0.0f) return;

    constexpr float kPadX = 9.0f;
    constexpr float kPadY = 4.0f;
    const float pillWidth = textWidth + kPadX * 2.0f;
    const float pillHeight = textHeight + kPadY * 2.0f;
    const float pillCenterX =
        (static_cast<float>(info.capsule.left) +
            static_cast<float>(info.capsule.right)) * 0.5f;
    const float pillLeft = pillCenterX - pillWidth * 0.5f;
    const float pillTop =
        generalSettings_.pageDotsVertical == 1
        ? static_cast<float>(info.capsule.bottom) + 8.0f
        : static_cast<float>(info.capsule.top) - pillHeight - 8.0f;
    const RECT pillRect = MakeRect(
        static_cast<LONG>(std::floor(pillLeft)),
        static_cast<LONG>(std::floor(pillTop)),
        static_cast<LONG>(std::ceil(pillLeft + pillWidth)),
        static_cast<LONG>(std::ceil(pillTop + pillHeight)));
    collectHintRect_ = pillRect;

    const float borderAlpha = collectHintHovered_ ? 0.85f : 0.45f;
    DrawD2DRoundedRectangle(
        ctx, pillRect, 6.0f,
        D2D1::ColorF(0.05f, 0.05f, 0.08f, 0.90f),
        D2D1::ColorF(0.4f, 0.6f, 1.0f, borderAlpha),
        1.0f);
    DrawD2DText(ctx, text, pillRect, fmt.Get(),
        D2D1::ColorF(0.95f, 0.96f, 1.0f, 0.95f));
}

/**
 * @brief 轻提示点击：把全部待收录文件移回各自归属页。
 */
void DesktopApp::CollectPendingToOwners()
{
    const CollectHintInfo hint = ComputeCollectHint();
    if (!hint.valid) return;
    for (const auto& [categoryId, ownerPageId] : hint.pending)
        MoveCategoryItemsToPage(categoryId, ownerPageId);
    InvalidatePageDotsIndicator();
}
