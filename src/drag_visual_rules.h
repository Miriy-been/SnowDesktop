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

constexpr bool ShouldApplyPreviewWindowPlacement(
    bool visible, bool cacheValid,
    const RECT& applied, const RECT& requested) noexcept
{
    return !visible || !cacheValid ||
        !EqualRect(applied, requested);
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
