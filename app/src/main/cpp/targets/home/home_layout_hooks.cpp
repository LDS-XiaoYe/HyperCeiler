/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "home_layout_config.h"
#include "home_title_render.h"
#include "home_layout_knobs.h"
#include "home_workspace_geometry.h"
#include "home_grid_autofit.h"
#include "home_folder_geometry.h"
#include "home_drop_geometry.h"
#include "home_indicator_pair.h"
#include "home_hotseat_capacity.h"
#include "home_widget_move.h"
#include "home_gadget_bridge.h"
#include "home_layout_elf_targets.h"
#include "nativehook/hook_bank.h"
#include "nativehook/memory_io.h"
#include "nativehook/native_image.h"

#include <android/log.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/system_properties.h>

// Launcher tweaks (targets/home/tweaks). The planner locates every site from the image's own
// symbol table, so these are values only - no addresses cross this boundary.
#include "targets/home/tweaks/blob.h"
namespace hometweaks {
void PushTweaksConfig(const Config &config);
bool HomeTweaksFindSymbol(const char *name, uint32_t *outVa, uint32_t *outSize);
bool HomeTweaksSymbolSpan(uint32_t va, uint32_t *outSpan);
bool HomeTweaksTargetImage(char *path, size_t cap);
}

/*
 * Panel-interactive gate for the maintenance worker, owned by the dock motion
 * transport (dock_native_motion.cpp) because that is the only component here that
 * already talks to system_server - and system_server is where PowerManagerService
 * lives, while every native carrier of the panel state on this ROM is root-only.
 *
 * Declared here rather than pulled from a header because the transport is compiled
 * in only with the dock feature: with it off there is nothing to ask, and the gate
 * degrades to "always interactive" (see layout_panel_refresh_and_check) so this
 * file still builds and the worker keeps its unconditional cadence.
 */
#if defined(HYPERCEILER_DOCK_NATIVE_MOTION)
bool refresh_dock_screen_state();
bool dock_motion_screen_active();
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <time.h>
#include <unistd.h>
#include <vector>

/*
 * The launcher's geometry values live in shared configuration objects, and Dart AOT inlines a getter
 * as small as `ldur d0, [x0, #imm]` at almost every call site - so the standalone accessor function is
 * called rarely or never, and hooking it changes nothing. Measured on device: with all eight geometry
 * accessors hooked, `GridController.currentConfig` (a static getter too large to inline) counted 601
 * calls while every geometry accessor counted zero.
 *
 * What every consumer - inlined or not - does read is the object `currentConfig` returns. So the
 * module installs two capture trampolines, takes the pointers to those shared objects, decodes each
 * knob's field path out of the launcher's own accessor code, and rewrites the fields in place. Nothing
 * is hardcoded: the symbol names come from the launcher image's own symbol table, and the field
 * offsets are read from the instructions that actually use them, so a launcher OTA moves everything
 * and this keeps working.
 */
extern "C" {
void hc_grid_autofit_0_entry(); void hc_grid_autofit_1_entry();
void *hc_grid_autofit_original[2]{};
uintptr_t hc_grid_autofit_resume[2]{};
uint32_t hc_grid_autofit_ready = 0, hc_grid_autofit_requested = 0;
int hc_grid_autofit_body(uintptr_t saved, uintptr_t frame, unsigned kind);
void hc_folder_layout_0_entry(); void hc_folder_layout_1_entry();
void hc_folder_layout_2_entry(); void hc_folder_layout_3_entry();
void hc_folder_layout_4_entry(); void hc_folder_layout_5_entry();
void hc_folder_layout_6_entry();
void hc_folder_layout_7_entry();
void *hc_folder_layout_original[8]{};
uint32_t hc_folder_layout_ready = 0;
uint64_t hc_folder_layout_requested = 0;
void hc_folder_layout_body(uintptr_t saved, uintptr_t pool, unsigned kind, uintptr_t heap);

void hc_gadget_select_entry();
void hc_gadget_return_entry();
void hc_gadget_span_entry();
void *hc_gadget_original[3]{};
uintptr_t hc_gadget_put_int = 0, hc_gadget_common = 0;
uintptr_t hc_gadget_select_continue = 0, hc_gadget_return_continue = 0;
uintptr_t hc_gadget_span_continue = 0, hc_gadget_span_reject = 0;
uint32_t hc_gadget_bridge_enabled = 0;
/*
 * One control block per knob, for the hook strategy: the trampoline calls the original layout
 * aggregator and adds this delta to its double result. `hits` is bumped by the trampoline, which is
 * how "this aggregator never runs" is told apart from "it runs and the delta does nothing".
 */
#define HC_DECLARE_KNOB(name, column, label, dflt, lo, hi) \
    uint8_t hc_layout_dart_##name##_enabled = 0; \
    uint64_t hc_layout_dart_##name##_delta = 0; \
    uint64_t hc_layout_dart_##name##_hits = 0; \
    uint64_t hc_layout_dart_##name##_last = 0; \
    uint64_t hc_layout_dart_##name##_caller = 0; \
    void *hc_layout_dart_##name##_original = nullptr; \
    void hc_layout_dart_##name##_entry();
HC_LAYOUT_KNOBS(HC_DECLARE_KNOB)
#undef HC_DECLARE_KNOB
/* Objects captured by the trampolines, and the heap base a compressed pointer is relative to. */
uint64_t hc_layout_config_object = 0;
uint64_t hc_layout_dock_object = 0;
uint64_t hc_layout_heap_base = 0;
uint64_t hc_layout_config_capture_hits = 0;
uint64_t hc_layout_dock_capture_hits = 0;
void *hc_layout_config_capture_original = nullptr;
void *hc_layout_dock_capture_original = nullptr;
void hc_layout_config_capture_entry();
void hc_layout_dock_capture_entry();
void hc_layout_passthrough_entry();
void *hc_layout_passthrough_original = nullptr;
void hc_title_color_entry();
void *hc_title_color_original = nullptr;
uintptr_t hc_title_color_continue = 0;
uint32_t hc_title_color_enabled = 0;
uint64_t hc_title_color_clone(uint64_t color, uint64_t thread);
void hc_drawer_title_entry();
double hc_drawer_title_baseline = 12.0;
void hc_drawer_title_height_entry();
void *hc_drawer_title_height_original = nullptr;
uintptr_t hc_drawer_title_height_continue = 0;
void *hc_drawer_title_original = nullptr;
uintptr_t hc_drawer_title_caller = 0;
uintptr_t hc_drawer_title_continue = 0;
uint32_t hc_drawer_title_sp = 12;
uint32_t hc_drawer_title_override = 0;
void hc_desktop_title_entry();
void *hc_desktop_title_original = nullptr;
uintptr_t hc_desktop_title_continue = 0;
uint32_t hc_desktop_title_sp = 12;
double hc_shared_drawer_title_baseline = 12.0;
double hc_desktop_title_baseline = 12.0; // Native scalar only, written by the Dart UI builder.
void hc_desktop_title_height_entry();
void *hc_desktop_title_height_original = nullptr;
uintptr_t hc_desktop_title_height_continue = 0;
uint32_t hc_title_hide_new_install = 0;
uint32_t hc_title_custom_enabled = 0;
void hc_title_custom_entry();
void *hc_title_custom_original = nullptr;
uintptr_t hc_title_custom_continue = 0;
uint64_t hc_title_custom_label(uint64_t original, uint64_t model, uint64_t heap,
    uint64_t thread, uint64_t dispatch, uint64_t null_object, uint32_t site);
void hc_title_prefix_entry();
void *hc_title_prefix_original = nullptr;
void hc_title_folder_new_entry();
void *hc_title_folder_new_original = nullptr;
uintptr_t hc_title_folder_new_caller = 0;
void hc_title_light_entry();
void *hc_title_light_original = nullptr;
uint32_t hc_title_is_new_asset(uint64_t text);
void hc_layout_capsule_entry();
void hc_layout_indicator_policy_entry();
void hc_layout_indicator_edit_result_entry();
void hc_layout_indicator_slide_only_entry();
uintptr_t hc_layout_indicator_idle_caller = 0;
uint32_t hc_layout_indicator_mode = 0;
uintptr_t hc_layout_indicator_edit_call = 0;
uintptr_t hc_layout_indicator_build_empty = 0;
uintptr_t hc_layout_indicator_build_dots = 0;
void hc_layout_indicator_pair(uintptr_t frame, uint64_t heap, uintptr_t thread,
    uint64_t dart_null);
void hc_layout_folder_0_entry();
void hc_layout_folder_1_entry();
void hc_layout_folder_2_entry();
void hc_layout_folder_3_entry();
void hc_layout_folder_4_entry();
uintptr_t hc_layout_folder_resume[5]{};
uint64_t hc_layout_folder_hits[5]{};
void *hc_layout_folder_original[5]{};
uint32_t hc_layout_folder_enabled = 0;
void hc_layout_folder_body(uintptr_t frame, uint64_t heap, uintptr_t saved, unsigned kind);
void hc_layout_drop_0_entry();
void hc_layout_drop_1_entry();
uintptr_t hc_layout_drop_resume[2]{};
uint64_t hc_layout_drop_hits[2]{};
void *hc_layout_drop_original[2]{};
uint32_t hc_layout_drop_enabled = 0;
void hc_layout_drop_body(uintptr_t frame, uint64_t heap, uintptr_t saved, unsigned kind);
void hc_layout_workspace_entry();
void hc_layout_workspace_occupied_entry();
void hc_layout_hotseat_horizontal_entry();
void hc_layout_container_probe_entry();
/* Independent second gate inside the original page-indicator builder. */
uint64_t hc_layout_dart_IndicatorDot_hits = 0;
void *hc_layout_dart_IndicatorDot_original = nullptr;
/* Slot bookkeeping for the companion, filled once by `bind_indicator_dot_target` and consumed by
 * `arm_hooks`. Kept beside the trampoline globals because the four fields always move together; the
 * `Words` payload is declared below, next to the other slot payloads, because it needs that type. */
uintptr_t hc_layout_dart_IndicatorDot_address = 0;
uint32_t hc_layout_dart_IndicatorDot_getter = 0;
uint32_t hc_layout_dart_IndicatorDot_size = 0;
nhk::CodeSource hc_layout_dart_IndicatorDot_source{};
bool hc_layout_dart_IndicatorDot_armed = false;
void hc_layout_workspace_layout(uintptr_t frame, uint64_t heap, int occupied);
void hc_layout_probe_container(uint64_t widget, uint64_t heap, uint64_t dart_null);
/* Probe readings, written by the passthrough stub itself. */
uint64_t hc_layout_probe_caller = 0;
uint64_t hc_layout_probe_value = 0;
uint32_t hc_layout_probe_hits = 0;
uint8_t hc_layout_animation_ratio_enabled = 0;
uint64_t hc_layout_animation_ratio_bits = 0;
uint64_t hc_layout_animation_ratio_hits = 0;
uint64_t hc_layout_animation_ratio_original_bits = 0;
void *hc_layout_animation_ratio_original = nullptr;
void hc_layout_animation_ratio_entry();
uint64_t hc_layout_magic_hits = 0;
uint64_t hc_layout_magic_original_bits = 0;
void *hc_layout_magic_original = nullptr;
uint8_t hc_layout_magic_enabled = 0;
uint64_t hc_layout_magic_bits = 0;
void hc_layout_magic_entry();
/* Called by both trampolines before they return, so the caller reads already-adjusted fields.
   `caller` is the Dart return address, recorded so the calibration can see which functions run. */
void hc_layout_apply_now(uint64_t config, uintptr_t caller);
}

namespace {
constexpr char kTag[] = "HyperCeiler.HomeLayout";
constexpr size_t kPatchWords = 4;
constexpr uint64_t kMaxLauncherImageBytes = 32U << 20;
constexpr int kMaxWorkerAttempts = 8;
/*
 * Worker cadences. The pass interval is what bounds how long a kernel-restored
 * patch can stay missing; the other two only bound how often an expensive question
 * is asked, so they are wall-clock throttles rather than iteration counts - the
 * iteration itself changes length once the panel is dozing.
 */
constexpr uint64_t kPassIntervalMs = 500;
constexpr uint64_t kPanelQueryIntervalMs = 1000;
constexpr uint64_t kDozingIterationMs = 10000;
constexpr uint64_t kGenerationCheckMs = 30000;
constexpr uint32_t kDartPrologue = 0xA9BF79FDu;
/* Slots: 0/1 the Rust grid handlers, 2/3 the two Dart object captures, then one per hook knob. */
constexpr size_t kConfigCaptureSlot = 2;
constexpr size_t kDockCaptureSlot = 3;
constexpr size_t kKnobHookSlotBase = 4;
constexpr size_t kAnimationHookSlot = kKnobHookSlotBase + HC_LAYOUT_KNOB_COUNT;
constexpr size_t kAnimationMagicSlot = kAnimationHookSlot + 1;
/*
 * The page-dot indicator accessor rides its own slot, outside the per-knob range.
 *
 * The capsule knob (index 5) already spends slot 9 on `LauncherIndicatorState._wrapWithAnimation`,
 * so the companion that moves the edit-mode page-dot indicator cannot reuse it: one slot holds one
 * patched address. Keeping it out of `kKnobHookSlotBase + index` also keeps `arm_hooks`'s
 * index-to-slot mapping (and the rollback scan in the slot verdict loop) untouched.
 */
constexpr size_t kIndicatorDotSlot = kAnimationMagicSlot + 1;
constexpr size_t kFolderGeometrySlotBase = kIndicatorDotSlot + 1;
constexpr size_t kDropGeometrySlotBase = kFolderGeometrySlotBase + 5;
constexpr size_t kTitleColorSlot = kDropGeometrySlotBase + 2;
constexpr size_t kDrawerTitleSlot = kTitleColorSlot + 1;
constexpr size_t kDesktopTitleSlot = kDrawerTitleSlot + 1;
constexpr size_t kTitlePrefixSlot = kDesktopTitleSlot + 1;
constexpr size_t kTitleFolderNewSlot = kTitlePrefixSlot + 1;
constexpr size_t kTitleLightSlot = kTitleFolderNewSlot + 1;
constexpr size_t kDesktopTitleHeightSlot = kTitleLightSlot + 1;
constexpr size_t kDrawerTitleHeightSlot = kDesktopTitleHeightSlot + 1;
constexpr size_t kTitleCustomSlot = kDrawerTitleHeightSlot + 1;
constexpr size_t kGadgetBridgeSlot = kTitleCustomSlot + 1;
constexpr size_t kFolderLayoutSlotBase = kGadgetBridgeSlot + 3;
constexpr size_t kGridAutofitSlotBase = kFolderLayoutSlotBase + 8;
constexpr size_t kSlotCount = kGridAutofitSlotBase + 2;
using Slot = nhk::InlineSlot<kPatchWords>;
using Words = nhk::SlotWords<kPatchWords>;
/* The companion slot's payload, declared here because `Words` only exists from this line down. */
Words hc_layout_dart_IndicatorDot_words{};

using HookFunction = int (*)(void *, void *, void **);
using UnhookFunction = int (*)(void *);

std::atomic_bool g_started{false};
/*
 * Field writes into the launcher's live objects are off by default: a wrong path there is a launcher
 * crash, not a no-op. Hook-based knobs (adjust a function's return value) do not need this gate.
 */
std::atomic_bool g_field_writes_enabled{false};
/* Only the title-size field is promoted to production: its exact getter and field path are
 * re-validated against the loaded Dart image before any heap write. Other experimental layout
 * field rewrites remain behind debug.hyperceiler.layout.knobs_enable. */
std::atomic_int g_title_desktop_sp{12};
std::atomic_int g_title_drawer_sp{12};
std::atomic_int g_title_color{-1};
bool g_desktop_title_bound = false;
bool g_title_color_bound = false;
bool g_drawer_title_bound = false;
bool g_title_hide_bound = false;
std::atomic_bool g_title_custom_bound{false};
uintptr_t g_title_component_method = 0, g_title_pin_method = 0;
using CustomTitles = std::vector<home_title::CustomTitle>;
std::shared_ptr<const CustomTitles> g_title_names;

bool g_folder_layout_bound = false, g_folder_layout_checked = false;
/*
 * `g_folder_layout_checked` only says "the admission scan reached at least the first symbol",
 * and the two log lines that could explain a refusal are both gated on it. So a launcher whose
 * FIRST symbol is missing - or whose eight derivation steps fail later - produced no log at all,
 * which is indistinguishable on the device from "the feature was never asked for". This flag
 * marks "the scan really ran", so every refusal below can name itself.
 */
bool g_folder_layout_attempted = false;
int g_folder_cell_width_field = -1, g_folder_gap_field = -1;
int g_folder_screen_width_field = -1, g_folder_screen_height_field = -1;
int g_folder_cling_width_field = -1;
int g_folder_controller_config_field = -1, g_folder_rx_value_field = -1;
uint32_t g_folder_center_pool = 0;
int g_folder_enum_index_field = -1;
// Slot 7 relocates the INNER Container instead of the outer Stack: the freshly built
// Container is a frame local of FolderHeaderWidget._buildText, and rewriting its
// alignment field needs no GC-rooted allocation, no enum index arithmetic and no
// dependence on the outer Stack's cross-axis enum. Every member is derived at bind
// time from the instruction stream (see home_layout::folder_inner_container).
home_layout::FolderInnerContainer g_folder_inner;
uint32_t g_folder_inner_owner_local = 0;   // offset from the post-dart_save x15
bool g_folder_inner_ready = false;

bool g_grid_autofit_bound = false, g_grid_autofit_checked = false;
home_layout::GridAutofitSites g_grid_autofit_fields;
void sync_folder_layout(const home_layout::Config &config) {
    __atomic_store_n(&hc_grid_autofit_requested,
        config.grid_enabled ? (1u | (unsigned(config.cell_y) << 8)) : 0u, __ATOMIC_RELEASE);
    const uint64_t packed =
        home_layout::pack_folder_layout(config.folder, config.tweaks.folder_cols);
    const uint64_t previous = __atomic_exchange_n(&hc_folder_layout_requested, packed, __ATOMIC_ACQ_REL);
    /*
     * `pack_folder_layout` collapses to 0 both for "the user asked for stock" and for "these
     * values cannot be represented", and the original-code bank gates on two of its low bits.
     * Without this line a closed gate is indistinguishable from a lost configuration: the bank
     * simply never binds and the only log it can emit is gated behind its own success flag.
     * Report the packed word itself so the two cases separate on the device.
     */
    if (previous != packed) {
        const auto c = home_layout::unpack_folder_layout(packed);
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "folder layout request packed=%#llx title_center=%d full_width=%d "
            "padding=%d/%d/%d/%d cols=%d",
            static_cast<unsigned long long>(packed), c.title_center, c.full_width,
            c.padding_enabled, c.phone_padding, c.landscape_padding, c.portrait_padding,
            config.tweaks.folder_cols);
    }
}
void sync_title_config(const home_layout::Config &config) {
    sync_folder_layout(config);
    const auto previous = std::atomic_load_explicit(&g_title_names, std::memory_order_acquire);
    if (!previous || *previous != config.title_custom_labels) {
        std::atomic_store_explicit(&g_title_names, std::make_shared<const CustomTitles>(config.title_custom_labels),
            std::memory_order_release);
        __android_log_print(ANDROID_LOG_INFO, kTag, "title custom snapshot entries=%zu",
            config.title_custom_labels.size());
    }
    __atomic_store_n(&hc_title_custom_enabled, !config.title_custom_labels.empty() ? 1U : 0U,
        __ATOMIC_RELEASE);
    __atomic_store_n(&hc_title_hide_new_install, config.title_hide_new_install ? 1U : 0U,
        __ATOMIC_RELEASE);
    const int old_desktop = g_title_desktop_sp.exchange(config.title_desktop_sp,
        std::memory_order_acq_rel);
    const int old_drawer = g_title_drawer_sp.exchange(config.title_drawer_sp,
        std::memory_order_acq_rel);
    const int old_color = g_title_color.exchange(config.title_color, std::memory_order_acq_rel);
    __atomic_store_n(&hc_desktop_title_sp, static_cast<uint32_t>(config.title_desktop_sp),
        __ATOMIC_RELEASE);
    __atomic_store_n(&hc_drawer_title_sp, static_cast<uint32_t>(config.title_drawer_sp),
        __ATOMIC_RELEASE);
    __atomic_store_n(&hc_drawer_title_override,
        config.title_drawer_sp != 12 ? 1U : 0U,
        __ATOMIC_RELEASE);
    __atomic_store_n(&hc_title_color_enabled, config.title_color != -1 ? 1U : 0U,
        __ATOMIC_RELEASE);
    if (old_desktop != config.title_desktop_sp || old_drawer != config.title_drawer_sp
        || old_color != config.title_color) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "title snapshot desktop=%d drawer=%d color=%#x",
            config.title_desktop_sp, config.title_drawer_sp, config.title_color);
    }
}
std::atomic_int g_attempts{0};
std::atomic_bool g_ready{false};
std::atomic_bool g_dart_ready{false};
std::atomic_int g_cell_x{0};
std::atomic_int g_cell_y{0};
HookFunction g_hook_function = nullptr;
UnhookFunction g_unhook_function = nullptr;
void *g_original_x = nullptr;
void *g_original_y = nullptr;
bool g_animation_hook_armed = false;
bool g_magic_hook_armed = false;
std::array<Slot, kSlotCount> g_slots{};
std::string g_container_path;
uint64_t g_view_begin = 0;
uint64_t g_view_end = 0;
std::string g_dart_container_path;
uint64_t g_dart_view_begin = 0;
uint64_t g_dart_view_end = 0;

int cell_x_replacement() {
    if (g_ready.load(std::memory_order_acquire)) return g_cell_x.load(std::memory_order_relaxed);
    const auto original = reinterpret_cast<int (*)()>(g_original_x);
    return original != nullptr ? original() : g_cell_x.load(std::memory_order_relaxed);
}

int cell_y_replacement() {
    if (g_ready.load(std::memory_order_acquire)) return g_cell_y.load(std::memory_order_relaxed);
    const auto original = reinterpret_cast<int (*)()>(g_original_y);
    return original != nullptr ? original() : g_cell_y.load(std::memory_order_relaxed);
}

void publish_animation_rate(const home_layout::TweaksConfig &tweaks) {
    /* Fully independent families: each gate and each duration ratio (30..200 percent, values
     * above 100 deliberately allowed so animations can run slower) publish separately. `recents`
     * feeds the Rust ratio consumers (recents, gestures, blur, wallpaper); `open` feeds the
     * gear-derived speed factor that app open/close actually follows. */
    const int open_percent = std::clamp(tweaks.animation_open_rate_percent, 30, 200);
    const int recents_percent = std::clamp(tweaks.animation_recents_rate_percent, 30, 200);
    uint64_t bits = 0;
    const double open_ratio = static_cast<double>(open_percent) / 100.0;
    std::memcpy(&bits, &open_ratio, sizeof(bits));
    __atomic_store_n(&hc_layout_magic_bits, bits, __ATOMIC_RELAXED);
    const double recents_ratio = static_cast<double>(recents_percent) / 100.0;
    std::memcpy(&bits, &recents_ratio, sizeof(bits));
    __atomic_store_n(&hc_layout_animation_ratio_bits, bits, __ATOMIC_RELAXED);
    __atomic_store_n(&hc_layout_animation_ratio_enabled,
        static_cast<uint8_t>(tweaks.animation_recents_enabled ? 1 : 0), __ATOMIC_RELEASE);
    __atomic_store_n(&hc_layout_magic_enabled,
        static_cast<uint8_t>(tweaks.animation_open_enabled ? 1 : 0), __ATOMIC_RELEASE);
}

/*
 * One row per geometry knob. `symbol` names the Dart accessor whose value the user is moving; it may
 * be replaced at run time by the calibration properties. The field path below it is derived from that
 * accessor's own instructions at run time.
 */
struct KnobRuntime {
    std::string symbol;
    int default_dp = 0;
    int min_dp = 0;
    int max_dp = 0;
    uint32_t getter = 0;
    /*
     * The live build's size for `getter`'s symbol, straight out of the same lookup that produced the
     * address. It is reported because a stale analysis symbol table is otherwise invisible: the
     * launcher image changes under the module (same versionName, new bytes) and every offset, size
     * and xref taken from the older copy silently describes a function that is no longer there.
     * With this on the log line, "is the thing I hooked the thing I analysed" is answerable from
     * logcat alone.
     */
    uint32_t getter_size = 0;
    /*
     * The field path is published atomically because the background worker derives it while the Dart
     * thread reads it inside the trampoline. A torn read here would mean a write to an unrelated
     * address, so the packed value is the only thing the trampoline trusts:
     *   bits 0..7 object (1 config, 2 dock), 8..15 off0 + 1 (0 means direct), 16..31 off1.
     */
    std::atomic<uint32_t> path{0};
    std::atomic<int> delta_dp{0};
    /*
     * Trampoline-thread only, never touched by the worker.
     *
     * `pristine` is the launcher's own value, remembered from a moment when this knob was not
     * touching the field, and every write is `pristine + delta` - an absolute value, never a
     * read-modify-write.
     *
     * That is not a style choice. The desktop rebuilds its configuration by copying the current one,
     * so a read-modify-write adds the delta again to a value that already carried it: with the
     * delta applied on every one of the thousands of `currentConfig` calls, a 32 px request grew
     * without bound and pushed the whole workspace off screen on the device. Deriving every write
     * from a value captured while the knob was off cannot compound, whatever the launcher copies.
     */
    double pristine = 0;
    bool pristine_valid = false;

    /*
     * Hook strategy. `hook_mode` is set when the calibration property points the knob at a layout
     * aggregator instead of at the field its accessor reads; the aggregators are large enough that
     * Dart cannot inline them, so a hook there is actually reached.
     */
    bool hook_mode = false;
    uintptr_t hook_address = 0;
    nhk::CodeSource hook_source{};
    Words hook_words{};
    void *hook_entry = nullptr;
    void **hook_original = nullptr;
    uint64_t *hook_delta = nullptr;
    uint64_t *hook_hits = nullptr;
    uint64_t *hook_last = nullptr;    // the launcher's own return value, raw double bits
    uint64_t *hook_caller = nullptr;  // the Dart caller's return address, for the call chain
    uint8_t *hook_enabled = nullptr;
    bool hook_armed = false;
};

/*
 * Knobs whose value is produced by a named accessor are moved by hooking that accessor and adding the
 * delta to the double it returns. This is the safe lever: it never writes into a live object, so a
 * wrong target produces "no effect" instead of a corrupted heap. A null entry means "no hook target
 * known yet" - that knob stays inert rather than guessing.
 *
 * Order matches HC_LAYOUT_KNOBS.
 */
constexpr const char *kKnobHookSymbols[HC_LAYOUT_KNOB_COUNT] = {
    /* Workspace margins rewrite the final original RenderBox layout arithmetic in its body,
     * not getter returns or a whole-Workspace Padding. Indicator and Dock are separate trees. */
    /*
     * Hotseat margin, wired on 7695 (RELEASE-8.01.02.7695). Measured on device with the
     * calibration probe: `GridController.hotSeatsMarginBottom` is consumed by
     * `WidgetPositionUtil._getCellRectInHotSeatPad` — a +20 delta moved the whole hotseat row up
     * by 20 px, confirmed visually, and the slider tests confirmed it live. On 6309 the same
     * accessor was a dead lever (overwritten by the Rust side), which is why this entry sat null;
     * a launcher OTA changed the consumer, so the null is gone.
     *
     * Protocol slot 1 used to be the retired hotseat-height knob. It now carries folder row
     * spacing. On launcher 7719 a zero-delta probe recorded 0 calls on the workspace and exactly
     * one call while opening a folder: the original result was 92.2699 px and the caller was
     * 0x146da4c, the return site immediately after `_buildScrollableGrid` calls
     * `FolderGridViewGetxController.folderCellHeight` at 0x146da48. The same value then enters
     * `_applyViewPropertiesWithoutPadding` and `_buildGrid`, so the hook changes the grid's row
     * extent at its stable accessor instead of patching a render offset.
     */
    /* HotseatMargin   */ "GridController.hotSeatsMarginBottom",
    /* FolderRowSpacing */ "FolderGridViewGetxController.folderCellHeight",
    /* WorkspaceTop owns an internal code splice; its local geometry uses slots 2..4. */
    /* WorkspaceTop    */ "GridCellDelegate.performLayout",
    /* WorkspaceBottom */ "GridOccupiedCellDelegate.performLayout",
    /* WorkspaceSide   */ "HotSeatLayoutDelegate.cellLayout",
    /* IndicatorMargin: intercept only the third animation wrapper return (the capsule subtree),
     * then wrap it in a zero-sum Padding. This changes the original tree's position without
     * touching the shared Dock/grid geometry or hiding the inner capsule label.
     * Retired search controls instead carry page-indicator policy in the unchanged wire ABI. */
    /* IndicatorMargin */ "LauncherIndicatorState._wrapWithAnimation",
    /* PageIndicatorMode (retired wire slot 6) */ "LauncherIndicatorState._buildScreenIndicator",
    /* PageIndicatorIdleGate (retired wire slot 7) */ "LauncherIndicatorState._showIndicator",
};

