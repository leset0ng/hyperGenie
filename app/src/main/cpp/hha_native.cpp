// SPDX-License-Identifier: Apache-2.0
//
// HyperHomeAnimation — native MVP for the HyperOS launcher's app-open animation.
//
// What it does
// ------------
// Adds a real 3D perspective tilt to the app window while the launcher's stock
// open animation runs. The stock animation is not replaced: its transaction is
// kept and one extra RotateState is written onto the same transaction.
//
//   launcher (Rust/Flutter)
//     hyper_os_ui::SurfaceControlTransaction::start_sf_animation
//       libhyper_os_ui_public.so  SurfaceControlTransaction_start_sf_animation
//         libhyper_os_ui_private.dylib.so  transaction_abi_impl::start_sf_animation
//           libgui.so  Transaction::startSFAnimation(sc, params)   <-- hooked
//             original()                       stock morph, untouched
//             setSurfaceRotation(sc, ...)      this module's tilt
//           libgui.so  Transaction::apply()
//             SurfaceFlinger  MiRenderEngine::applyRotateState()
//               real X/Y/Z rotation + perspective on the Skia canvas
//
// Why native, and why this hook point
// -----------------------------------
// On Android 17 / HyperOS 4 the launcher ships without any dex: it is a
// Rust/Flutter program started by /system_ext/bin/hyos_spawner, so java_init.list
// class hooks never run in it. The native_init entry is the only option.
//
// Among the native candidates, libgui's Transaction::startSFAnimation is the one
// safe boundary:
//   * The public Rust wrapper takes nine arguments with a register/stack split; a
//     mismatched trampoline silently corrupts the callee's stack.
//   * transaction_abi_impl::start_sf_animation is Rust and builds SfAnimationParams
//     through an sret (x8) return; a plain function pointer cannot express that.
//   * Transaction::startSFAnimation is an ordinary C++ member function taking two
//     sp<> by reference and returning *this, so it round-trips cleanly.
//
// RotateState slot layout (verified against the SF consumer)
// ---------------------------------------------------------
//   0..2  X/Y/Z angles in RADIANS (the Java wrapper calls them rotDegrees)
//   3..4  rotation anchor offsets from the bounds centre
//   5..7  extra X/Y/Z translation
//   8     camera field-of-view in degrees
//   9     camera distance (not a ratio of window height)
//
// Runtime tuning
// --------------
// /data/local/tmp/hha.conf is re-read at most every 250 ms, so angles can be
// changed over adb without rebuilding or restarting the launcher.

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <mutex>

