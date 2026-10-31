/**
 * @file desktop.h
 * @brief 桌面网格容器
 *
 * 定义 DesktopGrid 类，它是桌面图标网格布局的主容器，继承自 GridContainer。
 * 负责桌面区域的图标排列、命中测试、拖放操作以及绘制。
 *
 * @date 2026-06-07
 */
#pragma once
#include "container.h"
#include "slot.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct GridPage;
struct DesktopItem;
struct GridCell;
class DesktopApp;
class DesktopIcon;

/**
 * @class DesktopGrid
 * @brief 桌面图标网格布局的主容器
 *
 * DesktopGrid 是桌面的核心网格容器，继承自 GridContainer。
 * 它管理桌面图标的布局排列、鼠标命中测试、拖放交互以及界面绘制。
 * 内部持有对 GridPage 列表、DesktopItem 列表和 DesktopApp 的引用。
 */
class DesktopGrid : public GridContainer
{
public:
    /**
     * @brief 构造函数
     * @param pages  GridPage 列表指针，管理桌面分页信息
     * @param items  DesktopItem 列表指针，管理桌面图标项
     * @param app    DesktopApp 指针，桌面应用实例
     */
    DesktopGrid(std::vector<GridPage>* pages, std::vector<DesktopItem>* items, DesktopApp* app);
    /** @brief 析构函数 */
    ~DesktopGrid();
    /**
     * @brief 获取桌面标题
     * @return 桌面窗口的标题字符串
     */
    std::wstring GetTitle() const override;
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SlotSurfaceKind::Desktop;
    }
    /**
     * @brief 构建网格中的所有槽位
     * @return 包含所有 Slot 唯一指针的向量
     *
     * 根据当前分页和桌面项配置，创建并返回桌面网格中所有空闲和已占用的槽位。
     */
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    /**
     * @brief 处理桌面项拖放事件
     * @param sourceItems 被拖拽的源项列表
     * @param origin      源容器指针
     * @param targetSlot  目标槽位指针
     * @param region      命中区域类型
     * @param mods        键盘修饰键状态
     *
     * 当用户将图标拖放到桌面网格区域时调用，负责完成项的移动或复制逻辑。
     */
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    /**
     * @brief 绘制桌面装饰层（Chrome）
     * @param context D2D 设备上下文指针
     * @param mousePt 当前鼠标位置
     *
     * 在已有图标之上绘制拖放预览、选中高亮等额外 UI 元素。
     */
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;
    /**
     * @brief 获取桌面网格的边界矩形
     * @return 桌面区域对应的 RECT 结构
     */
    RECT GetBounds() const override;
    /**
     * @brief 获取桌面项列表（只读）
     * @return DesktopItem 向量的常量引用
     */
    const std::vector<DesktopItem>& GetItems() const { return *items_; }
    /**
     * @brief 获取桌面项列表（可写）
     * @return DesktopItem 向量的引用，允许修改桌面项
     */
    std::vector<DesktopItem>& GetItems() { return *items_; }
    /**
     * @brief 获取分页列表
     * @return GridPage 向量的常量引用
     */
    const std::vector<GridPage>& GetPages() const { return *pages_; }

    /**
     * @brief 直接命中测试（O(1)，无需槽位分配）
     * @param pt      鼠标点击坐标
     * @param outSlot 输出参数，返回命中的槽位指针
     * @return 命中区域类型
     *
     * 在桌面网格上直接对指定点进行命中测试，不经过槽位分配流程，性能为 O(1)。
     */
    HitRegion HitTestAtPoint(POINT pt, Slot*& outSlot);

    // ── 容器拖放虚函数 ──────────────────────────
    /**
     * @brief 获取当前选中的桌面项
     * @return 选中项的 Item 指针向量
     */
    std::vector<Item*> GetSelectedItems() const override;
    /**
     * @brief 拖拽操作的命中测试
     * @param pt      鼠标当前位置
     * @param outSlot 输出参数，返回命中的槽位指针
     * @return 命中区域类型
     */
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
    /**
     * @brief 获取拖放操作的提示文本
     * @param slot        目标槽位指针
     * @param region      命中区域类型
     * @param sourceItems 被拖拽的源项列表
     * @param origin      源容器指针
     * @param mods        键盘修饰键状态
     * @return 拖放提示字符串
     */
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const override;
    /**
     * @brief 绘制拖放预览效果
     * @param ctx    D2D 设备上下文指针
     * @param slot   目标槽位指针
     * @param region 命中区域类型
     *
     * 在桌面上绘制拖放操作的目标位置指示器，帮助用户确认放置位置。
     */
    void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region) override;

private:
    /**
     * @brief 命中测试使用的缓存槽位信息。
     *
     * 只在 gridPages_ 结构变化或 Slot 集合重建时刷新（由 slotGeneration 校验），
     * 指针移动的每个采样点直接复用，避免重复构建每列/行中心偏移表。
     */
    struct CachedSlotInfo
    {
        Slot* slot = nullptr;
        std::wstring pageId;
        int column = 0;
        int row = 0;
    };

    /**
     * @brief 已被桌面项占据的网格单元。
     *
     * 每个页面对应一张稀疏表，键为 column*rows+row（与 BuildSlots 的
     * 列主序一致）。只在 items_/widgets_ 布局变化后重建，替代每帧遍历
     * 全部 items_ 查找占用者。
     */
    using OccupancyMap = std::unordered_map<
        std::wstring, std::unordered_set<int>>;

    void EnsureHitTestCache();
    void EnsureOccupancyCache() const;

    std::vector<GridPage>* pages_;
    std::vector<DesktopItem>* items_;
    DesktopApp* app_;
    RECT bounds_{};
    mutable std::vector<std::unique_ptr<DesktopIcon>> dragSourceCache_;

    std::vector<CachedSlotInfo> cachedHitSlots_;
    std::unordered_map<std::wstring, std::size_t> cachedPageSlotBase_;
    std::uint64_t cachedHitSlotGeneration_ = 0;
    bool cachedHitSlotsValid_ = false;

    mutable OccupancyMap occupancyCache_;
    mutable std::uint64_t occupancyCacheRevision_ = 0;
    mutable bool occupancyCacheValid_ = false;
};
