#include "app.h"
#include "../desktop_drop_search.h"
#include "../widgets/collection_group_rules.h"

#include <map>

// Grid geometry, drag-group planning and cross-monitor migration.

void DesktopApp::UpdateLayoutWorkArea(bool preserveActiveDimensions)
{
    layoutWorkArea_ = MakeRect(0, 0, virtualWidth_, virtualHeight_);
    // Preserve the active page dimensions before rebuilding monitor geometry.
    // DPI, resolution and work-area changes may resize cells, but must not
    // replace an already established row/column count.
    if (preserveActiveDimensions)
    {
        for (const auto& page : gridPages_)
        {
            if (page.id.empty()) continue;
            savedPageColumns_[page.id] = std::max(1, page.columns);
            savedPageRows_[page.id] = std::max(1, page.rows);
        }
    }

    // The pages below are rebuilt from MONITORINFO::rcWork, so their work
    // areas no longer contain our previous Dock reservation. Discard the old
    // rectangles before ApplyDockWorkAreaReservation() runs; otherwise it
    // "restores" that stale reservation into the fresh work area and can
    // expand it across the Windows taskbar.
    dockAreas_.clear();
    gridPages_.clear();

    MonitorEnumContext ctx{};
    ctx.virtualLeft = virtualLeft_;
    ctx.virtualTop = virtualTop_;
    ctx.pages = &gridPages_;
    EnumDisplayMonitors(nullptr, nullptr, EnumGridPageMonitorProc, reinterpret_cast<LPARAM>(&ctx));

    if (gridPages_.empty())
    {
        GridPage fb;
        fb.id = L"Primary"; fb.monitorId = fb.id; fb.isPrimary = true;
        fb.bounds = layoutWorkArea_; fb.workArea = layoutWorkArea_;
        fb.visualWorkArea = fb.workArea;
        gridPages_.push_back(fb);
    }

    // 从枚举结果提取系统主屏 monitorId（供双锚点回退解析使用）
    primaryMonitorId_.clear();
    for (const auto& p : gridPages_)
        if (p.isPrimary) { primaryMonitorId_ = p.monitorId; break; }
    if (primaryMonitorId_.empty() && !gridPages_.empty())
        primaryMonitorId_ = gridPages_.front().monitorId;

    std::sort(gridPages_.begin(), gridPages_.end(), [](const GridPage& a, const GridPage& b) {
        return a.bounds.left < b.bounds.left;
    });

    for (auto& page : gridPages_)
    {
        page.workArea.left   = std::clamp<LONG>(page.workArea.left,   0, static_cast<LONG>(virtualWidth_));
        page.workArea.top    = std::clamp<LONG>(page.workArea.top,    0, static_cast<LONG>(virtualHeight_));
        page.workArea.right  = std::clamp<LONG>(page.workArea.right,  page.workArea.left, static_cast<LONG>(virtualWidth_));
        page.workArea.bottom = std::clamp<LONG>(page.workArea.bottom, page.workArea.top,  static_cast<LONG>(virtualHeight_));
        page.visualWorkArea = page.workArea;
        ConfigureGridPage(page);
        ApplyIconSpacingToPage(page);
    }

    ApplyPageMapping();
    ApplyDockWorkAreaReservation();
}

/**
 * @brief 根据工作区尺寸配置网格页面的默认列数与行数。
 * @param page 待配置的网格页面。
 */
void DesktopApp::ConfigureGridPage(GridPage& page) const
{
    const int marginX = kGridMarginX;
    const int marginY = kGridMarginY;
    // The work area is already in physical pixels. Default rows and columns are
    // derived from the physical screen area only, so changing Windows DPI does
    // not change the page grid.
    const int cw = kCellWidth;
    const int ch = kMinCellHeight;
    const int w  = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
    const int h  = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
    const int uw = std::max(1, w - marginX * 2);
    const int uh = std::max(1, h - marginY * 2);
    page.columns   = std::max(4, uw / cw);
    page.rows      = std::max(3, uh / ch);
}