namespace {

constexpr char kLogTag[] = "HHA";
constexpr char kConfigPrimary[] = "/data/local/tmp/hha.conf";
constexpr char kConfigFallback[] = "/data/adb/hha.conf";

// libxposed native module ABI: the framework passes this to native_init.
struct NativeAPIEntries {
    uint32_t version;
    int (*hookFunc)(void* target, void* replacement, void** backup);
    int (*unhookFunc)(void* target);
};

using NativeOnModuleLoaded = void (*)(const char* name, void* handle);

//-----------------------------------------------------------------------------
// libgui symbols
//-----------------------------------------------------------------------------

constexpr char kSymStart[] =
        "_ZN7android21SurfaceComposerClient11Transaction16startSFAnimation"
        "ERKNS_2spINS_14SurfaceControlEEERKNS2_INS_3gui17SfAnimationParamsEEE";
constexpr char kSymAdjust[] =
        "_ZN7android21SurfaceComposerClient11Transaction17adjustSFAnimation"
        "ERKNS_2spINS_14SurfaceControlEEERKNS2_INS_3gui17SfAnimationParamsEEE";
constexpr char kSymSetRotation[] =
        "_ZN7android21SurfaceComposerClient11Transaction18setSurfaceRotation"
        "ERKNS_2spINS_14SurfaceControlEEEffffffffff";
constexpr char kSymGetLayerId[] = "_ZNK7android14SurfaceControl10getLayerIdEv";

// Note: libhyper_os_ui_private.dylib.so's transaction_abi_impl::start_sf_animation
// is deliberately NOT hooked. See the header comment for why.
//
// SfAnimationParams::create is likewise not hooked: it returns sp<> through x8.

using StartSFAnimationFn = void* (*)(void* transaction, const void* surface_control_ref,
                                     const void* params);
using AdjustSFAnimationFn = void* (*)(void* transaction, const void* surface_control_ref,
                                      const void* params);
using SetSurfaceRotationFn = void* (*)(void* transaction, const void* surface_control_ref,
                                       float rot_x, float rot_y, float rot_z, float center_x,
                                       float center_y, float offset_x, float offset_y,
                                       float offset_z, float camera_degrees,
                                       float camera_distance);
using GetLayerIdFn = int32_t (*)(const void* surface_control);

SetSurfaceRotationFn g_set_surface_rotation = nullptr;
GetLayerIdFn g_get_layer_id = nullptr;
StartSFAnimationFn g_original_start = nullptr;
AdjustSFAnimationFn g_original_adjust = nullptr;

std::atomic<uint32_t> g_start_hits{0};
std::atomic<uint32_t> g_adjust_hits{0};
std::atomic<uint32_t> g_logged_hits{0};

//-----------------------------------------------------------------------------
// Tunables
//-----------------------------------------------------------------------------

struct Config {
    bool enabled = true;
    // Degrees here; RotateState stores radians.
    float rot_x_deg = 0.0f;
    float rot_y_deg = 26.0f;
    float rot_z_deg = 0.0f;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    float camera_degrees = 45.0f;
    float camera_distance = 1600.0f;
    bool normalize_icon_perspective = true;
    // Rotate only while the animation is being submitted; the animator keeps
    // re-asserting it on every adjust, and SurfaceFlinger clears the state itself
    // when the animation ends.
    bool tilt_adjusts = true;
    // Rotate the app window (type 2) and floating icon (type 1). The icon uses
    // native icon units, so its camera distance and anchor need normalization
    // before both surfaces can share the same parent-space projection.
    bool tilt_app_type = true;
    bool tilt_icon_type = true;
    bool verbose = true;
};

Config g_config;
std::atomic<uint64_t> g_config_stamp{0};
std::atomic<int64_t> g_config_next_check_ns{0};

int64_t MonotonicNs() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

float ParseFloat(const char* value, float fallback) {
    if (value == nullptr || *value == '\0') return fallback;
    char* end = nullptr;
    const float parsed = strtof(value, &end);
    if (end == value) return fallback;
    return parsed;
}

bool ParseBool(const char* value, bool fallback) {
    if (value == nullptr) return fallback;
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) return false;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) return true;
    return fallback;
}

void Trim(char* text) {
    if (text == nullptr) return;
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                          text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') ++start;
    if (start != 0) memmove(text, text + start, length - start + 1);
}

