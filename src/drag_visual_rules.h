#pragma once

#include <windows.h>

#include <cstddef>
#include <algorithm>
#include <cmath>

namespace snowdesktop::drag_visual_rules
{

inline constexpr std::size_t kMaximumStackedPreviewItems = 4;
inline constexpr LONG kStackedPreviewOffset = 6;

inline constexpr DWORD kPreviewWindowExStyle =
    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
    WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT;

constexpr bool ShouldShowPreview(
    bool active, bool visualVisible,
    bool hasItems) noexcept
{
    return active && visualVisible && hasItems;
}

constexpr bool ShouldSyncPreviewBeforePresentation(
    bool alreadySyncedForInput) noexcept
{
    return !alreadySyncedForInput;
}

// 多选拖拽不做“卡片堆”压缩：每个被拖图标按自身位置散开跟随鼠标
// （对齐旧版/Windows 原生的散开虚影，而不是叠成一摞）。
constexpr bool ShouldCompactPreview(
    std::size_t itemCount) noexcept
{
    (void)itemCount;
    return false;
}

constexpr bool ShouldSkipPreviewFallbackCandidate(
    bool visible,
    bool enabled,
    bool cloaked,
    bool presentationOnly) noexcept
{
    return !visible || !enabled || cloaked || presentationOnly;
}

constexpr bool IsLayeredTransparentPresentationWindow(
    DWORD extendedStyle) noexcept
{
    constexpr DWORD kPassthroughStyle =
        WS_EX_LAYERED | WS_EX_TRANSPARENT;
    return (extendedStyle & kPassthroughStyle) ==
        kPassthroughStyle;
}

constexpr bool PreviewFallbackRegionContainsPoint(
    int regionType,
    bool pointInRegion) noexcept
{
    // GetWindowRgn reports ERROR both for windows without an explicit region
    // and for an API failure. Keep the ordinary rectangular-window fallback,
    // but never treat a documented empty region as a hit.
    return regionType == ERROR ||
        (regionType != NULLREGION && pointInRegion);
}

constexpr bool PreviewFallbackNeedsExactRegionCheck(
    int regionType) noexcept
{
    return regionType == COMPLEXREGION;
}

constexpr bool EqualRect(
    const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left &&
        left.top == right.top &&
        left.right == right.right &&
        left.bottom == right.bottom;
}

/**
 * @brief surface 只增长不收缩；复用更宽的 surface 时须整体清空再绘制。
 */
constexpr bool PreviewSurfaceNeedsFullClear(
    bool surfaceReused,
    UINT surfaceWidth,
    UINT requestedWidth) noexcept
{
    return surfaceReused && surfaceWidth > requestedWidth;
}

/**
 * @brief Composed 窗口的原点（屏幕坐标）。
 *
 * 位置逐像素跟随指针，但只有与上次放置不同的轴才提交；未提交尺寸时窗口
 * 保持旧大小，登记区域取窗口实际矩形，避免下一采样点误判为位置变化。
 */
struct PreviewWindowPlacement
{
    LONG x = 0;
    LONG y = 0;
    UINT width = 0;
    UINT height = 0;
    bool sizeChanged = false;
};

constexpr bool ShouldApplyPreviewWindowPlacement(
    bool visible, bool cacheValid,
    const RECT& applied, const RECT& requested) noexcept
{
    return !visible || !cacheValid ||
        !EqualRect(applied, requested);
}

/**
 * @brief 是否可复用当前虚影 surface 而不重建。
 *
 * surface 只增长不收缩：几何在已有尺寸内变化时直接复用，内容仍按窗口
 * 大小绘制。拖拽中窗口长度会随内容宽度细微变化，旧实现每次都因此重建
 * 设备无关 surface 并重绘全部被拖图标。
 */
constexpr bool ShouldReusePreviewSurface(
    UINT surfaceWidth,
    UINT surfaceHeight,
    UINT requestedWidth,
    UINT requestedHeight) noexcept
{
    return surfaceWidth >= requestedWidth &&
        surfaceHeight >= requestedHeight;
}

/**
 * @brief 虚影窗口是否必须重新放置窗口。
 *
 * 位置必须跟随指针。尺寸变化本身不要求重新放置：只有 surface 需要重建
 * 时才提交新尺寸，避免逐像素的同步 WM_WINDOWPOSCHANGED 与整窗重绘。
 */
constexpr bool NeedsPreviewWindowPlacement(
    bool visible,
    bool cacheValid,
    const RECT& applied,
    const RECT& requested) noexcept
{
    return !visible || !cacheValid ||
        applied.left != requested.left ||
        applied.top != requested.top;
}

/**
 * @brief 本轮放置是否同时提交窗口尺寸。
 *
 * 首次显示、未登记几何或 surface 需要重建时才提交尺寸；其余只移动窗口。
 */
constexpr bool PreviewPlacementIncludesSize(
    bool firstPlacement,
    bool surfaceNeedsRender) noexcept
{
    return firstPlacement || surfaceNeedsRender;
}

/**
 * @brief 放置后登记的窗口区域。
 *
 * 未提交尺寸时窗口仍保持旧大小；登记区域取窗口实际矩形，避免下一采样点
 * 把“窗口比内容宽/高”误判成位置或尺寸变化。
 */
constexpr RECT RegisteredPreviewWindowBounds(
    const RECT& requested,
    UINT windowWidth,
    UINT windowHeight,
    bool includesSize) noexcept
{
    if (includesSize)
        return requested;
    return {
        requested.left,
        requested.top,
        requested.left + static_cast<LONG>(windowWidth),
        requested.top + static_cast<LONG>(windowHeight),
    };
}

/**
 * @brief 放置后登记的窗口尺寸。
 *
 * 窗口尺寸与 surface 尺寸语义不同：window 尺寸用于 SetWindowPos，surface
 * 尺寸用于 BeginDraw。只在重建 surface 时窗口才跟随几何变化。
 */
constexpr UINT PreviewWindowDimension(
    bool includesSize,
    UINT requested,
    UINT previous) noexcept
{
    return includesSize ? requested : previous;
}

struct PreviewWindowZOrderPolicy
{
    HWND insertAfter = nullptr;
    UINT flags = SWP_NOACTIVATE;
};

constexpr PreviewWindowZOrderPolicy
ResolvePreviewWindowZOrderPolicy(bool visible, HWND visibleHint = nullptr) noexcept
{
    (void)visible;
    return PreviewWindowZOrderPolicy{
        visibleHint ? visibleHint : HWND_TOPMOST, SWP_NOACTIVATE };
}

/**
 * @brief 逐像素放置虚影窗口时的 z 序更新策略。
 *
 * 拖拽中的每个指针采样点都会调用 SetWindowPos 让虚影跟随鼠标。z 序只需
 * 在首次显示、焦点层可能变化（Dock 弹窗进入顶层带）或提示窗出现/消失时
 * 写入：每次都传入 insertAfter 会让窗口管理器做一次同步的 z 序重排，高速
 * 移动时是明显的丢帧来源。
 * - 未登记 z 序或前置窗口变化：使用解析出的策略更新 z 序；
 * - 已登记同一前置窗口：只改位置，附加 SWP_NOZORDER。
 */
inline PreviewWindowZOrderPolicy ResolvePreviewPlacementZOrder(
    bool visible,
    bool zOrderApplied,
    HWND appliedAfter,
    HWND visibleHint) noexcept
{
    PreviewWindowZOrderPolicy policy =
        ResolvePreviewWindowZOrderPolicy(visible, visibleHint);
    const bool keepZOrder =
        visible && zOrderApplied && appliedAfter == policy.insertAfter;
    if (keepZOrder)
        policy.flags |= SWP_NOZORDER;
    return policy;
}

// Presentation only: retain aspect ratio and the pointer's relative grab point.
// DragSession and the drop solver continue using the full source bounds/span.
inline RECT FitPreviewInCell(RECT source, SIZE cell, POINT pointer) noexcept
{
    const double width = source.right - source.left;
    const double height = source.bottom - source.top;
    if (width <= 0 || height <= 0 || cell.cx <= 0 || cell.cy <= 0) return source;
    const double scale = std::min({1., cell.cx / width, cell.cy / height});
    const LONG targetWidth = std::max(1L, std::lround(width * scale));
    const LONG targetHeight = std::max(1L, std::lround(height * scale));
    const LONG left = pointer.x - std::lround((pointer.x - source.left) * scale);
    const LONG top = pointer.y - std::lround((pointer.y - source.top) * scale);
    return {left, top, left + targetWidth, top + targetHeight};
}

constexpr bool DropPreviewBelongsToRenderSurface(
    bool renderingFloatingDock,
    bool floatingDockOwnsDesktopCopy,
    bool targetIsFloatingDock) noexcept
{
    const bool belongsToFloatingDock =
        floatingDockOwnsDesktopCopy && targetIsFloatingDock;
    return renderingFloatingDock == belongsToFloatingDock;
}

} // namespace snowdesktop::drag_visual_rules