/*
 * Multiplier applied to a knob's delta when it is published to the trampoline.
 *
 * The workspace splice rewrites the original local origin/stride before the child-position loop.
 * Its helper consumes the three independent raw deltas; no Dart object allocation or field writes.
 */
constexpr double kKnobDeltaGain[HC_LAYOUT_KNOB_COUNT] = {
    1.0,             // HotseatMargin   (inert)
    1.0,             // FolderRowSpacing: extra row extent in px
    1.0,             // WorkspaceTop   : original coordinate code splice
    1.0,             // WorkspaceBottom: consumed by the coordinate splice
    1.0,             // WorkspaceSide  : consumed by the coordinate splice
    -1.0,            // IndicatorMargin: a larger bottom margin lifts the capsule, independent
                       // of the workspace/dock; the Container's zero-sum margin uses this delta.
    1.0,             // Retired wire column 6: page-indicator mode
    1.0,             // Retired wire column 7: independent idle-policy gate
};

std::array<KnobRuntime, HC_LAYOUT_KNOB_COUNT> g_knobs = {{
#define HC_KNOB_ROW(name, column, symbol, dflt, lo, hi) {symbol, dflt, lo, hi},
    HC_LAYOUT_KNOBS(HC_KNOB_ROW)
#undef HC_KNOB_ROW
}};

/*
 * Apply the property-gated read-only calibration targets before the launcher's first Dart layout.
 * The worker also calls this helper as a fallback, but that is too late for values cached while the
 * folder widget is first built. Keeping one parser for both paths prevents an early probe and the
 * worker from silently testing different symbols.
 */
void apply_debug_hook_overrides() {
    for (size_t index = 0; index < g_knobs.size(); ++index) {
        char name[PROP_VALUE_MAX] = {};
        const std::string key = "debug.hyperceiler.layout.hook" + std::to_string(index);
        if (__system_property_get(key.c_str(), name) <= 0 || name[0] == '\0') continue;
        if (name[0] == '-' || std::strcmp(name, "off") == 0) {
            if (g_knobs[index].hook_mode) {
                g_knobs[index].hook_mode = false;
                g_knobs[index].hook_address = 0;
                g_knobs[index].hook_armed = false;
                g_knobs[index].symbol.clear();
            }
            continue;
        }
        g_knobs[index].hook_mode = true;
        g_knobs[index].symbol = name;
    }
}

/* Bind the per-knob trampoline pointers from the same list, so the two can never drift apart. */
bool g_hook_globals_inited = false;

void init_knob_hooks() {
    // Not a function-local static: the flag would be inherited across the fork, and a desktop
    // forked after the spawner ran this once would keep null hook pointers forever.
    if (g_hook_globals_inited) return;
    g_hook_globals_inited = true;
    size_t index = 0;
#define HC_SET_HOOK(name, column, symbol, dflt, lo, hi)                                          \
    g_knobs[index].hook_entry = reinterpret_cast<void *>(hc_layout_dart_##name##_entry);         \
    g_knobs[index].hook_original = &hc_layout_dart_##name##_original;                            \
    g_knobs[index].hook_delta = &hc_layout_dart_##name##_delta;                                  \
    g_knobs[index].hook_hits = &hc_layout_dart_##name##_hits;                                    \
    g_knobs[index].hook_last = &hc_layout_dart_##name##_last;                                    \
    g_knobs[index].hook_caller = &hc_layout_dart_##name##_caller;                                \
    g_knobs[index].hook_enabled = &hc_layout_dart_##name##_enabled;                              \
    ++index;
    HC_LAYOUT_KNOBS(HC_SET_HOOK)
#undef HC_SET_HOOK
}

std::optional<std::vector<nhk::ExecutableMapping>> current_mappings() {
    std::ifstream maps("/proc/self/maps");
    if (!maps) return {};
    const auto all = nhk::parse_file_mappings(maps);
    std::vector<nhk::ExecutableMapping> owned;
    const auto collect = [&](const std::string &path, uint64_t begin, uint64_t end) {
        if (path.empty() || end <= begin) return;
        const std::vector<nhk::ImageContainer> container{{path, begin, end}};
        const auto found = nhk::owned_image_mappings(all, container);
        owned.insert(owned.end(), found.begin(), found.end());
    };
    collect(g_container_path, g_view_begin, g_view_end);
    collect(g_dart_container_path, g_dart_view_begin, g_dart_view_end);
    if (owned.empty()) return std::nullopt;
    return owned;
}

bool stable_read(const Slot &slot, Words &words) {
    const std::array<uintptr_t, 1> address{slot.address};
    const std::array<nhk::CodeSource, 1> expected{slot.source};
    const auto before = current_mappings();
    if (!before || nhk::mapping_state(*before, address, expected, sizeof(words))
            != nhk::MappingState::same) return false;
    if (!nhk::safe_read(slot.address, std::as_writable_bytes(std::span(&words, 1)))) return false;
    const auto after = current_mappings();
    return after && nhk::mapping_state(*after, address, expected, sizeof(words))
        == nhk::MappingState::same;
}

/*
 * Steady-state read of the armed slots' live words: one read per slot, nothing else.
 *
 * `stable_read` above rebuilds the whole /proc/self/maps inventory twice so it can prove the image
 * generation is still mapped at those offsets. That proof is what the health pass used to pay for on
 * every cadence - `ensure_slots_live` calls the validated read once per armed slot, and this pass runs
 * twice a second, which on this launcher (thousands of mappings) came to twenty-four full inventory
 * parses per pass. It is the same shape as the dock's 22%-of-a-core incident at four times the
 * frequency, and it was the entire cost of the layout feature's steady state.
 *
 * A slot whose live words still equal the recorded patch is a working patch. A torn read here cannot
 * be trusted, but it also cannot hurt: it fails the comparison and drops into the validated repair
 * below, which re-reads through `stable_read` and is the only path that writes code. See
 * `ordered_slots_healthy` in nativehook/hook_bank.h for why the split is safe.
 */
bool read_live_words(std::span<const uintptr_t> addresses, std::span<const nhk::CodeSource>,
    std::span<Words> observed) {
    for (size_t index = 0; index < addresses.size(); ++index) {
        if (!nhk::safe_read(addresses[index], std::as_writable_bytes(observed.subspan(index, 1)))) {
            return false;
        }
    }
    return true;
}

bool bank_live(const std::vector<size_t> &order) {
    return nhk::ordered_slots_healthy(g_slots, order, read_live_words);
}

/*
 * The generation proof `stable_read` performs, over one inventory shared by every armed slot.
 *
 * The steady state deliberately reads words only, and the words cannot see a generation change on
 * their own: what they prove is "the patch is still there", not "the address still belongs to the
 * image we bound it against". One inventory per interval, with a lookup per slot, is the periodic
 * restatement of that second half - and unlike the validated read it does not multiply the inventory
 * parse by the number of slots.
 */
bool bank_generation_holds(const std::vector<size_t> &order,
    const std::vector<nhk::ExecutableMapping> &inventory) {
    for (const size_t index : order) {
        const Slot &slot = g_slots[index];
        const std::array<uintptr_t, 1> address{slot.address};
        const std::array<nhk::CodeSource, 1> expected{slot.source};
        if (nhk::mapping_state(inventory, address, expected, sizeof(Words))
            != nhk::MappingState::same) {
            return false;
        }
    }
    return true;
}

std::optional<uint64_t> process_age_ms() {
    std::ifstream stat("/proc/self/stat");
    if (!stat) return {};
    std::string line;
    std::getline(stat, line);
    const size_t close = line.rfind(')');
    if (close == std::string::npos) return {};
    unsigned long long start_ticks = 0;
    if (std::sscanf(line.c_str() + close + 1,
            "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu",
            &start_ticks) != 1) return {};
    const long hz = sysconf(_SC_CLK_TCK);
    if (start_ticks == 0 || hz <= 0) return {};
    std::ifstream uptime("/proc/uptime");
    if (!uptime) return {};
    double seconds = 0;
    if (!(uptime >> seconds)) return {};
    const double start_seconds = static_cast<double>(start_ticks) / static_cast<double>(hz);
    if (seconds < start_seconds) return {};
    return static_cast<uint64_t>((seconds - start_seconds) * 1000.0);
}

uint64_t process_age_or_zero() {
    const auto age = process_age_ms();
    return age ? *age : 0;
}

nhk::InlineHookHost<kPatchWords> &slot_host() {
    static nhk::InlineHookHost<kPatchWords> host = [] {
        nhk::InlineHookHost<kPatchWords> value;
        value.read_slot = [](const nhk::InlineSlot<kPatchWords> &slot, nhk::SlotWords<kPatchWords> &out) {
            const bool ok = stable_read(slot, out);
            if (!ok) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                    "layout read_slot failed address=%p", reinterpret_cast<void *>(slot.address));
            }
            return ok;
        };
        value.write_words = [](uintptr_t address, const Words &words) {
            return nhk::write_code_bytes(address, std::as_bytes(std::span(&words, 1)));
        };
        value.hook_install = [](void *target, void *replacement, void **original) {
            const int rc = g_hook_function != nullptr
                ? g_hook_function(target, replacement, original)
                : -1;
            if (rc != 0) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                    "layout hook_install failed target=%p rc=%d", target, rc);
            }
            return rc;
        };
        value.hook_uninstall = [](void *target) {
            return g_unhook_function != nullptr ? g_unhook_function(target) : -1;
        };
        value.on_guard = [](const nhk::HookEvent &event) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "layout %s slot=%zu", event.reason,
                event.slot);
        };
        value.on_info = [](const nhk::HookEvent &event) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "layout %s slot=%zu", event.reason,
                event.slot);
        };
        return value;
    }();
    return host;
}

/* One stored library of the launcher APK, bound to its runtime view. */
struct Library {
    std::string path;
    uint64_t view_begin = 0;
    uint64_t view_end = 0;
    uint64_t load_base = 0;
    std::vector<std::byte> bytes;
    std::vector<nhk::elf::ProgramSegment> segments;
    std::vector<nhk::ExecutableMapping> owned;

    std::optional<uintptr_t> at(uint64_t va) const {
        if (load_base > UINTPTR_MAX - va) return std::nullopt;
        return static_cast<uintptr_t>(load_base + va);
    }

    std::optional<nhk::CodeSource> source(uintptr_t address) const {
        return nhk::source_at(owned, address, sizeof(Words));
    }

    std::optional<uint64_t> file_offset(uint64_t va, size_t count) const {
        for (const auto &segment : segments) {
            if (segment.type != nhk::elf::kProgramTypeLoad || va < segment.vaddr) continue;
            const uint64_t delta = va - segment.vaddr;
            if (delta > segment.filesz || count > segment.filesz - delta) continue;
            if (!nhk::in_image(bytes.size(), segment.offset + delta, count)) return std::nullopt;
            return segment.offset + delta;
        }
        return std::nullopt;
    }
};

template <typename Mappings>
std::optional<Library> open_library(const std::string &apk_path, std::string_view entry,
    const Mappings &all) {
    const auto stored = nhk::zip_stored_entry(apk_path, entry);
    if (!stored || stored->second == 0 || stored->second > kMaxLauncherImageBytes
        || nhk::add_overflows(stored->first, stored->second)) return {};
    std::ifstream apk(apk_path, std::ios::binary);
    if (!apk) return {};
    Library library;
    library.path = apk_path;
    library.view_begin = stored->first;
    library.view_end = stored->first + stored->second;
    library.bytes.resize(static_cast<size_t>(stored->second));
    apk.seekg(static_cast<std::streamoff>(stored->first));
    apk.read(reinterpret_cast<char *>(library.bytes.data()),
        static_cast<std::streamsize>(library.bytes.size()));
    if (static_cast<size_t>(apk.gcount()) != library.bytes.size()) return {};
    const auto segments = nhk::elf::parse_program_segments(library.bytes);
    if (!segments) return {};
    library.segments = *segments;
    const auto view = nhk::image_view_from_segments_in_window(all, apk_path, library.segments,
        library.view_begin, library.view_end);
    if (!view) return {};
    const std::vector<nhk::ImageContainer> container{{
        apk_path, library.view_begin, library.view_end}};
    library.owned = nhk::owned_image_mappings(all, container);
    if (library.owned.empty()) return {};
    library.load_base = view->load_base;
    return library;
}

struct Located {
    uintptr_t x = 0;
    uintptr_t y = 0;
    nhk::CodeSource x_source{};
    nhk::CodeSource y_source{};
    Words x_words{};
    Words y_words{};
    uintptr_t animation = 0;
    nhk::CodeSource animation_source{};
    Words animation_words{};
    uint64_t animation_owner_va = 0;
    uintptr_t magic = 0;
    nhk::CodeSource magic_source{};
    Words magic_words{};
    std::string container_path;
    uint64_t view_begin = 0;
    uint64_t view_end = 0;
    std::string dart_container_path;
    uint64_t dart_view_begin = 0;
    uint64_t dart_view_end = 0;
};

/*
 * The Dart snapshot stays useful after locate(): the object fields are rewritten long after the
 * container is found, so only the mapping metadata is retained and the few instruction words are
 * re-read from the APK on demand.
 */
struct DartLibrary {
    std::string path;
    uint64_t view_begin = 0;
    uint64_t view_end = 0;
    uint64_t load_base = 0;
    std::vector<nhk::elf::ProgramSegment> segments;
    std::vector<nhk::ExecutableMapping> owned;
};

std::optional<DartLibrary> g_dart;

std::optional<std::string> launcher_apk(const std::vector<nhk::FileMapping> &all) {
    std::optional<std::string> path;
    for (const auto &mapping : all) {
        const std::string_view candidate = nhk::strip_deleted(mapping.path);
        if (candidate.find("/com.miui.home-") == std::string_view::npos
            || !candidate.ends_with("/base.apk")) continue;
        if (path && *path != candidate) return {};
        path = std::string(candidate);
    }
    if (path) return path;
    /*
     * The /data upgrade pattern above is not the only shape a launcher APK has: the 7654 desktop
     * ships from /product/priv-app/MiuiHome, which no directory guess should have to know. The
     * tweaks module has already proven which image is actually loaded (dl_iterate_phdr), so hand
     * its answer over instead of a second, wrong guess. Without this fallback every geometry site
     * and every hook knob lost its image anchor on 7654 — "layout targets unavailable" plus a
     * silent knobs=0/8.
     */
    char proven[512]{};
    if (hometweaks::HomeTweaksTargetImage(proven, sizeof(proven))) {
        return std::string(proven);
    }
    return path;
}

bool ensure_dart_library() {
    if (g_dart) return true;
    std::ifstream maps("/proc/self/maps");
    if (!maps) return false;
    const auto all = nhk::parse_file_mappings(maps);
    const auto path = launcher_apk(all);
    if (!path) return false;
    /*
     * Cheap gate before the expensive open: this is polled while the launcher starts, and
     * open_library() reads the whole 28 MB entry out of the APK before it can discover the window is
     * not mapped yet. An embedded library's first segment is mapped at the entry's own file offset.
     */
    const auto stored = nhk::zip_stored_entry(*path, "libapp.so");
    if (!stored) return false;
    /*
     * Cheap gate before the expensive open: this is polled while the launcher starts, and
     * open_library() reads the whole 28 MB entry out of the APK before it can discover the window is
     * not mapped yet.
     *
     * The old check required a mapping whose file offset equals the entry's own start, which almost
     * never holds: an ELF segment with a non-zero p_offset maps at entry_start + p_offset, and ART
     * also splits mappings at page boundaries. Accept any mapping of this APK whose offset falls
     * inside the entry's span - the same condition open_library's window check applies later.
     */
    bool entry_mapped = false;
    for (const auto &mapping : all) {
        if (nhk::strip_deleted(mapping.path) != *path) continue;
        if (mapping.file_offset < stored->first
            || mapping.file_offset >= stored->first + stored->second) {
            continue;
        }
        entry_mapped = true;
        break;
    }
    if (!entry_mapped) {
        // Polled, so report once: a silent false here used to look exactly like "the image is not
        // loaded yet", and cost a whole debugging session (the probe never bound).
        static bool reported = false;
        if (!reported) {
            reported = true;
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "layout dart entry mapped check failed: entry=%#llx size=%llu maps=%zu",
                static_cast<unsigned long long>(stored->first),
                static_cast<unsigned long long>(stored->second), all.size());
        }
        return false;
    }
    const auto dart = open_library(*path, "libapp.so", all);
    if (!dart) return false;
    DartLibrary retained;
    retained.path = dart->path;
    retained.view_begin = dart->view_begin;
    retained.view_end = dart->view_end;
    retained.load_base = dart->load_base;
    retained.segments = dart->segments;
    retained.owned = dart->owned;
    g_dart = std::move(retained);
    g_dart_container_path = g_dart->path;
    g_dart_view_begin = g_dart->view_begin;
    g_dart_view_end = g_dart->view_end;
    /*
     * Identity of the Dart image this process is running, reported so that static analysis material
     * can be checked against it instead of assumed. The launcher image changes under the module -
     * same versionName, new bytes - and yesterday's symbol table then describes functions that are
     * no longer there: an address can land inside a *different* function while still looking like a
     * plausible entry. Entry offset, entry size and the APK's own mtime are enough to tell the two
     * apart from logcat alone.
     */
    {
        struct stat apk {};
        const bool have_apk = stat(g_dart->path.c_str(), &apk) == 0;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "layout dart container base=%p entry=%#llx size=%#llx apk_size=%llu apk_mtime=%lld",
            reinterpret_cast<void *>(static_cast<uintptr_t>(g_dart->load_base)),
            static_cast<unsigned long long>(g_dart->view_begin),
            static_cast<unsigned long long>(g_dart->view_end - g_dart->view_begin),
            have_apk ? static_cast<unsigned long long>(apk.st_size) : 0ULL,
            have_apk ? static_cast<long long>(apk.st_mtime) : 0LL);
    }
    return true;
}

std::optional<uint64_t> dart_file_offset(uint64_t va, size_t count) {
    if (!g_dart) return {};
    for (const auto &segment : g_dart->segments) {
        if (segment.type != nhk::elf::kProgramTypeLoad || va < segment.vaddr) continue;
        const uint64_t delta = va - segment.vaddr;
        if (delta > segment.filesz || count > segment.filesz - delta) continue;
        return segment.offset + delta;
    }
    return {};
}

/* Instruction words of a Dart function, read from the APK entry the image is mapped from. */
bool dart_words(uint32_t va, size_t count, std::vector<uint32_t> &out) {
    if (!g_dart || va == 0 || count == 0) return false;
    for (size_t attempt : {count, size_t{32}, size_t{24}, size_t{16}}) {
        if (attempt > count) continue;
        const auto file = dart_file_offset(va, attempt * 4);
        if (!file || g_dart->view_begin > UINT64_MAX - *file) continue;
        std::ifstream apk(g_dart->path, std::ios::binary);
        if (!apk) return false;
        out.assign(attempt, 0);
        apk.seekg(static_cast<std::streamoff>(g_dart->view_begin + *file));
        apk.read(reinterpret_cast<char *>(out.data()),
            static_cast<std::streamsize>(out.size() * 4));
        if (apk.gcount() == static_cast<std::streamsize>(out.size() * 4)) return true;
    }
    return false;
}

/*
 * Slurp one whole Dart function into memory for scanning. `dart_words` reopens the APK per
 * call, which is fine for a handful of words but not for the instruction sweeps the Gadget
 * anchor search runs -- a single miss would cost hundreds of reads.
 *
 * `bytes` is a length in bytes, the same unit the function spans are expressed in, and the
 * result holds one entry per instruction. Mixing the two up here is invisible for a short body
 * and catastrophic for a long one: passing a byte count where a word count was expected made the
 * read ask for four times the range, which runs off the end of the mapped APK and fails the
 * whole read -- so the scan reported "not found" for functions that are present and unchanged.
 * The size comes from the symbol table rather than a literal, so a launcher OTA that grows or
 * shrinks the body is followed instead of silently truncating the scan.
 */
bool dart_function_words(uint32_t va, uint32_t bytes, std::vector<uint32_t> &out) {
    if (!g_dart || va == 0 || bytes < 4 || bytes > (1u << 20) || (bytes & 3) != 0) return false;
    const size_t words = bytes / 4;
    const auto file = dart_file_offset(va, bytes);
    if (!file || g_dart->view_begin > UINT64_MAX - *file) return false;
    std::ifstream apk(g_dart->path, std::ios::binary);
    if (!apk) return false;
    out.assign(words, 0);
    apk.seekg(static_cast<std::streamoff>(g_dart->view_begin + *file));
    apk.read(reinterpret_cast<char *>(out.data()),
        static_cast<std::streamsize>(out.size() * 4));
    return apk.gcount() == static_cast<std::streamsize>(out.size() * 4);
}

/*
 * Bind a Dart image VA to its runtime address, code source and instruction words. This is only used
 * for the two capture trampolines: the geometry fields no longer need a hook.
 */
bool bind_dart_target(uint32_t va, uintptr_t &address, nhk::CodeSource &source, Words &words) {
    if (!g_dart || va == 0 || g_dart->load_base > UINTPTR_MAX - va) return false;
    const auto file = dart_file_offset(va, sizeof(Words));
    if (!file || g_dart->view_begin > UINT64_MAX - *file) return false;
    address = static_cast<uintptr_t>(g_dart->load_base + va);
    const auto origin = nhk::source_at(g_dart->owned, address, sizeof(Words));
    if (!origin || origin->file_offset != *file) return false;
    std::ifstream apk(g_dart->path, std::ios::binary);
    if (!apk) return false;
    std::array<std::byte, sizeof(Words)> bytes{};
    apk.seekg(static_cast<std::streamoff>(g_dart->view_begin + *file));
    apk.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (apk.gcount() != static_cast<std::streamsize>(bytes.size())) return false;
    source = *origin;
    std::memcpy(words.data(), bytes.data(), sizeof(Words));
    return true;
}

bool bl_target(uint32_t word, uint32_t pc, uint32_t *target);

/*
 * Run-time anchor search over a Dart function body.
 *
 * Every position the layout hooks need is derived here instead of being written down: sizes
 * come from the symbol table's own ordering, callee addresses from the names it carries. A
 * site is reported only when exactly one instruction run matches. Zero matches means the
 * launcher reshaped that code, several means the shape does not identify it, and both decline
 * the hook rather than fall back to a remembered number -- a mis-bound splice replaces working
 * launcher code with ours, which is strictly worse than leaving the feature off.
 */
namespace dartscan {

/* Some reported sizes are partial (LauncherIndicatorState.build), but the next function in
 * the complete symbol table is a stronger bound than the next whitelisted target. Oversized,
 * unaligned or missing spans are declined: clamping them would allow a partial view to claim
 * uniqueness, or growing a short span would inspect another function's receiver. */
constexpr uint32_t kMaxScanBytes = 0x4000;

bool function_span(uint32_t va, uint32_t *out_size) {
    if (va == 0 || out_size == nullptr) return false;
    uint32_t span = 0;
    if (!hometweaks::HomeTweaksSymbolSpan(va, &span)) return false;
    if (span < 4 || span > kMaxScanBytes || (span & 3)) return false;
    *out_size = span;
    return true;
}

/* The single offset at which `seq` occurs verbatim. Several occurrences are a failure. */
bool unique_sequence(const std::vector<uint32_t> &body, const uint32_t *seq, size_t count,
    uint32_t *offset) {
    if (seq == nullptr || count == 0 || body.size() < count) return false;
    size_t found = SIZE_MAX;
    for (size_t i = 0; i + count <= body.size(); ++i) {
        if (!std::equal(seq, seq + count, body.begin() + static_cast<long>(i))) continue;
        if (found != SIZE_MAX) return false;
        found = i;
    }
    if (found == SIZE_MAX) return false;
    if (offset != nullptr) *offset = static_cast<uint32_t>(found) * 4;
    return true;
}


bool body(uint32_t va, std::vector<uint32_t> &words) {
    uint32_t span = 0;
    return function_span(va, &span) && dart_function_words(va, span, words);
}
bool site(uint32_t va, std::span<const uint32_t> words, uint32_t *offset) {
    std::vector<uint32_t> code;
    return body(va, code) && unique_sequence(code, words.data(), words.size(), offset);
}
bool site(uint32_t va, std::initializer_list<uint32_t> words, uint32_t *offset) {
    return site(va, std::span<const uint32_t>(words.begin(), words.size()), offset);
}
// Allocator identity is its class/size tag, not an old relative BL encoding.
bool tagged_call(uint32_t va, uint32_t first, uint32_t second, uint32_t *factory) {
    std::vector<uint32_t> code;
    if (!body(va, code)) return false;
    size_t hits = 0; uint32_t result = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        uint32_t target = 0; std::vector<uint32_t> tag;
        if (bl_target(code[i], va + static_cast<uint32_t>(i * 4), &target)
            && dart_words(target, 2, tag) && tag[0] == first && tag[1] == second) {
            ++hits; result = target;
        }
    }
    if (hits != 1) return false;
    if (factory) *factory = result;
    return true;
}

} // namespace dartscan

/*
 * Locate the capsule splice point in `LauncherIndicatorState.build`.
 *
 * The launcher builds the capsule indicator by calling `_wrapWithAnimation` and then packing
 * the result into the two-element array it hands on. The splice goes inside that packing, on the
 * instruction that starts passing the second element along. Nothing about that position is
 * written down here: the packing run is found by scanning, the call in front of it is decoded
 * to confirm it is the wrapper's, and the instruction is counted from there. A launcher that
 * reorders the sequence therefore either resolves to its new position or declines -- it cannot
 * be left pointing at whatever slid into the old slot, which is what a stored offset would do.
 *
 * Every step must be unique. The body calls the wrapper three times, so the call target alone
 * does not identify anything; it is the packing run that pins the site down, and the call is
 * only checked to agree.
 */
bool capsule_wrapper_layout_compatible(uint32_t wrapper_va, uint32_t,
    uint32_t *caller_va) {
    if (!caller_va) return false;
    uint32_t launcher = 0, container = 0, padding = 0, unused = 0;
    if (!hometweaks::HomeTweaksFindSymbol("LauncherIndicatorState.build", &launcher, &unused)
        || !hometweaks::HomeTweaksFindSymbol("Container.build", &container, &unused)
        || !hometweaks::HomeTweaksFindSymbol("Padding.createRenderObject", &padding, &unused)
        || !dartscan::tagged_call(wrapper_va, 0xd2868382, 0xf2a04302, nullptr)
        || !dartscan::site(padding, {0xb840f002}, nullptr)) return false;
    constexpr uint32_t padding_replay[] = {0xaa0003e1, 0xf85f03a0, 0xb800f020, 0xf85e83a0, 0xb800b020};
    std::vector<uint32_t> container_code;
    if (!dartscan::body(container, container_code)) return false;
    size_t padding_hits = 0;
    for (size_t i = 1; i + std::size(padding_replay) <= container_code.size(); ++i) {
        uint32_t factory = 0; std::vector<uint32_t> tag;
        if (std::equal(std::begin(padding_replay), std::end(padding_replay), container_code.begin() + i)
            && bl_target(container_code[i - 1], container + static_cast<uint32_t>((i - 1) * 4), &factory)
            && dart_words(factory, 2, tag) && tag[0] == 0xd28a4382 && tag[1] == 0xf2a03c42) ++padding_hits;
    }
    if (padding_hits != 1) return false;
    constexpr uint32_t head[] = {0xaa1603e1, 0xd28000c2, 0xf81f83a0};
    constexpr uint32_t tail[] = {0xaa0003e2, 0xf85f03a0, 0xf81e03a2, 0xb800f040,
        0xf85e83a0, 0xb8013040, 0xf85f83a0, 0xb8017040};
    std::vector<uint32_t> code;
    if (!dartscan::body(launcher, code)) return false;
    size_t hits = 0; uint32_t patch = 0;
    for (size_t i = 1; i + 12 <= code.size(); ++i) {
        uint32_t wrapper = 0, factory = 0; std::vector<uint32_t> tag;
        if (std::equal(std::begin(head), std::end(head), code.begin() + i)
            && std::equal(std::begin(tail), std::end(tail), code.begin() + i + 4)
            && bl_target(code[i - 1], launcher + static_cast<uint32_t>((i - 1) * 4), &wrapper)
            && wrapper == wrapper_va
            && bl_target(code[i + 3], launcher + static_cast<uint32_t>((i + 3) * 4), &factory)
            && dart_words(factory, 4, tag) && tag[0] == 0x37000402 && tag[1] == 0xb27d37f1
            && tag[2] == 0x6b11005f && tag[3] == 0x540003a8) {
            ++hits; patch = launcher + static_cast<uint32_t>((i + 4) * 4);
        }
    }
    if (hits != 1) return false;
    *caller_va = patch; return true;
}

