/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <cstdint>

namespace home_layout {
inline constexpr const char *kWidgetMoveSymbol = "AssistantDragToPAHandler.canDragToPA";
inline constexpr uint32_t kWidgetMoveSize = 0xa04;
inline constexpr uint32_t kWidgetMoveOffset = 0x7f8;
inline constexpr uint32_t kWidgetMoveOriginal = 0x37200500; // tbnz w0,#4, reject
inline constexpr uint32_t kWidgetMoveReplacement = 0xd503201f; // nop; continue to span check
inline constexpr int32_t kWidgetMoveMagic = 0x48435731; // HCW1; appended after custom titles
template <typename Read>
bool read_widget_move_extension(Read read, bool &enabled) {
    int32_t magic = 0, flag = 0;
    enabled = false;
    if (!read(magic)) return true; // Old endpoints have no trailing extension.
    if (magic != kWidgetMoveMagic || !read(flag) || (flag != 0 && flag != 1)) return false;
    enabled = flag == 1;
    return true;
}
struct WidgetMoveGuard { uint32_t offset; uint32_t words[4]; };
inline constexpr WidgetMoveGuard kWidgetMoveGuards[] = {
    {0, {0xa9bf79fd, 0xaa0f03fd, 0xd100a1ef, 0xf81f83a1}},
    // The stack-model path and widget-class range stay unchanged.
    {0x4ac, {0xf85ff040, 0xd34c7c00, 0xf11f9c1f, 0x540015a1}},
    {0x770, {0xd11fb010, 0xf1001a1f, 0x54000fe8, 0xf85e83a3}},
    // Only the isMIUIWidget rejection is replaced. The original size check still runs.
    {0x7f0, {0xb84fb040, 0x8b1c8000, kWidgetMoveOriginal, 0xaa0403e1}},
    {0x800, {0xaa0603e2, 0x940000f0, 0x37200480, 0xf85f83a0}},
    {0x818, {0x9400007b, 0xf85e83a2, 0xb842b040, 0x8b1c8000}},
};
template <typename ReadWords>
bool widget_move_guards_match(ReadWords read) {
    for (const auto &guard : kWidgetMoveGuards) {
        uint32_t words[4]{};
        if (!read(guard.offset, words)) return false;
        for (int i = 0; i < 4; ++i) if (words[i] != guard.words[i]) return false;
    }
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
