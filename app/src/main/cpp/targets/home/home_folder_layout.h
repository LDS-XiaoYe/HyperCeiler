/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace home_layout {
// Appended protocol extension: old endpoints/caches retain stock folder geometry.
constexpr int32_t kFolderLayoutMagic = 0x48434631; // HCF1
struct FolderLayoutConfig {
    int title_center = 0, full_width = 0, padding_enabled = 0;
    int phone_padding = 0, landscape_padding = 0, portrait_padding = 0;
    int tablet = 0;
    bool operator==(const FolderLayoutConfig &) const = default;
};
inline bool valid_folder_layout(const FolderLayoutConfig &c) {
    const auto flag = [](int n) { return n == 0 || n == 1; };
    return flag(c.title_center) && flag(c.full_width) && flag(c.padding_enabled)
        && flag(c.tablet) && c.phone_padding >= 0 && c.phone_padding <= 50
        && c.landscape_padding >= 0 && c.landscape_padding <= 450
        && c.portrait_padding >= 0 && c.portrait_padding <= 200;
}
template<class Read> bool read_folder_layout(Read read, FolderLayoutConfig &out, bool optional = true) {
    int32_t magic = 0;
    if (!read(magic)) { if (optional) { out = {}; return true; } return false; }
    if (magic != kFolderLayoutMagic) return false;
    FolderLayoutConfig c;
    for (int *p : {&c.title_center, &c.full_width, &c.padding_enabled, &c.phone_padding,
            &c.landscape_padding, &c.portrait_padding, &c.tablet}) {
        int32_t n = 0; if (!read(n)) return false; *p = n;
    }
    if (!valid_folder_layout(c)) return false;
    out = c; return true;
}
// Single release/acquire word: a Dart layout never sees a torn gate/padding/column snapshot.
inline uint64_t pack_folder_layout(const FolderLayoutConfig &c, int cols) {
    if (!valid_folder_layout(c) || cols < 3 || cols > 6) return 0;
    return uint64_t(c.title_center) | (uint64_t(c.full_width) << 1)
        | (uint64_t(c.padding_enabled) << 2) | (uint64_t(c.tablet) << 3)
        | (uint64_t(c.phone_padding) << 4) | (uint64_t(c.landscape_padding) << 10)
        | (uint64_t(c.portrait_padding) << 19) | (uint64_t(cols) << 27);
}
inline FolderLayoutConfig unpack_folder_layout(uint64_t p) {
    return {int(p & 1), int((p >> 1) & 1), int((p >> 2) & 1), int((p >> 4) & 63),
        int((p >> 10) & 511), int((p >> 19) & 255), int((p >> 3) & 1)};
}
inline double folder_cell_width(double original, double width, double height,
        double gap, uint64_t packed) {
    const auto c = unpack_folder_layout(packed);
    const int cols = int((packed >> 27) & 7);
    if (!c.full_width || cols < 3 || cols > 6 || !std::isfinite(width)
        || !std::isfinite(height) || !std::isfinite(gap)
        || width < 100 || width > 4000 || height < 100 || height > 5000
        || gap < 0 || gap > 200) return original;
    double padding = c.padding_enabled ? c.phone_padding : 0;
    if (c.tablet && c.padding_enabled)
        padding = width > height ? c.landscape_padding : c.portrait_padding;
    // Keep positive cells even with an excessive tablet inset, without changing stored settings.
    padding = std::clamp(padding, 0.0, std::max(0.0, (width - cols * 24.0 - (cols - 1) * gap) / 2));
    const double value = (width - 2 * padding - (cols - 1) * gap) / cols;
    return std::isfinite(value) && value >= 24 ? value : original;
}

struct FolderReturnSite { uint32_t offset = 0; int field = -1; };
// One owning-function return, not a guessed symbol+offset. The native stub executes the
// original epilogue; the allocator slow path still rejoins its ORIGINAL Dart store PC.
inline bool folder_return_site(std::span<const uint32_t> body, unsigned rn,
        bool store, FolderReturnSite &out) {
    unsigned hits = 0; FolderReturnSite found;
    for (size_t i = 0; i + 3 < body.size(); ++i) {
        const auto w = body[i];
        const uint32_t op = store ? 0xfc000000 : 0xfc400000;
        if ((w & 0xffe00c1f) != op || ((w >> 5) & 31) != rn
            || body[i + 1] != 0xaa1d03ef || body[i + 2] != 0xa8c179fd
            || body[i + 3] != 0xd65f03c0) continue;
        int field = int((w >> 12) & 511); if (field & 256) field -= 512;
        if (field < 0 || field > 255 || (field & 7) != 7) return false;
        found = {uint32_t(i * 4), field}; ++hits;
    }
    if (hits != 1) return false;
    out = found; return true;
}
// Select CENTER_HORIZONTAL (gravity masked to 1), not RIGHT (5).
// Check the branch's common join and independently resolve View.TEXT_ALIGNMENT_CENTER (4).
// The two named functions must agree on the root; no persistent Dart object pointer.
inline bool folder_center_pool(std::span<const uint32_t> body, uint32_t &pool) {
    unsigned hits = 0; uint32_t found = 0;
    for (size_t i = 0; i + 3 < body.size(); ++i) {
        if (body[i] != 0x7100043f || body[i + 1] != 0x54000061
            || (body[i + 2] & 0xffc003ff) != 0xf9400360
            || (body[i + 3] & 0xfc000000) != 0x14000000) continue;
        int32_t jump = int32_t(body[i + 3] & 0x03ffffff);
        if (jump & 0x02000000) jump -= 0x04000000;
        const int64_t join = int64_t(i + 3) + jump;
        if (join <= int64_t(i + 3) || join + 3 >= int64_t(body.size())
            || body[size_t(join)] != 0x6b16001f
            || body[size_t(join + 1)] != 0x54000041
            || (body[size_t(join + 2)] & 0xffc003ff) != 0xf9400360
            || body[size_t(join + 3)] != 0xaa1d03ef) continue;
        found = ((body[i + 2] >> 10) & 4095) * 8; ++hits;
    }
    if (hits != 1 || !found) return false;
    pool = found; return true;
}
inline bool folder_text_alignment_center_pool(std::span<const uint32_t> body, uint32_t &pool) {
    unsigned hits = 0; uint32_t found = 0;
    for (size_t i = 0; i + 3 < body.size(); ++i) {
        if (body[i] != 0xf100107f || body[i + 1] != 0x5400006c
            || (body[i + 2] & 0xffc003ff) != 0xf9400360
            || body[i + 3] != 0xd65f03c0) continue;
        found = ((body[i + 2] >> 10) & 4095) * 8; ++hits;
    }
    if (hits != 1 || !found) return false;
    pool = found; return true;
}
} // namespace home_layout

namespace home_layout {
// Gravity.CENTER (17) returns the Alignment.center GC root, not a CrossAxis enum.
inline bool folder_stack_center_pool(std::span<const uint32_t> w, uint32_t &pool) {
    unsigned hits = 0; uint32_t found = 0;
    for (size_t i = 0; i + 6 < w.size(); ++i) {
        if (w[i] != 0xf100443f || w[i+1] != 0x540000c1
            || (w[i+2] & 0xffc003ff) != 0x91400360
            || (w[i+3] & 0xffc003ff) != 0xf9400000
            || w[i+4] != 0xaa1d03ef || w[i+5] != 0xa8c179fd || w[i+6] != 0xd65f03c0) continue;
        found = ((w[i+2] >> 10) & 4095) * 4096 + ((w[i+3] >> 10) & 4095) * 8; ++hits;
    }
    if (hits != 1) return false;
    pool = found; return true;
}
}