/**
 * @brief 将保存的列/行数设置应用到当前网格页面。
 */
void DesktopApp::ApplySavedGridDimensions()
{
    for (auto& page : gridPages_)
    {
        auto colIt = savedPageColumns_.find(page.id);
        auto rowIt = savedPageRows_.find(page.id);
        if (colIt != savedPageColumns_.end() && rowIt != savedPageRows_.end() &&
            colIt->second >= 1 && rowIt->second >= 1)
        {
            page.columns = colIt->second;
            page.rows = rowIt->second;
            ApplyIconSpacingToPage(page);
        }
    }
}

/**
 * @brief 根据固定行列数与图标间距比例重新计算页面布局。
 * @param page 目标网格页面。
 */
void DesktopApp::ApplyIconSpacingToPage(GridPage& page)
{
    page.columns = std::max(1, page.columns);
    page.rows = std::max(1, page.rows);

    const int pageW = static_cast<int>(std::max<LONG>(
        1, page.workArea.right - page.workArea.left));
    const int pageH = static_cast<int>(std::max<LONG>(
        1, page.workArea.bottom - page.workArea.top));
    const RECT visualArea = IsRectEmptyRect(page.visualWorkArea)
        ? page.workArea : page.visualWorkArea;
    const int visualW = static_cast<int>(std::max<LONG>(
        1, visualArea.right - visualArea.left));
    const int visualH = static_cast<int>(std::max<LONG>(
        1, visualArea.bottom - visualArea.top));

    const float pageVisualScale = std::max(0.1f, std::min(
        static_cast<float>(visualW) /
            static_cast<float>(page.columns * kCellWidth),
        static_cast<float>(visualH) /
            static_cast<float>(page.rows * kMinCellHeight)));
    const int baseMarginX = std::max(1, static_cast<int>(
        std::round(kGridMarginX * pageVisualScale)));
    const int baseMarginY = std::max(1, static_cast<int>(
        std::round(kGridMarginY * pageVisualScale)));

    const int innerWidth = std::max(
        page.columns, visualW - baseMarginX * 2);
    const int innerHeight = std::max(
        page.rows, visualH - baseMarginY * 2);
    page.itemPitchWidth = std::max(
        1, static_cast<int>(std::round(
            static_cast<float>(innerWidth) /
            static_cast<float>(page.columns))));
    page.itemPitchHeight = std::max(
        1, static_cast<int>(std::round(
            static_cast<float>(innerHeight) /
            static_cast<float>(page.rows))));
    const auto visualMetrics = GetPageItemVisualMetrics(page);

    const auto horizontal = snowdesktop::grid_spacing_rules::ResolveAxis(
        pageW, page.columns, baseMarginX, kGapPercentX,
        visualMetrics.minimumGridWidth, iconSpacingScale_);
    const auto vertical = snowdesktop::grid_spacing_rules::ResolveAxis(
        pageH, page.rows, baseMarginY, kGapPercentY,
        visualMetrics.minimumGridHeight, iconSpacingScale_);
    page.marginX = horizontal.margin;
    page.cellWidth = horizontal.cell;
    page.gapX = horizontal.gap;
    page.marginY = vertical.margin;
    page.cellHeight = vertical.cell;
    page.gapY = vertical.gap;
}

// ── 拖拽辅助函数 ──────────────────────────────────────────────

extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
extern inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal);
extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);
extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);

/**
 * @brief 根据坐标计算网格轴上的索引位置（采用最近距离法）。
 * @param page 网格页面。
 * @param coordinate 像素坐标。
 * @param horizontal true 为水平轴，false 为垂直轴。
 * @return 最近的轴索引。
 */
