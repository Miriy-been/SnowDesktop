#include "app.h"
#include "../page_dots_geometry.h"

namespace pageDots = snowdesktop::page_dots;

// Desktop page-navigation layout and rendering.

void DesktopApp::GetNavButtonRects(RECT& outPrev, RECT& outNext) const
{
    outPrev = {};
    outNext = {};
    if (gridPages_.empty()) return;

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

    constexpr LONG buttonW = 40, buttonH = 96, padX = 0;
    const LONG cy = (targetPage->workArea.top + targetPage->workArea.bottom) / 2;
    const LONG halfH = buttonH / 2;

    outPrev = MakeRect(
        targetPage->workArea.left + padX, cy - halfH,
        targetPage->workArea.left + padX + buttonW, cy + halfH);
    outNext = MakeRect(
        targetPage->workArea.right - padX - buttonW, cy - halfH,
        targetPage->workArea.right - padX, cy + halfH);
}

void DesktopApp::DrawPageNavButtons(ID2D1DeviceContext* ctx)
{
    if (MaxPageOffset() <= 0) return;
    if (gridPages_.empty()) return;

    // 默认隐藏翻页按钮；仅在换页通知期间或拖拽悬停时显示，提示用户按钮位置
    const bool dragging = (dragSession_.IsActive() &&
        (!dragSession_.Items().empty() ||
            dragDropController_.IsTransportActive())) ||
        widgetAction_ == WidgetAction::Move;
    const bool hovering = (navHoverSide_ != 0);
    if (!pageNotifyActive_ && !dragging && !hovering) return;

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    auto drawArrow = [&](const RECT& rect, bool pointingLeft, bool enabled, bool hovered) {
        D2D1_RECT_F d2dRect = D2D1::RectF(
            static_cast<float>(rect.left), static_cast<float>(rect.top),
            static_cast<float>(rect.right), static_cast<float>(rect.bottom));

        // 取当前主题色（设置窗口未就绪时回落到暗色预设）
        const PersonalizationSettings& p =
            settingsWindow_ ? settingsWindow_->GetPersonalization()
                            : PersonalizationSettings::DarkPreset();

        // 背景色：浅色主题下 widgetBg 平均亮度偏高，使用更亮的中性底
        // 避免在亮色桌面上出现突兀的深色方块；深色主题下沿用主题色。
        const float bgAvg = (p.widgetBgR + p.widgetBgG + p.widgetBgB) / 3.0f;
        const bool darkTheme = bgAvg < 0.5f;
        const float bgR = darkTheme ? p.widgetBgR : 0.96f;
        const float bgG = darkTheme ? p.widgetBgG : 0.97f;
        const float bgB = darkTheme ? p.widgetBgB : 0.99f;
        const float bgBaseAlpha = p.widgetAlpha;

        if (enabled)
        {
            const float bgAlpha = hovered
                ? std::min(1.0f, bgBaseAlpha + 0.2f)
                : bgBaseAlpha * 0.85f;
            ComPtr<ID2D1SolidColorBrush> bg;
            ctx->CreateSolidColorBrush(
                D2D1::ColorF(bgR, bgG, bgB, bgAlpha), &bg);
            if (bg) ctx->FillRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), bg.Get());

            ComPtr<ID2D1SolidColorBrush> borderBrush;
            ctx->CreateSolidColorBrush(
                D2D1::ColorF(
                    p.widgetBorderR,
                    p.widgetBorderG,
                    p.widgetBorderB,
                    0.12f),
                &borderBrush);
            if (borderBrush) ctx->DrawRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f),
                borderBrush.Get(), 1.0f);
        }
        else
        {
            // 置灰：半透明深色背景
            ComPtr<ID2D1SolidColorBrush> bg;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.35f), &bg);
            if (bg) ctx->FillRoundedRectangle(
                D2D1::RoundedRect(d2dRect, 8.0f, 8.0f), bg.Get());
        }

        // 箭头描边色：contentTheme 0=白字（深色背景），1=黑字（浅色背景）
        const float strokeAlpha = enabled ? (hovered ? 1.0f : 0.78f) : 0.3f;
        const float strokeR = p.contentTheme == 0 ? 1.0f : 0.10f;
        const float strokeG = p.contentTheme == 0 ? 1.0f : 0.12f;
        const float strokeB = p.contentTheme == 0 ? 1.0f : 0.16f;
        ComPtr<ID2D1SolidColorBrush> strokeBrush;
        ctx->CreateSolidColorBrush(
            D2D1::ColorF(strokeR, strokeG, strokeB, strokeAlpha),
            &strokeBrush);
        if (!strokeBrush) return;

        // 用 D2D 基础图元画 V 形箭头：顶点偏向按钮中心左/右，
        // 两条线段向对侧延伸。
        const float cx = (d2dRect.left + d2dRect.right) * 0.5f;
        const float cy = (d2dRect.top + d2dRect.bottom) * 0.5f;
        constexpr float kArmLen = 12.0f;        // 半臂长
        constexpr float kVerticalSpan = 12.0f;  // 上下臂垂直跨度
        const float strokeWidth = hovered ? 2.5f : 2.0f;
        const float tipX = pointingLeft
            ? cx - kArmLen * 0.5f
            : cx + kArmLen * 0.5f;
        const float tailX = pointingLeft
            ? cx + kArmLen * 0.5f
            : cx - kArmLen * 0.5f;
        const D2D1_POINT_2F upper = D2D1::Point2F(
            tipX, cy - kVerticalSpan * 0.5f);
        const D2D1_POINT_2F lower = D2D1::Point2F(
            tipX, cy + kVerticalSpan * 0.5f);
        const D2D1_POINT_2F back = D2D1::Point2F(tailX, cy);
        ctx->DrawLine(upper, back, strokeBrush.Get(), strokeWidth);
        ctx->DrawLine(lower, back, strokeBrush.Get(), strokeWidth);
    };

    bool hoverPrev = (navHoverSide_ == -1);
    bool hoverNext = (navHoverSide_ == 1);

    drawArrow(prevRect, true,  hasPrev, dragging || hoverPrev);
    drawArrow(nextRect, false, hasNext, dragging || hoverNext);
}

