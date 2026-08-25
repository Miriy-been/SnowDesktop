/**
 * @file page_dots_geometry.h
 * @brief 桌面分页指示器（圆点）几何参数纯函数（无副作用，便于单测）。
 */
#pragma once

namespace snowdesktop::page_dots
{

/**
 * @brief 圆点指示器几何常量与参数解析。
 */
struct PageDotsGeometry
{
    float activeRadius = 5.0f;     // active 圆点半径
    float inactiveRadius = 3.0f;   // inactive 圆点半径
    float dotGap = 6.0f;           // 圆点中心间距 - 2*radius
    float capsulePadX = 12.0f;     // 胶囊水平内边距
    float capsulePadY = 8.0f;      // 胶囊垂直内边距
    float marginBottom = 32.0f;    // 距末屏底部的外边距

    // 计算结果
    float uniformSpacing = 0.0f;   // 圆点中心间距
    float totalWidth = 0.0f;       // 整条指示器宽度
    float capsuleWidth = 0.0f;     // 胶囊宽度
    float capsuleHeight = 0.0f;    // 胶囊高度
    float firstDotCenterX = 0.0f;  // 第 0 个圆点圆心 X（相对 capsuleLeft）
};

/**
 * @brief 根据总页数计算圆点指示器几何参数。
 * @param totalPages      总页数（<=0 时所有派生字段保持 0）
 * @param activeRadius    当前页圆点半径
 * @param inactiveRadius  其它页圆点半径
 * @param dotGap          圆点中心间距 - 2*radius
 * @return 完整几何结构（含计算结果）。
 */
inline PageDotsGeometry ComputePageDotsGeometry(
    int totalPages,
    float activeRadius = 5.0f,
    float inactiveRadius = 3.0f,
    float dotGap = 6.0f)
{
    PageDotsGeometry g;
    if (totalPages <= 0)
    {
        return g;
    }
    g.activeRadius = activeRadius;
    g.inactiveRadius = inactiveRadius;
    g.dotGap = dotGap;
    g.uniformSpacing = g.inactiveRadius * 2.0f + g.dotGap;
    g.totalWidth = g.uniformSpacing * static_cast<float>(totalPages - 1)
        + g.activeRadius * 2.0f;
    g.capsuleWidth = g.totalWidth + g.capsulePadX * 2.0f;
    g.capsuleHeight = g.activeRadius * 2.0f + g.capsulePadY * 2.0f;
    g.firstDotCenterX = g.capsulePadX + g.activeRadius;
    return g;
}

} // namespace snowdesktop::page_dots