int DesktopApp::GetGridAxisIndexFromPoint(const GridPage& page, int coordinate, bool horizontal)
{
    const int count = horizontal ? page.columns : page.rows;
    if (count <= 1) return 0;
    int bestIndex = 0;
    int bestDistance = INT_MAX;
    for (int i = 0; i < count; ++i)
    {
        const int left = (horizontal ? page.workArea.left : page.workArea.top) +
            (horizontal ? page.marginX : page.marginY) + GetGridAxisOffset(page, i, horizontal);
        const int center = left + (horizontal ? page.cellWidth : page.cellHeight) / 2;
        const int distance = std::abs(coordinate - center);
        if (distance < bestDistance) { bestDistance = distance; bestIndex = i; }
    }
    return bestIndex;
}

/**
 * @brief 根据点坐标获取对应的网格单元格（页面 ID + 行列）。
 * @param point 客户区坐标。
 * @return 对应的 GridCell。
 */
GridCell DesktopApp::CellFromPoint(POINT point) const
{
    const GridPage* page = GridPageFromPoint(point);
    GridCell cell;
    if (!page) return cell;
    cell.pageId = page->id;
    cell.column = GetGridAxisIndexFromPoint(*page, point.x, true);
    cell.row = GetGridAxisIndexFromPoint(*page, point.y, false);
    return cell;
}

/**
 * @brief 拖拽放置用的网格命中：按左上角边界包含而非中心距离，
 *        使图标左上角越过单元格边界即命中该格，消除半格偏移。
 */
GridCell DesktopApp::CellFromPointForDrag(POINT point) const
{
    const GridPage* page = GridPageFromPoint(point);
    GridCell cell;
    if (!page) return cell;
    cell.pageId = page->id;

    auto axisIndexForDrag = [&](bool horizontal) -> int {
        const int count = horizontal ? page->columns : page->rows;
        const int coordinate = horizontal ? point.x : point.y;
        const int margin = horizontal ? page->marginX : page->marginY;
        const int cellSize = horizontal ? page->cellWidth : page->cellHeight;
        const int origin = (horizontal ? page->workArea.left : page->workArea.top) + margin;

        for (int i = 0; i < count; ++i)
        {
            const int cellLeft = origin + GetGridAxisOffset(*page, i, horizontal);
            const int cellRight = cellLeft + cellSize;
            if (coordinate < cellRight)
                return i;
        }
        return count - 1;
    };

    cell.column = axisIndexForDrag(true);
    cell.row = axisIndexForDrag(false);
    return cell;
}

/**
 * @brief 检查网格区域是否被未选中的项目或组件占据。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 被占据返回 true。
 */