/*
 * Locate a set of instruction runs inside a Dart body and require every one of them.
 *
 * The runs are what these hooks have always compared against; what changed is where they are
 * looked for. Each used to carry a fixed offset, and the same run appears at a different offset
 * in every one of these bodies -- 0x18 in the empty-grid delegate, 0x7b4 in the occupied one --
 * so a fixed offset identifies a function, not a site, and silently matches the wrong code
 * once the launcher reorganises the bodies. Scanning also removes the need to assert the
 * reported symbol size, which is shorter than the real body for two of these functions and so
 * would truncate the scan before it reached the last run.
 */
static bool require_runs(uint32_t va, const std::vector<std::pair<const uint32_t *, size_t>> &runs,
    size_t patch_run, size_t patch_word, uint32_t *patch_offset) {
    uint32_t span = 0;
    if (!dartscan::function_span(va, &span)) return false;
    std::vector<uint32_t> body;
    if (!dart_function_words(va, span, body)) return false;
    if (!patch_offset || patch_run >= runs.size()) return false;
    uint32_t candidate = 0;
    for (size_t i = 0; i < runs.size(); ++i) {
        const auto &run = runs[i];
        uint32_t at = 0;
        if (!dartscan::unique_sequence(body, run.first, run.second, &at)) return false;
        if (i == patch_run) {
            if (patch_word >= run.second) return false;
            candidate = at + static_cast<uint32_t>(patch_word * 4);
        }
    }
    if (candidate + 16 > span) return false;
    *patch_offset = candidate;
    return true;
}

#define HC_RUN(name) std::make_pair(static_cast<const uint32_t *>(name), std::size(name))

bool workspace_geometry_code_compatible(uint32_t va, bool occupied, uint32_t *patch_offset) {
    // The real RenderBox layout / ParentData.offset consumers. Each run below is the original
    // load, frame slot, displaced code or final constraint store at one of those sites.
    // The earlier _buildChildren splice was overwritten by these delegates.
    if (occupied == false) {
        static constexpr uint32_t words0[] = {0xb841b003u, 0x8b1c8063u, 0xf81f83a3u, 0xfc42b060u, 0xfc1b03a0u, 0xfc433061u, 0xfc1b83a1u, 0xb846b061u};
        static constexpr uint32_t words1[] = {0xf85f83a0u, 0xf81d83a2u, 0xb843b003u, 0x8b1c8063u, 0xf81e03a3u, 0xfc407060u, 0xfc1a83a0u, 0xf85f03a4u, 0xfc5b03a1u, 0xfc5b83a2u, 0xfc5c03a3u, 0xf85ff040u, 0xd34c7c00u};
        static constexpr uint32_t words2[] = {0xfc5a83a0u, 0xf8407002u, 0x9e620044u, 0x1e610885u, 0x1e652804u, 0xfc1983a4u, 0xf840f002u, 0x9e620045u, 0x1e6208a6u, 0x1e6328c5u, 0xfc1a03a5u};
        static constexpr uint32_t words3[] = {0xfc5b03a0u, 0xf81d03a0u, 0xfc007000u, 0xfc00f000u, 0xfc5b83a1u, 0xfc017001u, 0xfc01f001u, 0xf85f03a3u};
        return require_runs(va, {HC_RUN(words0), HC_RUN(words1), HC_RUN(words2), HC_RUN(words3)},
            1, 7, patch_offset);
    }
    static constexpr uint32_t cwords0[] = {0xf85f83a2u, 0xb8417040u, 0x8b1c8000u, 0xfc42b000u, 0xfc1a03a0u, 0xfc433001u, 0xfc1a83a1u, 0xb840f043u};
    static constexpr uint32_t cwords1[] = {0xb843b001u, 0x8b1c8021u, 0xfc407022u, 0xfc1b03a2u, 0xa9460345u, 0x910040a5u, 0xeb05001fu, 0x54002a09u};
    static constexpr uint32_t cwords2[] = {0xfc5a03a0u, 0xfc5a83a1u, 0xfc5b03a2u, 0xf85f03a0u, 0xf85e83a1u, 0xf8437002u, 0x9e620043u, 0x1e600864u, 0x1e642843u, 0xfc1903a3u, 0xf843f002u, 0x9e620044u, 0x1e610885u, 0xfc1983a5u, 0xf9403f40u, 0xf9524800u, 0xf9402370u, 0x6b10001fu};
    static constexpr uint32_t cwords3[] = {0xfc5803a0u, 0xf81c83a0u, 0xfc007000u, 0xfc00f000u, 0xfc5883a0u, 0xfc017000u, 0xfc01f000u, 0xf85f83a3u, 0xb840b064u, 0x8b1c8084u};
    return require_runs(va, {HC_RUN(cwords0), HC_RUN(cwords1), HC_RUN(cwords2), HC_RUN(cwords3)},
        2, 14, patch_offset);
}

bool hotseat_geometry_code_compatible(uint32_t va, uint32_t *patch_offset) {
    // The final Dock ParentData.offset.x write, after the original per-icon calculation:
    // delegate count, the compressed ItemInfo chain, the column and the Offset stores.
    static constexpr uint32_t words0[] = {0xa9bf79fdu, 0xaa0f03fdu, 0xd10281efu, 0xf81f83a1u, 0xf81f03a2u, 0xd28000c1u, 0x9411837cu, 0xaa0003e1u};
    static constexpr uint32_t words1[] = {0xf85f83a5u, 0xf81c03a4u, 0xf84130a6u, 0x937f78c0u, 0xeb8004dfu, 0x54000060u, 0x941187bcu, 0xf8007006u, 0xf81c83a0u, 0xfc42b0a0u, 0xfc1883a0u, 0xd2800001u, 0xfc5903a1u, 0xf81d03a2u};
    static constexpr uint32_t words2[] = {0xf85b03a2u, 0x97c6e9ecu, 0xaa0003e3u, 0xf85b03a2u, 0xb840f040u, 0x8b1c8000u, 0xb8407001u, 0x8b1c8021u, 0xf8437024u, 0x937f7880u, 0xeb80049fu, 0x54000060u, 0x94118778u, 0xf8007004u, 0xf85c83b0u};
    static constexpr uint32_t words3[] = {0xfc1783a2u, 0xa9461340u, 0x91004000u, 0xeb00009fu, 0x54001529u};
    static constexpr uint32_t words4[] = {0x97c6c09eu, 0xf85b03a2u, 0xb8413040u, 0x8b1c8000u, 0xfc407000u, 0xfc1803a0u, 0x9406fdaeu, 0xfc5803a0u, 0xf81983a0u, 0xfc007000u, 0xfc5783a0u, 0xfc00f000u, 0xf85f83a3u};
    return require_runs(va, {HC_RUN(words0), HC_RUN(words1), HC_RUN(words2), HC_RUN(words3), HC_RUN(words4)},
        4, 7, patch_offset);
}

bool bind_target(const Library &library, uint64_t va, uintptr_t &address,
    nhk::CodeSource &source, Words &words) {
    const auto at = library.at(va);
    const auto file = library.file_offset(va, sizeof(Words));
    if (!at || !file) return false;
    const auto origin = library.source(*at);
    if (!origin || origin->file_offset != *file) return false;
    address = *at;
    source = *origin;
    std::memcpy(words.data(), library.bytes.data() + *file, sizeof(Words));
    return true;
}

bool bind_animation_consumer(Located &located) {
    if (located.animation != 0) return true;
    if (located.animation_owner_va == 0 || !g_dart) return false;
    /*
     * Prefer the shared ratio entry over the flight-only accessor: the app-open move blends the
     * icon flight with Folme spring, blur and wallpaper timings that all read the same upstream,
     * and replacing only the flight's share lets the rest run ahead or behind it. Each candidate
     * still has to earn its binding with the standard Dart prologue, so a launcher build that
     * inlines or restyles either one simply falls through to the next.
     */
    static constexpr const char *kCandidates[] = {
        "getAnimDurationRatio",
        "FlightCohort.animDurationRatio",
    };
    for (const char *name : kCandidates) {
        uint32_t va = 0;
        uint32_t size = 0;
        if (!hometweaks::HomeTweaksFindSymbol(name, &va, &size) || size < 16) continue;
        if (!bind_dart_target(va, located.animation, located.animation_source,
                located.animation_words)
            || located.animation_words[0] != kDartPrologue) {
            located.animation = 0;
            continue;
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "animation duration Dart consumer target=%p va=%#x size=%u symbol=%s",
            reinterpret_cast<void *>(located.animation), va, size, name);
        return true;
    }
    located.animation = 0;
    return false;
}

/*
 * The gear-derived duration factor, the second animation boundary. App open/close responds to this
 * value and not to the ratio above: the window springs scale 1:1 with it (measured through the
 * official three gears), so replacing its result is what puts open/close on the custom rate. Both
 * boundaries are "1.0 = nominal duration" doubles and share the one published value.
 */
bool bind_magic_consumer(Located &located) {
    if (located.magic != 0) return true;
    if (!g_dart) return false;
    uint32_t va = 0;
    uint32_t size = 0;
    if (!hometweaks::HomeTweaksFindSymbol("Utilities.getDefaultGestureAnimMagicSpeed", &va, &size)
        || size < 16
        || !bind_dart_target(va, located.magic, located.magic_source, located.magic_words)
        || located.magic_words[0] != kDartPrologue) {
        located.magic = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "animation magic Dart consumer target=%p va=%#x size=%u",
        reinterpret_cast<void *>(located.magic), va, size);
    return true;
}

std::optional<Located> locate() {
    std::ifstream maps("/proc/self/maps");
    if (!maps) return {};
    const auto all = nhk::parse_file_mappings(maps);
    const auto path = launcher_apk(all);
    if (!path) return {};

    Located located;
    if (const auto rust = open_library(*path, "libapp_launcher.so", all)) {
        const auto targets = home_layout::elf_targets::resolve(rust->bytes);
        if (targets
            && bind_target(*rust, targets->cell_count_x, located.x, located.x_source,
                located.x_words)
            && bind_target(*rust, targets->cell_count_y, located.y, located.y_source,
                located.y_words)) {
            located.container_path = rust->path;
            located.view_begin = rust->view_begin;
            located.view_end = rust->view_end;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "layout rust targets X=%p Y=%p", reinterpret_cast<void *>(located.x),
                reinterpret_cast<void *>(located.y));
        }
        const auto animation_owner = home_layout::elf_targets::resolve_animation_duration_update(
            rust->bytes);
        if (animation_owner) {
            located.animation_owner_va = *animation_owner;
            located.container_path = rust->path;
            located.view_begin = rust->view_begin;
            located.view_end = rust->view_end;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "animation duration owner validated va=%#llx",
                static_cast<unsigned long long>(*animation_owner));
        }
    }
    if (ensure_dart_library()) {
        located.dart_container_path = g_dart->path;
        located.dart_view_begin = g_dart->view_begin;
        located.dart_view_end = g_dart->view_end;
        (void) bind_animation_consumer(located);
        (void) bind_magic_consumer(located);
    }
    if (located.x == 0 && located.animation == 0 && located.dart_container_path.empty()) return {};
    return located;
}

void delay_ms(long milliseconds) {
    timespec delay{milliseconds / 1000, (milliseconds % 1000) * 1000000};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

/*
 * Wall clock for the worker's own throttles. CLOCK_MONOTONIC through the vDSO costs tens of
 * nanoseconds, which is what makes "ask at most once a second" cheaper than the question it guards.
 * Zero means the clock call failed: the callers read that as "not due yet" rather than as an
 * interval of zero, so a broken clock degrades to a slower check instead of a busy loop.
 */
uint64_t monotonic_ms() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000ULL
        + static_cast<uint64_t>(now.tv_nsec) / 1000000ULL;
}

/*
 * Throttle predicate for a wall-clock interval, with zero as the "never asked yet" sentinel.
 */
bool interval_due(uint64_t last, uint64_t now, uint64_t interval) {
    if (last == 0) return true;
    if (now == 0) return false;
    return now - last >= interval;
}

/*
 * Ask whether the panel is interactive and the launcher window can be drawn.
 *
 * Fail-open in both directions: built without the dock transport there is nobody to ask, and with it
 * a failed query keeps the transport's previous answer (which starts at "interactive"). A missing
 * endpoint therefore costs the saving, never the hooks.
 */
bool layout_panel_refresh_and_check() {
#if defined(HYPERCEILER_DOCK_NATIVE_MOTION)
    (void) refresh_dock_screen_state();
    return dock_motion_screen_active();
#else
    return true;
#endif
}

void *attempt_finished() {
    g_started.store(false, std::memory_order_release);
    return nullptr;
}

void push_tweaks(const home_layout::Config &values) {
    hometweaks::Config tweaks;
    tweaks.flags = 1u; // master on
    if (values.grid_enabled) tweaks.enabled.push_back(hometweaks::kFeaturePhoneGrid);
    if (values.tweaks.folder_enabled) tweaks.enabled.push_back(hometweaks::kFeatureFolderCols);
    if (values.tweaks.pad_enabled) tweaks.enabled.push_back(hometweaks::kFeaturePadGrid);
    if (values.tweaks.fold_enabled) tweaks.enabled.push_back(hometweaks::kFeatureFoldGrid);
    if (values.tweaks.icon_scale_enabled) tweaks.enabled.push_back(hometweaks::kFeatureIconSize);
    if (values.tweaks.recents_no_clear) tweaks.enabled.push_back(hometweaks::kFeatureNoClear);
    if (values.tweaks.recents_hide_clear) tweaks.enabled.push_back(hometweaks::kFeatureHideClear);
    tweaks.phoneCols = values.cell_x;
    tweaks.phoneRows = values.cell_y;
    tweaks.folderCols = values.tweaks.folder_cols;
    tweaks.padMajor = values.tweaks.pad_major;
    tweaks.padMinor = values.tweaks.pad_minor;
    tweaks.foldMajor = values.tweaks.fold_major;
    tweaks.foldMinor = values.tweaks.fold_minor;
    tweaks.iconScaleCode = values.tweaks.icon_scale_code;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "layout tweaks grid=%d/%dx%d folder=%d/%d pad=%d/%d fold=%d/%d icon=%d noClear=%d "
        "hideClear=%d", values.grid_enabled ? 1 : 0, values.cell_x, values.cell_y,
        values.tweaks.folder_enabled ? 1 : 0, values.tweaks.folder_cols,
        values.tweaks.pad_major, values.tweaks.pad_minor, values.tweaks.fold_major,
        values.tweaks.fold_minor, values.tweaks.icon_scale_code,
        values.tweaks.recents_no_clear ? 1 : 0, values.tweaks.recents_hide_clear ? 1 : 0);
    hometweaks::PushTweaksConfig(tweaks);
}

// ---------------------------------------------------------------------------
// Field path derivation.
// ---------------------------------------------------------------------------

bool is_ldur_double(uint32_t word) {
    return (word & 0xFFE00C00u) == 0xFC400000u;
}

bool is_ldur_word(uint32_t word) {
    return (word & 0xFFE00C00u) == 0xB8400000u;
}

bool is_add_heap(uint32_t word, uint32_t reg) {
    // add xR, xR, x28, lsl #32
    return word == (0x8B000000u | (28u << 16) | (reg << 5) | reg | (32u << 10));
}

int imm9(uint32_t word) {
    return static_cast<int>((word >> 12) & 0x1FFu);
}

uint32_t rn(uint32_t word) {
    return (word >> 5) & 0x1Fu;
}

uint32_t rt(uint32_t word) {
    return word & 0x1Fu;
}

bool bl_target(uint32_t word, uint32_t pc, uint32_t *target) {
    if ((word & 0xFC000000u) != 0x94000000u) return false;
    int64_t imm = static_cast<int32_t>(word & 0x03FFFFFFu);
    if ((imm & 0x02000000) != 0) imm -= 0x04000000;
    const int64_t destination = static_cast<int64_t>(pc) + imm * 4;
    if (target == nullptr || destination <= 0 || destination > UINT32_MAX) return false;
    *target = static_cast<uint32_t>(destination);
    return true;
}

struct FieldPath {
    uint8_t object = 0; // 1 config, 2 dock
    int off0 = -1;      // nested pointer offset, -1 for a direct field
    int off1 = -2;      // field offset, -2 unresolved
};

/* Pack a resolved path into the single word the trampoline reads. */
uint32_t pack_path(uint8_t object, int off0, int off1) {
    const uint32_t nested = static_cast<uint32_t>(off0 + 1) & 0xFFu;
    return static_cast<uint32_t>(object) | (nested << 8) | (static_cast<uint32_t>(off1) << 16);
}

/*
 * Find the field an accessor reads out of a captured object.
 *
 * The body is `bl <capture target>` followed by either `ldur d0, [x0, #off]` or
 * `ldur wN, [x0, #nested]` / `add xN, xN, x28, lsl #32` / `ldur d0, [xN, #off]`. The last such site
 * wins, because an accessor may call another accessor first (workspaceIndicatorMarginBottom calls
 * hotSeatsMarginBottom and then reads the config itself).
 */
bool decode_path(std::span<const uint32_t> code, uint32_t base, uint32_t config_va,
    uint32_t dock_va, FieldPath &path) {
    uint8_t object = 0;
    FieldPath best;
    for (size_t at = 0; at + 4 < code.size(); ++at) {
        uint32_t target = 0;
        if (!bl_target(code[at], base + static_cast<uint32_t>(at * 4), &target)) continue;
        if (target == config_va) object = 1;
        else if (dock_va != 0 && target == dock_va) object = 2;
        else continue;
        const size_t limit = std::min(code.size(), at + 6);
        for (size_t probe = at + 1; probe < limit; ++probe) {
            const uint32_t word = code[probe];
            if (is_ldur_double(word) && rn(word) == 0) {
                best = FieldPath{object, -1, imm9(word)};
                break;
            }
            if (is_ldur_word(word) && rn(word) == 0) {
                const uint32_t reg = rt(word);
                for (size_t nested = probe + 1; nested < std::min(code.size(), probe + 4); ++nested) {
                    if (is_add_heap(code[nested], reg)
                        && nested + 1 < code.size() && is_ldur_double(code[nested + 1])
                        && rn(code[nested + 1]) == reg) {
                        best = FieldPath{object, imm9(word), imm9(code[nested + 1])};
                        break;
                    }
                    if (is_ldur_double(code[nested]) && rn(code[nested]) == reg) {
                        best = FieldPath{object, imm9(word), imm9(code[nested])};
                        break;
                    }
                }
                break;
            }
        }
    }
    if (best.off1 == -2) return false;
    path = best;
    return true;
}

uint32_t g_config_capture_va = 0;
uint32_t g_dock_capture_va = 0;
uintptr_t g_config_capture_address = 0;
uintptr_t g_dock_capture_address = 0;
nhk::CodeSource g_config_capture_source{};
nhk::CodeSource g_dock_capture_source{};
Words g_config_capture_words{};
Words g_dock_capture_words{};
bool g_captures_armed = false;
bool g_probe_primed = false;
int g_device_object_offset = -1;

/* Bind only verified interior control-flow sites, never a margin getter. */
struct IndicatorPolicyAnchors { uint32_t gate = 0, result = 0; };
bool indicator_policy_compatible(uint32_t va, uint32_t, IndicatorPolicyAnchors *out) {
    if (!out) return false;
    uint32_t editing = 0, unused = 0;
    if (!hometweaks::HomeTweaksFindSymbol("LauncherIndicatorState.isInEditing", &editing, &unused)) return false;
    constexpr uint32_t gate[] = {0xf100041f, 0x540001ec, 0xf85e83a3, 0x362001a3,
        0xf85f83a1, 0xb840f024, 0x8b1c8084, 0xaa0403e1};
    constexpr uint32_t result[] = {0x362000e0, 0xf85e03a0, 0x362000a0,
        0xf9712b60, 0xaa1d03ef, 0xa8c179fd, 0xd65f03c0,
        0xf85c83a2, 0xf85c03a0, 0xf85e83a1};
    std::vector<uint32_t> code;
    uint32_t g = 0, r = 0, target = 0;
    if (!dartscan::body(va, code) || !dartscan::unique_sequence(code, gate, std::size(gate), &g)
        || !dartscan::unique_sequence(code, result, std::size(result), &r) || r != g + 36
        || !bl_target(code[g / 4 + 8], va + g + 32, &target) || target != editing) return false;
    *out = {g, r}; return true;
}
bool indicator_slide_compatible(uint32_t va, uint32_t *patch, uint32_t *idle_caller) {
    if (!patch || !idle_caller) return false;
    uint32_t animate = 0, refresh = 0, current_type = 0, unused = 0, fallback = 0;
    if (!dartscan::site(va, {0xf85f83a0, 0xf81f03a3, 0xf841b002, 0xeb03005f}, patch)
        || !hometweaks::HomeTweaksFindSymbol("LauncherIndicatorState._animateIndicator", &animate, &unused)
        || !dartscan::site(animate, {0x7100103f, 0x54000081, 0xb846b061, 0x8b1c8021,
            0x14000003, 0xb8463061, 0x8b1c8021}, &fallback)
        || !hometweaks::HomeTweaksFindSymbol("LauncherIndicatorState._refreshIndicator", &refresh, &unused)
        || !hometweaks::HomeTweaksFindSymbol("LauncherIndicatorState._getCurrentIndicatorType", &current_type, &unused)) return false;
    std::vector<uint32_t> code;
    if (!dartscan::body(refresh, code)) return false;
    size_t hits = 0; uint32_t caller = 0;
    for (size_t i = 3; i < code.size(); ++i) {
        uint32_t target = 0, producer = 0;
        if (code[i - 2] == 0xf85f03a1 && code[i - 1] == 0xaa0003e2
            && bl_target(code[i - 3], refresh + static_cast<uint32_t>((i - 3) * 4), &producer)
            && producer == current_type && bl_target(code[i], refresh + static_cast<uint32_t>(i * 4), &target) && target == va) {
            ++hits; caller = refresh + static_cast<uint32_t>((i + 1) * 4);
        }
    }
    if (hits != 1) return false;
    *idle_caller = caller; return true;
}

void bind_indicator_dot_target() {
    if (hc_layout_dart_IndicatorDot_address != 0) return;
    uint32_t va = 0, size = 0; IndicatorPolicyAnchors anchors;
    if (!hometweaks::HomeTweaksFindSymbol("LauncherIndicatorState._buildScreenIndicator", &va, &size)
        || !indicator_policy_compatible(va, size, &anchors)) return;
    uintptr_t address = 0;
    if (!bind_dart_target(va + anchors.result, address, hc_layout_dart_IndicatorDot_source,
        hc_layout_dart_IndicatorDot_words)) return;
    hc_layout_dart_IndicatorDot_address = address;
    hc_layout_dart_IndicatorDot_getter = va + anchors.result;
    hc_layout_dart_IndicatorDot_size = size;
    hc_layout_indicator_edit_call = g_dart->load_base + va + anchors.gate + 16;
    hc_layout_indicator_build_empty = g_dart->load_base + va + anchors.result + 16;
    hc_layout_indicator_build_dots = g_dart->load_base + va + anchors.result + 28;
}

home_layout::GridFieldOffsets g_grid_field{};
void resolve_grid_fields(const char *symbol);

// All original instructions are obtained from this APK, never a remembered patch VA.
// Return-store splices leave Dart allocation and its slow-path return PC untouched.
bool folder_layout_anchors(uint32_t grid, uint32_t cell, uint32_t config,
        uint32_t cling, uint32_t top, const std::vector<uint32_t> &gb,
        const std::vector<uint32_t> &cb, const std::vector<uint32_t> &wb,
        const std::vector<uint32_t> &tb, int &gap, int &width, int &height,
        home_layout::FolderReturnSite &cs, home_layout::FolderReturnSite &ws) {
    if (gb.size() < 8 || gb[0] != kDartPrologue || gb[1] != 0xaa0f03fd
        || gb[2] != 0xd10041ef || gb[3] != 0xf81f83a1
        || !home_layout::folder_return_site(cb, 1, false, cs)) return false;
    uint32_t target = 0;
    // Getter is a configuration-derived scalar, not an arbitrary double in a closure.
    if (cs.offset < 12 || !bl_target(cb[cs.offset / 4 - 3], cell + cs.offset - 12, &target)
        || target != config || !is_ldur_word(cb[cs.offset / 4 - 2])
        || rn(cb[cs.offset / 4 - 2]) != 0 || rt(cb[cs.offset / 4 - 2]) != 1
        || cb[cs.offset / 4 - 1] != 0x8b1c8021) return false;
    unsigned gaps = 0, widths = 0, heights = 0, pad_returns = 0;
    for (size_t i = 0; i + 6 < gb.size(); ++i) {
        if (!bl_target(gb[i], grid + uint32_t(i * 4), &target) || target != cell
            || gb[i + 1] != 0xf85f03a1 || gb[i + 2] != 0x93417c30
            || gb[i + 3] != 0x1e620201 || gb[i + 4] != 0x1e610802
            || gb[i + 5] != 0xf85f83a2 || !is_ldur_double(gb[i + 6])
            || rn(gb[i + 6]) != 2 || rt(gb[i + 6]) != 0) continue;
        gap = imm9(gb[i + 6]); ++gaps;
    }
    for (size_t i = 0; i + 4 < wb.size(); ++i) {
        if ((wb[i] & 0xffe00c1f) != 0xfc400000 || rn(wb[i]) != 0
            || wb[i + 1] != 0xaa1d03ef || wb[i + 2] != 0xa8c179fd
            || wb[i + 3] != 0xd65f03c0) continue;
        // Screen-width return follows the named currentConfig call; tablet width is
        // the other path and may reach the shared entry without a currentConfig BL.
        if (i && bl_target(wb[i - 1], cling + uint32_t((i - 1) * 4), &target) && target == config) {
            width = imm9(wb[i]); ++widths;
        } else { ws = {uint32_t(i * 4), imm9(wb[i])}; ++pad_returns; }
    }
    for (size_t i = 0; i + 3 < tb.size(); ++i) {
        if (!bl_target(tb[i], top + uint32_t(i * 4), &target) || target != config
            || !is_ldur_double(tb[i + 1]) || rn(tb[i + 1]) != 0 || rt(tb[i + 1]) != 1
            || tb[i + 2] != 0xf85f03a0 || (tb[i + 3] & 0xfff8001f) != 0x37200000) continue;
        height = imm9(tb[i + 1]); ++heights;
    }
    return gaps == 1 && widths == 1 && heights == 1 && pad_returns == 1
        && gap > 0 && gap <= 255 && (gap & 7) == 3
        && width > 0 && height > 0 && width != height && ws.field > 0;
}

// BOTH fixed-row and variable-row paths converge here, AFTER the original max
// height cap. Intercept final gap redistribution before GridInfo publication.
bool bind_grid_autofit() {
    if (g_grid_autofit_bound) return true;
    if (g_grid_autofit_checked || !g_dart
        || !__atomic_load_n(&hc_grid_autofit_requested, __ATOMIC_ACQUIRE)) return false;
    const char *names[] = {"GridSizeCalRules._calVariableHeight", "GridSizeCalRules.calVarCellHeight",
        "PhoneCellSizeHandler.calVariableValues"};
    std::array<uint32_t, 3> va{}; std::array<std::vector<uint32_t>, 3> bodies;
    for (size_t i = 0; i < 3; ++i) {
        uint32_t size = 0;
        if (!hometweaks::HomeTweaksFindSymbol(names[i], &va[i], &size)
            || !dartscan::body(va[i], bodies[i])) return false;
        if (i == 0) g_grid_autofit_checked = true;
    }
    if (bodies[0].size() < 4 || bodies[2].size() < 4
        || bodies[0][0] != kDartPrologue || bodies[0][1] != 0xaa0f03fd
        || bodies[0][2] != 0xd10101ef || bodies[0][3] != 0xaa0103e3
        || bodies[2][0] != kDartPrologue || bodies[2][1] != 0xaa0f03fd
        || bodies[2][2] != 0xd10241ef || bodies[2][3] != 0xf81f83a1) return false;
    home_layout::GridAutofitSites fields;
    if (!home_layout::grid_autofit_sites(bodies[0], bodies[1], bodies[2], fields)) return false;
    unsigned cap_calls = 0;
    for (size_t j = 0; j + 2 < bodies[0].size(); ++j) {
        int32_t height = -1;
        if (home_layout::dart_call_to(bodies[0][j], va[0] + uint32_t(j * 4), va[1])
            && bodies[0][j + 1] == 0xf85f83a1
            && home_layout::dart_ldur_d(bodies[0][j + 2], 1, 0, &height)
            && height == fields.legacy_height) ++cap_calls;
    }
    if (cap_calls != 1) return false;
    const uint32_t sites[] = {fields.legacy, fields.handler};
    const void *entries[] = {reinterpret_cast<void *>(hc_grid_autofit_0_entry),
        reinterpret_cast<void *>(hc_grid_autofit_1_entry)};
    std::array<Slot, 2> prepared{};
    for (size_t i = 0; i < 2; ++i) {
        auto &slot = prepared[i];
        if (!bind_dart_target(va[i ? 2 : 0] + sites[i], slot.address, slot.source, slot.original_words)) return false;
        slot.replacement = const_cast<void *>(entries[i]); slot.original = &hc_grid_autofit_original[i];
    }
    g_grid_autofit_fields = fields;
    for (size_t i = 0; i < 2; ++i) {
        g_slots[kGridAutofitSlotBase + i] = prepared[i];
        hc_grid_autofit_resume[i] = prepared[i].address + 16;
    }
    g_grid_autofit_bound = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "grid autofit original-code bank bound=2 after final cap; workspace fills; widths and stock dock preserved; Dart8");
    return true;
}