void RefreshConfig() {
    const int64_t now = MonotonicNs();
    if (now < g_config_next_check_ns.load(std::memory_order_relaxed)) return;
    g_config_next_check_ns.store(now + 250000000LL, std::memory_order_relaxed);

    struct stat info {};
    const char* path = kConfigPrimary;
    if (stat(path, &info) != 0) {
        path = kConfigFallback;
        if (stat(path, &info) != 0) return;
    }

    const uint64_t stamp = (static_cast<uint64_t>(info.st_mtime) << 32) ^
                           static_cast<uint64_t>(info.st_size);
    if (stamp == g_config_stamp.load(std::memory_order_relaxed)) return;

    FILE* file = fopen(path, "re");
    if (file == nullptr) return;

    Config next = g_config;
    char line[512];
    while (fgets(line, sizeof(line), file) != nullptr) {
        Trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char* separator = strchr(line, '=');
        if (separator == nullptr) continue;
        *separator = '\0';
        Trim(line);
        char* value = separator + 1;
        Trim(value);
        if (strcmp(line, "enabled") == 0) {
            next.enabled = ParseBool(value, next.enabled);
        } else if (strcmp(line, "x_deg") == 0) {
            next.rot_x_deg = ParseFloat(value, next.rot_x_deg);
        } else if (strcmp(line, "y_deg") == 0) {
            next.rot_y_deg = ParseFloat(value, next.rot_y_deg);
        } else if (strcmp(line, "z_deg") == 0) {
            next.rot_z_deg = ParseFloat(value, next.rot_z_deg);
        } else if (strcmp(line, "center_x") == 0) {
            next.center_x = ParseFloat(value, next.center_x);
        } else if (strcmp(line, "center_y") == 0) {
            next.center_y = ParseFloat(value, next.center_y);
        } else if (strcmp(line, "offset_x") == 0) {
            next.offset_x = ParseFloat(value, next.offset_x);
        } else if (strcmp(line, "offset_y") == 0) {
            next.offset_y = ParseFloat(value, next.offset_y);
        } else if (strcmp(line, "offset_z") == 0) {
            next.offset_z = ParseFloat(value, next.offset_z);
        } else if (strcmp(line, "camera_degrees") == 0) {
            next.camera_degrees = ParseFloat(value, next.camera_degrees);
        } else if (strcmp(line, "camera_distance") == 0) {
            next.camera_distance = ParseFloat(value, next.camera_distance);
        } else if (strcmp(line, "tilt_adjusts") == 0) {
            next.tilt_adjusts = ParseBool(value, next.tilt_adjusts);
        } else if (strcmp(line, "tilt_app_type") == 0) {
            next.tilt_app_type = ParseBool(value, next.tilt_app_type);
        } else if (strcmp(line, "tilt_icon_type") == 0) {
            next.tilt_icon_type = ParseBool(value, next.tilt_icon_type);
        } else if (strcmp(line, "normalize_icon_perspective") == 0) {
            next.normalize_icon_perspective = ParseBool(value, next.normalize_icon_perspective);
        } else if (strcmp(line, "verbose") == 0) {
            next.verbose = ParseBool(value, next.verbose);
        }
    }
    fclose(file);

    g_config = next;
    g_config_stamp.store(stamp, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "config loaded from %s enabled=%d rot=(%.2f,%.2f,%.2f) cam=%.1f/%.1f",
                        path, next.enabled ? 1 : 0, next.rot_x_deg, next.rot_y_deg,
                        next.rot_z_deg, next.camera_degrees, next.camera_distance);
}

inline float DegreesToRadians(float degrees) { return degrees * 0.017453292519943295f; }

// android::gui::SfAnimationParams layout, confirmed from the accessors in
// libhyper_os_sys.dylib.so: `anim_type()` compiles to `ldr w0,[x0,#0x40]` and
// `adjust_anim_type()` to `ldr w0,[x0,#0x44]`. The hook receives an sp<> by
// reference, so the object sits one dereference further in.
constexpr size_t kSfAnimationParamsAnimTypeOffset = 0x40;

// SfAnimationType discriminants as used by the launcher's submission calls.
constexpr int32_t kAnimTypeIcon = 1;
constexpr int32_t kAnimTypeApp = 2;

int32_t AnimTypeOf(const void* params_ref) {
    if (params_ref == nullptr) return -1;
    const void* object = *reinterpret_cast<const void* const*>(params_ref);
    if (object == nullptr) return -1;
    return *reinterpret_cast<const int32_t*>(static_cast<const char*>(object) +
                                             kSfAnimationParamsAnimTypeOffset);
}

bool ShouldTilt(int32_t anim_type) {
    if (anim_type == kAnimTypeApp) return g_config.tilt_app_type;
    if (anim_type == kAnimTypeIcon) return g_config.tilt_icon_type;
    // Leave unsupported animation types unchanged.
    return false;
}

int32_t LayerIdOf(const void* surface_control_ref) {
    if (g_get_layer_id == nullptr || surface_control_ref == nullptr) return -1;
    // sp<SurfaceControl> is a single pointer; the id lives on the control itself.
    const void* surface_control = *reinterpret_cast<const void* const*>(surface_control_ref);
    if (surface_control == nullptr) return -1;
    return g_get_layer_id(surface_control);
}