bool DesktopApp::IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const
{
    for (const auto& item : items_)
    {
        if (item.selected || item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (item.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = item.gridCell.column + std::max(1, item.gridSpan.columns);
        const int bottom2 = item.gridCell.row + std::max(1, item.gridSpan.rows);
        if (cell.column < right2 && right1 > item.gridCell.column &&
            cell.row < bottom2 && bottom1 > item.gridCell.row)
            return true;
    }
    for (const auto& w : widgets_)
    {
        if (!WidgetOccupiesGridSlots(w))
            continue;
        if (w.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = w.gridCell.column + std::max(1, w.gridSpan.columns);
        const int bottom2 = w.gridCell.row + std::max(1, w.gridSpan.rows);
        if (cell.column < right2 && right1 > w.gridCell.column &&
            cell.row < bottom2 && bottom1 > w.gridCell.row)
            return true;
    }
    return false;
}

/**
 * @brief 构建选中项目的移动计划（将选中项目平移到目标单元格并保持相对位置）。
 * @param targetCell 目标单元格。
 * @return 移动计划列表，无法移动时返回空列表。
 */
std::vector<DesktopApp::PendingGridMove> DesktopApp::BuildSelectedMove(
    GridCell targetCell, bool allowOverlap) const
{
    std::vector<PendingGridMove> moves;
    std::vector<size_t> selectedIndexes;
    int minColumn = INT_MAX, minRow = INT_MAX;
    int maxColumn = 0, maxRow = 0;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].selected)
        {
            selectedIndexes.push_back(i);
            minColumn = std::min(minColumn, items_[i].gridCell.column);
            minRow = std::min(minRow, items_[i].gridCell.row);
            maxColumn = std::max(maxColumn, items_[i].gridCell.column + std::max(1, items_[i].gridSpan.columns));
            maxRow = std::max(maxRow, items_[i].gridCell.row + std::max(1, items_[i].gridSpan.rows));
        }
    }
    if (selectedIndexes.empty()) return moves;

    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return moves;

    const int groupColumns = std::max(1, maxColumn - minColumn);
    const int groupRows = std::max(1, maxRow - minRow);
    const bool stacked = (groupColumns == 1 && groupRows == 1 && selectedIndexes.size() > 1);
    const int spreadCols = stacked ? std::min(static_cast<int>(selectedIndexes.size()), page->columns) : groupColumns;
    const bool hasLargeIcon = std::any_of(selectedIndexes.begin(), selectedIndexes.end(),
        [&](size_t index) { return items_[index].largeIcon.has_value(); });
    if (hasLargeIcon && (targetCell.column < 0 || targetCell.row < 0 ||
        targetCell.column + spreadCols > page->columns || targetCell.row + groupRows > page->rows)) return moves;
    targetCell.column = std::clamp(targetCell.column, 0, std::max(0, page->columns - spreadCols));
    targetCell.row = std::clamp(targetCell.row, 0, std::max(0, page->rows - groupRows));

    int seqIndex = 0;
    std::unordered_set<std::wstring> usedSlots;
    for (size_t itemIndex : selectedIndexes)
    {
        GridCell movedCell = targetCell;
        if (stacked)
        {
            for (int attempt = 0; attempt < page->columns * page->rows; ++attempt)
            {
                int col = seqIndex / page->rows;
                int row = seqIndex % page->rows;
                GridCell probe = targetCell;
                probe.column += col;
                probe.row += row;
                ++seqIndex;
                std::wstring slotKey = probe.pageId + L":" + std::to_wstring(SlotFromCell(gridPages_, probe));
                if (probe.column <= page->columns - 1 && probe.row <= page->rows - 1 &&
                    !usedSlots.contains(slotKey) &&
                    !IsGridAreaOccupiedByUnselected(probe, items_[itemIndex].gridSpan))
                {
                    movedCell = probe;
                    usedSlots.insert(slotKey);
                    break;
                }
            }
        }
        else
        {
            movedCell.column += items_[itemIndex].gridCell.column - minColumn;
            movedCell.row += items_[itemIndex].gridCell.row - minRow;
        }

        if (!IsGridAreaValid(movedCell, items_[itemIndex].gridSpan) ||
            movedCell.column + items_[itemIndex].gridSpan.columns > page->columns ||
            movedCell.row + items_[itemIndex].gridSpan.rows > page->rows ||
            (!allowOverlap &&
                IsGridAreaOccupiedByUnselected(movedCell, items_[itemIndex].gridSpan)))
        {
            moves.clear();
            return moves;
        }
        moves.push_back({ itemIndex, movedCell });
    }
    return moves;
}

/**
 * @brief 寻找最佳的放置单元格（优先沿拖拽方向查找空闲位置）。
 * @param targetCell 初始目标单元格。
 * @return 最佳的可用单元格。
 */