bool bind_folder_layout() {
    if (g_folder_layout_bound) return true;
    if (g_folder_layout_checked) return false;
    if (!g_dart || !(__atomic_load_n(&hc_folder_layout_requested, __ATOMIC_ACQUIRE) & 3)) {
        /*
         * This gate is polled on every pass of the layout loop, so a "not yet" answer is normal
         * and must stay quiet. A "not ever" answer is not: without this line a launcher whose
         * `libapp.so` never maps looks exactly like a user who never turned the feature on,
         * because both produce no bank log whatsoever. Rate-limited, not one-shot, so a gate
         * that opens late is still reported.
         */
        static unsigned gate_reported = 0;
        if ((++gate_reported & 0x3ff) == 1) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "folder layout gate closed: dart=%s requested=%#llx (bit0 title_center, "
                "bit1 full_width)", g_dart ? g_dart->path.c_str() : "(unmapped)",
                static_cast<unsigned long long>(
                    __atomic_load_n(&hc_folder_layout_requested, __ATOMIC_ACQUIRE)));
        }
        return false;
    }
    const char *names[] = {"FolderGridViewGetxController.calGridWidth",
        "FolderGridViewGetxController.folderCellWidth",
        "FolderGridViewGetxController.folderGridOuterHorizontalPadding",
        "FolderGridViewGetxController.folderGridPaddingLeft",
        "FolderHeaderWidget._buildText", "FolderHeaderWidget._buildEditor",
        "FolderClingWidget.getFolderClingWidth",
        "FolderClingGetxController._calcFolderPaddingTop",
        "_FlutterTextViewState._resolveEffectiveTextAlign", "GridController.currentConfig",
        "RxObjectMixin.value", "AndroidAttributeUtils.convertGravity",
        "_encodeParagraphStyle", "AndroidAttributeUtils.convertTextAlignment",
        // The last three exist only so the inner-Container scan can read the Container's
        // own named functions. Nothing else consumes them, and a launcher build that
        // renames any of them simply refuses slot 7 instead of guessing at a field
        // offset. `Stack.updateRenderObject` and `FolderHeaderWidget.build` used to be
        // admitted here for the outer-Stack route; that route is gone, and leaving them
        // in would keep two pure liabilities in the admission set.
        "Container.build", "Container._paddingIncludingDecoration"};
    std::array<uint32_t, 16> va{}; std::array<std::vector<uint32_t>, 16> bodies;
    g_folder_layout_attempted = true;
    for (size_t i = 0; i < va.size(); ++i) {
        uint32_t size = 0;
        if (!hometweaks::HomeTweaksFindSymbol(names[i], &va[i], &size)
            || !dartscan::body(va[i], bodies[i])) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "folder layout admission missing symbol/body=%s (index %zu)", names[i], i);
            g_folder_layout_checked = true;
            return false;
        }
        if (i == 0) g_folder_layout_checked = true; // One admission per image, no repeated APK scans.
    }
    int gap = -1, width = -1, height = -1;
    home_layout::FolderReturnSite cell, cling, outer, left;
    if (!folder_layout_anchors(va[0], va[1], va[9], va[6], va[7],
            bodies[0], bodies[1], bodies[6], bodies[7], gap, width, height, cell, cling)
        || !home_layout::folder_return_site(bodies[2], 0, true, outer) || outer.field != 7
        || !home_layout::folder_return_site(bodies[3], 0, true, left) || left.field != 7) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "folder layout anchors unresolved gap=%d width=%d height=%d cell=%d/%#x "
            "cling=%d/%#x outer=%d/%#x left=%d/%#x",
            gap, width, height, cell.field, cell.offset, cling.field, cling.offset,
            outer.field, outer.offset, left.field, left.offset);
        return false;
    }
    // Both boxed padding values are freshly allocated Double objects. Check their tag
    // and the original GC rejoin: the new stub must cover that shared store, not its predecessor.
    for (size_t i : {size_t(2), size_t(3)}) {
        uint32_t tag = 0;
        if (!dartscan::unique_sequence(bodies[i], std::array<uint32_t, 3>{0xd29c2b81,
                0xf2a00061, 0xf81ff001}.data(), 3, &tag)) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "folder layout padding prologue %zu not unique (tag=%#x want %#x)", i, tag,
                0xd29c2b81);
            return false;
        }
        const uint32_t store = i == 2 ? outer.offset : left.offset;
        if (tag + 12 != store) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "folder layout padding %zu store=%#x does not follow tag=%#x", i, store, tag);
            return false;
        }
        unsigned rejoin = 0;
        for (size_t j = store / 4 + 4; j < bodies[i].size(); ++j)
            if ((bodies[i][j] & 0xfc000000) == 0x14000000
                && int64_t(va[i] + j * 4) + int64_t(home_layout::bl_imm_words(bodies[i][j])) * 4
                    == va[i] + store) ++rejoin;
        if (rejoin != 1) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "folder layout padding %zu rejoin count=%u want 1", i, rejoin);
            return false;
        }
    }
    uint32_t center = 0, independent_center = 0;
    if (!home_layout::folder_center_pool(bodies[8], center)
        // The independent semantic root is convertTextAlignment's own
        // TEXT_ALIGNMENT_CENTER branch (index 13). It must agree with
        // _resolveEffectiveTextAlign's root, so a launcher that changes one and not
        // the other refuses the bank instead of half-centring the title.
        || !home_layout::folder_text_alignment_center_pool(bodies[13], independent_center)
        || center != independent_center) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "folder layout text-align roots disagree resolve=%#x convert=%#x", center,
            independent_center);
        return false;
    }
    // Admit Text and TextField's local TextAlign store with their own surrounding ABI.
    uint32_t text = 0, editor = 0;
    if (!dartscan::site(va[4], {0xb801b001, 0x9141c361, 0xf9421021, 0xb802b001}, &text)
        || !dartscan::site(va[5], {0xb805b001, 0x9142cf61, 0xf940d421, 0xb805f001}, &editor)
        || text < 4 || editor < 4
        || (bodies[4][text / 4 - 1] & 0xffc003ff) != 0xf9400361
        || bodies[4][text / 4 - 1] != bodies[5][editor / 4 - 1]) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "folder layout TextAlign stores unresolved text=%#x editor=%#x", text, editor);
        return false;
    }
    // Slot 7: relocate the freshly built Container rather than the outer Stack. The
    // window is four instructions - exactly the 4-word patch budget - and rewriting the
    // Container's own alignment field needs neither a new allocation nor the enum index
    // arithmetic the Stack route depended on. Any step that cannot be proven refuses the
    // whole bank: a half-relocated Container is worse than an untouched one, because a
    // mis-centred title is indistinguishable from several unrelated causes.
    //
    // Nothing here derives the outer Stack's alignment slot any more. That scan existed
    // only to forge slot 7's return value; the inner route writes the Container's own
    // field and lets the launcher's instructions run unchanged, so the whole
    // `column`/`stack_field`/`start_pool` chain - and the `Stack.updateRenderObject`
    // admission it needed - is gone rather than left behind as unused evidence.
    home_layout::FolderInnerContainer inner;
    if (!home_layout::folder_inner_container(bodies[4], bodies[14], bodies[15],
            va[4], inner)) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "folder layout inner Container plan unresolved; bank refused");
        return false;
    }
    // _encodeParagraphStyle is the paragraph encoder: it forwards TextAlign.index
    // into an Int32 field. There is no Smi shift here - Enum.index is an UNBOXED
    // 64-bit int, not a Smi.
    unsigned index_hits = 0; int index_field = -1;
    for (size_t j = 0; j + 2 < bodies[12].size(); ++j) {
        const auto &w = bodies[12];
        if ((w[j] & 0xffe00fff) == 0xf8400022 && w[j + 1] == 0x93407c42
            && w[j + 2] == 0xb801b002) { index_field = imm9(w[j]); ++index_hits; }
    }
    if (index_hits != 1 || index_field <= 0 || index_field > 255) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "folder layout paragraph index hits=%u field=%d want exactly 1 in 1..255",
            index_hits, index_field);
        return false;
    }
    int controller_config = -1, rx_value = -1; unsigned rx_calls = 0, values = 0;
    for (size_t j = 2; j < bodies[9].size(); ++j) {
        uint32_t target = 0;
        if (bl_target(bodies[9][j], va[9] + uint32_t(j * 4), &target) && target == va[10]
            && is_ldur_word(bodies[9][j - 2]) && rn(bodies[9][j - 2]) == 0
            && rt(bodies[9][j - 2]) == 1 && bodies[9][j - 1] == 0x8b1c8021) {
            controller_config = imm9(bodies[9][j - 2]); ++rx_calls;
        }
    }
    for (size_t j = 0; j + 4 < bodies[10].size(); ++j) {
        if (is_ldur_word(bodies[10][j]) && rn(bodies[10][j]) == 1 && rt(bodies[10][j]) == 0
            && bodies[10][j + 1] == 0x8b1c8000 && bodies[10][j + 2] == 0xf9402370
            && bodies[10][j + 3] == 0x6b10001f
            && (bodies[10][j + 4] & 0xff00001f) == 0x54000000) {
            rx_value = imm9(bodies[10][j]); ++values;
        }
    }
    if (rx_calls != 1 || values != 1 || controller_config <= 0 || rx_value <= 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "folder layout Rx chain rx_calls=%u values=%u config_field=%d value_field=%d "
            "want 1/1 and both fields > 0",
            rx_calls, values, controller_config, rx_value);
        return false;
    }
    const uint32_t offsets[] = {0, cell.offset, outer.offset, left.offset, text, editor, cling.offset, 0};
    const void *entries[] = {reinterpret_cast<void *>(hc_folder_layout_0_entry),
        reinterpret_cast<void *>(hc_folder_layout_1_entry), reinterpret_cast<void *>(hc_folder_layout_2_entry),
        reinterpret_cast<void *>(hc_folder_layout_3_entry), reinterpret_cast<void *>(hc_folder_layout_4_entry),
        reinterpret_cast<void *>(hc_folder_layout_5_entry), reinterpret_cast<void *>(hc_folder_layout_6_entry), reinterpret_cast<void *>(hc_folder_layout_7_entry)};
    std::array<Slot, 8> prepared{};
    for (size_t i = 0; i < prepared.size(); ++i) {
        auto &slot = prepared[i];
        // Slot 7 is the only one whose target is not expressed as symbol+offset: the
        // replay window is an absolute VA derived from _buildText's own instructions,
        // so the base moves with it rather than being the Stack's `column` store.
        const uint32_t target = (i == 7) ? inner.window : va[i] + offsets[i];
        if (!bind_dart_target(target, slot.address, slot.source, slot.original_words)) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "folder layout slot %zu unbindable at target=%#x (symbol va=%#x offset=%#x)", i,
                target, va[i], offsets[i]);
            return false;
        }
        slot.replacement = const_cast<void *>(entries[i]); slot.original = &hc_folder_layout_original[i];
    }
    g_folder_cell_width_field = cell.field; g_folder_gap_field = gap;
    g_folder_screen_width_field = width; g_folder_screen_height_field = height;
    g_folder_controller_config_field = controller_config; g_folder_rx_value_field = rx_value;
    g_folder_cling_width_field = cling.field; g_folder_center_pool = center;
    g_folder_enum_index_field = index_field;
    // `dart_save` pushes 32 bytes onto x15 before the body runs; the closure's own
    // prologue published FP from x15 and then allocated `frame`, so FP sits
    // dart_save + frame above the saved x15 and the Container sits owner_slot below FP.
    // Fold all three into one distance so slot 7 needs no register it cannot read -
    // the splice never spills x29 and must not rely on the callee preserving it.
    g_folder_inner = inner;
    g_folder_inner_owner_local = 32 + inner.frame - inner.owner_slot;
    g_folder_inner_ready = true;
    for (size_t i = 0; i < prepared.size(); ++i) g_slots[kFolderLayoutSlotBase + i] = prepared[i];
    g_folder_layout_bound = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "folder layout bank bound=8 fields=%d/%d/%d/%d center_pool=%x inner=%#x/%#x/%#x/%#x roots=%x/%x/%x",
        cell.field, gap, width, height, center, inner.window, inner.owner_slot, inner.frame,
        inner.alignment, inner.center_pool, inner.direction_pool[0], inner.direction_pool[1]);
    return true;
}

void bind_folder_geometry() {
    resolve_grid_fields(home_layout::kDropGeometrySymbol);
    if (!g_grid_field.usable()) return;
    if (g_slots[kFolderGeometrySlotBase].address != 0 || !g_dart) return;
    const void *entries[] = {reinterpret_cast<void *>(hc_layout_folder_0_entry),
        reinterpret_cast<void *>(hc_layout_folder_1_entry),
        reinterpret_cast<void *>(hc_layout_folder_2_entry),
        reinterpret_cast<void *>(hc_layout_folder_3_entry), reinterpret_cast<void *>(hc_layout_folder_4_entry)};
    std::array<Slot, 5> candidates{};
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto &spec = home_layout::kFolderGeometrySites[i];
        uint32_t va = 0, size = 0;
        std::vector<uint32_t> words;
        if (!hometweaks::HomeTweaksFindSymbol(spec.symbol, &va, &size)) return;
        // The site is wherever its four-word run is, and only there: see the note on
        // FolderGeometrySite. Reading the whole body is what admits the complete known
        // function, so the reported symbol size is not consulted at all.
        uint32_t span = 0;
        if (!dartscan::function_span(va, &span)) return;
        std::vector<uint32_t> body;
        if (!dart_function_words(va, span, body)) return;
        uint32_t offset = 0;
        if (!dartscan::unique_sequence(body, spec.words, std::size(spec.words), &offset)
            || offset + 16 > span) return;
        // The scalar carry crosses original Dart calls. Admit only the complete
        // known bodies, including the outgoing-argument writes and last screen read.
        const auto full = i < 2 || i == 4
            ? std::span<const uint32_t>(home_layout::kFolderPositionOriginal)
            : std::span<const uint32_t>(home_layout::kFolderSizeOriginal);
        if (body.size() < full.size()
            || !std::equal(full.begin(), full.end(), body.begin())) return;
        // Validate the owning frame. The continuation is exactly patch+16.
        if (body[0] != kDartPrologue || body[1] != 0xaa0f03fd || body[2] != full[2]) return;
        auto &slot = candidates[i];
        if (!bind_dart_target(va + offset, slot.address, slot.source, slot.original_words)) return;
        slot.replacement = const_cast<void *>(entries[i]);
        slot.original = &hc_layout_folder_original[i];
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        g_slots[kFolderGeometrySlotBase + i] = candidates[i];
        hc_layout_folder_resume[i] = candidates[i].address + 16;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "folder geometry bank bound at scanned sites=%zu first=%#llx", candidates.size(),
        static_cast<unsigned long long>(candidates[0].address));
}

// All consumers use one atomically admitted geometry contract from owning bodies.
home_layout::DropGeometryFields drop_geometry_fields() {
    return home_layout::drop_geometry_fields(g_grid_field);
}
void resolve_grid_fields(const char *symbol) {
    if (g_grid_field.columns > 0) return;
    const auto read = [](const char *name, uint32_t &va, std::vector<uint32_t> &body) {
        uint32_t size = 0, span = 0;
        return hometweaks::HomeTweaksFindSymbol(name, &va, &size)
            && dartscan::function_span(va, &span) && dart_function_words(va, span, body);
    };
    uint32_t drop = 0, cell = 0, occupied = 0, counts = 0, config = 0, unused = 0;
    std::vector<uint32_t> db, cb, ob, nb;
    if (!read(symbol, drop, db) || !read("GridCellDelegate.performLayout", cell, cb)
        || !read("GridOccupiedCellDelegate.performLayout", occupied, ob)
        || !read("CellLayoutGetxController.isItemPosEmpty", counts, nb)
        || !hometweaks::HomeTweaksFindSymbol("GridController.currentConfig", &config, &unused)) return;
    auto found = home_layout::read_grid_field_offsets(db);
    home_layout::read_grid_cell_size(cb, found);
    // Prove the origin's receiver is the same named config used by the bounds consumer.
    size_t origin_calls = 0;
    for (size_t i = 0; i + 3 < db.size(); ++i) {
        int32_t off = 0;
        if (home_layout::dart_call_to(db[i], drop + static_cast<uint32_t>(i * 4), config)
            && home_layout::dart_ldur_w(db[i + 1], 0, 1, &off) && off == found.origin
            && db[i + 2] == 0x8b1c8021 && db[i + 3] == 0xfc407020) ++origin_calls;
    }
    if (origin_calls != 1 || found.cell_width <= 0 || found.cell_height <= 0
        || !home_layout::read_grid_counts(nb, counts, config, found)
        || !home_layout::read_occupied_fields(ob, found)) return;
    uint32_t dock = 0; std::vector<uint32_t> hb;
    if (read("HotSeatLayoutDelegate.cellLayout", dock, hb)) home_layout::read_hotseat_fields(hb, found);
    g_grid_field = found;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "grid contract admitted cols=%d rows=%d origin=%d item=%d,%d cell=%d,%d occupied=%d",
        found.columns, found.rows, found.origin, found.item_col, found.item_row,
        found.cell_width, found.cell_height, found.occupied_grid);
}

void bind_drop_geometry() {
    if (g_slots[kDropGeometrySlotBase].address != 0 || !g_dart) return;
    uint32_t va = 0, size = 0;
    if (!hometweaks::HomeTweaksFindSymbol(home_layout::kDropGeometrySymbol, &va, &size)) return;
    // The whole body is the identity here -- it is what the field offsets below are read out
    // of -- so it is scanned to the next symbol rather than to the reported size, which
    // understates this body and would cut the search short of most of it.
    uint32_t span = 0;
    if (!dartscan::function_span(va, &span)) return;
    std::vector<uint32_t> body;
    if (!dart_function_words(va, span, body)) return;
    if (body[0] != kDartPrologue || body[1] != 0xaa0f03fd) return;
    // The GridInfo fields this hook reads, resolved from the body rather than stored. Any that
    // the code does not pin down stays at -1 and the body declines to adjust, which leaves the
    // drop where the stock code would have put it.
    resolve_grid_fields(home_layout::kDropGeometrySymbol);
    if (!g_grid_field.usable()) return;
    const void *entries[] = {reinterpret_cast<void *>(hc_layout_drop_0_entry),
        reinterpret_cast<void *>(hc_layout_drop_1_entry)};
    std::array<Slot, 2> candidates{};
    for (size_t i = 0; i < candidates.size(); ++i) {
        uint32_t offset = 0;
        if (!dartscan::unique_sequence(body, home_layout::kDropGeometrySites[i].words,
                std::size(home_layout::kDropGeometrySites[i].words), &offset)) return;
        auto &slot = candidates[i];
        if (!bind_dart_target(va + offset, slot.address, slot.source,
                slot.original_words)) return;
        slot.replacement = const_cast<void *>(entries[i]);
        slot.original = &hc_layout_drop_original[i];
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        g_slots[kDropGeometrySlotBase + i] = candidates[i];
        hc_layout_drop_resume[i] = candidates[i].address + 16;
    }
    const home_layout::DropGeometryFields field = drop_geometry_fields();
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "drop geometry bank bound at scanned sites=%zu cols=%d rows=%d origin=%d "
        "item=%d,%d usable=%d", candidates.size(), field.columns, field.rows, field.origin,
        field.item_col, field.item_row, field.usable() ? 1 : 0);
}

size_t bind_knobs() {
    if (!ensure_dart_library()) return 0;
    resolve_grid_fields(home_layout::kDropGeometrySymbol);

    // The shipped hook targets, unless the calibration channel retargeted the knob already.
    for (size_t index = 0; index < g_knobs.size(); ++index) {
        if (kKnobHookSymbols[index] == nullptr || g_knobs[index].hook_mode) continue;
        g_knobs[index].hook_mode = true;
        g_knobs[index].symbol = kKnobHookSymbols[index];
    }

    // Hook-mode knobs: resolve the layout aggregator and bind its entry for the geometry trampoline.
    for (KnobRuntime &knob : g_knobs) {
        if (!knob.hook_mode || knob.hook_address != 0) continue;
        uint32_t va = 0;
        uint32_t size = 0;
        if (!hometweaks::HomeTweaksFindSymbol(knob.symbol.c_str(), &va, &size) || size < 16) {
            continue;
        }
        uint32_t capsule_patch_va = 0, policy_patch_offset = 0, slide_patch_offset = 0;
        if (&knob == &g_knobs[5]) {
            if (knob.symbol == "LauncherIndicatorState._wrapWithAnimation") {
                uint32_t caller_va = 0;
                if (!capsule_wrapper_layout_compatible(va, size, &caller_va)) {
                    continue;
                }
                capsule_patch_va = caller_va;
                knob.hook_entry = reinterpret_cast<void *>(hc_layout_capsule_entry);
            } else {
                knob.hook_entry = reinterpret_cast<void *>(hc_layout_dart_IndicatorMargin_entry);
            }
        }
        const bool indicator_policy = &knob == &g_knobs[6]
            && knob.symbol == "LauncherIndicatorState._buildScreenIndicator";
        if (indicator_policy) {
            IndicatorPolicyAnchors anchors;
            if (!indicator_policy_compatible(va, size, &anchors)) continue;
            policy_patch_offset = anchors.gate;
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_indicator_policy_entry);
        }
        const bool indicator_slide = &knob == &g_knobs[7]
            && knob.symbol == "LauncherIndicatorState._showIndicator";
        if (indicator_slide) {
            uint32_t idle_caller = 0;
            if (!indicator_slide_compatible(va, &slide_patch_offset, &idle_caller)) continue;
            hc_layout_indicator_idle_caller = g_dart->load_base + idle_caller;
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_indicator_slide_only_entry);
        }
        uint32_t geometry_patch_offset = 0;
        const bool workspace_cell = &knob == &g_knobs[2]
            && knob.symbol == "GridCellDelegate.performLayout";
        const bool workspace_occupied = &knob == &g_knobs[3]
            && knob.symbol == "GridOccupiedCellDelegate.performLayout";
        const bool hotseat_horizontal = &knob == &g_knobs[4]
            && knob.symbol == "HotSeatLayoutDelegate.cellLayout";
        if (workspace_cell || workspace_occupied) {
            if (!g_grid_field.usable()) continue;
            if (!workspace_geometry_code_compatible(va, workspace_occupied, &geometry_patch_offset)) continue;
            knob.hook_entry = workspace_cell
                ? reinterpret_cast<void *>(hc_layout_workspace_entry)
                : reinterpret_cast<void *>(hc_layout_workspace_occupied_entry);
        }
        if (&knob == &g_knobs[2] && knob.symbol == "Container.build") {
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_container_probe_entry);
        }
        if (hotseat_horizontal) {
            if (g_grid_field.dock_columns <= 0 || g_grid_field.dock_item <= 0
                || g_grid_field.dock_info <= 0) continue;
            if (!hotseat_geometry_code_compatible(va, &geometry_patch_offset)) continue;
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_hotseat_horizontal_entry);
        }
        const bool workspace_splice = workspace_cell || workspace_occupied || hotseat_horizontal
            || capsule_patch_va != 0 || indicator_policy || indicator_slide;
        const uint32_t patch_va = capsule_patch_va != 0 ? capsule_patch_va : va + (indicator_slide ? slide_patch_offset : indicator_policy ? policy_patch_offset : (workspace_cell || workspace_occupied || hotseat_horizontal) ? geometry_patch_offset : 0);
        if (!bind_dart_target(patch_va, knob.hook_address, knob.hook_source, knob.hook_words)) continue;
        if (!workspace_splice && knob.hook_words[0] != kDartPrologue) {
            knob.hook_address = 0;
            continue;
        }
        knob.getter = va;
        knob.getter_size = size;
    }
    // The page-dot companion is independent of the field-write experiments: it is a hook target, so
    // it binds and arms even when the field-write channel is off (the default).
    bind_indicator_dot_target();
    bind_folder_geometry();
    bind_drop_geometry();
    if (!g_field_writes_enabled.load(std::memory_order_relaxed)) {
        size_t hooked = 0;
        for (const KnobRuntime &knob : g_knobs) {
            if (knob.hook_address != 0) ++hooked;
        }
        return hooked;
    }

    if (g_config_capture_va == 0) {
        uint32_t size = 0;
        hometweaks::HomeTweaksFindSymbol("GridController.currentConfig", &g_config_capture_va,
            &size);
    }
    if (g_dock_capture_va == 0) {
        uint32_t size = 0;
        hometweaks::HomeTweaksFindSymbol("HotSeatsConstants2._dockGridConfig", &g_dock_capture_va,
            &size);
    }
    if (g_config_capture_va == 0 || g_dock_capture_va == 0) return 0;

    // Pass one: accessors that reach a captured singleton, decoded into a field path.
    for (KnobRuntime &knob : g_knobs) {
        if (knob.hook_mode || knob.path.load(std::memory_order_relaxed) != 0) continue;
        uint32_t va = 0;
        uint32_t size = 0;
        if (!hometweaks::HomeTweaksFindSymbol(knob.symbol.c_str(), &va, &size) || size < 16) {
            continue;
        }
        std::vector<uint32_t> code;
        // Bound the scan by the symbol's own size: reading past the end would pick a neighbouring
        // function's field read, which is how searchBarWidthPx first decoded as searchBarWidthDeltaPx.
        const size_t words = std::clamp<size_t>(size / 4, 8, 48);
        if (!dart_words(va, words, code) || code.empty() || code[0] != kDartPrologue) continue;
        FieldPath path;
        if (!decode_path(code, va, g_config_capture_va, g_dock_capture_va, path)) continue;
        knob.getter = va;
        knob.path.store(pack_path(path.object, path.off0, path.off1), std::memory_order_release);
        /*
         * The decoded path, printed once when it binds. The startup snapshot prints each knob before
         * `bind_knobs` has run, so without this line the offset that is actually written - the one
         * thing a field-write experiment has to get right - is never visible on the device.
         */
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "layout field path %s object=%u off0=%d off1=%#x va=%#x",
            knob.symbol.c_str(), static_cast<unsigned>(path.object), path.off0, path.off1, va);
        if (path.off0 > 0 && g_device_object_offset < 0) g_device_object_offset = path.off0;
    }
    /*
     * There is deliberately no second pass.
     *
     * A fallback used to take the nested-object offset from a *different* accessor and combine it with
     * a receiver-relative field read, which produced a path nothing had proved: the offset belonged to
     * one object and the field to whatever the caller happened to pass. Writing an eight-byte double
     * through such a path corrupted the Dart heap and the launcher died inside libhyper_os_flutter.so.
     * A knob is now bound only when its own accessor decodes the whole path - a call to a captured
     * singleton followed by the field read - and is otherwise simply left unbound.
     */
    size_t bound = 0;
    for (const KnobRuntime &knob : g_knobs) {
        const bool ready = knob.hook_mode ? knob.hook_address != 0
                                          : knob.path.load(std::memory_order_relaxed) != 0;
        if (ready) ++bound;
    }
    return bound;
}