// The two maps use the ROM's arm64 libc++ tree layout. Read existing nodes
// only: no foreign C++ constructors, allocations, or ownership changes.
const unsigned char* FindParam(const void* params_ref, size_t root_offset,
                               const char* key) {
    if (params_ref == nullptr) return nullptr;
    const auto* object = *static_cast<const unsigned char* const*>(params_ref);
    if (object == nullptr) return nullptr;
    const auto* node = *reinterpret_cast<const unsigned char* const*>(object + root_offset);
    const size_t length = strlen(key);
    for (unsigned depth = 0; node != nullptr && depth < 64; ++depth) {
        const unsigned char tag = node[0x20];
        const bool is_long = (tag & 1) != 0;
        const size_t size = is_long ? *reinterpret_cast<const size_t*>(node + 0x28) : tag >> 1;
        if (size > 256) return nullptr;
        const char* data = is_long ? *reinterpret_cast<const char* const*>(node + 0x30)
                                   : reinterpret_cast<const char*>(node + 0x21);
        if (data == nullptr) return nullptr;
        int comparison = memcmp(key, data, length < size ? length : size);
        if (comparison == 0) comparison = length < size ? -1 : length > size ? 1 : 0;
        if (comparison == 0) return node + 0x38;
        node = *reinterpret_cast<const unsigned char* const*>(node + (comparison > 0 ? 8 : 0));
    }
    return nullptr;
}

float FixedParam(const void* params_ref, const char* key) {
    const auto* value = FindParam(params_ref, 0x30, key);
    return value != nullptr ? *reinterpret_cast<const float*>(value) : 0.0f;
}

float SpringExtent(const void* params_ref, const char* key) {
    const auto* value = FindParam(params_ref, 0x18, key);
    if (value == nullptr) return 0.0f;
    const auto* property = *reinterpret_cast<const unsigned char* const*>(value);
    if (property == nullptr) return 0.0f;
    const float start = *reinterpret_cast<const float*>(property + 0x10);
    const float end = *reinterpret_cast<const float*>(property + 0x14);
    return std::isfinite(start) && std::isfinite(end) ? fmaxf(start, end) : 0.0f;
}

struct TiltTarget {
    int32_t id = -1;
    int32_t type = -1;
    float camera_scale = 1.0f;
    int64_t started_ns = 0;
};
std::mutex g_targets_mutex;
TiltTarget g_targets[64];
size_t g_next_target = 0;

TiltTarget RememberTarget(const void* surface_control_ref,
                          const void* params_ref, int32_t type) {
    const int64_t now = MonotonicNs();
    TiltTarget target{LayerIdOf(surface_control_ref), type, 1.0f, now};
    if (type == kAnimTypeIcon && g_config.normalize_icon_perspective) {
        const bool width_based = FixedParam(params_ref, "tileType") != 0.0f;
        const float icon_far = FixedParam(params_ref, width_based ? "validIconWidth" : "validIconHeight");
        const float icon_near = FixedParam(params_ref, width_based ? "validIconLeft" : "validIconTop");
        const float icon_span = icon_far - icon_near;
        const float app_extent = SpringExtent(params_ref, width_based ? "width" : "height");
        // SF uses -1 / (distance * length(basisY)). The icon expands from its
        // native span to the app extent, so equal raw distances are unequal
        // physical cameras. Convert icon units; do not weaken the app camera.
        if (std::isfinite(app_extent) && std::isfinite(icon_span) &&
                std::isfinite(icon_near) && std::isfinite(icon_far) &&
                app_extent > 0.0f && icon_span > 0.0f) {
            target.camera_scale = icon_span / app_extent;
        }
        if (g_config.verbose) {
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "projection layer=%d axis=%s appExtent=%.2f iconSpan=%.2f scale=%.6f",
                                target.id, width_based ? "width" : "height", app_extent,
                                icon_span, target.camera_scale);
        }
    }
    if (target.id >= 0 && (type == kAnimTypeApp || type == kAnimTypeIcon)) {
        std::lock_guard<std::mutex> lock(g_targets_mutex);
        g_targets[g_next_target++ % 64] = target;
    }
    return target;
}