GridCell DesktopApp::FindBestDropCell(const DragSourceList& sourceList, GridCell targetCell) const
{
    const bool exactPlacement = !containers_.empty() &&
        sourceList.origin == containers_.front().get() &&
        std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
            [&](const DragSourceEntry& entry) {
                return entry.kind == DropSourceKind::DesktopIcon && !entry.fromDock &&
                    entry.desktopIndex < items_.size() &&
                    items_[entry.desktopIndex].largeIcon.has_value();
            });
    const POINT current = dragSession_.CurrentPoint();
    const POINT mouseDown = dragSession_.MouseDownPoint();
    const auto direction =
        snowdesktop::desktop_drop_cache::
            ResolveSearchDirection(
                static_cast<long long>(current.x) -
                    mouseDown.x,
                static_cast<long long>(current.y) -
                    mouseDown.y);
    const snowdesktop::desktop_drop_cache::BestCellKey cacheKey{
        targetCell,
        direction,
        dragSession_.StaticSceneRevision() };
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    return snowdesktop::desktop_drop_cache::FindBestCell(
        bestDropCellCache_, dragSession_.IsActive(), cacheKey,
        exactPlacement, page ? page->columns : 0, page ? page->rows : 0,
        [this](const GridCell& cell) { return !BuildSelectedMove(cell).empty(); });
}

/**
 * @brief 就近原则兜底①：精确落点整组放不下时，在目标页内按曼哈顿距离
 *        从近到远搜索第一个可整体放置的单元。
 */
std::vector<DesktopApp::PendingGridMove> DesktopApp::BuildNearestSelectedMove(
    GridCell targetCell) const
{
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return {};

    std::vector<PendingGridMove> best;
    int bestDistance = INT_MAX;
    for (int r = 0; r < page->rows; ++r)
    {
        for (int c = 0; c < page->columns; ++c)
        {
            GridCell probe{ targetCell.pageId, c, r };
            const int distance =
                std::abs(r - targetCell.row) +
                std::abs(c - targetCell.column);
            if (distance >= bestDistance) continue;
            std::vector<PendingGridMove> moves =
                BuildSelectedMove(probe);
            if (moves.empty()) continue;
            best = std::move(moves);
            bestDistance = distance;
        }
    }
    return best;
}

/**
 * @brief 就近原则兜底②：整组放置失败时，把每个被拖图标逐项就近放到
 *        最近空闲单元（优先落在拖放目标页），避免整个拖拽“跳回原处”。
 * @return 逐项移动计划（可能只覆盖能被放置的部分项）。
 */