size_t arm_captures() {
    /*
     * Every refusal says which one it was, once per distinct reason.
     *
     * The pair is all-or-nothing - one failing `bind_dart_target` leaves both capture slots out - and
     * the worker's startup snapshot prints `captures=0` either way. That made "the field-write path
     * was never enabled" and "the path was enabled but a bind failed" indistinguishable on a live
     * device, which cost a whole calibration round.
     */
    const auto decline = [](const char *why) -> size_t {
        static const char *last = nullptr;
        if (last != why) {
            last = why;
            __android_log_print(ANDROID_LOG_WARN, kTag, "layout capture arm declined: %s", why);
        }
        return 0;
    };
    if (g_captures_armed) return 0;
    if (g_config_capture_va == 0 || g_dock_capture_va == 0) {
        return decline("capture symbol unresolved");
    }
    if (!bind_dart_target(g_config_capture_va, g_config_capture_address, g_config_capture_source,
            g_config_capture_words)) {
        return decline("config bind_dart_target failed");
    }
    if (!bind_dart_target(g_dock_capture_va, g_dock_capture_address, g_dock_capture_source,
            g_dock_capture_words)) {
        return decline("dock bind_dart_target failed");
    }
    g_slots[kConfigCaptureSlot] = {g_config_capture_address,
        reinterpret_cast<void *>(hc_layout_config_capture_entry),
        &hc_layout_config_capture_original, g_config_capture_source, g_config_capture_words};
    g_slots[kDockCaptureSlot] = {g_dock_capture_address,
        reinterpret_cast<void *>(hc_layout_dock_capture_entry),
        &hc_layout_dock_capture_original, g_dock_capture_source, g_dock_capture_words};
    g_captures_armed = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "layout captures armed config_va=%#x dock_va=%#x",
        g_config_capture_va, g_dock_capture_va);
    return 2;
}

bool bind_title_color() {
    if (g_title_color_bound) return true;
    if (g_title_color.load(std::memory_order_acquire) == -1 || !g_dart) return false;
    uint32_t getter = 0, getter_size = 0, clone = 0, clone_size = 0;
    if (!hometweaks::HomeTweaksFindSymbol("ShortcutIconWidget.getTextColor", &getter,
            &getter_size) || getter_size != 0x4c
        || !hometweaks::HomeTweaksFindSymbol("Color.withAlpha", &clone, &clone_size)
        || clone_size != 0xd4) return false;
    std::vector<uint32_t> getter_code, clone_code;
    uint32_t ignored = 0, factory = 0;
    if (!dartscan::body(getter, getter_code) || getter_code.size() < 4 || getter_code[0] != kDartPrologue
        || !dartscan::body(clone, clone_code) || clone_code.size() < 50 || clone_code[0] != kDartPrologue
        || !dartscan::site(clone, {0xb8027001, 0xfc5d03a0, 0xfc007000}, &ignored)
        || !dartscan::site(clone, {0xfc00f002}, &ignored)
        || !dartscan::site(clone, {0xfc017002}, &ignored)
        || !dartscan::site(clone, {0xfc01f002}, &ignored)
        || !dartscan::tagged_call(clone, 0xd28e6382, 0xf2a047a2, &factory)) return false;
    std::vector<uint32_t> factory_code, nursery_code;
    uint32_t nursery = 0;
    if (!dart_words(factory, 3, factory_code) || (factory_code[2] & 0xfc000000) != 0x14000000
        || !bl_target(factory_code[2] | 0x80000000, factory + 8, &nursery)
        || !dart_words(nursery, 7, nursery_code)
        || nursery_code[0] != 0xd3482c44 || nursery_code[1] != 0xd37cec84
        || nursery_code[2] != 0xa9461740 || nursery_code[3] != 0x8b040003
        || nursery_code[4] != 0xeb0300bf || nursery_code[6] != 0xf9003343) return false;
    uint32_t builder = 0, builder_size = 0;
    uint32_t selected_offset = 0;
    if (!hometweaks::HomeTweaksFindSymbol("ShortcutIconWidget._buildTextWidget", &builder, &builder_size)
        || !dartscan::site(builder, {0xf81d83a1, 0xf9403f40, 0xf9538800, 0x6b16001f}, &selected_offset)) return false;
    uintptr_t address = 0;
    nhk::CodeSource source{};
    Words words{};
    if (!bind_dart_target(builder + selected_offset, address, source, words)
        || address > UINTPTR_MAX - 16) return false;
    hc_title_color_continue = address + 16;
    g_slots[kTitleColorSlot] = {address, reinterpret_cast<void *>(hc_title_color_entry),
        &hc_title_color_original, source, words};
    g_title_color_bound = true;
    return true;
}

/* Patch the no-call load/store window after AppIcon.build returns from its original font
 * getter. Its original Dart call PC/frame remain intact even on the initializer/GC path. */
// Replay only verified non-allocating model fields and the original nursery fast path.
// Bind once to this AOT image; no title/model/database mutation or relocated Dart BL.
bool bind_title_custom() {
    if (g_title_custom_bound) return true;
    if (!g_dart || !__atomic_load_n(&hc_title_custom_enabled, __ATOMIC_ACQUIRE)) return false;
    const auto guard = [](const char *name, uint32_t size, uint32_t offset,
                          std::initializer_list<uint32_t> expected, uint32_t &va) {
        uint32_t actual_size = 0; std::vector<uint32_t> code;
        if (!hometweaks::HomeTweaksFindSymbol(name, &va, &actual_size) || !dartscan::body(va, code)) return false;
        if (offset == 0) return actual_size == size && code.size() >= expected.size()
            && std::equal(expected.begin(), expected.end(), code.begin());
        uint32_t located = 0;
        if (!dartscan::unique_sequence(code, expected.begin(), expected.size(), &located)) return false;
        va += located; return true;
    };
    uint32_t component = 0, pin = 0, ignored = 0, builder = 0;
    if (!guard("ShortcutInfoModel.getPackageName", 0x70, 0, {
        0xa9bf79fdu, 0xaa0f03fdu, 0xd10021efu, 0xaa0103e2u, 0xf81f83a1u, 0xf85ff040u, 0xd34c7c00u, 0xaa0203e1u, 0xd13f5c1eu, 0xf87e7abeu, 0xd63f03c0u, 0x6b16001fu, 0x54000061u, 0xaa1603e1u, 0x14000003u, 0xb8407001u, 0x8b1c8021u, 0x6b16003fu, 0x540000c1u, 0xf85f83a2u, 0xb84c3043u, 0x8b1c8063u, 0xaa0303e0u, 0x14000002u, 0xaa0103e0u, 0xaa1d03efu, 0xa8c179fdu, 0xd65f03c0u}, ignored)) return false;
    if (!guard("ShortcutInfoModel.getComponentName", 0x48, 0, {
        0xd28020f1u, 0xb8716822u, 0x8b1c8042u, 0x36200062u, 0xaa1603e0u, 0xd65f03c0u, 0xb84bb022u, 0x8b1c8042u, 0xf9402370u, 0x6b10005fu, 0x54000080u, 0xb841f040u, 0x8b1c8000u, 0xd65f03c0u, 0xa9bf79fdu, 0xaa0f03fdu, 0xf97ecf69u, 0x940c1624u}, component)) return false;
    if (!guard("PinShortcutInfoModel.getComponentName", 0x1c, 0, {
        0xa9bf79fdu, 0xaa0f03fdu, 0xaa0103e0u, 0x97c2d5c8u, 0xaa1d03efu, 0xa8c179fdu, 0xd65f03c0u}, pin)) return false;
    if (!guard("allocateTwoByteString", 0xec, 0, {
        0xf94001e2u, 0x93407c42u, 0x37000362u, 0xb27c37f1u, 0x6b11005fu, 0x54000308u, 0xaa0203e6u, 0x91007c42u, 0x927cec42u, 0xf9403340u, 0xab020001u, 0x54000242u, 0xf9403747u, 0xeb07003fu, 0x540001e2u, 0xf9003341u, 0x91000400u, 0xa93f7c3fu, 0xf103c05fu, 0xd37cec42u, 0x9a9f9042u, 0xd29e0b90u, 0xf2a000b0u, 0xaa100042u, 0xf81ff002u, 0xf800701fu, 0xb8007006u, 0x14000001u, 0xd65f03c0u}, ignored)) return false;
    // Verify model, Intent and ComponentName class ids through their original factory calls.
    struct Factory { const char *name; uint32_t first, second; };
    constexpr Factory factories[] = {
        {"ShortcutInfoModel.copyShortcutModel", 0xd28a0382, 0xf2a00fe2},
        {"PinShortcutInfoModel.copyShortcutModel", 0xd2980382, 0xf2a00fe2},
        {"PinShortcutInfoModel.copyShortcutModel", 0xd2906382, 0xf2a01ae2},
        {"PinShortcutInfoModel.makePinAppComponentName", 0xd2962382, 0xf2a01ae2},
    };
    for (const auto &factory : factories) {
        uint32_t va = 0, size = 0;
        if (!hometweaks::HomeTweaksFindSymbol(factory.name, &va, &size)
            || !dartscan::tagged_call(va, factory.first, factory.second, nullptr)) return false;
    }
    if (!guard("ShortcutIconWidget.getPrefixAssetName", 0x74, 0x30,
        {0xd2802a71u, 0xb8716801u, 0x8b1c8021u}, ignored)
        || !guard("ShortcutIconWidget._buildTextWidget", 0x714, 0x214,
        {0xf85e83a2u, 0xb8447043u, 0x8b1c8063u, 0xf81c83a3u}, builder)) return false;
    uintptr_t address = 0; nhk::CodeSource source{}; Words words{};
    if (!bind_dart_target(builder, address, source, words)
        || address > UINTPTR_MAX - 16
        || g_dart->load_base > UINTPTR_MAX - component
        || g_dart->load_base > UINTPTR_MAX - pin) return false;
    g_title_component_method = g_dart->load_base + component;
    g_title_pin_method = g_dart->load_base + pin;
    hc_title_custom_continue = address + 16;
    g_slots[kTitleCustomSlot] = {address, reinterpret_cast<void *>(hc_title_custom_entry),
        &hc_title_custom_original, source, words};
    g_title_custom_bound = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "title custom original-code bank bound; Dart8; model/database unchanged");
    return true;
}

bool bind_drawer_title() {
    if (g_drawer_title_bound) return true;
    if (!g_dart || (g_title_drawer_sp.load(std::memory_order_acquire) == 12
        && !g_title_custom_bound)) return false;
    uint32_t getter = 0, getter_size = 0, caller = 0, caller_size = 0;
    if (!hometweaks::HomeTweaksFindSymbol("GridConfig.getTitleTextSize", &getter,
            &getter_size) || getter_size != 0x68
        || !hometweaks::HomeTweaksFindSymbol("AppIcon.build", &caller, &caller_size)
        || caller_size != 0x8b8) return false;
    uint32_t font_offset = 0, height_offset = 0, target = 0;
    std::vector<uint32_t> body;
    if (!dartscan::site(caller, {0xf85f83a0, 0xfc1c03a0, 0xb8413001, 0x8b1c8021}, &font_offset)
        || !dartscan::site(caller, {0xf85f03a0, 0xfc1b83a0, 0xb845f001, 0x8b1c8021}, &height_offset)
        || font_offset < 4 || !dartscan::body(caller, body)
        || !bl_target(body[font_offset / 4 - 1], caller + font_offset - 4, &target)
        || target != getter) return false;
    uintptr_t height_address = 0;
    nhk::CodeSource height_source{};
    Words height_words{};
    if (!bind_dart_target(caller + height_offset, height_address, height_source, height_words)
        || height_address > UINTPTR_MAX - 16) return false;
    uintptr_t address = 0;
    nhk::CodeSource source{};
    Words words{};
    if (!bind_dart_target(caller + font_offset, address, source, words)
        || g_dart->load_base > UINTPTR_MAX - caller - font_offset) return false;
    hc_drawer_title_caller = static_cast<uintptr_t>(g_dart->load_base + caller + font_offset);
    hc_drawer_title_continue = address + 16;
    g_slots[kDrawerTitleSlot] = {address, reinterpret_cast<void *>(hc_drawer_title_entry),
        &hc_drawer_title_original, source, words};
    hc_drawer_title_height_continue = height_address + 16;
    g_slots[kDrawerTitleHeightSlot] = {height_address,
        reinterpret_cast<void *>(hc_drawer_title_height_entry), &hc_drawer_title_height_original,
        height_source, height_words};
    g_drawer_title_bound = true;
    return true;
}

/*
 * Install the hook trampolines for every knob calibrated onto a layout aggregator. A knob whose
 * aggregator is not resolved yet is simply skipped and retried by the health loop.
 */
// Capacity patches are ordinary aligned original-image instructions. No Dart
// getter replacement, heap edits, extra hook slots, per-frame helper, or timer.
// Loader owns the first synchronous bank publication; the maintenance worker
// starts binding only after it has finished. No two setup threads can claim
// hook_armed before the first continuation is registered.
std::atomic<bool> g_loader_prime_finished{false};
std::atomic_flag g_loader_priming = ATOMIC_FLAG_INIT;
std::atomic_flag g_capacity_busy = ATOMIC_FLAG_INIT;
home_layout::CapacityWord g_capacity_words[home_layout::kCapacitySiteCount]{};
bool g_capacity_bound = false;
bool g_capacity_enabled = false;
bool g_capacity_known = true;

bool sync_hotseat_capacity(bool enabled) {
    if (g_capacity_busy.test_and_set(std::memory_order_acquire)) return false;
    struct Release { ~Release() { g_capacity_busy.clear(std::memory_order_release); } } release;
    if (!g_dart) return false;
    if (!g_capacity_bound) {
        home_layout::CapacityWord candidate[home_layout::kCapacitySiteCount]{};
        for (int i = 0; i < home_layout::kCapacitySiteCount; ++i) {
            const auto &site = home_layout::kCapacitySites[i];
            uint32_t va = 0, size = 0;
            std::vector<uint32_t> body;
            if (!hometweaks::HomeTweaksFindSymbol(site.symbol, &va, &size)) return false;
            if (!dart_words(va, 4, body) || body[0] != kDartPrologue) return false;
            // The guard run is the site; the patched instruction is the word it ends on. The
            // reported symbol size is not consulted: it is shorter than the real body for
            // `calculatePositionX` (0x18c against a 0x138 offset), so requiring it to match
            // would refuse a site that is present and unchanged.
            uint32_t span = 0;
            if (!dartscan::function_span(va, &span)) return false;
            std::vector<uint32_t> full;
            if (!dart_function_words(va, span, full)) return false;
            uint32_t guard_at = 0;
            if (!dartscan::unique_sequence(full, site.guard, home_layout::kCapacityGuardWords,
                    &guard_at)) {
                return false;
            }
            const uint32_t offset = guard_at + home_layout::kCapacityGuardLead;
            if (offset + 4 > span) return false;
            // source() pins the runtime address to this loaded image, not another libapp mapping.
            if (g_dart->load_base > UINTPTR_MAX - va - offset) return false;
            const uintptr_t address = g_dart->load_base + va + offset;
            const auto file = dart_file_offset(va + offset, 4);
            const auto origin = nhk::source_at(g_dart->owned, address, 4);
            if (!file || !origin || origin->file_offset != *file || (address & 3)) return false;
            candidate[i] = {address, site.guard[home_layout::kCapacityGuardWords - 1],
                site.replacement};
        }
        std::copy(std::begin(candidate), std::end(candidate), std::begin(g_capacity_words));
        g_capacity_bound = true;
        __android_log_print(ANDROID_LOG_INFO, kTag, "hotseat capacity original-code bank bound sites=%d",
            home_layout::kCapacitySiteCount);
    }
    if (g_capacity_known && enabled == g_capacity_enabled) return true;
    const auto read = [](uintptr_t address, uint32_t &word) {
        return nhk::safe_read(address, std::as_writable_bytes(std::span(&word, 1)));
    };
    const auto write = [](uintptr_t address, uint32_t word) {
        return nhk::write_code_bytes(address, std::as_bytes(std::span(&word, 1)));
    };
    const bool applied = home_layout::apply_capacity_words(g_capacity_words, enabled, read, write);
    g_capacity_known = applied;
    if (applied) g_capacity_enabled = enabled;
    __android_log_print(applied ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
        "hotseat capacity original-code requested=%d applied=%d sites=%d getters=unchanged",
        enabled ? 1 : 0, applied ? 1 : 0, home_layout::kCapacitySiteCount);
    return applied;
}

// One original-body word; all drag state, widget type, span and handoff checks stay native.
std::atomic_flag g_widget_move_busy = ATOMIC_FLAG_INIT;
uintptr_t g_widget_move_address = 0;
uint32_t g_widget_move_original = 0;
bool g_widget_move_checked = false;
bool g_widget_move_enabled = false;
bool g_widget_move_known = true;
std::atomic<bool> g_gadget_requested{false};
bool g_gadget_bound = false;

bool sync_widget_move(bool enabled) {
    g_gadget_requested.store(enabled, std::memory_order_release);
    if (g_widget_move_busy.test_and_set(std::memory_order_acquire)) return false;
    struct Release { ~Release() { g_widget_move_busy.clear(std::memory_order_release); } } release;
    if (!g_dart || (!enabled && !g_widget_move_address)) return !enabled;
    if (!g_widget_move_checked) {
        g_widget_move_checked = true;
        uint32_t va = 0, size = 0;
        /*
         * No neighbour-placement assertion here. `_isSpanSupportedByPa` and
         * `_ensureDragSessionId` have independent symbols, so a fixed distance or ordering says
         * nothing about whether the hook is still aimed at the right code. The scan below is
         * what decides: it either finds exactly one branch whose shape identifies the widget
         * rejection, or the hook stays off.
         */
        bool compatible = hometweaks::HomeTweaksFindSymbol(home_layout::kWidgetMoveSymbol, &va, &size);
        size_t gate = SIZE_MAX;
        int32_t flag_field = 0;
        uint32_t body_span = 0, span_callee = 0, span_size = 0;
        if (compatible) compatible = dartscan::function_span(va, &body_span)
            && hometweaks::HomeTweaksFindSymbol("AssistantDragToPAHandler._isSpanSupportedByPa",
                &span_callee, &span_size);
        if (compatible) {
            gate = home_layout::widget_move_gate_offset(body_span,
                [&](size_t count, std::vector<uint32_t> &out) {
                    return dart_function_words(va, static_cast<uint32_t>(count), out);
                }, &flag_field, va, span_callee);
            compatible = gate != SIZE_MAX && gate + 4 <= body_span;
        }
        if (compatible && g_dart->load_base <= UINTPTR_MAX - va - gate) {
            const uintptr_t address = g_dart->load_base + va + gate;
            const auto file = dart_file_offset(va + static_cast<uint32_t>(gate), 4);
            const auto source = nhk::source_at(g_dart->owned, address, 4);
            if (file && source && source->file_offset == *file && !(address & 3)) {
                std::vector<uint32_t> original;
                if (dart_words(va + static_cast<uint32_t>(gate), 1, original)) {
                    g_widget_move_original = original[0];
                    g_widget_move_address = address;
                }
            }
        }
        __android_log_print(g_widget_move_address ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
            "widget minus-one original-code compatible=%d symbol=%s gate=%#zx flag=%#x",
            g_widget_move_address != 0, home_layout::kWidgetMoveSymbol, gate,
            static_cast<uint32_t>(flag_field));
    }
    if (!g_widget_move_address) return false;
    if (g_widget_move_known && enabled == g_widget_move_enabled) return true;
    const auto read = [](uintptr_t address, uint32_t &word) {
        return nhk::safe_read(address, std::as_writable_bytes(std::span(&word, 1)));
    };
    const auto write = [](uintptr_t address, uint32_t word) {
        return nhk::write_code_bytes(address, std::as_bytes(std::span(&word, 1)));
    };
    const bool applied = home_layout::apply_widget_move_word(g_widget_move_address, enabled, read, write, g_widget_move_original);
    g_widget_move_known = applied;
    if (applied) g_widget_move_enabled = enabled;
    __android_log_print(applied ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
        "widget minus-one original-code requested=%d applied=%d span=original model=unchanged",
        enabled ? 1 : 0, applied ? 1 : 0);
    return applied;
}

/* Inject the exact inlined load that _buildTextWidget actually uses. It is a different cache
 * from GridController.textSize; changing that getter's field does not change rendered titles.
 * No heap/config mutation, allocation, GC call or background writer is involved. */
bool bind_desktop_title() {
    if (g_desktop_title_bound) return true;
    if (!g_dart || (g_title_desktop_sp.load(std::memory_order_acquire) == 12
        && g_title_drawer_sp.load(std::memory_order_acquire) == 12)) return false;
    uint32_t va = 0, size = 0;
    if (!hometweaks::HomeTweaksFindSymbol("ShortcutIconWidget._buildTextWidget", &va, &size)
        || size != 0x714) return false;
    // Both desktop and modern drawer use this builder. Only the drawer/search creates
    // customShortcutIconConfig (true at +0xb); preserve independent settings at that branch.
    uint32_t custom = 0, custom_size = 0;
    std::vector<uint32_t> custom_code;
    uint32_t font_offset = 0, height_offset = 0, config_offset = 0;
    if (!hometweaks::HomeTweaksFindSymbol("ShortcutIconWidgetConfig.customShortcutIconConfig", &custom, &custom_size)
        || !dartscan::body(custom, custom_code) || custom_code.empty() || custom_code[0] != kDartPrologue
        || !dartscan::site(custom, {0xb8007001, 0x910082c1, 0xb800b001}, &config_offset)
        || !dartscan::tagged_call(custom, 0xd29a2382, 0xf2a012c2, nullptr)
        || !dartscan::site(va, {0xfc443000, 0xf85f83a0, 0xfc1a83a0, 0xb8417001,
            0x8b1c8021, 0xb840b022, 0x8b1c8042}, &font_offset)
        || !dartscan::site(va, {0xb846f001, 0x8b1c8021, 0xfc427020, 0xfc1a03a0}, &height_offset)) return false;
    uintptr_t height_address = 0;
    nhk::CodeSource height_source{};
    Words height_words{};
    if (!bind_dart_target(va + height_offset, height_address, height_source, height_words)
        || height_address > UINTPTR_MAX - 16) return false;
    uintptr_t address = 0;
    nhk::CodeSource source{};
    Words words{};
    if (!bind_dart_target(va + font_offset, address, source, words)
        || address > UINTPTR_MAX - 16) return false;
    hc_desktop_title_continue = address + 16;
    g_slots[kDesktopTitleSlot] = {address, reinterpret_cast<void *>(hc_desktop_title_entry),
        &hc_desktop_title_original, source, words};
    hc_desktop_title_height_continue = height_address + 16;
    g_slots[kDesktopTitleHeightSlot] = {height_address,
        reinterpret_cast<void *>(hc_desktop_title_height_entry), &hc_desktop_title_height_original,
        height_source, height_words};
    g_desktop_title_bound = true;
    return true;
}

/* Hide appearance only; never clear database flags, shortcut models or installation lists. */
bool bind_title_hide() {
    if (g_title_hide_bound) return true;
    if (!__atomic_load_n(&hc_title_hide_new_install, __ATOMIC_ACQUIRE) || !g_dart) return false;
    constexpr const char *names[] = {"ShortcutIconWidget.getPrefixAssetName",
        "FolderInfoModel.hasNewInstalledApp", "ShortcutIconWidget._addNewInstallLight"};
    constexpr uint32_t sizes[] = {0x74, 0xc8, 0x104};
    constexpr size_t slots[] = {kTitlePrefixSlot, kTitleFolderNewSlot, kTitleLightSlot};
    void *entries[] = {reinterpret_cast<void *>(hc_title_prefix_entry),
        reinterpret_cast<void *>(hc_title_folder_new_entry), reinterpret_cast<void *>(hc_title_light_entry)};
    void **originals[] = {&hc_title_prefix_original, &hc_title_folder_new_original, &hc_title_light_original};
    uint32_t vas[3]{}, size = 0, caller = 0, caller_size = 0;
    std::vector<uint32_t> words;
    for (unsigned index = 0; index < 3; ++index) {
        if (!hometweaks::HomeTweaksFindSymbol(names[index], &vas[index], &size)
            || size != sizes[index] || !dart_words(vas[index], 4, words)
            || words[0] != kDartPrologue) return false;
    }
    if (!hometweaks::HomeTweaksFindSymbol("FolderIconGetxController.updateNewInstallNotification", &caller, &caller_size)
        || !dartscan::body(caller, words)) return false;
    size_t hits = 0; uint32_t return_pc = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        uint32_t target = 0;
        if (bl_target(words[i], caller + static_cast<uint32_t>(i * 4), &target) && target == vas[1]) {
            ++hits; return_pc = caller + static_cast<uint32_t>((i + 1) * 4);
        }
    }
    uint32_t prefix = 0;
    if (hits != 1 || g_dart->load_base > UINTPTR_MAX - return_pc
        || !dartscan::site(vas[0], {0xf9407f61, 0xaa0103e0, 0xaa1d03ef, 0xa8c179fd, 0xd65f03c0}, &prefix)) return false;
    prefix += 4;
    Slot prepared[3]{};
    for (unsigned index = 0; index < 3; ++index) {
        uintptr_t address = 0; nhk::CodeSource source{}; Words original{};
        if (!bind_dart_target(vas[index] + (index == 0 ? prefix : 0), address, source, original))
            return false;
        prepared[index] = {address, entries[index], originals[index], source, original};
    }
    for (unsigned index = 0; index < 3; ++index) g_slots[slots[index]] = prepared[index];
    hc_title_folder_new_caller = g_dart->load_base + return_pc;
    g_title_hide_bound = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "title hide appearance bank bound=3; database flags preserved");
    return true;
}

/*
 * Locate every position the Gadget bridge needs by scanning the loaded image for instruction
 * semantics. See home_gadget_bridge.h for why a zero- or two-match scan is a hard failure.
 * Nothing below carries a displacement: the sizes come from the symbol table and the offsets
 * come from the scans.
 */
bool gadget_scan_anchors(uint32_t serializer, uint32_t serializer_size,
    uint32_t put_int_va, uint32_t gate, uint32_t gate_size, uint32_t span_callee,
    home_layout::GadgetAnchors &out) {
    using namespace home_layout;
    out = {};
    std::vector<uint32_t> body, gate_body;
    if (!dart_function_words(serializer, serializer_size, body)
        || !dart_function_words(gate, gate_size, gate_body)) return false;
    const auto run = [](const auto &v, size_t at, std::initializer_list<uint32_t> words) {
        return at <= v.size() && words.size() <= v.size() - at
            && std::equal(words.begin(), words.end(), v.begin() + at);
    };
    const auto calls = [](uint32_t word, uint32_t base, size_t index, uint32_t callee) {
        return is_bl(word) && static_cast<int64_t>(base) + static_cast<int64_t>(index) * 4
            + static_cast<int64_t>(bl_imm_words(word)) * 4 == callee;
    };
    // These replay windows are an ABI contract, not candidate addresses. The assembly still
    // uses these registers, stack roots and pool fields; a changed ABI must decline.
    size_t put_hits = 0, select_hits = 0, common_hits = 0, span_hits = 0, widget_hits = 0;
    for (size_t i = 5; i + 4 < body.size(); ++i) {
        if (!calls(body[i], serializer, i, put_int_va)
            || !run(body, i - 5, {0xf85e83a0, 0xf85f03a1, 0x9140db62,
                0xf947fc42, 0xd2800083})
            || !run(body, i + 1, {0xf85e83a0, 0xd2802871, 0xb8716803, 0x8b1c8063})) continue;
        ++put_hits;
        out.put_int = static_cast<uint32_t>(i * 4);
        out.ret = static_cast<uint32_t>((i + 1) * 4); // original return PC, never the key load
    }
    for (size_t i = 0; i + 3 < body.size(); ++i) {
        if (body[i] == 0xf11fb43f && (body[i + 1] & 0xff00001fu) == 0x54000001u
            && body[i + 2] == 0xf85e83a0 && body[i + 3] == 0xf85f03a1) {
            ++select_hits; out.select = static_cast<uint32_t>(i * 4);
        }
        if (run(body, i, {0xf85e83a0, 0xb849f003, 0x8b1c8063, 0xf85f03a1})) {
            ++common_hits; out.common = static_cast<uint32_t>(i * 4); out.id_offset = 0x9f;
        }
    }
    for (size_t i = 1; i + 3 < gate_body.size(); ++i) {
        if (!is_tbnz_w(gate_body[i], 0, 4)
            || !calls(gate_body[i - 1], gate, i - 1, span_callee)
            || !run(gate_body, i + 1, {0xf85f83a0, 0xf85e83a2, 0xaa0003e1})) continue;
        const int64_t dest = static_cast<int64_t>(i) * 4
            + static_cast<int64_t>(branch_disp_words(gate_body[i])) * 4;
        if (dest < 0 || dest + 4 > gate_size) return false;
        ++span_hits; out.span_entry = static_cast<uint32_t>(i * 4);
        out.span_reject = static_cast<uint32_t>(dest);
    }
    if (span_hits != 1) return false;
    for (size_t i = 2; i < gate_body.size(); ++i) {
        if (!is_tbnz_w(gate_body[i], 0, 4)
            || !run(gate_body, i - 2, {0xb84fb040, 0x8b1c8000})) continue;
        const int64_t dest = static_cast<int64_t>(i) * 4
            + static_cast<int64_t>(branch_disp_words(gate_body[i])) * 4;
        if (dest != out.span_reject) continue;
        ++widget_hits; out.widget_gate = static_cast<uint32_t>(i * 4);
    }
    // The original rooted closure/model path used by the span stub must still exist exactly once.
    size_t root_hits = 0;
    for (size_t i = 0; i + 3 < gate_body.size(); ++i)
        if (run(gate_body, i, {0xb8407002, 0x8b1c8042, 0xaa0203e0, 0xf85e83a3})) ++root_hits;
    if (put_hits != 1 || select_hits != 1 || common_hits != 1 || widget_hits != 1 || root_hits != 1)
        return false;
    const uint32_t branch = body[out.select / 4 + 1];
    const uint32_t raw = (branch >> 5) & 0x7ffff;
    const int64_t disp = raw & 0x40000 ? static_cast<int64_t>(raw) - 0x80000 : raw;
    if (static_cast<int64_t>(out.select) + 4 + disp * 4 != out.common) return false;
    out.ok = true;
    return true;
}

