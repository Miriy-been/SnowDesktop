#include "page_dots_geometry.h"

#include <iostream>

namespace pageDots = snowdesktop::page_dots;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void CheckNear(float actual, float expected, float tolerance, const char* message)
{
    const float diff = actual - expected;
    if (diff <= tolerance && diff >= -tolerance) return;
    ++failures;
    std::cerr << "FAILED: " << message
              << " (actual=" << actual << " expected=" << expected
              << " tolerance=" << tolerance << ")\n";
}

void TestZeroOrNegativePages()
{
    for (int pages : {0, -1, -100})
    {
        const auto g = pageDots::ComputePageDotsGeometry(pages);
        Check(g.uniformSpacing == 0.0f,
            "zero/negative pages must yield zero uniformSpacing");
        Check(g.totalWidth == 0.0f,
            "zero/negative pages must yield zero totalWidth");
        Check(g.capsuleWidth == 0.0f,
            "zero/negative pages must yield zero capsuleWidth");
        Check(g.capsuleHeight == 0.0f,
            "zero/negative pages must yield zero capsuleHeight");
        Check(g.firstDotCenterX == 0.0f,
            "zero/negative pages must yield zero firstDotCenterX");
    }
}

void TestSinglePage()
{
    // 1 页时 DrawPageDotsIndicator 早退，但仍能安全计算几何
    const auto g = pageDots::ComputePageDotsGeometry(1);
    // inactiveRadius=3, dotGap=6 → uniformSpacing = 6+6 = 12
    CheckNear(g.uniformSpacing, 12.0f, 0.001f,
        "single page uniformSpacing = inactiveRadius*2 + dotGap");
    // totalWidth = 0*(0) + 5*2 = 10
    CheckNear(g.totalWidth, 10.0f, 0.001f,
        "single page totalWidth = activeRadius * 2");
    // capsuleWidth = 10 + 12*2 = 34
    CheckNear(g.capsuleWidth, 34.0f, 0.001f,
        "single page capsuleWidth = totalWidth + 2*capsulePadX");
    // capsuleHeight = 5*2 + 8*2 = 26
    CheckNear(g.capsuleHeight, 26.0f, 0.001f,
        "capsuleHeight = activeRadius*2 + 2*capsulePadY");
    // firstDotCenterX = 12 + 5 = 17
    CheckNear(g.firstDotCenterX, 17.0f, 0.001f,
        "firstDotCenterX = capsulePadX + activeRadius");
}

void TestTwoPages()
{
    const auto g = pageDots::ComputePageDotsGeometry(2);
    CheckNear(g.uniformSpacing, 12.0f, 0.001f,
        "two pages uniformSpacing unchanged");
    // totalWidth = 12*1 + 10 = 22
    CheckNear(g.totalWidth, 22.0f, 0.001f,
        "two pages totalWidth");
    CheckNear(g.capsuleWidth, 46.0f, 0.001f,
        "two pages capsuleWidth");
}

void TestManyPages()
{
    const auto g = pageDots::ComputePageDotsGeometry(10);
    // totalWidth = 12*9 + 10 = 118
    CheckNear(g.totalWidth, 118.0f, 0.001f,
        "ten pages totalWidth scales linearly");
    // capsuleWidth = 118 + 24 = 142
    CheckNear(g.capsuleWidth, 142.0f, 0.001f,
        "ten pages capsuleWidth");
    // 所有圆点从左到右依次排列，第 0 个中心 X = firstDotCenterX
    // 第 9 个中心 X = firstDotCenterX + 9*uniformSpacing
    const float lastCenter = g.firstDotCenterX
        + 9.0f * g.uniformSpacing;
    const float rightmost = lastCenter + 5.0f; // activeRadius
    const float leftmost = g.firstDotCenterX - 5.0f; // activeRadius
    CheckNear(rightmost - leftmost, g.totalWidth, 0.001f,
        "first to last dot span equals totalWidth");
}

void TestMonotonicGrowth()
{
    // 验证 width 随总页数严格单调递增
    float prev = -1.0f;
    for (int pages = 1; pages <= 20; ++pages)
    {
        const auto g = pageDots::ComputePageDotsGeometry(pages);
        Check(g.capsuleWidth > prev,
            "capsuleWidth must strictly increase with page count");
        prev = g.capsuleWidth;
    }
}

void TestHeightInvariant()
{
    // 胶囊高度不随总页数变化（仅由 activeRadius + 2*capsulePadY 决定）
    for (int pages = 1; pages <= 10; ++pages)
    {
        const auto g = pageDots::ComputePageDotsGeometry(pages);
        CheckNear(g.capsuleHeight, 26.0f, 0.001f,
            "capsuleHeight invariant across page counts");
    }
}

} // namespace

int main()
{
    TestZeroOrNegativePages();
    TestSinglePage();
    TestTwoPages();
    TestManyPages();
    TestMonotonicGrowth();
    TestHeightInvariant();

    if (failures == 0)
    {
        std::cout << "All page_dots_geometry tests passed.\n";
        return 0;
    }
    std::cerr << failures << " page_dots_geometry test(s) failed.\n";
    return 1;
}
