/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace home_layout {
inline constexpr const char *kWidgetMoveSymbol = "AssistantDragToPAHandler.canDragToPA";

// The patched branch: `tbnz w0,#4, reject` that fires for an MIUI widget. Found by scan, not
// stored -- see widget_move_gate_offset(). The two words below are the only ones this file
// names, and they are matched against, never written into an address.
inline constexpr uint32_t kWidgetMoveOriginal = 0x37200500; // tbnz w0,#4, reject
inline constexpr uint32_t kWidgetMoveReplacement = 0xd503201f; // nop; continue to span check
inline constexpr int32_t kWidgetMoveMagic = 0x48435731; // HCW1; appended after custom titles

/*
 * Locate the isMIUIWidget rejection inside the gate function.
 *
 * Four `tbnz w0,#4` sites in this body load a flag byte from the dragged object and test one
 * bit of it, so the instruction shape alone is not a determination -- an earlier version of
 * this hook picked the first match and patched the wrong branch. What separates the widget
 * test is that its flag is the last byte of the object's own storage: every other site reads
 * a small field near the start of the same struct. Requiring exactly one site keeps the scan
 * from silently choosing, and SIZE_MAX means "leave the original code alone".
 *
 * `flag_field` is the offset that test reads, learned here rather than assumed; a launcher that
 * reorders the struct simply stops matching and the hook goes inert instead of misfiring.
 */
template <typename ReadWords>
size_t widget_move_gate_offset(size_t size, ReadWords read, int32_t *flag_field = nullptr) {
    std::vector<uint32_t> code;
    if (size == 0 || size > (1u << 20) || !read(size, code)) return SIZE_MAX;
    size_t found = SIZE_MAX;
    int32_t learned = 0;
    for (size_t i = 2; i < code.size(); ++i) {
        const uint32_t branch = code[i];
        if ((branch >> 24) != 0x37u || (branch & 0x1Fu) != 0u
            || ((branch >> 19) & 0x1Fu) != 4u) continue;
        const uint32_t load = code[i - 2];
        // ldur w0,[x2,#imm9]: the flag byte, with its base pinned to the object register the
        // drag handlers pass around.
        if ((load & 0xFFE00C00u) != 0xB8400000u || (load & 0x1Fu) != 0u) continue;
        if (((load >> 5) & 0x1Fu) != 2u) continue;
        uint32_t raw = (load >> 12) & 0x1FFu;
        const int32_t field = (raw & 0x100u) ? static_cast<int32_t>(raw) - 0x200
                                             : static_cast<int32_t>(raw);
        if (field < 0x80) continue;  // the trailing flag, not a leading struct field
        // The sign-extension pair that completes the narrow load; matched locally so this
        // header stays independent of the Gadget bridge's instruction helpers.
        if ((code[i - 1] & 0xFFFFFC00u) != 0x8B1C8000u) continue;
        if (found != SIZE_MAX) return SIZE_MAX;  // ambiguous
        found = i * 4;
        learned = field;
    }
    if (flag_field != nullptr) *flag_field = learned;
    return found;
}

template <typename Read>
bool read_widget_move_extension(Read read, bool &enabled) {
    int32_t magic = 0, flag = 0;
    enabled = false;
    if (!read(magic)) return true; // Old endpoints have no trailing extension.
    if (magic != kWidgetMoveMagic || !read(flag) || (flag != 0 && flag != 1)) return false;
    enabled = flag == 1;
    return true;
}


// One aligned instruction: no trampoline, Dart stack/heap edit, allocation or timer.
// Reject foreign bytes; undo even a writer that changed memory before returning failure.
template <typename Read, typename Write>
bool apply_widget_move_word(uintptr_t address, bool enabled, Read read, Write write) {
    uint32_t before = 0, after = 0;
    if (!address || (address & 3) || !read(address, before)
        || (before != kWidgetMoveOriginal && before != kWidgetMoveReplacement)) return false;
    const uint32_t wanted = enabled ? kWidgetMoveReplacement : kWidgetMoveOriginal;
    if (before == wanted) return true;
    if (write(address, wanted) && read(address, after) && after == wanted) return true;
    (void) write(address, before);
    return false;
}
} // namespace home_layout