// The clone field-copy ABI and allocator class tags are independent evidence.
bool gadget_clone_compatible(uint32_t clone, uint32_t clone_span) {
    std::vector<uint32_t> clone_body, code;
    if (!dart_function_words(clone, clone_span, clone_body)) return false;
    size_t factory_hits = 0, field_hits = 0;
    for (size_t i = 0; i < clone_body.size(); ++i) {
        if (i + 3 < clone_body.size() && clone_body[i] == 0xd2802371
            && clone_body[i + 1] == 0xb8716840 && clone_body[i + 2] == 0x8b1c8000
            && clone_body[i + 3] == 0xd2802371) ++field_hits;
        uint32_t candidate = 0;
        if (!bl_target(clone_body[i], clone + static_cast<uint32_t>(i * 4), &candidate)) continue;
        if (dart_words(candidate, 2, code) && code.size() == 2
            && code[0] == 0xd2840382 && code[1] == 0xf2a00fe2) ++factory_hits;
    }
    return factory_hits == 1 && field_hits == 1;
}

// Two original-body splices; original putInt BL/return PC retain the Dart stack map.
bool bind_gadget_bridge() {
    if (g_gadget_bound) return true;
    if (!g_dart || !g_gadget_requested.load(std::memory_order_acquire)) return false;
    uint32_t va = 0, size = 0, put = 0, put_size = 0, clone = 0, clone_size = 0;
    uint32_t can = 0, can_size = 0, span_callee = 0, span_size = 0;
    home_layout::GadgetAnchors anchors;
    if (!hometweaks::HomeTweaksFindSymbol(home_layout::kGadgetSerializer, &va, &size)
        || !hometweaks::HomeTweaksFindSymbol(home_layout::kGadgetBundlePut, &put, &put_size)
        || !hometweaks::HomeTweaksFindSymbol(home_layout::kWidgetMoveSymbol, &can, &can_size)
        || !hometweaks::HomeTweaksFindSymbol(home_layout::kGadgetModel, &clone, &clone_size)
        || !hometweaks::HomeTweaksFindSymbol("AssistantDragToPAHandler._isSpanSupportedByPa",
            &span_callee, &span_size)
        || !dartscan::function_span(va, &size) || !dartscan::function_span(can, &can_size)
        || !gadget_scan_anchors(va, size, put, can, can_size, span_callee, anchors)) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "Gadget12 bridge declined at scan; putInt=%x ret=%x select=%x common=%x id=%x "
            "span=%x reject=%x gate=%x",
            anchors.put_int, anchors.ret, anchors.select, anchors.common, anchors.id_offset,
            anchors.span_entry, anchors.span_reject, anchors.widget_gate);
        return false;
    }
    uint32_t clone_span = 0;
    if (!dartscan::function_span(clone, &clone_span)
        || !gadget_clone_compatible(clone, clone_span)) return false;
    Slot prepared[3]{};
    const uint32_t offsets[] = {anchors.select, anchors.ret};
    void *entries[] = {reinterpret_cast<void *>(hc_gadget_select_entry),
        reinterpret_cast<void *>(hc_gadget_return_entry)};
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = 0; nhk::CodeSource source{}; Words words{};
        if (!bind_dart_target(va + offsets[i], address, source, words)) return false;
        prepared[i] = {address, entries[i], &hc_gadget_original[i], source, words};
    }
    {
        uintptr_t address = 0; nhk::CodeSource source{}; Words words{};
        if (!bind_dart_target(can + anchors.span_entry, address, source, words)) return false;
        prepared[2] = {address, reinterpret_cast<void *>(hc_gadget_span_entry),
            &hc_gadget_original[2], source, words};
        hc_gadget_span_continue = address + 16;
        hc_gadget_span_reject = g_dart->load_base + can + anchors.span_reject;
    }
    hc_gadget_select_continue = prepared[0].address + 16;
    hc_gadget_return_continue = prepared[1].address + 16;
    hc_gadget_put_int = g_dart->load_base + va + anchors.put_int;
    hc_gadget_common = g_dart->load_base + va + anchors.common;
    for (unsigned i = 0; i < 3; ++i) g_slots[kGadgetBridgeSlot + i] = prepared[i];
    g_gadget_bound = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "Gadget12 bridge bound at scanned anchors putInt=%x ret=%x select=%x common=%x id=%x "
        "span=%x reject=%x gate=%x; no heap writes",
        anchors.put_int, anchors.ret, anchors.select, anchors.common, anchors.id_offset,
        anchors.span_entry, anchors.span_reject, anchors.widget_gate);
    return true;
}

size_t arm_hooks(std::vector<size_t> &order) {
    const bool autofit_was_checked = g_grid_autofit_checked;
    (void) bind_grid_autofit();
    if (!autofit_was_checked && g_grid_autofit_checked && !g_grid_autofit_bound)
        __android_log_print(ANDROID_LOG_WARN, kTag, "grid autofit original-code guard declined image; stock retained");
    const bool folder_was_attempted = g_folder_layout_attempted;
    (void) bind_folder_layout();
    /*
     * `attempted` rather than `checked`: the scan itself refuses long before the first symbol is
     * known good, and a refusal there used to be completely silent. The detailed reason is logged
     * by the branch that refused; this line only has to say that a refusal happened at all.
     */
    if (!folder_was_attempted && g_folder_layout_attempted && !g_folder_layout_bound)
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "folder layout original-code guard declined this launcher image; stock layout retained"
            " (first symbol resolved=%d)", g_folder_layout_checked ? 1 : 0);
    (void) bind_gadget_bridge();
    size_t added = 0;
    if (g_grid_autofit_bound) for (size_t i = 0; i < 2; ++i) {
        const size_t index = kGridAutofitSlotBase + i;
        if (std::find(order.begin(), order.end(), index) == order.end()) { order.push_back(index); ++added; }
    }
    if (g_folder_layout_bound) for (size_t i = 0; i < 8; ++i) {
        const size_t index = kFolderLayoutSlotBase + i;
        if (std::find(order.begin(), order.end(), index) == order.end()) { order.push_back(index); ++added; }
    }
    if (g_gadget_bound) for (unsigned i = 0; i < 3; ++i) {
        const size_t slot = kGadgetBridgeSlot + i;
        if (std::find(order.begin(), order.end(), slot) == order.end()) {
            order.push_back(slot); ++added;
        }
    }
    if (g_title_custom_bound && std::find(order.begin(), order.end(), kTitleCustomSlot) == order.end()) {
        order.push_back(kTitleCustomSlot);
        ++added;
    }
    if (g_title_hide_bound) {
        for (size_t index : {kTitlePrefixSlot, kTitleFolderNewSlot, kTitleLightSlot}) {
            if (std::find(order.begin(), order.end(), index) == order.end()) {
                order.push_back(index);
                ++added;
            }
        }
    }
    if (g_desktop_title_bound
        && std::find(order.begin(), order.end(), kDesktopTitleSlot) == order.end()) {
        order.push_back(kDesktopTitleSlot);
        order.push_back(kDesktopTitleHeightSlot);
        ++added;
    }
    if (g_drawer_title_bound
        && std::find(order.begin(), order.end(), kDrawerTitleSlot) == order.end()) {
        order.push_back(kDrawerTitleSlot);
        order.push_back(kDrawerTitleHeightSlot);
        ++added;
    }
    if (g_title_color_bound
        && std::find(order.begin(), order.end(), kTitleColorSlot) == order.end()) {
        order.push_back(kTitleColorSlot);
        ++added;
    }
    for (size_t index = 0; index < g_knobs.size(); ++index) {
        KnobRuntime &knob = g_knobs[index];
        if (!knob.hook_mode || knob.hook_address == 0) continue;
        const size_t owned_slot = kKnobHookSlotBase + index;
        if (knob.hook_armed) {
            if (std::find(order.begin(), order.end(), owned_slot) == order.end()) order.push_back(owned_slot);
            continue;
        }
        // `prime_home_layout_knobs` can bind before the worker initializes generic pointers.
        // Choose the specialized entry at the final slot assignment, not only during binding.
        if (index == 5 && knob.symbol == "LauncherIndicatorState._wrapWithAnimation") {
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_capsule_entry);
        }
        if (index == 2 && knob.symbol == "GridCellDelegate.performLayout") {
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_workspace_entry);
        }
        if (index == 3 && knob.symbol == "GridOccupiedCellDelegate.performLayout") {
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_workspace_occupied_entry);
        }
        if (index == 2 && knob.symbol == "Container.build") {
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_container_probe_entry);
        }
        if (index == 6 && knob.symbol == "LauncherIndicatorState._buildScreenIndicator") {
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_indicator_policy_entry);
        }
        if (index == 7 && knob.symbol == "LauncherIndicatorState._showIndicator") {
            knob.hook_entry = reinterpret_cast<void *>(hc_layout_indicator_slide_only_entry);
        }
        const size_t slot = kKnobHookSlotBase + index;
        g_slots[slot] = {knob.hook_address, knob.hook_entry, knob.hook_original, knob.hook_source,
            knob.hook_words};
        order.push_back(slot);
        knob.hook_armed = true;
        ++added;
    }
    /*
     * The page-dot companion rides its own slot. It is armed from the same call so the two indicator
     * targets install or fail together - a desktop that shows the capsule and page-dots from the same
     * knob is only coherent if both hooks are live.
     */
    if (hc_layout_dart_IndicatorDot_address != 0 && !hc_layout_dart_IndicatorDot_armed) {
        g_slots[kIndicatorDotSlot] = {hc_layout_dart_IndicatorDot_address,
            reinterpret_cast<void *>(hc_layout_indicator_edit_result_entry),
            &hc_layout_dart_IndicatorDot_original, hc_layout_dart_IndicatorDot_source,
            hc_layout_dart_IndicatorDot_words};
        order.push_back(kIndicatorDotSlot);
        hc_layout_dart_IndicatorDot_armed = true;
        ++added;
    }
    if (hc_layout_dart_IndicatorDot_armed
        && std::find(order.begin(), order.end(), kIndicatorDotSlot) == order.end()) {
        order.push_back(kIndicatorDotSlot);
    }
    for (size_t i = 0; i < 5; ++i) {
        const size_t index = kFolderGeometrySlotBase + i;
        if (g_slots[index].address != 0
            && std::find(order.begin(), order.end(), index) == order.end()) {
            order.push_back(index);
            ++added;
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        const size_t index = kDropGeometrySlotBase + i;
        if (g_slots[index].address != 0
            && std::find(order.begin(), order.end(), index) == order.end()) {
            order.push_back(index);
            ++added;
        }
    }
    return added;
}

/*
 * Publish the hook delta only after its slot is installed: a knob whose hook was refused keeps a zero
 * delta and a cleared enable flag, so the launcher behaves exactly as unpatched.
 */
void publish_indicator_dot_delta();

size_t publish_hooks() {
    bool autofit_ready = g_grid_autofit_bound;
    for (size_t i = 0; i < 2; ++i) autofit_ready &= g_slots[kGridAutofitSlotBase + i].registered;
    __atomic_store_n(&hc_grid_autofit_ready, autofit_ready ? 1u : 0u, __ATOMIC_RELEASE);
    bool folder_layout_ready = g_folder_layout_bound;
    for (size_t i = 0; i < 8; ++i) folder_layout_ready &= g_slots[kFolderLayoutSlotBase + i].registered;
    __atomic_store_n(&hc_folder_layout_ready, folder_layout_ready ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&hc_gadget_bridge_enabled,
        uint32_t(g_gadget_requested.load(std::memory_order_acquire)
            && g_slots[kGadgetBridgeSlot].registered
            && g_slots[kGadgetBridgeSlot + 1].registered
            && g_slots[kGadgetBridgeSlot + 2].registered), __ATOMIC_RELEASE);
    bool folder_ready = true;
    for (size_t i = 0; i < 5; ++i) folder_ready &= g_slots[kFolderGeometrySlotBase + i].registered;
    folder_ready &= g_slots[kKnobHookSlotBase + 2].registered
        && g_slots[kKnobHookSlotBase + 3].registered;
    const bool drop_ready = g_slots[kDropGeometrySlotBase].registered
        && g_slots[kDropGeometrySlotBase + 1].registered
        && g_slots[kKnobHookSlotBase + 2].registered
        && g_slots[kKnobHookSlotBase + 3].registered;
    size_t live = 0;
    for (size_t index = 0; index < g_knobs.size(); ++index) {
        KnobRuntime &knob = g_knobs[index];
        if (!knob.hook_mode || knob.hook_enabled == nullptr || knob.hook_delta == nullptr) continue;
        const int delta = knob.delta_dp.load(std::memory_order_relaxed);
        // Once the complete folder bank is available, also observe stock (zero
        // inset) layouts. Otherwise off/on can reuse a stale rendered margin.
        const bool workspace_active = (index == 2 || index == 3)
            && (folder_ready || drop_ready || g_knobs[2].delta_dp.load(std::memory_order_relaxed) != 0
            || g_knobs[3].delta_dp.load(std::memory_order_relaxed) != 0
            || g_knobs[4].delta_dp.load(std::memory_order_relaxed) != 0);
        if ((!workspace_active && delta == 0) || !knob.hook_armed) {
            *knob.hook_enabled = 0;
            continue;
        }
        // The slider is in the user's units; the accessor is not always, so the gain is what makes a
        // step read the way the label says (see kKnobDeltaGain).
        const double value = static_cast<double>(delta) * kKnobDeltaGain[index];
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        *knob.hook_delta = bits;
        *knob.hook_enabled = 1;
        ++live;
    }
    publish_indicator_dot_delta();
    __atomic_store_n(&hc_layout_folder_enabled, folder_ready ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&hc_layout_drop_enabled, drop_ready ? 1u : 0u, __ATOMIC_RELEASE);
    return live;
}

/* Publish a policy only after all original-code continuations exist. */
void publish_indicator_dot_delta() {
    const int mode = g_knobs[6].delta_dp.load(std::memory_order_relaxed);
    const bool ready = g_slots[kKnobHookSlotBase + 6].registered
        && g_slots[kKnobHookSlotBase + 7].registered && g_slots[kIndicatorDotSlot].registered
        && hc_layout_dart_SearchBarMargin_original != nullptr
        && hc_layout_dart_SearchBarWidth_original != nullptr && hc_layout_dart_IndicatorDot_original != nullptr;
    __atomic_store_n(&hc_layout_indicator_mode,
        ready && mode >= 0 && mode <= 2 ? static_cast<uint32_t>(mode) : 0u, __ATOMIC_RELEASE);
}

/*
 * Write every requested delta into the captured object.
 *
 * The launcher owns the field and can rewrite it at any time (rotation, density change, a settings
 * change that rebuilds the config), so the write is self-healing: the launcher's own value is
 * remembered as `base`, and a field that no longer carries `base + applied_delta` is treated as a
 * fresh launcher value. Switching a knob off stops writing, which leaves the launcher's own value in
 * place.
 */
/*
 * Field-write telemetry, read out by the worker's periodic line.
 *
 * `applied` counts the fields we put at the wanted value, `same` counts the calls that found the
 * field already carrying it, and `refused` counts the backstop rejections. `same` is the only
 * in-process proof that a write landed *and persisted*: the launcher rewrites these fields whenever
 * it rebuilds the config, so a field that still holds our value on a later call can only be ours.
 * Without it, "the write landed but nothing consumed it" and "the write never landed" look the same
 * from outside - which is exactly the ambiguity that stalled the previous round.
 *
 * Plain counters written from the Dart thread inside the trampoline, like the capture hit counters.
 */
uint64_t hc_layout_field_writes_applied = 0;
uint64_t hc_layout_field_writes_same = 0;
uint64_t hc_layout_field_writes_refused = 0;

size_t apply_knob_fields() {
    const uintptr_t config = static_cast<uintptr_t>(hc_layout_config_object);
    const uintptr_t dock = static_cast<uintptr_t>(hc_layout_dock_object);
    const uint64_t heap = hc_layout_heap_base;
    size_t applied = 0;
    for (KnobRuntime &knob : g_knobs) {
        const uint32_t packed = knob.path.load(std::memory_order_acquire);
        if (packed == 0) continue;
        const int delta = knob.delta_dp.load(std::memory_order_relaxed);
        const uint8_t object = packed & 0xFFu;
        const int off0 = static_cast<int>((packed >> 8) & 0xFFu) - 1;
        const int off1 = static_cast<int>((packed >> 16) & 0xFFFFu);
        uintptr_t base_object = object == 1 ? config : dock;
        if (base_object == 0) continue;
        uintptr_t target = base_object;
        if (off0 >= 0) {
            if (heap == 0) continue;
            uint32_t compressed = 0;
            std::memcpy(&compressed, reinterpret_cast<const void *>(target + off0), 4);
            if (compressed == 0) continue;
            target = static_cast<uintptr_t>(compressed) + (heap << 32);
        }
        auto *field = reinterpret_cast<double *>(target + off1);
        const double current = *field;
        if (delta == 0) {
            // Switched off: remember the launcher's own value and leave the field alone.
            knob.pristine = current;
            knob.pristine_valid = true;
            continue;
        }
        if (!knob.pristine_valid) {
            knob.pristine = current;
            knob.pristine_valid = true;
        }
        const double wanted = knob.pristine + static_cast<double>(delta);
        /*
         * Backstop. Every legitimate request is bounded by the settings page's dp range times the
         * display density, so a value outside this window means the field is not what it was assumed
         * to be - refuse it instead of letting one bad write take the launcher's layout out.
         */
        if (!(wanted > -4000.0 && wanted < 4000.0)) {
            ++hc_layout_field_writes_refused;
            continue;
        }
        if (current == wanted) {
            ++hc_layout_field_writes_same;
            ++applied;
            continue;
        }
        *field = wanted;
        ++hc_layout_field_writes_applied;
        ++applied;
    }
    return applied;
}

/*
 * Caller histogram: which Dart functions ask for the config while the desktop lays out. Calibration
 * only - it turns "the geometry is computed somewhere in here" into a concrete list of functions to
 * hook, without guessing. Fixed-size and lock-free; a collision just replaces a sample.
 */
constexpr size_t kCallerSlots = 256;
uint64_t g_caller_key[kCallerSlots] = {};
uint32_t g_caller_count[kCallerSlots] = {};

void hc_layout_record_caller(uintptr_t caller) {
    if (caller == 0) return;
    const size_t slot = (caller >> 2) & (kCallerSlots - 1);
    if (g_caller_key[slot] != caller) {
        g_caller_key[slot] = caller;
        g_caller_count[slot] = 0;
    }
    ++g_caller_count[slot];
}

/*
 * The trampoline entry point: runs on the Dart thread, inside the launcher's own call, so it must do
 * nothing but a handful of field writes. There is no allocation, no lock and no logging here.
 *
 * Only the trampoline calls this. The background worker deliberately does not: two writers racing on
 * the same self-healing bookkeeping would corrupt `base`, and the trampoline already covers every
 * moment the launcher can read a field.
 */
extern "C" void hc_layout_apply_now(uint64_t config, uintptr_t caller) {
    hc_layout_record_caller(caller);
    (void) config;
    apply_knob_fields();
}