std::vector<DesktopApp::PendingGridMove> DesktopApp::BuildFallbackPerItemMove(
    const DragSourceList& sourceList, GridCell targetCell) const
{
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (WidgetOccupiesGridSlots(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (item.selected) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    std::vector<const DragSourceEntry*> entries;
    for (const auto& entry : sourceList.entries)
        if (entry.desktopIndex < items_.size())
            entries.push_back(&entry);
    std::sort(entries.begin(), entries.end(),
        [&](const DragSourceEntry* a, const DragSourceEntry* b) {
            return items_[a->desktopIndex].slot <
                items_[b->desktopIndex].slot;
        });

    const std::wstring preferredPage = !targetCell.pageId.empty()
        ? targetCell.pageId
        : (GetFirstPageGridPage() ? GetFirstPageGridPage()->id : L"");
    std::vector<PendingGridMove> moves;
    for (const auto* entry : entries)
    {
        GridSpan span{
            std::max(1, entry->originalSpan.columns),
            std::max(1, entry->originalSpan.rows)
        };
        GridCell freeCell;
        const int startSlot =
            SlotFromCell(gridPages_, items_[entry->desktopIndex].gridCell);
        if (!TryFindFreeCell(span, usedSlots, freeCell, preferredPage, startSlot))
            continue;
        MarkGridArea(usedSlots, freeCell, span);
        moves.push_back({ entry->desktopIndex, freeCell });
    }
    return moves;
}

/**
 * @brief 桌面插入顺延：被拖图标已精确落到目标格（可能与被占格重叠）后，
 *        把同页被挤到/冲突的未选中图标沿读取顺序向后顺延到最近的空位，
 *        实现“把一个图标插到另一个图标下面”的原生手感。
 * @param sourceList 被拖源（用于识别被拖图标，避免自己挤自己）。
 */
void DesktopApp::ResolveDesktopInsertionCollisions(
    const DragSourceList& sourceList)
{
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (WidgetOccupiesGridSlots(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    std::unordered_set<size_t> draggedIndexes;
    for (const auto& entry : sourceList.entries)
    {
        if (entry.desktopIndex >= items_.size()) continue;
        draggedIndexes.insert(entry.desktopIndex);
        const auto& item = items_[entry.desktopIndex];
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    std::map<std::wstring, std::vector<size_t>> byPage;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (draggedIndexes.count(i)) continue;
        const auto& item = items_[i];
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        byPage[item.gridCell.pageId].push_back(i);
    }

    for (auto& [pageId, indexes] : byPage)
    {
        std::sort(indexes.begin(), indexes.end(),
            [&](size_t a, size_t b) {
                return items_[a].slot < items_[b].slot;
            });
        for (const size_t i : indexes)
        {
            auto& item = items_[i];
            GridSpan span{
                std::max(1, item.gridSpan.columns),
                std::max(1, item.gridSpan.rows)
            };
            if (!AreGridSlotsMarked(
                    usedSlots, item.gridCell, span))
            {
                MarkGridArea(usedSlots, item.gridCell, span);
                continue;
            }
            GridCell freeCell;
            if (TryFindFreeCell(span, usedSlots, freeCell,
                    item.gridCell.pageId, item.slot))
            {
                item.gridCell = freeCell;
                item.slot = SlotFromCell(gridPages_, freeCell);
                MarkGridArea(usedSlots, freeCell, span);
            }
            else
            {
                // 满屏无空位：保留原处（极少数情形）
                MarkGridArea(usedSlots, item.gridCell, span);
            }
        }
    }
}

/**
 * @brief 将选中的项目移动到目标网格单元格。
 * @param targetCell 目标单元格。
 */
void DesktopApp::MoveSelectedItemsToCell(GridCell targetCell)
{
    std::vector<PendingGridMove> moves = BuildSelectedMove(std::move(targetCell));
    if (moves.empty()) return;
    for (const auto& move : moves)
    {
        items_[move.index].gridCell = move.cell;
        items_[move.index].slot = SlotFromCell(gridPages_, move.cell);
    }
    LayoutItems();
    SaveLayoutSlots();
}

/**
 * @brief 获取桌面项目真实路径的扩展名（大写，含点号）。
 *
 * 快捷方式的显示名不含真实扩展名，必须经 PIDL 解析出实际文件路径。
 */
static std::wstring CollectItemExtensionUpper(const DesktopItem& item)
{
    wchar_t path[MAX_PATH]{};
    if (item.absolutePidl.get() &&
        SHGetPathFromIDListW(item.absolutePidl.get(), path))
        return ToUpperInvariant(PathFindExtensionW(path));
    return ToUpperInvariant(PathFindExtensionW(item.name.c_str()));
}

/**
 * @brief 判断桌面项目是否为文件系统上的真实文件夹。
 */
static bool IsCollectFolderItem(const DesktopItem& item)
{
    wchar_t path[MAX_PATH]{};
    if (!item.absolutePidl.get() ||
        !SHGetPathFromIDListW(item.absolutePidl.get(), path))
        return false;
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/**
 * @brief 判断桌面项目是否命中收录分类（folders 按目录属性，其余按扩展名规则）。
 */
static bool IsCollectCategoryMatch(
    const DesktopItem& item, const CategorySettings& categories,
    const std::wstring& categoryId)
{
    if (categoryId == L"folders")
        return IsCollectFolderItem(item);
    return CategoryIdForExtension(categories,
        CollectItemExtensionUpper(item)) == categoryId;
}

bool DesktopApp::IsCategoryMatchForItem(const DesktopItem& item,
    const std::wstring& categoryId) const
{
    return IsCollectCategoryMatch(item, GetCategorySettings(), categoryId);
}

/**
 * @brief 收录：把全部分类匹配的顶层桌面文件移动到目标页。
 *
 * 与“桌面文件”组件的无差别收集不同，收录只移动与所选分类匹配的
 * 文件，且无论它们当前位于哪个页面，都汇聚到目标页，实现按类型的
 * 页面级整理。目标页空间不足时放不下的文件保留原位（部分收录）。
 */
void DesktopApp::MoveCategoryItemsToPage(const std::wstring& categoryId,
    const std::wstring& targetPageId)
{
    if (categoryId.empty() || targetPageId.empty()) return;
    const GridPage* targetPage = nullptr;
    for (const auto& page : gridPages_)
    {
        if (page.id == targetPageId)
        {
            targetPage = &page;
            break;
        }
    }
    if (!targetPage) return;

    // 标记所有顶层项与组件的占用格（收录移动会跨页腾挪）。
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& widget : widgets_)
        if (WidgetOccupiesGridSlots(widget))
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (item.gridCell.pageId == kDockPageId) continue;
        if (IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    // 收集匹配项：保持现有 slot 顺序，目标页上的项无需移动。
    std::vector<size_t> matches;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        const auto& item = items_[i];
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (item.gridCell.pageId == kDockPageId ||
            item.gridCell.pageId == targetPageId)
            continue;
        if (IsItemInAnyWidget(item)) continue;
        if (!IsCollectCategoryMatch(item, GetCategorySettings(), categoryId))
            continue;
        matches.push_back(i);
    }
    if (matches.empty()) return;
    std::sort(matches.begin(), matches.end(),
        [&](size_t a, size_t b) { return items_[a].slot < items_[b].slot; });

    // 页内顺序找空位（不溢出到其他页）；大图标跨度需先收敛到页宽高内。
    int nextSlot = 0;
    const int capacity = std::max(1, targetPage->columns * targetPage->rows);
    for (const size_t i : matches)
    {
        auto& item = items_[i];
        GridSpan span{
            std::max(1, item.gridSpan.columns),
            std::max(1, item.gridSpan.rows)
        };
        span.columns = std::clamp(span.columns, 1,
            std::max(1, targetPage->columns));
        span.rows = std::clamp(span.rows, 1,
            std::max(1, targetPage->rows));
        bool placed = false;
        for (int slot = nextSlot; slot < capacity && !placed; ++slot)
        {
            GridCell candidate;
            candidate.pageId = targetPageId;
            candidate.column = slot / std::max(1, targetPage->rows);
            candidate.row = slot % std::max(1, targetPage->rows);
            if (candidate.column + span.columns <= targetPage->columns &&
                candidate.row + span.rows <= targetPage->rows &&
                !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                item.gridCell = candidate;
                item.slot = SlotFromCell(gridPages_, candidate);
                MarkGridArea(usedSlots, candidate, span);
                nextSlot = slot + 1;
                placed = true;
            }
        }
        // 目标页已满：保留原位（部分收录）。
    }

    LayoutItems();
    SaveLayoutSlots();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

std::wstring DesktopApp::GetCategoryClaimOwner(
    const std::wstring& categoryId) const
{
    const auto it = categoryClaims_.find(categoryId);
    if (it == categoryClaims_.end()) return {};
    // 占用页可能已被删除：视为无占用（持久化条目在下次写入时清理）。
    if (std::find(savedPageIds_.begin(), savedPageIds_.end(), it->second) ==
        savedPageIds_.end())
        return {};
    return it->second;
}

void DesktopApp::SetCategoryClaim(const std::wstring& categoryId,
    const std::wstring& pageId)
{
    if (categoryId.empty()) return;
    if (pageId.empty())
        categoryClaims_.erase(categoryId);
    else
        categoryClaims_[categoryId] = pageId;
    SaveCategoryClaims(GetCategoryClaimsPath().c_str(), categoryClaims_);
}

void DesktopApp::ToggleCategoryCollection(const std::wstring& categoryId,
    const std::wstring& targetPageId)
{
    if (categoryId.empty() || targetPageId.empty()) return;
    if (GetCategoryClaimOwner(categoryId) == targetPageId)
    {
        // 取消收录：只解除占用关系，文件留在原地不做搬移——
        // 避免“文件突然搬家”打断用户，分类仍可随时重新勾选。
        SetCategoryClaim(categoryId, {});
    }
    else
    {
        MoveCategoryItemsToPage(categoryId, targetPageId);
        SetCategoryClaim(categoryId, targetPageId);
    }
    // 收录/取消会改变轻提示的待归位数量。
    InvalidatePageDotsIndicator();
}

/**
 * @brief 更新拖拽组的原点位置（用于计算拖拽时的偏移）。
 */
void DesktopApp::UpdateDragGroupOrigin()
{
    int minCol = INT_MAX, minRow = INT_MAX;
    std::wstring anchorPageId;
    for (const auto& item : items_)
    {
        if (item.selected)
        {
            if (anchorPageId.empty()) anchorPageId = item.gridCell.pageId;
            minCol = std::min(minCol, item.gridCell.column);
            minRow = std::min(minRow, item.gridCell.row);
        }
    }
    GridCell groupOrigin;
    const GridPage* firstPage = GetFirstPageGridPage();
    groupOrigin.pageId = anchorPageId.empty()
        ? (firstPage ? firstPage->id : L"")
        : anchorPageId;
    groupOrigin.column = minCol != INT_MAX ? minCol : 0;
    groupOrigin.row = minRow != INT_MAX ? minRow : 0;
    RECT groupRect = GetGridRect(gridPages_, groupOrigin, GridSpan{});
    dragGroupOriginX_ = groupRect.left;
    dragGroupOriginY_ = groupRect.top;
}

/**
 * @brief 将选中的项目迁移到最后一个监视器页面。
 */
void DesktopApp::MigrateSelectedItemsToLastMonitorPage()
{
    // Page turning during a large-icon drag is a preview only. Retain the
    // entire group's source cells/spans until its accepted drop commits them;
    // this also makes cancellation leave the original layout intact.
    if (std::any_of(items_.begin(), items_.end(), [](const auto& item) { return item.selected && item.largeIcon; }))
        return;
    if (gridPages_.empty() || lastMonitorPageId_.empty())
        return;
    const GridPage* targetPage = FindGridPage(gridPages_, lastMonitorPageId_);
    if (!targetPage) return;

    // 整组保形迁移：与松手落位（BuildDesktopLandings 多选分支）复用同一套
    // 规划——把整组按内部相对位置当一块安置到目标页的空闲区域。逐项放置
    // 在目标页有占用时会各自散开、再被落位固化，是跨页乱序的根源；整组
    // 规划保证迁移结果与最终落位一致，顺序不会乱。
    GridCell groupAnchor;
    groupAnchor.pageId = lastMonitorPageId_;
    int minColumn = INT_MAX, minRow = INT_MAX;
    for (const auto& item : items_)
    {
        if (!item.selected) continue;
        minColumn = std::min(minColumn, item.gridCell.column);
        minRow = std::min(minRow, item.gridCell.row);
    }
    if (minColumn == INT_MAX) return;
    groupAnchor.column = std::min(minColumn, std::max(0, targetPage->columns - 1));
    groupAnchor.row = std::min(minRow, std::max(0, targetPage->rows - 1));

    std::vector<PendingGridMove> moves = BuildSelectedMove(groupAnchor);
    if (moves.empty())
        moves = BuildNearestSelectedMove(groupAnchor);
    if (moves.empty())
    {
        // 目标页整组放不下：保留原位（与落位失败行为一致）。
        return;
    }

    for (const auto& move : moves)
    {
        auto& item = items_[move.index];
        item.gridCell = move.cell;
    }
}

/**
 * @brief 获取拖拽目标点的屏幕坐标。
 * @param current 当前鼠标位置。
 * @return 拖拽目标点。
 */
