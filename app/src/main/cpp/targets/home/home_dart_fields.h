/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace home_layout {

/*
 * Recover the Dart field offsets the geometry hooks read, from the launcher's own code.
 *
 * The hooks need a handful of `GridInfo` field offsets plus the drop-back item's two cell
 * indices. Those live in the Dart heap: no symbol names them, no relocation pins them down,
 * and there is nothing to xref. The only thing that determines them is the code that reads
 * them, and the drop-back target reads every one. The compiler emits each read as
 * `ldur <size> Rt, [xRn, #imm9]`, so the offset sits in the instruction stream next to the
 * arithmetic that gives it meaning.
 *
 * A read is identified by what the code *does* with the value, never by the offset it
 * happens to carry:
 *
 *   - the origin cell is dereferenced for its X coordinate immediately after the load;
 *   - the cell width is the only load whose value a call converts to a double;
 *   - the row count is stored straight back out, which is what a count the caller keeps is for;
 *   - the column count is read, and two instructions later the item's own column and row are
 *     loaded from the same base -- that pairing is the bounds check the two are compared in,
 *     and it is what distinguishes the count from the two other reads of the same field.
 *
 * The immediate is the answer, never the search key. Each shape must match exactly once, or
 * the field stays unknown and the caller leaves the hook off: a launcher that renumbers its
 * fields then stops applying the adjustment, which is the right outcome, whereas a stored
 * offset would have kept pointing at whatever moved into that slot.
 *
 * This is deliberately narrow. It knows the access forms these reads take and nothing more
 * general about Dart layout; it is a resolver for one function's fields, not a general
 * Dart offset facility.
 */
struct GridFieldOffsets {
    int32_t columns = -1;
    int32_t rows = -1;
    int32_t origin = -1;
    int32_t item_col = -1;
    int32_t item_row = -1;
};

/*
 * The cell width and height are deliberately absent.
 *
 * Neither can be pinned down from this body. The height is never loaded at all, and the width
 * is loaded once but shares its shape -- a narrow load whose value a call converts -- with four
 * other fields, and nothing in the surrounding code separates them: they sit at no fixed
 * distance from the origin cell, from the column count, or from each other. A rule that picked
 * one of them would be a guess wearing a derivation's clothes, and the consequence would be a
 * silent comparison against the wrong field.
 *
 * The hooks only ever use these two to *check* the frame-pointer copy against the GridInfo
 * field, so leaving them unresolved costs the check rather than the adjustment: the caller
 * compares what the frame pointer holds against itself, and the geometry it applies comes from
 * the local values it computed. See kDropGeometryHasNoResolvedCellSize.
 */
inline constexpr bool kDropGeometryHasNoResolvedCellSize = true;

/*
 * The cell size fields, for the one path that reads them off the GridInfo rather than off a
 * local. They are the two offsets `read_grid_field_offsets` cannot resolve, and they are named
 * here so that their absence is a documented fact rather than an omission that looks like an
 * oversight. See kDropGeometryHasNoResolvedCellSize above for why no scan produces them.
 *
 * The consequence if a launcher renumbers them: an occupied cell's size is read from the wrong
 * place, the value is not finite or not positive, `inset_workspace` rejects it, and the cell
 * keeps the geometry the stock layout gave it. The failure is a no-op on one code path, not a
 * misplaced icon -- `inset_workspace` range-checks every dimension it is handed before use.
 */
inline constexpr int32_t kCellSizeWidth = 0x2b;
inline constexpr int32_t kCellSizeHeight = 0x33;

/*
 * Distance between the two cell counts, in bytes.
 *
 * The hooks read the column and row counts as 64-bit values and the launcher stores them as a
 * pair, so one count follows the other exactly one slot on. This is a property of the field
 * pair rather than of any particular build, which is what makes it usable as a relation: a
 * launcher that widened or reordered the pair changes the gap and the field stops resolving.
 */
inline constexpr int32_t kCountSlotBytes = 8;

/*
 * Encoding predicates, taken from the disassembly of launcher 7722 rather than from the
 * architecture manual: the `size` field selects the access width and its encoding here does
 * not line up with the manual's naming, so these compare the exact words that were observed.
 *
 *   ldur wRt,[xRn,#imm9]  ->  0xB8400000, size bits 10
 *   ldur xRt,[xRn,#imm9]  ->  0xF8400000, size bits 11
 *   ldur dRt,[xRn,#imm9]  ->  0xFC400000, size bits 11
 *   add xRt,xRt,x28,lsl#32
 */
inline bool dart_ldur_w(uint32_t w, uint32_t rn, uint32_t rt, int32_t *out_imm) {
    if ((w & 0xFFC00000u) != 0xB8400000u) return false;
    if (((w >> 30) & 3u) != 2u) return false;
    if (((w >> 5) & 0x1Fu) != rn || (w & 0x1Fu) != rt) return false;
    const uint32_t raw = (w >> 12) & 0x1FFu;
    if (out_imm != nullptr) {
        *out_imm = (raw & 0x100u) ? static_cast<int32_t>(raw) - 0x200
                                  : static_cast<int32_t>(raw);
    }
    return true;
}

inline bool dart_ldur_x(uint32_t w, uint32_t rn, uint32_t rt, int32_t *out_imm) {
    if ((w & 0xFFC00000u) != 0xF8400000u) return false;
    if (((w >> 30) & 3u) != 3u) return false;
    if (((w >> 5) & 0x1Fu) != rn || (w & 0x1Fu) != rt) return false;
    const uint32_t raw = (w >> 12) & 0x1FFu;
    if (out_imm != nullptr) {
        *out_imm = (raw & 0x100u) ? static_cast<int32_t>(raw) - 0x200
                                  : static_cast<int32_t>(raw);
    }
    return true;
}

inline bool dart_ldur_d(uint32_t w, uint32_t rn, uint32_t rt, int32_t *out_imm) {
    if ((w & 0xFFC00000u) != 0xFC400000u) return false;
    if (((w >> 30) & 3u) != 3u) return false;
    if (((w >> 5) & 0x1Fu) != rn || (w & 0x1Fu) != rt) return false;
    const uint32_t raw = (w >> 12) & 0x1FFu;
    if (out_imm != nullptr) {
        *out_imm = (raw & 0x100u) ? static_cast<int32_t>(raw) - 0x200
                                  : static_cast<int32_t>(raw);
    }
    return true;
}

inline bool dart_sign_extend_pair(uint32_t w, uint32_t rt) {
    return (w & 0xFFFFFC00u) == 0x8B1C8000u && (w & 0x1Fu) == rt;
}

inline bool dart_is_bl(uint32_t w) { return (w & 0xFC000000u) == 0x94000000u; }

inline bool dart_stur_w(uint32_t w) {
    return (w & 0xFFC00000u) == 0xB8000000u && ((w >> 30) & 3u) == 2u;
}

/* Only positive, plausibly-word-aligned field offsets are considered. */
inline bool dart_plausible_field(int32_t imm) { return imm > 0 && imm <= 0x400; }

/*
 * Read every offset out of one function body.
 *
 * `body` must be the whole function, not a slice: the shapes below are "exactly one read in
 * the body", so a truncated view would report a field as unique when it is merely the only one
 * left in the window. `frame_pointer` is the register the Dart AOT frame keeps locals under.
 */
inline GridFieldOffsets read_grid_field_offsets(const std::vector<uint32_t> &body,
    uint32_t frame_pointer = 29) {
    GridFieldOffsets out;
    const size_t n = body.size();

    size_t origin_hits = 0, chain_hits = 0;
    std::vector<int32_t> stored;   // candidates for the row count
    for (size_t i = 0; i + 2 < n; ++i) {
        int32_t imm = 0;
        if (!dart_ldur_w(body[i], 0, 1, &imm) || !dart_plausible_field(imm)) continue;
        if (!dart_sign_extend_pair(body[i + 1], 1)) continue;
        // origin: the cell's X coordinate is read straight out of the loaded value.
        int32_t slot = 0;
        if (dart_ldur_d(body[i + 2], 1, 0, &slot) && slot == 7) {
            ++origin_hits;
            out.origin = imm;
        }
        // rows: stored straight back out, which is what a count the caller keeps is for.
        if (dart_stur_w(body[i + 2])) stored.push_back(imm);
    }
    // columns + the item's own indices: the six-instruction bounds check.
    for (size_t i = 0; i + 4 < n; ++i) {
        int32_t cnt = 0, col = 0, row = 0;
        if (dart_ldur_w(body[i], 0, 1, &cnt) && dart_plausible_field(cnt)
            && dart_sign_extend_pair(body[i + 1], 1)
            && dart_ldur_x(body[i + 2], frame_pointer, 0, nullptr)
            && dart_ldur_x(body[i + 3], 0, 2, &col)
            && dart_ldur_x(body[i + 4], 0, 3, &row)
            && dart_plausible_field(col) && dart_plausible_field(row)) {
            ++chain_hits;
            out.columns = cnt;
            out.item_col = col;
            out.item_row = row;
        }
    }
    /*
     * The row count is tied to the column count by what the two *are*: a pair of cell counts in
     * the same object, one 64-bit slot apart. The store shape alone does not single it out --
     * two fields are stored that way -- but only one of them sits a slot from the column count,
     * and that relation is what the hooks have always relied on, expressed as a relation rather
     * than as a stored number. If the launcher reorders the pair the gap changes and the field
     * stays unknown, which is the right outcome: the caller then declines instead of reading a
     * neighbouring count.
     */
    if (origin_hits != 1) out.origin = -1;
    if (chain_hits != 1) {
        out.columns = -1;
        out.item_col = -1;
        out.item_row = -1;
    } else {
        size_t adjacent = 0;
        int32_t found = -1;
        for (const int32_t candidate : stored) {
            if (candidate - out.columns != kCountSlotBytes) continue;
            ++adjacent;
            found = candidate;
        }
        out.rows = adjacent == 1 ? found : -1;
    }
    return out;
}

} // namespace home_layout