void *worker(void *) {
    /*
     * Named for field triage. The audit that produced this worker's power fix could not attribute
     * this loop to anything: it saw five unnamed threads in the launcher and no way to tell which
     * one was ours, which is the difference between "the loop does not run" and "the loop runs but
     * says nothing". One line costs less than that investigation did.
     */
    (void) pthread_setname_np(pthread_self(), "hc-home-layout");
    home_layout::Config config;
    bool queried = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (home_layout::query_config(config)) { queried = true; break; }
        delay_ms(100);
    }
    if (queried) sync_title_config(config);
    if (!queried && g_title_desktop_sp.load(std::memory_order_acquire) == 12
        && g_title_drawer_sp.load(std::memory_order_acquire) == 12
        && g_title_color.load(std::memory_order_acquire) == -1
        && !__atomic_load_n(&hc_title_hide_new_install, __ATOMIC_ACQUIRE)
        && !__atomic_load_n(&hc_title_custom_enabled, __ATOMIC_ACQUIRE)) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "layout config unavailable; leaving launcher unmodified");
        return attempt_finished();
    }
    // The early loader callback must win first layout. Bounded startup wait,
    // not a new polling thread; the existing worker adopts the loader's slots.
    for (int attempt = 0; attempt < 200
        && !g_loader_prime_finished.load(std::memory_order_acquire); ++attempt) delay_ms(25);
    auto sync_requested = [&]() {
        bool any = false;
        for (size_t index = 0; index < HC_LAYOUT_KNOB_COUNT; ++index) {
            const bool enabled = config.knobs[index].enabled;
            g_knobs[index].delta_dp.store(enabled ? config.knobs[index].delta_dp : 0,
                std::memory_order_relaxed);
            any = any || enabled;
        }
        return any;
    };
    bool any_knob = sync_requested();
    /*
     * Stage-one probe: hook the workspace top padding accessor and change nothing.
     *
     * It answers the questions a value change cannot: is the hook itself stable, how often is the
     * function called, what does the launcher really return, and which Dart function is asking. Only
     * after that is a value change worth trying - and only a tiny one.
     */
    char probe_top[PROP_VALUE_MAX] = {};
    const bool top_probe =
        __system_property_get("debug.hyperceiler.layout.probe_top", probe_top) > 0
        && probe_top[0] == '1';
    if (top_probe) {
        g_knobs[2].hook_mode = true;
        g_knobs[2].symbol = "GridSizeCalRules.stableWorkspaceCellPaddingTop";
        g_knobs[2].delta_dp.store(0, std::memory_order_relaxed);
        /* Minimal stub: proves whether the hook itself can run here at all. */
        g_knobs[2].hook_entry = reinterpret_cast<void *>(hc_layout_passthrough_entry);
        g_knobs[2].hook_original = &hc_layout_passthrough_original;
    }

    /*
     * Whether any code-patch feature (the tweaks half of the panel) asked for something. The gate
     * below used to check only the grid and the knobs, so a desktop with only, say, the icon scale
     * enabled pushed no config at all: every tweaks feature silently read as "not enabled" no
     * matter what the page said - which is exactly how "folder columns / icon scale do nothing"
     * presented on a live device. The tweaks pipeline consumes this same Config, so its ask
     * belongs in this gate, not after it.
     */
    const bool any_tweak = config.tweaks.folder_enabled || config.tweaks.pad_enabled
        || config.tweaks.fold_enabled || config.tweaks.icon_scale_enabled
        || config.tweaks.recents_hide_clear || config.tweaks.recents_no_clear
        || config.tweaks.animation_open_enabled || config.tweaks.animation_recents_enabled
        || config.tweaks.hotseat_unlimited || config.widget_allow_move
        || config.folder.full_width || config.folder.title_center;
    if (!config.grid_enabled && !any_knob && !top_probe && !any_tweak
        && g_title_desktop_sp.load(std::memory_order_acquire) == 12
        && g_title_drawer_sp.load(std::memory_order_acquire) == 12
        && g_title_color.load(std::memory_order_acquire) == -1
        && !__atomic_load_n(&hc_title_hide_new_install, __ATOMIC_ACQUIRE)
        && !__atomic_load_n(&hc_title_custom_enabled, __ATOMIC_ACQUIRE)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "layout preferences disabled; no hooks installed");
        return attempt_finished();
    }
    g_cell_x.store(config.cell_x, std::memory_order_relaxed);
    g_cell_y.store(config.cell_y, std::memory_order_relaxed);
    publish_animation_rate(config.tweaks);
    push_tweaks(config);

    std::optional<Located> located;
    for (int attempt = 0; attempt < 60; ++attempt) {
        located = locate();
        if (located) break;
        delay_ms(100);
    }
    if (!located) {
        /*
         * The geometry sites are only the first two slots (the grid cell replacements). A launcher
         * build whose xref sites moved costs the grid knobs, not the whole worker: the hook-mode
         * knobs below are symbol-resolved against the same image and can still bind, which is
         * exactly what a probe run on a moved launcher needs. The 6309→7654-260904 rollback hit
         * this: tweaks patches landed (they are symbol-named) while the worker quit here and the
         * probe hooks never armed.
         */
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "layout targets unavailable; continuing with hook knobs only");
    } else {
        g_container_path = located->container_path;
        g_view_begin = located->view_begin;
        g_view_end = located->view_end;
        g_dart_container_path = located->dart_container_path;
        g_dart_view_begin = located->dart_view_begin;
        g_dart_view_end = located->dart_view_end;
    }

    /*
     * Calibration channel, read once per process and gated on `debug.hyperceiler.layout.override=1`.
     *
     * The gate is not cosmetic: `debug.hyperceiler.layout.symbols` / `hook0..7` / `raw0..7` retarget a
     * knob, and while they were read unconditionally a leftover property from an earlier calibration
     * run silently overrode the shipped table - which is exactly how "the settings page does nothing"
     * was produced once already. With the gate, an unset or stale property cannot influence production
     * behaviour at all.
     */
    char debug_gate[PROP_VALUE_MAX] = {};
    const bool debug_channel =
        __system_property_get("debug.hyperceiler.layout.override", debug_gate) > 0
        && debug_gate[0] == '1';
    if (debug_channel) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
            "layout calibration channel is ON; preferences are overridden by debug properties");
        char probe[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.hyperceiler.layout.probe", probe) > 0
            && probe[0] == '1') {
            for (size_t index = 0; index < g_knobs.size(); ++index) {
                char name[PROP_VALUE_MAX] = {};
                const std::string key = "debug.hyperceiler.layout.probe" + std::to_string(index);
                if (__system_property_get(key.c_str(), name) <= 0) continue;
                if (name[0] != '\0') g_knobs[index].symbol = name;
            }
        } else {
            char value[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.hyperceiler.layout.symbols", value) > 0) {
                const std::string_view text(value);
                size_t cursor = 0;
                for (size_t index = 0; index < g_knobs.size(); ++index) {
                    const size_t comma = text.find(',', cursor);
                    const std::string_view entry = text.substr(cursor,
                        comma == std::string_view::npos ? std::string_view::npos : comma - cursor);
                    if (!entry.empty()) g_knobs[index].symbol = std::string(entry);
                    if (comma == std::string_view::npos) break;
                    cursor = comma + 1;
                }
            }
        }
        /*
         * `debug.hyperceiler.layout.hook0..hook7` switch a single knob to the hook strategy and name
         * the layout aggregator it should hook. This is the calibration path for the values whose
         * accessor Dart inlines away.
         */
        apply_debug_hook_overrides();
        /*
         * There is deliberately no "write this offset" calibration property here any more. Such a
         * channel was used once to sweep the configuration object for a field, and because it wrote an
         * eight-byte double at offsets that are not all doubles it corrupted the Dart heap: the
         * launcher then died inside libhyper_os_flutter.so on a DartWorker thread. A field path may
         * only ever come from decoding a `ldur d0, [..]` in the launcher's own accessor, which is what
         * bind_knobs does; a free-form offset has no place in a release build.
         */
    }
    /*
     * Unconditional. These pointers used to be filled only inside the calibration branch, so with the
     * channel closed every hook knob had a null `hook_enabled` and `publish_hooks` wrote through it -
     * a null-pointer write on the worker thread, which took the whole launcher process down. Every
     * crash that was blamed on the hook itself was this line.
     */
    init_knob_hooks();

    const auto knob_ready = [](const KnobRuntime &knob) {
        return knob.hook_mode ? knob.hook_address != 0
                              : knob.path.load(std::memory_order_relaxed) != 0;
    };
    const auto all_bound = [&]() {
        return std::all_of(g_knobs.begin(), g_knobs.end(), knob_ready);
    };
    /*
     * Resolve before the desktop computes its layout. The snapshot is mapped shortly before Dart runs
     * its one-time layout, so the wait is short and tight; anything resolved after that layout is only
     * seen once something forces the desktop to lay out again.
     */
    if (any_knob || top_probe) {
        for (int attempt = 0; attempt < 200 && !all_bound(); ++attempt) {
            if (bind_knobs() == g_knobs.size() || all_bound()) break;
            delay_ms(25);
        }
        bind_knobs();
    }
    if ((config.tweaks.animation_open_enabled || config.tweaks.animation_recents_enabled)
        && located) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (bind_animation_consumer(*located) && bind_magic_consumer(*located)) break;
            delay_ms(25);
        }
    }

    std::vector<size_t> order;
    if (config.grid_enabled && located && located->x != 0 && located->y != 0) {
        g_slots[0] = {located->x, reinterpret_cast<void *>(cell_x_replacement), &g_original_x,
            located->x_source, located->x_words};
        g_slots[1] = {located->y, reinterpret_cast<void *>(cell_y_replacement), &g_original_y,
            located->y_source, located->y_words};
        order.push_back(0);
        order.push_back(1);
    }
    if (config.tweaks.animation_recents_enabled && located && located->animation != 0) {
        g_slots[kAnimationHookSlot] = {located->animation,
            reinterpret_cast<void *>(hc_layout_animation_ratio_entry),
            &hc_layout_animation_ratio_original, located->animation_source,
            located->animation_words};
        order.push_back(kAnimationHookSlot);
        g_animation_hook_armed = true;
    }
    if (config.tweaks.animation_open_enabled && located && located->magic != 0) {
        g_slots[kAnimationMagicSlot] = {located->magic,
            reinterpret_cast<void *>(hc_layout_magic_entry),
            &hc_layout_magic_original, located->magic_source,
            located->magic_words};
        order.push_back(kAnimationMagicSlot);
        g_magic_hook_armed = true;
    }
    /*
     * The field-rewriting path is opt-in while it is still being validated, and off by default.
     *
     * It writes into the launcher's own live objects, so a wrong path is a launcher crash rather than
     * a no-op; a build that could do that without being asked is not shippable. `debug.hyperceiler.
     * layout.knobs_enable=1` is the explicit request.
     */
    char knobs_gate[PROP_VALUE_MAX] = {};
    const bool field_writes =
        __system_property_get("debug.hyperceiler.layout.knobs_enable", knobs_gate) > 0
        && knobs_gate[0] == '1';
    g_field_writes_enabled.store(field_writes, std::memory_order_relaxed);
    if ((any_knob || top_probe) && field_writes) {
        if (arm_captures() != 0) {
            order.push_back(kConfigCaptureSlot);
            order.push_back(kDockCaptureSlot);
        }
    }
    (void) bind_title_color();
    (void) bind_title_custom();
    (void) bind_drawer_title();
    (void) bind_desktop_title();
    (void) bind_title_hide();
    arm_hooks(order);
    publish_hooks();
    (void) sync_hotseat_capacity(config.tweaks.hotseat_unlimited);
    (void) sync_widget_move(config.widget_allow_move);
    const bool grid_live =
        config.grid_enabled && located && located->x != 0 && located->y != 0;
    g_ready.store(grid_live, std::memory_order_release);
    g_dart_ready.store(g_captures_armed || !order.empty(), std::memory_order_release);
    {
        size_t bound = 0;
        for (const KnobRuntime &knob : g_knobs) {
            if (knob_ready(knob)) ++bound;
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "layout hooks live grid=%d cell=%dx%d knobs=%zu/%zu captures=%d "
            "animation=%d/%d m%d%% r%d%% age=%llums",
            config.grid_enabled ? 1 : 0, config.cell_x, config.cell_y, bound, g_knobs.size(),
            g_captures_armed ? 1 : 0, g_animation_hook_armed ? 1 : 0, g_magic_hook_armed ? 1 : 0,
            config.tweaks.animation_open_rate_percent,
            config.tweaks.animation_recents_rate_percent,
            static_cast<unsigned long long>(process_age_or_zero()));
        for (const KnobRuntime &knob : g_knobs) {
            const uint32_t packed = knob.path.load(std::memory_order_relaxed);
            double last = 0.0;
            if (knob.hook_last != nullptr) std::memcpy(&last, knob.hook_last, sizeof(last));
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "layout knob %s mode=%s va=%#x sym_size=%u w0=%08x addr=%p object=%u off0=%d "
                "off1=%d delta=%d hits=%llu last=%.4f caller=%#llx",
                knob.symbol.c_str(), knob.hook_mode ? "hook" : "field", knob.getter,
                knob.getter_size, knob.hook_words[0],
                reinterpret_cast<void *>(knob.hook_address), packed & 0xFFu,
                static_cast<int>((packed >> 8) & 0xFFu) - 1,
                static_cast<int>((packed >> 16) & 0xFFFFu),
                knob.delta_dp.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(knob.hook_hits != nullptr ? *knob.hook_hits : 0),
                last,
                static_cast<unsigned long long>(
                    knob.hook_caller != nullptr ? *knob.hook_caller : 0));
        }
        /*
         * The companion's own line. It has no slider to read back, so without this the only way to
         * tell "the page-dot hook is live" from "it never bound" would be the absence of a line -
         * which is exactly the ambiguity this file's verdicts exist to kill.
         */
        if (hc_layout_dart_IndicatorDot_address != 0 || hc_layout_dart_IndicatorDot_armed) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "layout indicator policy va=%#x registered=%d mode=%u hits=%llu",
                hc_layout_dart_IndicatorDot_getter, g_slots[kIndicatorDotSlot].registered ? 1 : 0,
                __atomic_load_n(&hc_layout_indicator_mode, __ATOMIC_ACQUIRE),
                static_cast<unsigned long long>(hc_layout_dart_IndicatorDot_hits));
        }
    }

    int iterations = 0;
    /*
     * Worker-local throttle state. Both intervals are wall-clock rather than iteration counts because
     * the iteration itself changes length once the panel is dozing.
     */
    uint64_t last_panel_query = 0;
    uint64_t last_generation_check = 0;
    for (;;) {
        const uint64_t now = monotonic_ms();
        /*
         * Drawable-desktop gate, the same one the dock maintenance worker uses. While the panel
         * dozes or another app covers home, the desktop is not drawn, so no patched call site can
         * affect a visible frame and there is nothing to
         * maintain - yet this loop kept asking system_server for its configuration over a synchronous
         * Binder transaction and re-verifying every slot, twice a second, for the whole night. The
         * dock measured that exact pattern as essentially the entire cost of the launcher process
         * while the screen was off.
         *
         * Only a *fresh* answer skips a pass, so a stale "dozing" cannot stop maintenance: the loop
         * is asleep for kDozingIterationMs, which is also what makes the query itself one call per
         * that interval while dozing.
         */
        if (interval_due(last_panel_query, now, kPanelQueryIntervalMs)) {
            last_panel_query = now;
            if (!layout_panel_refresh_and_check()) {
                /*
                 * A shorter doze pause than the dock's 30 s. The dock maintains a hook chain that
                 * only matters while a gesture is running, while this one owns on-screen geometry: a
                 * patch the kernel refilled overnight has to be back before the desktop is laid out
                 * again, and while dozing a pass is a handful of word reads, so the tighter bound
                 * costs nothing.
                 */
                delay_ms(kDozingIterationMs);
                continue;
            }
        }
        delay_ms(kPassIntervalMs);
        ++iterations;
        /*
         * Probe readout: the passthrough stub records every call into these globals, and this is
         * where they get printed. The caller is reported as an image-relative address so it can be
         * named straight from the symbol table, without keeping a load base anywhere else.
         */
        {
            static uint32_t probe_reported = 0;
            const uint32_t probe_hits = hc_layout_probe_hits;
            if (probe_hits != probe_reported) {
                probe_reported = probe_hits;
                double probe_value = 0;
                std::memcpy(&probe_value, &hc_layout_probe_value, sizeof(probe_value));
                const uint64_t caller = hc_layout_probe_caller;
                const uint64_t in_image =
                    g_dart && caller >= g_dart->load_base ? caller - g_dart->load_base : 0;
                __android_log_print(ANDROID_LOG_INFO, kTag,
                    "layout probe hits=%u value=%f caller=0x%llx (image 0x%llx)", probe_hits,
                    probe_value, static_cast<unsigned long long>(caller),
                    static_cast<unsigned long long>(in_image));
            }
        }
        if (iterations % 10 == 0) {
            home_layout::Config latest;
            if (home_layout::query_config(latest)) {
                sync_title_config(latest);
                if (latest.grid_enabled && located && located->x != 0 && located->y != 0
                    && std::find(order.begin(), order.end(), size_t{0}) == order.end()) {
                    g_slots[0] = {located->x, reinterpret_cast<void *>(cell_x_replacement),
                        &g_original_x, located->x_source, located->x_words};
                    g_slots[1] = {located->y, reinterpret_cast<void *>(cell_y_replacement),
                        &g_original_y, located->y_source, located->y_words};
                    order.push_back(0);
                    order.push_back(1);
                    g_ready.store(true, std::memory_order_release);
                }
                if (located) {
                    if (latest.tweaks.animation_recents_enabled && !g_animation_hook_armed
                        && bind_animation_consumer(*located)) {
                        g_slots[kAnimationHookSlot] = {located->animation,
                            reinterpret_cast<void *>(hc_layout_animation_ratio_entry),
                            &hc_layout_animation_ratio_original, located->animation_source,
                            located->animation_words};
                        order.push_back(kAnimationHookSlot);
                        g_animation_hook_armed = true;
                    }
                    if (latest.tweaks.animation_open_enabled && !g_magic_hook_armed
                        && bind_magic_consumer(*located)) {
                        g_slots[kAnimationMagicSlot] = {located->magic,
                            reinterpret_cast<void *>(hc_layout_magic_entry),
                            &hc_layout_magic_original, located->magic_source,
                            located->magic_words};
                        order.push_back(kAnimationMagicSlot);
                        g_magic_hook_armed = true;
                    }
                }
                const bool cell_changed = latest.cell_x != config.cell_x
                    || latest.cell_y != config.cell_y || latest.grid_enabled != config.grid_enabled;
                const bool knobs_changed = latest.knobs != config.knobs;
                const bool tweaks_changed = latest.tweaks != config.tweaks;
                config = latest;
                if (cell_changed) {
                    g_cell_x.store(config.cell_x, std::memory_order_relaxed);
                    g_cell_y.store(config.cell_y, std::memory_order_relaxed);
                    g_ready.store(config.grid_enabled && located && located->x != 0
                        && located->y != 0, std::memory_order_release);
                }
                if (tweaks_changed) {
                    publish_animation_rate(config.tweaks);
                    push_tweaks(config);

                }
                (void) sync_hotseat_capacity(config.tweaks.hotseat_unlimited);
                (void) sync_widget_move(config.widget_allow_move);
                (void) knobs_changed;
            }
            any_knob = sync_requested();
            (void) bind_title_color();
            (void) bind_title_custom();
    (void) bind_drawer_title();
            (void) bind_desktop_title();
            (void) bind_title_hide();
            if (arm_hooks(order) != 0 || publish_hooks() != 0) {
                g_dart_ready.store(true, std::memory_order_release);
            }
        }
        if ((any_knob || top_probe) && !all_bound()) bind_knobs();
        if (any_knob && !g_captures_armed) {
            if (arm_captures() != 0) {
                order.push_back(kConfigCaptureSlot);
                order.push_back(kDockCaptureSlot);
                g_dart_ready.store(true, std::memory_order_release);
            }
        }
        if (arm_hooks(order) != 0) publish_hooks();
        if (iterations % 20 == 0) {
            size_t bound = 0;
            int deltas = 0;
            for (const KnobRuntime &knob : g_knobs) {
                if (knob_ready(knob)) ++bound;
                if (knob.delta_dp.load(std::memory_order_relaxed) != 0) ++deltas;
            }
            /*
             * `writes=applied/same/refused` is the field-write read-back: `same` non-zero means a
             * written value survived to a later call. `path0` is the first decoded field path as
             * `object|off0|off1`, so the offset actually being written is visible without a reboot.
             */
            uint32_t path0 = 0;
            for (const KnobRuntime &knob : g_knobs) {
                const uint32_t packed = knob.path.load(std::memory_order_relaxed);
                if (packed != 0) {
                    path0 = packed;
                    break;
                }
            }
            /*
             * Folder-bank state, on the same cadence as the capture readout. The bank's own logs
             * are edge-triggered - one line per transition - so a refusal that happened during
             * early boot scrolls out of the ring buffer long before anyone reads it, and the
             * capture line above says nothing about this feature. These five values are the whole
             * decision: dart mapped, gate bits, whether the scan ran, whether it latched a
             * refusal, and whether it bound.
             */
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "folder bank dart=%d req=%#llx gate=%#llx attempted=%d checked=%d bound=%d inner=%d",
                g_dart ? 1 : 0,
                static_cast<unsigned long long>(
                    __atomic_load_n(&hc_folder_layout_requested, __ATOMIC_ACQUIRE)),
                static_cast<unsigned long long>(
                    __atomic_load_n(&hc_folder_layout_requested, __ATOMIC_ACQUIRE) & 3),
                g_folder_layout_attempted ? 1 : 0, g_folder_layout_checked ? 1 : 0,
                g_folder_layout_bound ? 1 : 0, g_folder_inner_ready ? 1 : 0);
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "layout captures config=%#llx dock=%#llx heap=%#llx hits=%llu/%llu bound=%zu "
                "deltas=%d writes=%llu/%llu/%llu path0=%#x recents=%d/%llu/%.4f/%d%% "
                "open=%d/%llu/%.4f/%d%%",
                static_cast<unsigned long long>(hc_layout_config_object),
                static_cast<unsigned long long>(hc_layout_dock_object),
                static_cast<unsigned long long>(hc_layout_heap_base),
                static_cast<unsigned long long>(hc_layout_config_capture_hits),
                static_cast<unsigned long long>(hc_layout_dock_capture_hits), bound, deltas,
                static_cast<unsigned long long>(hc_layout_field_writes_applied),
                static_cast<unsigned long long>(hc_layout_field_writes_same),
                static_cast<unsigned long long>(hc_layout_field_writes_refused), path0,
                g_animation_hook_armed ? 1 : 0,
                static_cast<unsigned long long>(hc_layout_animation_ratio_hits),
                [&] {
                    double value = 0.0;
                    const uint64_t bits = __atomic_load_n(
                        &hc_layout_animation_ratio_original_bits, __ATOMIC_RELAXED);
                    std::memcpy(&value, &bits, sizeof(value));
                    return value;
                }(),
                config.tweaks.animation_recents_rate_percent,
                g_magic_hook_armed ? 1 : 0,
                static_cast<unsigned long long>(hc_layout_magic_hits),
                [&] {
                    double value = 0.0;
                    const uint64_t bits = __atomic_load_n(
                        &hc_layout_magic_original_bits, __ATOMIC_RELAXED);
                    std::memcpy(&value, &bits, sizeof(value));
                    return value;
                }(),
                config.tweaks.animation_open_rate_percent);
            /*
             * Per-knob probe readout. `hits` alone can only say "the hook runs"; the verdict a
             * read-only probe has to support is "this function is the control point for that value",
             * which needs the launcher's own return value and the caller as well. `last` is the raw
             * double the patched call returned, `caller` the Dart return address (reported as an
             * image VA so the symbol table can name it), `d` the delta currently requested for the
             * knob (zero until a preference enables it) - so a passthrough probe is distinguishable
             * from an active one at a glance.
             */
            std::string hits;
            for (const KnobRuntime &knob : g_knobs) {
                if (!knob.hook_mode || knob.hook_hits == nullptr) continue;
                double last = 0;
                uint64_t caller = 0;
                if (knob.hook_last != nullptr) {
                    std::memcpy(&last, knob.hook_last, sizeof(last));
                }
                if (knob.hook_caller != nullptr) caller = *knob.hook_caller;
                const uint64_t in_image =
                    g_dart && caller >= g_dart->load_base ? caller - g_dart->load_base : 0;
                char entry[256] = {};
                snprintf(entry, sizeof(entry), " %s=%llu last=%.4f caller=%#llx d=%d",
                    knob.symbol.c_str(), static_cast<unsigned long long>(*knob.hook_hits), last,
                    static_cast<unsigned long long>(in_image),
                    knob.delta_dp.load(std::memory_order_relaxed));
                hits += entry;
            }
            if (!hits.empty()) {
                __android_log_print(ANDROID_LOG_INFO, kTag, "layout hook hits%s", hits.c_str());
            }
            // Caller histogram, as image VAs so the offline symbol table can name them.
            const uint64_t load_base = g_dart ? g_dart->load_base : 0;
            std::vector<std::pair<uint32_t, uint32_t>> callers;
            callers.reserve(kCallerSlots);
            for (size_t slot = 0; slot < kCallerSlots; ++slot) {
                if (g_caller_count[slot] == 0) continue;
                const uintptr_t caller = static_cast<uintptr_t>(g_caller_key[slot]);
                if (caller < load_base) continue;
                callers.emplace_back(static_cast<uint32_t>(caller - load_base), g_caller_count[slot]);
            }
            std::sort(callers.begin(), callers.end(),
                [](const auto &a, const auto &b) { return a.second > b.second; });
            for (size_t i = 0; i < callers.size() && i < 12; ++i) {
                __android_log_print(ANDROID_LOG_INFO, kTag, "layout caller va=%#x count=%u",
                    callers[i].first, callers[i].second);
            }
        }
        /*
         * Health pass. The steady state is one word read per armed slot - no mapping inventory - so
         * the cadence above is no longer what this loop costs, it only bounds how long a lost patch
         * can stay lost.
         */
        bool live = bank_live(order);
        /*
         * Periodic generation proof, at the backstop cadence rather than per pass: one inventory for
         * every armed slot instead of one per slot. A read failure here is treated like any other
         * unproven state and falls into the repair below, exactly as the per-slot validated read did.
         */
        if (live && interval_due(last_generation_check, now, kGenerationCheckMs)) {
            last_generation_check = now;
            const auto inventory = current_mappings();
            live = inventory.has_value() && bank_generation_holds(order, *inventory);
        }
        if (!live) {
            /*
             * The validated repair - the only place in this loop that builds a mapping inventory.
             * It used to be the steady-state check as well, which is what made a healthy desktop pay
             * for twenty-four inventory parses per pass; now it runs when a patch was actually lost,
             * or when the bank has not been installed yet.
             */
            if (nhk::ensure_slots_live(g_slots, slot_host(), order) && bank_live(order)) {
                live = true;
                __android_log_print(ANDROID_LOG_INFO, kTag, "layout hook bank re-armed");
            }
        }
        if (!live) {
            __atomic_store_n(&hc_grid_autofit_ready, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&hc_folder_layout_ready, 0u, __ATOMIC_RELEASE);
            g_ready.store(false, std::memory_order_release);
            g_dart_ready.store(false, std::memory_order_release);
            g_captures_armed = false;
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                "layout hook bank unhealthy; stopped overriding");
            return attempt_finished();
        }
    }
}
} // namespace

// Scalar stores within the original owning calculation, not a saved heap
// pointer or an already-published GridInfo. No Dart calls/GC, polling or allocation.
extern "C" int hc_grid_autofit_body(uintptr_t saved, uintptr_t frame, unsigned kind) {
    const uint32_t requested = __atomic_load_n(&hc_grid_autofit_requested, __ATOMIC_ACQUIRE);
    if (!saved || !frame || kind >= 2 || !(requested & 1)) return 0;
    const auto &f = g_grid_autofit_fields;
    auto u64 = [](uintptr_t p) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void *>(p), 8); return v; };
    auto num = [](uintptr_t p) { double v; std::memcpy(&v, reinterpret_cast<const void *>(p), 8); return v; };
    auto put = [](uintptr_t p, auto v) { std::memcpy(reinterpret_cast<void *>(p), &v, sizeof(v)); };
    const auto q = [&](unsigned n) { return saved + 160 + n * 16; };
    const uintptr_t owner = u64(frame - 8);
    if ((owner & 7) != 1) return 0;
    const int64_t rows = kind == 0 ? int64_t(u64(saved)) - 1 : int64_t(u64(owner + f.rows));
    const uintptr_t box = kind == 1 ? u64(saved) : 0;
    if (kind == 1 && (box & 7) != 1) return 0;
    const double base_width = kind == 0 ? num(frame - 0x30) - num(frame - 0x38) : num(owner + f.width);
    const double remaining = kind == 0 ? num(frame - 0x28) : num(box + 7);
    const double dock_height = num(owner + (kind == 0 ? f.legacy_dock : f.dock));
    double height = 0;
    if (!home_layout::grid_autofit_height(base_width, remaining, dock_height, rows, requested >> 8, height)) return 0;
    put(owner + (kind == 0 ? f.legacy_height : f.height), height);
    // The original cap assumed row/dock heights grew together (rows+1). Rejoin
    // its original redistribution with ZERO leftover, preserving the stock dock.
    put(q(0), remaining); put(q(0) + 8, uint64_t{0});
    put(q(1), kind == 0 ? 0.0 : remaining); put(q(1) + 8, uint64_t{0});
    put(q(2), kind == 0 ? remaining : 0.0); put(q(2) + 8, uint64_t{0});
    return 1;
}

// UI-isolate-local native scalars only. No captured Dart pointer outlives a callback.
static thread_local double folder_layout_gap = 0;
static thread_local bool folder_layout_gap_valid = false;
extern "C" void hc_folder_layout_body(uintptr_t saved, uintptr_t pool, unsigned kind, uintptr_t heap) {
    if (kind >= 8 || !saved) return;
    auto read64 = [](uintptr_t at) { uint64_t value = 0; std::memcpy(&value, reinterpret_cast<const void *>(at), 8); return value; };
    auto scalar = [](uintptr_t at) { double value = 0; std::memcpy(&value, reinterpret_cast<const void *>(at), 8); return value; };
    const uintptr_t object = read64(saved), argument = read64(saved + 8);
    const uint64_t packed = __atomic_load_n(&hc_folder_layout_requested, __ATOMIC_ACQUIRE);
    const bool wide = (packed & 2) != 0;
    const uintptr_t d0 = saved + 160;
    if (kind == 1 || kind == 6) {
        const uint64_t zero = 0; // Original LDUR D0 clears V0's upper64.
        std::memcpy(reinterpret_cast<void *>(d0 + 8), &zero, 8);
    }
    if (kind == 0) {
        folder_layout_gap = scalar(argument + g_folder_gap_field);
        folder_layout_gap_valid = std::isfinite(folder_layout_gap) && folder_layout_gap >= 0 && folder_layout_gap <= 200;
    } else if (kind == 1) {
        double value = scalar(argument + g_folder_cell_width_field);
        if (wide && folder_layout_gap_valid) value = home_layout::folder_cell_width(value,
            scalar(object + g_folder_screen_width_field), scalar(object + g_folder_screen_height_field),
            folder_layout_gap, packed);
        std::memcpy(reinterpret_cast<void *>(d0), &value, 8);
    } else if (kind == 2 || kind == 3) {
        if (wide && folder_layout_gap_valid) { const double zero = 0; std::memcpy(reinterpret_cast<void *>(d0), &zero, 8); }
    } else if (kind == 6) {
        double value = scalar(object + g_folder_cling_width_field);
        uintptr_t config = 0;
        uint32_t rx = 0, root = 0;
        std::memcpy(&rx, reinterpret_cast<const void *>(object + g_folder_controller_config_field), 4);
        if ((rx & 7) == 1 && (!pool || rx != uint32_t(read64(pool + 0x40)))) {
            std::memcpy(&root, reinterpret_cast<const void *>((heap & ~uint64_t{0xffffffff}) + rx + g_folder_rx_value_field), 4);
            if ((root & 7) == 1 && (!pool || root != uint32_t(read64(pool + 0x40)))) config = (heap & ~uint64_t{0xffffffff}) + root;
        }
        const double width = config ? scalar(config + g_folder_screen_width_field) : 0;
        if (wide && std::isfinite(width) && width >= 100 && width <= 4000) value = width;
        std::memcpy(reinterpret_cast<void *>(d0), &value, 8);
    } else if (kind == 7 && (packed & 1) && pool && g_folder_inner_ready) {
        // Slot 7 no longer forges the returned pointer. It sits on the replay window,
        // which is the FIRST consumer of the freshly built Container: the window
        // reloads the owner local and stores it as the return value. Rewriting the
        // alignment field before that replay means the launcher's own Container.build
        // later reads OUR value, so the enum identity, the StackFit and the Clip all
        // stay exactly as the launcher left them.
        //
        // Everything here is addressed off the saved x15 rather than off x29, because
        // the splice does not spill x29 and must not depend on the callee preserving
        // it: FP = saved_x15 + dart_save(32) + frame, and the owner local is owner_slot
        // below that. g_folder_inner_owner_local is that folded distance.
        const uintptr_t base = heap & ~uint64_t{0xffffffff};
        const uintptr_t x15_saved = read64(saved + 128);
        // The saved x15 is a GC-root stack pointer: it is non-zero on every real
        // entry, but the native save block is 8 raw bytes we do not own, so a zero
        // there would fault on the dereference below. Refuse instead.
        if (x15_saved == 0) return;
        uint32_t owner_off = 0;
        std::memcpy(&owner_off, reinterpret_cast<const void *>(x15_saved + g_folder_inner_owner_local), 4);
        const uintptr_t owner = base + owner_off;
        // Refuse anything but a live Container whose alignment is one of the two
        // AlignmentDirectional roots the launcher itself resolves between, or null.
        // A Container carrying some other alignment was built for a different call
        // site and must keep it - a mis-owned write is indistinguishable from a
        // layout bug, which is the one failure mode worth refusing to risk.
        if (owner_off != 0 && (owner & 7) == 1) {
            uint32_t cur = 0, center = 0, low = 0, high = 0;
            std::memcpy(&cur, reinterpret_cast<const void *>(owner + g_folder_inner.alignment), 4);
            std::memcpy(&center, reinterpret_cast<const void *>(pool + g_folder_inner.center_pool), 4);
            std::memcpy(&low, reinterpret_cast<const void *>(pool + g_folder_inner.direction_pool[0]), 4);
            std::memcpy(&high, reinterpret_cast<const void *>(pool + g_folder_inner.direction_pool[1]), 4);
            const bool known = cur == 0 || cur == low || cur == high;
            if (known && center != 0 && (center & 7) == 1 && center != cur) {
                std::memcpy(reinterpret_cast<void *>(owner + g_folder_inner.alignment), &center, 4);
                static std::atomic_uint reported{0};
                if (!(reported.load(std::memory_order_relaxed) & 1U)
                    && !(reported.fetch_or(1U, std::memory_order_relaxed) & 1U))
                    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "folder inner alignment rewritten: align=%#x cur=%#x dirs=%#x/%#x -> center=%#x bit=%u",
                        g_folder_inner.alignment, cur, low, high, center, g_folder_inner.direction_bit);
            }
        }
    } else if ((packed & 1) && pool) {
        const uintptr_t center = read64(pool + g_folder_center_pool);
        // Pool entries are GC roots; reread on every build. The enums must share their
        // class, with index start=4 and center=2. No pool scan, allocation or constant writes.
        uint64_t start_index = 0, center_index = 0;
        if ((argument & 7) == 1 && (center & 7) == 1
            && ((read64(argument - 1) >> 12) & 0xfffff) == ((read64(center - 1) >> 12) & 0xfffff)) {
            std::memcpy(&start_index, reinterpret_cast<const void *>(argument + g_folder_enum_index_field), 8);
            std::memcpy(&center_index, reinterpret_cast<const void *>(center + g_folder_enum_index_field), 8);
            if (start_index == 4 && center_index == 2) std::memcpy(reinterpret_cast<void *>(saved + 8), &center, 8);
        }
    }
}

extern "C" uint64_t hc_title_custom_label(uint64_t original, uint64_t model, uint64_t heap,
    uint64_t thread, uint64_t dispatch, uint64_t null_object, uint32_t site) {
    if (!g_title_custom_bound) return original;
    const auto names = std::atomic_load_explicit(&g_title_names, std::memory_order_acquire);
    if (!names || names->empty()) return original;
    const auto result = home_title::custom_label(original, model, heap, thread, dispatch, null_object,
        g_title_component_method, g_title_pin_method, *names);
    static std::atomic_uint reported{0};
    const unsigned bit = 1U << (site & 1U);
    if (result != original && !(reported.load(std::memory_order_relaxed) & bit)
        && !(reported.fetch_or(bit, std::memory_order_relaxed) & bit)) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "title custom first-match site=%u model_cid=%#x cloned=1 entries=%zu",
            site, home_title::dart_cid(model), names->size());
    }
    return result;
}

extern "C" uint32_t hc_title_is_new_asset(uint64_t text) {
    return home_title::new_install_asset(text) ? 1U : 0U;
}

