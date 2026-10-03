/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <climits>
#include "home_dart_fields.h"
#include "home_folder_geometry.h"

namespace home_layout {
inline constexpr const char *kDropGeometrySymbol =
    "CellLayoutGetxController.calculateCenterGlobalPosition";
/*
 * The two splice windows, identified by the instructions at them. The offsets are not stored:
 * each run is unique in this body, so the scan finds the site and a launcher that shifts it
 * moves the hook with it. See home_dart_fields.h for the field offsets read alongside them.
 */
struct DropGeometrySite {
    uint32_t words[4];
};
inline constexpr DropGeometrySite kDropGeometrySites[] = {
    {{0xb843b001, 0x8b1c8021, 0xfc407020, 0xf85f03a0}},
    {{0x1e610802, 0xfc407020, 0x1e622801, 0xf85f83a1}},
};

/*
 * The GridInfo field offsets this hook reads, resolved once from the drop-back target's own
 * code. See home_dart_fields.h for how each one is identified; the short version is that they
 * are found by what the surrounding instructions do with the value, and any that the code does
 * not pin down stays at -1.
 *
 * A field left unresolved is not fatal. The bounds checks below compare the item's cell
 * indices against the counts, so an unresolved count makes them fail and the adjustment is
 * skipped -- the drag lands where the stock code would have put it. That is the correct
 * degradation: the alternative, reading a neighbouring field, would move the icon somewhere
 * the user did not ask for.
 */
struct DropGeometryFields {
    int32_t columns = -1;
    int32_t rows = -1;
    int32_t origin = -1;
    int32_t item_col = -1;
    int32_t item_row = -1;

    bool usable() const {
        return columns > 0 && rows > 0 && origin >= 0
            && item_col > 0 && item_row > 0;
    }
};

inline DropGeometryFields drop_geometry_fields(const GridFieldOffsets &found) {
    DropGeometryFields out;
    out.columns = found.columns;
    out.rows = found.rows;
    out.origin = found.origin;
    out.item_col = found.item_col;
    out.item_row = found.item_row;
    return out;
}

// x0 is GridInfo after currentConfig returns. The stock code uses its raw
// cell width/height to compute the drop-back center, while RenderBox may
// already have painted the workspace with insets. Replace only local values.
inline bool drop_geometry_body(uintptr_t fp, uint64_t heap, uintptr_t saved,
    unsigned kind, const DropGeometryFields *field, double top, double bottom, double side,
    WorkspaceRenderSnapshot *rendered) {
    const auto d = [saved](int n, double v) {
        workspace_write(saved, 160 + n * 16, v);
        workspace_write(saved, 168 + n * 16, uint64_t{0});
    };
    if (kind == 0) {
        const uintptr_t grid = workspace_read<uintptr_t>(saved, 0);
        if (field == nullptr || !field->usable()) return false;
        const uintptr_t origin = workspace_read<uint32_t>(grid, field->origin)
            + (heap << 32);
        const double raw_x = workspace_read<double>(origin, 7);
        const double raw_width = workspace_read<double>(fp, -0x38);
        const double raw_height = workspace_read<double>(fp, -0x40);
        const int64_t column = workspace_read<int64_t>(fp, -0x10);
        const int64_t row = workspace_read<int64_t>(fp, -0x18);
        double g[4]{};
        /*
         * The cell size is not compared against the GridInfo field here. The frame-pointer
         * copies are what the stock code computed with, and the field they would be checked
         * against cannot be located from this function (see kDropGeometryHasNoResolvedCellSize),
         * so the check would be against a guess. What is still checked is the part that can
         * be: that the item really is inside the grid, using the two counts and the two cell
         * indices that were resolved.
         */
        const int64_t columns = workspace_read<int64_t>(grid, field->columns);
        const int64_t rows = workspace_read<int64_t>(grid, field->rows);
        const bool valid = folder_grid_geometry(grid, top, bottom, side, g, rendered)
            && std::isfinite(raw_x) && std::isfinite(raw_width) && std::isfinite(raw_height)
            && columns >= 1 && rows >= 1
            && column >= 0 && column < columns
            && row >= 0 && row < rows;
        workspace_write(saved, 8, origin);
        d(0, raw_x + (valid ? g[0] : 0));
        workspace_write(saved, 0, column);
        if (valid) {
            workspace_write(fp, -0x38, g[2]);
            workspace_write(fp, -0x40, g[3]);
            if (rendered) rendered->begin_drop(fp, g[1]);
        }
        return valid;
    }
    if (kind == 1) {
        // Replay fmuls/ldur/fadd/ldur from +1b8 and add the same rendered
        // top inset that was used for X's stride in this frame.
        const uintptr_t origin = workspace_read<uintptr_t>(saved, 8);
        const double row = workspace_read<double>(saved, 160);
        const double height = workspace_read<double>(saved, 176);
        const double origin_y = workspace_read<double>(origin, 7);
        const double rendered_top = rendered ? rendered->finish_drop(fp, 0) : 0;
        const double product = row * height;
        d(2, product);
        d(0, origin_y);
        d(1, origin_y + product + rendered_top);
        workspace_write(saved, 8, workspace_read<uintptr_t>(fp, -8));
        return rendered_top != 0;
    }
    return false;
}
} // namespace home_layout