TiltTarget FindTarget(int32_t id) {
    std::lock_guard<std::mutex> lock(g_targets_mutex);
    const int64_t now = MonotonicNs();
    for (size_t i = 0; i < 64; ++i) {
        const auto& target = g_targets[(g_next_target + 63 - i) % 64];
        if (target.id == id && now - target.started_ns < 10000000000LL) return target;
    }
    return {};
}

void ApplyTilt(void* transaction, const void* surface_control_ref, const char* phase,
               float camera_scale) {
    if (g_set_surface_rotation == nullptr || transaction == nullptr ||
            surface_control_ref == nullptr) {
        return;
    }
    const Config config = g_config;
    g_set_surface_rotation(transaction, surface_control_ref,
                           DegreesToRadians(config.rot_x_deg),
                           DegreesToRadians(config.rot_y_deg),
                           DegreesToRadians(config.rot_z_deg), config.center_x * camera_scale,
                           config.center_y * camera_scale, config.offset_x, config.offset_y,
                           config.offset_z, config.camera_degrees,
                           config.camera_distance * camera_scale);

    if (config.verbose && g_logged_hits.fetch_add(1, std::memory_order_relaxed) < 40) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s layer=%d rot=%.2f cam=%.0f/%.0f",
                            phase, LayerIdOf(surface_control_ref), config.rot_y_deg,
                            config.camera_degrees, config.camera_distance * camera_scale);
    }
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------

void* HookStartSFAnimation(void* transaction, const void* surface_control_ref,
                           const void* params) {
    void* result = nullptr;
    if (g_original_start != nullptr) {
        result = g_original_start(transaction, surface_control_ref, params);
    }
    RefreshConfig();
    g_start_hits.fetch_add(1, std::memory_order_relaxed);
    const int32_t anim_type = AnimTypeOf(params);
    const TiltTarget target = RememberTarget(surface_control_ref, params, anim_type);
    if (g_config.enabled && ShouldTilt(anim_type)) {
        ApplyTilt(transaction, surface_control_ref, "start", target.camera_scale);
    } else if (g_config.verbose && g_logged_hits.fetch_add(1, std::memory_order_relaxed) < 40) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "start layer=%d animType=%d skipped",
                            LayerIdOf(surface_control_ref), anim_type);
    }
    return result;
}

void* HookAdjustSFAnimation(void* transaction, const void* surface_control_ref,
                            const void* params) {
    void* result = nullptr;
    if (g_original_adjust != nullptr) {
        result = g_original_adjust(transaction, surface_control_ref, params);
    }
    RefreshConfig();
    g_adjust_hits.fetch_add(1, std::memory_order_relaxed);
    const TiltTarget target = FindTarget(LayerIdOf(surface_control_ref));
    if (g_config.enabled && g_config.tilt_adjusts && ShouldTilt(target.type)) {
        ApplyTilt(transaction, surface_control_ref, "adjust", target.camera_scale);
    }
    return result;
}

//-----------------------------------------------------------------------------
// Installation
//-----------------------------------------------------------------------------

const NativeAPIEntries* g_api = nullptr;
std::atomic<bool> g_installed{false};

bool InstallInlineHook(void* target, void* replacement, void** backup, const char* label) {
    if (g_api == nullptr || g_api->hookFunc == nullptr) return false;
    if (target == nullptr || replacement == nullptr || backup == nullptr) return false;
    *backup = nullptr;
    const int status = g_api->hookFunc(target, replacement, backup);
    if (status != 0 || *backup == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "hook failed for %s (status=%d)", label,
                            status);
        if (status == 0 && *backup == nullptr && g_api->unhookFunc != nullptr) {
            g_api->unhookFunc(target);
        }
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "hooked %s", label);
    return true;
}