extern "C" uint64_t hc_title_color_clone(uint64_t color, uint64_t thread) {
    const auto result = home_title::clone_color(color, thread,
        g_title_color.load(std::memory_order_acquire));
    static std::atomic_bool reported{false};
    if (!reported.load(std::memory_order_relaxed)
        && !reported.exchange(true, std::memory_order_relaxed)) {
        uint64_t header = 0, top = 0, end = 0;
        if (color & 1U) std::memcpy(&header, reinterpret_cast<const void *>(color - 1), 8);
        if (thread) {
            std::memcpy(&top, reinterpret_cast<const void *>(thread + 0x60), 8);
            std::memcpy(&end, reinterpret_cast<const void *>(thread + 0x68), 8);
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "title color first-render cid=%#llx cloned=%d top_align=%llu nursery_room=%llu argb=%#x",
            static_cast<unsigned long long>((header >> 12) & 0xfffffU), result != color ? 1 : 0,
            static_cast<unsigned long long>(top & 15),
            static_cast<unsigned long long>(end > top ? end - top : 0),
            g_title_color.load(std::memory_order_relaxed));
    }
    return result;
}

// Both callbacks run on the corresponding Dart thread. Settings publication
// never accesses this cache; different isolates cannot mix rendered geometry.
static thread_local home_layout::WorkspaceRenderSnapshot rendered_workspace;

extern "C" void hc_layout_folder_body(uintptr_t frame, uint64_t heap, uintptr_t saved,
    unsigned kind) {
    const bool ready = __atomic_load_n(&hc_layout_folder_enabled, __ATOMIC_ACQUIRE) != 0;
    const int top = ready ? g_knobs[2].delta_dp.load(std::memory_order_relaxed) : 0;
    const int bottom = ready ? g_knobs[3].delta_dp.load(std::memory_order_relaxed) : 0;
    const int side = ready ? g_knobs[4].delta_dp.load(std::memory_order_relaxed) : 0;
    const uint64_t hits_before = rendered_workspace.hits;
    const bool valid = home_layout::folder_geometry_body(frame, heap, saved, kind, top, bottom, side,
        &rendered_workspace, &g_grid_field);
    const bool render_hit = rendered_workspace.hits != hits_before;
    const uint64_t count = __atomic_fetch_add(&hc_layout_folder_hits[kind], uint64_t{1}, __ATOMIC_RELAXED);
    if (count < 4 || (render_hit && !(rendered_workspace.logged_hits & (1u << kind)))) {
        if (render_hit) rendered_workspace.logged_hits |= 1u << kind;
        __android_log_print(ANDROID_LOG_INFO, "HyperCeiler.HomeLayout",
            "folder geometry body kind=%u ready=%d delta=%d/%d/%d valid=%d render-hit=%d",
            kind, ready ? 1 : 0, top, bottom, side, valid ? 1 : 0, render_hit ? 1 : 0);
        if (kind == 0 || kind == 2) {
            // Diagnostic scalars only; never dereference cached Dart identity.
            for (const auto &l : rendered_workspace.layouts) if (l.grid != 0) {
                __android_log_print(ANDROID_LOG_INFO, "HyperCeiler.HomeLayout",
                    "folder rendered grid cols=%lld rows=%lld raw=%.3f/%.3f inset=%.3f/%.3f stride=%.3f/%.3f",
                    static_cast<long long>(l.columns), static_cast<long long>(l.rows),
                    l.raw_width, l.raw_height, l.side, l.top, l.width, l.height);
            }
        }
    }
}

extern "C" void hc_layout_drop_body(uintptr_t frame, uint64_t heap, uintptr_t saved,
    unsigned kind) {
    const int top = g_knobs[2].delta_dp.load(std::memory_order_relaxed);
    const int bottom = g_knobs[3].delta_dp.load(std::memory_order_relaxed);
    const int side = g_knobs[4].delta_dp.load(std::memory_order_relaxed);
    const uint64_t hits_before = rendered_workspace.hits;
    const home_layout::DropGeometryFields field = drop_geometry_fields();
    const bool valid = home_layout::drop_geometry_body(frame, heap, saved, kind,
        &field, top, bottom, side, &rendered_workspace);
    const uint64_t count = __atomic_fetch_add(&hc_layout_drop_hits[kind], uint64_t{1}, __ATOMIC_RELAXED);
    if (count < 4) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "drop geometry body kind=%u delta=%d/%d/%d valid=%d render-hit=%d",
            kind, top, bottom, side, valid ? 1 : 0,
            rendered_workspace.hits != hits_before ? 1 : 0);
    }
}

extern "C" void hc_layout_workspace_layout(uintptr_t frame, uint64_t heap, int occupied) {
    const int top = g_knobs[2].delta_dp.load(std::memory_order_relaxed);
    const int bottom = g_knobs[3].delta_dp.load(std::memory_order_relaxed);
    const int side = g_knobs[4].delta_dp.load(std::memory_order_relaxed);
    double geometry[4] = {};
    const bool hotseat = occupied == 2;
    const bool valid = hotseat ? home_layout::inset_hotseat_frame(frame, heap, side, geometry, &g_grid_field)
        : home_layout::inset_workspace_frame(frame, heap, occupied != 0,
            &g_grid_field, top, bottom, side, geometry, &rendered_workspace);
    static std::atomic<uint32_t> reports[3]{};
    if (reports[hotseat ? 2 : occupied != 0].fetch_add(1, std::memory_order_relaxed) < 8) {
        __android_log_print(ANDROID_LOG_INFO, "HyperCeiler.HomeLayout",
            "workspace layout splice occupied=%d delta=%d/%d/%d valid=%d "
            "origin=%.3f,%.3f stride=%.3f,%.3f",
            occupied, top, bottom, side, valid ? 1 : 0,
            geometry[0], geometry[1], geometry[2], geometry[3]);
    }
}

extern "C" void hc_layout_indicator_pair(uintptr_t frame, uint64_t heap, uintptr_t thread,
    uint64_t dart_null) {
    uint64_t capsule = 0, dots = 0, top = 0, end = 0;
    std::memcpy(&capsule, reinterpret_cast<const void *>(frame - 8), 8);
    std::memcpy(&dots, reinterpret_cast<const void *>(frame - 0x18), 8);
    std::memcpy(&top, reinterpret_cast<const void *>(thread + 0x60), 8);
    std::memcpy(&end, reinterpret_cast<const void *>(thread + 0x68), 8);
    double delta = 0;
    const uint64_t bits = __atomic_load_n(&hc_layout_dart_IndicatorMargin_delta, __ATOMIC_ACQUIRE);
    std::memcpy(&delta, &bits, 8);
    const bool valid = hc::indicator::wrap_pair(capsule, dots, top, end, heap, dart_null, delta);
    if (valid) {
        std::memcpy(reinterpret_cast<void *>(frame - 8), &capsule, 8);
        std::memcpy(reinterpret_cast<void *>(frame - 0x18), &dots, 8);
        std::memcpy(reinterpret_cast<void *>(thread + 0x60), &top, 8);
    }
    static uint32_t reports = 0;
    if (reports++ < 8) __android_log_print(ANDROID_LOG_INFO, kTag,
        "indicator shared-position delta=%.2f valid=%d", delta, valid ? 1 : 0);
}

extern "C" void hc_layout_probe_container(uint64_t widget, uint64_t heap,
    uint64_t dart_null) {
    static std::atomic<uint32_t> seen{0};
    const uint32_t ordinal = seen.fetch_add(1, std::memory_order_relaxed);
    if ((widget & 1u) == 0) {
        if (ordinal < 12) __android_log_print(ANDROID_LOG_INFO, "HyperCeiler.HomeLayout",
            "container build probe n=%u untagged=%#llx", ordinal,
            static_cast<unsigned long long>(widget));
        return;
    }
    uint64_t header = 0;
    std::memcpy(&header, reinterpret_cast<const void *>(widget - 1), 8);
    const uint32_t widget_cid = static_cast<uint32_t>((header >> 12) & 0xfffffu);
    if (ordinal < 12) __android_log_print(ANDROID_LOG_INFO, "HyperCeiler.HomeLayout",
        "container build probe n=%u cid=%#x widget=%#llx", ordinal, widget_cid,
        static_cast<unsigned long long>(widget));
    if (widget_cid != 0x2017) return;
    uint32_t compressed = 0;
    std::memcpy(&compressed, reinterpret_cast<const void *>(widget + 0x27), 4);
    const uint64_t margin = (heap << 32) + compressed;
    if (ordinal < 12) {
        __android_log_print(ANDROID_LOG_INFO, "HyperCeiler.HomeLayout",
            "container build probe n=%u widget=%#llx margin=%#llx null=%#llx",
            ordinal, static_cast<unsigned long long>(widget),
            static_cast<unsigned long long>(margin), static_cast<unsigned long long>(dart_null));
    }
    if (margin == dart_null || (margin & 1u) == 0) return;
    std::memcpy(&header, reinterpret_cast<const void *>(margin - 1), 8);
    if (((header >> 12) & 0xfffffu) != 0x15c9) return;
    double top = 0, bottom = 0;
    std::memcpy(&top, reinterpret_cast<const void *>(margin + 0xf), 8);
    std::memcpy(&bottom, reinterpret_cast<const void *>(margin + 0x1f), 8);
    if (top == 0 && bottom == 0) return;
    static std::atomic<uint32_t> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 12) {
        __android_log_print(ANDROID_LOG_INFO, "HyperCeiler.HomeLayout",
            "container probe widget=%#llx margin=%#llx top=%.2f bottom=%.2f",
            static_cast<unsigned long long>(widget),
            static_cast<unsigned long long>(margin), top, bottom);
    }
}

/*
 * Install the stage-one probe synchronously, from the loader callback for libapp.so.
 *
 * The probe's target is called exactly once, during start-up. The ordinary path resolves the symbol
 * table (an xz decode plus a 74k-symbol scan, tens of milliseconds) on a background thread while the
 * launcher keeps running, so the patch could land while that single call was executing - a torn
 * instruction stream, which is one of the two ways the launcher died. Doing the resolution here, in
 * the dlopen callback, is before the Dart runtime runs at all, so the function is patched before it
 * can ever be called.
 */
/*
 * Adopt the home-layout state for this process, resetting anything inherited across a fork.
 *
 * Every flag below is process-global and is inherited by a forked child, while the thread that
 * produced it is not. HYOS forks the desktop out of a process that shares this module's code and its
 * command line, so a desktop can start life already holding the parent's "worker is running" or
 * "attempts exhausted" verdict - and then refuse to create a worker for the rest of its life.
 *
 * Adoption is pid-stamped, so it is a no-op everywhere except the first call in a process: the
 * explicit call at each start site and the one inside start_home_layout_hooks cannot double-reset a
 * live desktop, and a start site that forgets to prepare cannot wedge the desktop either. Returns
 * true when this call was the one that adopted (and therefore reset) the state.
 *
 * The reset body is the one that used to live in home_layout_prepare_for_launcher_child: the spawner
 * shares this code and burns worker attempts and arm flags on its own behalf, which was observed live
 * as a launcher with no layout logs at all.
 */
std::atomic<pid_t> g_state_owner{0};

bool adopt_layout_state() {
    const pid_t self = getpid();
    if (g_state_owner.load(std::memory_order_acquire) == self) return false;
    g_state_owner.store(self, std::memory_order_release);

    g_started.store(false, std::memory_order_release);
    g_attempts.store(0, std::memory_order_release);
    g_ready.store(false, std::memory_order_release);
    g_dart_ready.store(false, std::memory_order_release);
    g_field_writes_enabled.store(false, std::memory_order_release);
    g_cell_x.store(0, std::memory_order_relaxed);
    g_cell_y.store(0, std::memory_order_relaxed);
    __atomic_store_n(&hc_layout_animation_ratio_enabled, uint8_t{0}, __ATOMIC_RELEASE);
    __atomic_store_n(&hc_layout_animation_ratio_bits, uint64_t{0}, __ATOMIC_RELAXED);
    __atomic_store_n(&hc_layout_animation_ratio_original_bits, uint64_t{0}, __ATOMIC_RELAXED);
    hc_layout_animation_ratio_hits = 0;
    hc_layout_animation_ratio_original = nullptr;
    g_animation_hook_armed = false;
    __atomic_store_n(&hc_layout_magic_enabled, uint8_t{0}, __ATOMIC_RELEASE);
    __atomic_store_n(&hc_layout_magic_bits, uint64_t{0}, __ATOMIC_RELAXED);
    __atomic_store_n(&hc_layout_magic_original_bits, uint64_t{0}, __ATOMIC_RELAXED);
    hc_layout_magic_hits = 0;
    hc_layout_magic_original = nullptr;
    g_magic_hook_armed = false;
    g_loader_prime_finished.store(false, std::memory_order_release);
    g_loader_priming.clear(std::memory_order_release);
    g_capacity_busy.clear(std::memory_order_release);
    g_capacity_bound = g_capacity_enabled = false;
    g_capacity_known = true;
    for (auto &word : g_capacity_words) word = {};
    g_widget_move_busy.clear(std::memory_order_release);
    g_gadget_requested.store(false, std::memory_order_release);
    g_gadget_bound = false;
    hc_gadget_bridge_enabled = 0;
    hc_gadget_original[0] = hc_gadget_original[1] = hc_gadget_original[2] = nullptr;
    hc_gadget_span_continue = hc_gadget_span_reject = 0;
    hc_gadget_put_int = hc_gadget_common = 0;
    hc_gadget_select_continue = hc_gadget_return_continue = 0;
    g_widget_move_address = 0;
    g_widget_move_original = 0;
    g_grid_field = {};
    g_widget_move_checked = g_widget_move_enabled = false;
    g_widget_move_known = true;
    g_captures_armed = false;
    g_desktop_title_bound = false;
    hc_desktop_title_original = nullptr;
    hc_desktop_title_continue = 0;
    hc_desktop_title_height_original = nullptr;
    hc_desktop_title_height_continue = 0;
    g_title_hide_bound = false;
    hc_title_prefix_original = nullptr;
    hc_title_folder_new_original = nullptr;
    hc_title_folder_new_caller = 0;
    hc_title_light_original = nullptr;
    g_title_custom_bound = false;
    hc_title_custom_original = nullptr;
    hc_title_custom_continue = 0;
    g_title_component_method = g_title_pin_method = 0;
    g_title_color_bound = false;
    hc_title_color_original = nullptr;
    hc_title_color_continue = 0;
    g_drawer_title_bound = false;
    hc_drawer_title_original = nullptr;
    hc_drawer_title_height_original = nullptr;
    hc_drawer_title_height_continue = 0;
    hc_drawer_title_caller = 0;
    hc_drawer_title_continue = 0;
    __atomic_store_n(&hc_layout_indicator_mode, uint32_t{0}, __ATOMIC_RELEASE);
    hc_layout_dart_IndicatorDot_address = 0;
    hc_layout_dart_IndicatorDot_getter = 0;
    hc_layout_dart_IndicatorDot_size = 0;
    hc_layout_dart_IndicatorDot_source = {};
    hc_layout_dart_IndicatorDot_words = {};
    hc_layout_dart_IndicatorDot_original = nullptr;
    hc_layout_dart_IndicatorDot_armed = false;
    hc_layout_dart_IndicatorDot_hits = 0;
    hc_layout_indicator_edit_call = 0;
    hc_layout_indicator_build_empty = 0;
    hc_layout_indicator_build_dots = 0;
    hc_layout_indicator_idle_caller = 0;
    __atomic_store_n(&hc_layout_folder_enabled, 0u, __ATOMIC_RELEASE);
    for (size_t i = 0; i < 5; ++i) {
        hc_layout_folder_resume[i] = 0; hc_layout_folder_hits[i] = 0; hc_layout_folder_original[i] = nullptr;
    }
    __atomic_store_n(&hc_layout_drop_enabled, 0u, __ATOMIC_RELEASE);
    for (size_t i = 0; i < 2; ++i) {
        hc_layout_drop_resume[i] = 0; hc_layout_drop_hits[i] = 0; hc_layout_drop_original[i] = nullptr;
    }
    g_grid_autofit_bound = g_grid_autofit_checked = false;
    g_grid_autofit_fields = {};
    __atomic_store_n(&hc_grid_autofit_ready, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&hc_grid_autofit_requested, 0u, __ATOMIC_RELEASE);
    for (size_t i = 0; i < 2; ++i) { hc_grid_autofit_original[i] = nullptr; hc_grid_autofit_resume[i] = 0; }
    g_folder_layout_bound = g_folder_layout_checked = false;
    __atomic_store_n(&hc_folder_layout_ready, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&hc_folder_layout_requested, uint64_t{0}, __ATOMIC_RELEASE);
    g_folder_cell_width_field = g_folder_gap_field = -1;
    g_folder_screen_width_field = g_folder_screen_height_field = g_folder_cling_width_field = -1;
    g_folder_center_pool = 0;
    g_folder_controller_config_field = g_folder_rx_value_field = g_folder_enum_index_field = -1;
    g_folder_inner = home_layout::FolderInnerContainer{};
    g_folder_inner_owner_local = 0;
    g_folder_inner_ready = false;
    for (auto &original : hc_folder_layout_original) original = nullptr;
    g_probe_primed = false;
    g_hook_globals_inited = false;
    init_knob_hooks();
    g_dart.reset();
    g_config_capture_va = 0;
    g_dock_capture_va = 0;
    g_config_capture_address = 0;
    g_dock_capture_address = 0;
    hc_layout_config_object = 0;
    hc_layout_config_capture_hits = 0;
    g_dart_container_path.clear();
    g_dart_view_begin = 0;
    g_dart_view_end = 0;
    g_container_path.clear();
    g_view_begin = 0;
    g_view_end = 0;
    for (Slot &slot : g_slots) slot = Slot{};
    for (KnobRuntime &knob : g_knobs) {
        knob.hook_mode = false;
        knob.hook_address = 0;
        knob.hook_armed = false;
        knob.symbol.clear();
        knob.path.store(0, std::memory_order_relaxed);
        knob.getter = 0;
        knob.getter_size = 0;
        knob.pristine_valid = false;
        knob.delta_dp.store(0, std::memory_order_relaxed);
    }
    return true;
}

void home_layout_prepare_for_launcher_child() {
    (void) adopt_layout_state();
}

void home_layout_set_desktop_title_size(int sp) {
    if (sp < 0 || sp > 20) return;
    g_title_desktop_sp.store(sp, std::memory_order_release);
    __atomic_store_n(&hc_desktop_title_sp, static_cast<uint32_t>(sp), __ATOMIC_RELEASE);
    __atomic_store_n(&hc_drawer_title_override,
        g_title_drawer_sp.load(std::memory_order_acquire) != 12 ? 1U : 0U,
        __ATOMIC_RELEASE);
}

void home_layout_set_drawer_title_size(int sp) {
    if (sp < 0 || sp > 20) return;
    g_title_drawer_sp.store(sp, std::memory_order_release);
    __atomic_store_n(&hc_drawer_title_sp, static_cast<uint32_t>(sp), __ATOMIC_RELEASE);
    __atomic_store_n(&hc_drawer_title_override,
        sp != 12 ? 1U : 0U,
        __ATOMIC_RELEASE);
}

void home_layout_set_title_color(int argb) {
    g_title_color.store(argb, std::memory_order_release);
    __atomic_store_n(&hc_title_color_enabled, argb != -1 ? 1U : 0U, __ATOMIC_RELEASE);
}

/*
 * Bind and arm the geometry hooks inside the same callback window as the probe. The desktop computes
 * its geometry once, right after this library loads; a hook installed after that point is never
 * called again, which is exactly how a "successfully armed" knob ends up invisible on screen.
 */
void prime_home_layout_knobs(HookFunction hook, UnhookFunction unhook) {
    if (hook == nullptr || g_loader_prime_finished.load(std::memory_order_acquire)) return;
    if (g_loader_priming.test_and_set(std::memory_order_acquire)) return;
    struct ReleasePrime { ~ReleasePrime() { g_loader_priming.clear(std::memory_order_release); } } release;

    g_hook_function = hook;
    g_unhook_function = unhook;
    if (!ensure_dart_library()) return;
    char debug_gate[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.hyperceiler.layout.override", debug_gate) > 0
        && debug_gate[0] == '1') {
        apply_debug_hook_overrides();
    }
    // Shipped hook targets, the same pass the worker runs later.
    for (size_t index = 0; index < g_knobs.size(); ++index) {
        if (kKnobHookSymbols[index] == nullptr || g_knobs[index].hook_mode) continue;
        g_knobs[index].hook_mode = true;
        g_knobs[index].symbol = kKnobHookSymbols[index];
    }
    (void)bind_knobs();
    /*
     * The symbol table is parsed on first use and the monitor thread may have started that parse a
     * moment earlier; until it finishes, every lookup misses. A bounded wait here is what makes the
     * callback actually beat the first layout instead of leaving it to the slower worker.
     */
    for (int attempt = 0; attempt < 25; ++attempt) {
        bool pending = false;
        for (const KnobRuntime &knob : g_knobs) {
            if (knob.hook_mode && knob.hook_address == 0) pending = true;
        }
        if (!pending) break;
        delay_ms(20);
        (void)bind_knobs();
    }
    /*
     * Pull the deltas here as well: the very first layout should already run on the user's values
     * instead of waiting for the slower worker path to publish them.
     */
    home_layout::Config config;
    if (home_layout::query_config(config)) {
        sync_title_config(config);
        for (size_t index = 0; index < HC_LAYOUT_KNOB_COUNT; ++index) {
            g_knobs[index].delta_dp.store(config.knobs[index].delta_dp,
                std::memory_order_relaxed);
        }
    }
    std::vector<size_t> order; // worker adopts these same registered slots after publication
    (void) bind_title_color();
    (void) bind_title_custom();
    (void) bind_drawer_title();
    (void) bind_desktop_title();
    (void) bind_title_hide();
    const size_t added = arm_hooks(order);
    if (added == 0 && order.empty()) return;
    /*
     * One slot per ensure call, each with its own verdict. A batched ensure returns a single false
     * for the whole order, which hides which address the bank refused - the difference between a
     * page that cannot be written and a prologue that cannot be replayed is exactly the diagnosis.
     */
    for (const size_t index : order) {
        Slot &slot = g_slots[index];
        Words live{};
        const bool read_ok = stable_read(slot, live);
        const bool same = read_ok && live == slot.original_words;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "layout slot %zu addr=%p read=%d same=%d live0=%08x orig0=%08x", index,
            reinterpret_cast<void *>(slot.address), read_ok, same, read_ok ? live[0] : 0,
            slot.original_words[0]);
        std::vector<size_t> single{index};
        const bool one = nhk::ensure_slots_live(g_slots, slot_host(), single);
        __android_log_print(ANDROID_LOG_INFO, kTag, "layout slot %zu live=%d", index, one ? 1 : 0);
        if (!one) {
            for (KnobRuntime &knob : g_knobs) {
                if (knob.hook_mode && knob.hook_armed &&
                    kKnobHookSlotBase + (&knob - g_knobs.data()) == index) {
                    knob.hook_armed = false;
                }
            }
        }
    }
    publish_hooks();
    (void) sync_hotseat_capacity(config.tweaks.hotseat_unlimited);
    (void) sync_widget_move(config.widget_allow_move);
    g_loader_prime_finished.store(true, std::memory_order_release);
}

void prime_home_layout_probe(HookFunction hook, UnhookFunction unhook) {
    /*
     * Idempotent: the loader reports libapp.so more than once. Overwriting an already registered slot
     * reset its bookkeeping and made the bank install a second hook on the same address, which killed
     * the launcher - the hook itself had installed cleanly on the first call (live=1).
     */
    if (g_knobs[2].hook_armed || g_probe_primed) return;
    char probe[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.hyperceiler.layout.probe_top", probe) <= 0
        || probe[0] != '1') return;
    if (hook == nullptr) return;
    g_hook_function = hook;
    g_unhook_function = unhook;
    if (!ensure_dart_library()) return;
    g_knobs[2].hook_mode = true;
    g_knobs[2].symbol = "GridSizeCalRules.stableWorkspaceCellPaddingTop";
    g_knobs[2].hook_entry = reinterpret_cast<void *>(hc_layout_passthrough_entry);
    g_knobs[2].hook_original = &hc_layout_passthrough_original;
    uint32_t va = 0;
    uint32_t size = 0;
    if (!hometweaks::HomeTweaksFindSymbol(g_knobs[2].symbol.c_str(), &va, &size) || size < 16) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "layout probe: symbol not found");
        return;
    }
    if (!bind_dart_target(va, g_knobs[2].hook_address, g_knobs[2].hook_source,
            g_knobs[2].hook_words)) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "layout probe: bind failed va=%#x", va);
        return;
    }
    /*
     * Fill the same two diagnostic fields bind_knobs fills, so this slot's log line reports the
     * address it actually hooked instead of a zero. The worker's per-knob line is the one place a
     * reader checks "what did we hook, in which build" - a zero there is worse than useless.
     */
    g_knobs[2].getter = va;
    g_knobs[2].getter_size = size;
    static std::vector<size_t> order;
    const size_t slot = kKnobHookSlotBase + 2;
    g_slots[slot] = {g_knobs[2].hook_address, g_knobs[2].hook_entry, g_knobs[2].hook_original,
        g_knobs[2].hook_source, g_knobs[2].hook_words};
    order.push_back(slot);
    g_knobs[2].hook_armed = true;
    g_probe_primed = true;
    const bool live = nhk::ensure_slots_live(g_slots, slot_host(), order);
    __android_log_print(ANDROID_LOG_WARN, kTag,
        "layout probe primed va=%#x addr=%p live=%d", va,
        reinterpret_cast<void *>(g_knobs[2].hook_address), live ? 1 : 0);
}

/*
 * One line per distinct start verdict per process.
 *
 * Every gate below used to return without a word, which made "this desktop has no layout worker"
 * indistinguishable from "the worker runs and says nothing" - the question POWER_AUDIT_20260917 §5.1
 * could not answer, and the one a whole device session was spent on. Each verdict is reported once,
 * because the property hook that calls this fires on every property read and would otherwise turn
 * the instrumentation into a log storm.
 */
uint32_t start_verdict_bit(const char *verdict) {
    if (verdict == nullptr) return 1u << 0;
    if (std::strcmp(verdict, "hook-null") == 0) return 1u << 1;
    if (std::strcmp(verdict, "ready") == 0) return 1u << 2;
    if (std::strcmp(verdict, "dart-ready") == 0) return 1u << 3;
    if (std::strcmp(verdict, "attempts-exhausted") == 0) return 1u << 4;
    return 1u << 5;
}

void report_start_verdict(const char *site, const char *verdict, bool adopted, int attempts) {
    static std::atomic<uint32_t> reported{0};
    const uint32_t bit = start_verdict_bit(verdict);
    if ((reported.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) return;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "layout start site=%s pid=%d adopted=%d verdict=%s attempts=%d ready=%d dart_ready=%d "
        "started=%d",
        site != nullptr ? site : "?", static_cast<int>(getpid()), adopted ? 1 : 0,
        verdict != nullptr ? verdict : "starting", attempts,
        g_ready.load(std::memory_order_relaxed) ? 1 : 0,
        g_dart_ready.load(std::memory_order_relaxed) ? 1 : 0,
        g_started.load(std::memory_order_relaxed) ? 1 : 0);
}

/*
 * `site` names the signal that asked for the worker (native-init / property / setprogname / library /
 * configure), so the log above can say which of the five call sites got what verdict.
 */
void start_home_layout_hooks(const char *site, HookFunction hook, UnhookFunction unhook) {
    /*
     * Adopt (and therefore reset) inherited state before reading a single gate: a desktop that
     * inherited "already running" would otherwise never create its own worker, and every call site
     * relying on its own prepare call is exactly the discipline that failed here.
     */
    const bool adopted = adopt_layout_state();
    const int attempts = g_attempts.load(std::memory_order_acquire);
    const char *verdict = nullptr;
    if (hook == nullptr) verdict = "hook-null";
    else if (g_ready.load(std::memory_order_relaxed)) verdict = "ready";
    else if (g_dart_ready.load(std::memory_order_relaxed)) verdict = "dart-ready";
    else if (attempts >= kMaxWorkerAttempts) verdict = "attempts-exhausted";
    if (verdict == nullptr) {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            verdict = "already-started";
        }
    }
    if (verdict != nullptr) {
        report_start_verdict(site, verdict, adopted, attempts);
        return;
    }
    report_start_verdict(site, nullptr, adopted, attempts);
    g_attempts.fetch_add(1, std::memory_order_acq_rel);
    g_hook_function = hook;
    g_unhook_function = unhook;
    pthread_attr_t attributes{};
    if (pthread_attr_init(&attributes) != 0) {
        g_started.store(false, std::memory_order_release);
        return;
    }
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread{};
    const int created = pthread_create(&thread, &attributes, worker, nullptr);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        g_started.store(false, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "layout worker unavailable=%d", created);
    }
}