/**
 * @brief 绘制常驻圆点分页指示器（移动端风格，末屏底部居中）。
 *
 * 几何与配色：
 * - 仅在桌面可见且总页数 >= 2 时绘制
 * - 位置：末屏底部居中，距底 32px
 * - active 圆点 直径 10px，inactive 直径 6px，间距 10px
 * - 颜色取 PersonalizationSettings：active 用 contentTheme 决定的白/黑
 *   + alpha 0.85；inactive 同色 alpha 0.35
 * - 加半透明胶囊背景（widgetAlpha），与桌面浮窗风格统一
 *
 * 不接受点击/悬停，不参与命中测试，纯指示。
 *
 * @param ctx D2D 设备上下文
 */
void DesktopApp::DrawPageDotsIndicator(ID2D1DeviceContext* ctx)
{
    if (!ctx) return;
    if (desktopIconsHidden_) return;
    if (gridPages_.empty()) return;

    // 总页数 = 最大偏移 + 1
    const int totalPages = MaxPageOffset() + 1;
    if (totalPages <= 1) return;

    // 末屏定位策略与 GetNavButtonRects / DrawPageNotify 一致
    const GridPage* targetPage = nullptr;
    if (!lastMonitorPageId_.empty())
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == lastMonitorPageId_)
            {
                targetPage = &page;
                break;
            }
        }
    }
    if (!targetPage)
    {
        std::vector<size_t> order = BuildMonitorRenderOrder();
        if (!order.empty()) targetPage = &gridPages_[order.back()];
    }
    if (!targetPage) targetPage = &gridPages_.back();

    const RECT workArea = targetPage->workArea;
    if (IsRectEmptyRect(workArea)) return;

    // 几何参数（由纯函数计算，便于单测）
    const pageDots::PageDotsGeometry geom =
        pageDots::ComputePageDotsGeometry(totalPages);
    const float capsuleH = geom.capsuleHeight;
    const float capsuleW = geom.capsuleWidth;
    const float uniformSpacing = geom.uniformSpacing;
    const float marginBottom = geom.marginBottom;
    const float capsuleCenterX = static_cast<float>(workArea.left + workArea.right)
        * 0.5f;
    const float capsuleTop = static_cast<float>(workArea.bottom)
        - marginBottom - capsuleH;
    const float capsuleLeft = capsuleCenterX - capsuleW * 0.5f;
    const RECT capsuleRect = MakeRect(
        static_cast<LONG>(std::floor(capsuleLeft)),
        static_cast<LONG>(std::floor(capsuleTop)),
        static_cast<LONG>(std::ceil(capsuleLeft + capsuleW)),
        static_cast<LONG>(std::ceil(capsuleTop + capsuleH)));

    // 主题色
    const PersonalizationSettings& p =
        settingsWindow_ ? settingsWindow_->GetPersonalization()
                        : PersonalizationSettings::DarkPreset();
    const bool darkTheme = p.contentTheme == 0;
    const float dotR = darkTheme ? 1.0f : 0.10f;
    const float dotG = darkTheme ? 1.0f : 0.12f;
    const float dotB = darkTheme ? 1.0f : 0.16f;

    // 背景胶囊
    {
        ComPtr<ID2D1SolidColorBrush> bg;
        ctx->CreateSolidColorBrush(
            D2D1::ColorF(p.widgetBgR, p.widgetBgG, p.widgetBgB,
                p.widgetAlpha * 0.65f),
            &bg);
        if (bg) ctx->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    static_cast<float>(capsuleRect.left),
                    static_cast<float>(capsuleRect.top),
                    static_cast<float>(capsuleRect.right),
                    static_cast<float>(capsuleRect.bottom)),
                capsuleH * 0.5f, capsuleH * 0.5f),
            bg.Get());

        ComPtr<ID2D1SolidColorBrush> border;
        ctx->CreateSolidColorBrush(
            D2D1::ColorF(p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, 0.10f),
            &border);
        if (border) ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    static_cast<float>(capsuleRect.left),
                    static_cast<float>(capsuleRect.top),
                    static_cast<float>(capsuleRect.right),
                    static_cast<float>(capsuleRect.bottom)),
                capsuleH * 0.5f, capsuleH * 0.5f),
            border.Get(), 1.0f);
    }

    // 圆点：第一个圆点中心 = capsuleLeft + capsulePadX + activeRadius
    const float dotsCenterY = capsuleTop + capsuleH * 0.5f;
    const float firstDotCenterX = capsuleLeft + geom.firstDotCenterX;
    const int currentPage = pageOffset_;

    for (int i = 0; i < totalPages; ++i)
    {
        const bool isActive = (i == currentPage);
        const float radius = isActive ? geom.activeRadius : geom.inactiveRadius;
        const float alpha = isActive ? 0.85f : 0.35f;
        const float centerX = firstDotCenterX
            + uniformSpacing * static_cast<float>(i);

        ComPtr<ID2D1SolidColorBrush> dotBrush;
        ctx->CreateSolidColorBrush(
            D2D1::ColorF(dotR, dotG, dotB, alpha), &dotBrush);
        if (!dotBrush) continue;

        ctx->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(centerX, dotsCenterY),
                radius, radius),
            dotBrush.Get());
    }
}