void WriteDefaultConfigIfMissing() {
    struct stat info {};
    if (stat(kConfigPrimary, &info) == 0) return;
    FILE* file = fopen(kConfigPrimary, "we");
    if (file == nullptr) {
        // The launcher runs as the app UID and normally cannot create files under
        // /data/local/tmp; the fallback path in /data/adb is only writable by root,
        // so defaults simply stay in the compiled-in Config.
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "cannot create %s (%s); using built-in defaults", kConfigPrimary,
                            strerror(errno));
        return;
    }
    const Config defaults;
    fprintf(file,
            "# HyperHomeAnimation tunables. Rotations are degrees; RotateState stores\n"
            "# radians internally. Re-read at most every 250 ms.\n"
            "enabled=%d\n"
            "x_deg=%.2f\n"
            "y_deg=%.2f\n"
            "z_deg=%.2f\n"
            "center_x=%.2f\n"
            "center_y=%.2f\n"
            "offset_x=%.2f\n"
            "offset_y=%.2f\n"
            "offset_z=%.2f\n"
            "camera_degrees=%.2f\n"
            "camera_distance=%.2f\n"
            "tilt_adjusts=%d\n"
            "tilt_app_type=%d\n"
            "tilt_icon_type=%d\n"
            "normalize_icon_perspective=%d\n"
            "verbose=%d\n",
            defaults.enabled ? 1 : 0, defaults.rot_x_deg, defaults.rot_y_deg,
            defaults.rot_z_deg, defaults.center_x, defaults.center_y, defaults.offset_x,
            defaults.offset_y, defaults.offset_z, defaults.camera_degrees,
            defaults.camera_distance, defaults.tilt_adjusts ? 1 : 0,
            defaults.tilt_app_type ? 1 : 0, defaults.tilt_icon_type ? 1 : 0,
            defaults.normalize_icon_perspective ? 1 : 0, defaults.verbose ? 1 : 0);
    fclose(file);
    chmod(kConfigPrimary, 0666);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "wrote default config to %s",
                        kConfigPrimary);
}

void InstallHooks(void* handle) {
    if (g_api == nullptr || g_api->hookFunc == nullptr) return;
    if (g_installed.exchange(true, std::memory_order_acq_rel)) return;

    void* start = dlsym(handle, kSymStart);
    void* adjust = dlsym(handle, kSymAdjust);
    void* set_rotation = dlsym(handle, kSymSetRotation);
    void* get_layer_id = dlsym(handle, kSymGetLayerId);

    if (set_rotation == nullptr || start == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "required libgui symbols missing (start=%p rotate=%p); this ROM "
                            "cannot be patched by this build",
                            start, set_rotation);
        return;
    }
    g_set_surface_rotation = reinterpret_cast<SetSurfaceRotationFn>(set_rotation);
    g_get_layer_id = reinterpret_cast<GetLayerIdFn>(get_layer_id);

    WriteDefaultConfigIfMissing();
    g_config_stamp.store(0, std::memory_order_relaxed);
    RefreshConfig();

    InstallInlineHook(start, reinterpret_cast<void*>(&HookStartSFAnimation),
                      reinterpret_cast<void**>(&g_original_start),
                      "Transaction::startSFAnimation");
    if (adjust != nullptr) {
        InstallInlineHook(adjust, reinterpret_cast<void*>(&HookAdjustSFAnimation),
                          reinterpret_cast<void**>(&g_original_adjust),
                          "Transaction::adjustSFAnimation");
    }
}

void OnLibraryLoaded(const char* name, void* handle) {
    if (name == nullptr || handle == nullptr) return;
    if (strstr(name, "libgui.so") != nullptr) InstallHooks(handle);
}

void BackfillLoadedLibrary(const char* name) {
    if (name == nullptr) return;
    void* handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) return;
    OnLibraryLoaded(name, handle);
    dlclose(handle);
}

}  // namespace

extern "C" __attribute__((visibility("default"), unused))
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries) {
    if (entries == nullptr || entries->hookFunc == nullptr ||
            entries->unhookFunc == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "native_init rejected: bad entries");
        return nullptr;
    }
    g_api = entries;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "native_init api=%u pid=%d uid=%d",
                        entries->version, getpid(), getuid());
    // libgui is usually already mapped by the time the module is injected here, so
    // the on-library-loaded callback alone would miss it.
    BackfillLoadedLibrary("libgui.so");
    return &OnLibraryLoaded;
}
