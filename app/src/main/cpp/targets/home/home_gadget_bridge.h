/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <cstdint>

namespace home_layout {
// Private protocol, NOT Gadget itemType=5 or PA ServiceDelivery widget_type=5.
// Fits a compressed Dart Smi (31 signed bits); original putInt performs conversion.
inline constexpr uint32_t kClearGadgetWireType = 0x18430c01;
inline constexpr uint32_t kClearGadgetCid = 0x7f2;
inline constexpr uint32_t kClearGadgetIdOffset = 0x11b;
inline constexpr const char *kGadgetSerializer = "AssistantDragDataHelper.putWidgetIntoBundle";
inline constexpr uint32_t kGadgetSerializerSize = 0x8d0;
inline constexpr uint32_t kGadgetSelectOffset = 0x6ac;
inline constexpr uint32_t kGadgetReturnOffset = 0x280;
inline constexpr uint32_t kGadgetPutIntOffset = 0x27c;
inline constexpr uint32_t kGadgetCommonOffset = 0x704;
constexpr bool is_clear_gadget(uint32_t cid, uint32_t id_smi, int64_t x, int64_t y) {
    return cid == kClearGadgetCid && id_smi == 24 && x == 1 && y == 1;
}
struct GadgetBridgeGuard { uint32_t offset; uint32_t words[4]; };
inline constexpr GadgetBridgeGuard kGadgetBridgeGuards[] = {
    {0, {0xa9bf79fd, 0xaa0f03fd, 0xd100c1ef, 0xf81f83b6}},
    {0x268, {0xf85e83a0, 0xf85f03a1, 0x9140db62, 0xf947fc42}},
    {kGadgetReturnOffset, {0xf85e83a0, 0xd2802871, 0xb8716803, 0x8b1c8063}},
    {kGadgetSelectOffset, {0xf11fb43f, 0x540002a1, 0xf85e83a0, 0xf85f03a1}},
    {kGadgetCommonOffset, {0xf85e83a0, 0xb849f003, 0x8b1c8063, 0xf85f03a1}},
};
template <typename Read>
bool gadget_bridge_guards_match(Read read) {
    for (const auto &guard : kGadgetBridgeGuards) {
        uint32_t actual[4]{};
        if (!read(guard.offset, actual)) return false;
        for (unsigned i = 0; i < 4; ++i) if (actual[i] != guard.words[i]) return false;
    }
    return true;
}
} // namespace home_layout
