// SPDX-License-Identifier: Apache-2.0
// Real VRI content-layer renderer for the verified HyperOS 4 / Android 17 ROM.
#include "hha_genie.h"

#include <android/api-level.h>
#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr char kTag[] = "HHA.Content";
constexpr char kFingerprint[] =
    "Xiaomi/hongkong/hongkong:17/CP2A.260605.016/"
    "17OS4.0.261009.103256994.QCPECN.S:user/release-keys";
constexpr char kRegister[] =
    "_ZN7android21SurfaceComposerClient14registerShaderERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_";
constexpr char kPost[] =
    "_ZN7android21SurfaceComposerClient11Transaction14setPostProcessERKNS_2spINS_14SurfaceControlEEERKNS2_INS_7IBinderEEERKNSt3__110shared_ptrINSB_6vectorIhNSB_9allocatorIhEEEEEENS_13layer_state_t12SampleTargetE";
constexpr char kRotate[] =
    "_ZN7android21SurfaceComposerClient11Transaction18setSurfaceRotationERKNS_2spINS_14SurfaceControlEEEffffffffff";

using StrongFn = void (*)(const void*, const void*);
using PostFn = void* (*)(void*, const void*, const void*, const void*, int32_t);
using RotateFn = void* (*)(void*, const void*, float, float, float, float, float,
                           float, float, float, float, float);
using LayerIdFn = int32_t (*)(const void*);
StrongFn g_inc_strong = nullptr;
StrongFn g_dec_strong = nullptr;
PostFn g_post = nullptr;
RotateFn g_rotate = nullptr;
LayerIdFn g_layer_id = nullptr;

// Nontrivial destructor preserves registerShader's AAPCS64 x8 return ABI.
// IBinder virtually inherits RefBase: the offset is vtable[-3], not a virtual
// destructor slot. SurfaceControl itself uses its direct RefBase address.
struct BinderResult {
    void* ptr = nullptr;
    ~BinderResult() {
        if (ptr == nullptr || g_dec_strong == nullptr) return;
        const auto* table = *static_cast<const char* const*>(ptr);
        intptr_t offset = 0;
        std::memcpy(&offset, table - 3 * sizeof(void*), sizeof(offset));
        g_dec_strong(static_cast<char*>(ptr) + offset, this);
    }
};
using RegisterFn = BinderResult (*)(const std::string&, const std::string&);

// Nine destination/source vertices, triangulated as four cells. All values are
// normalized; unlike a row-only inverse this also supports lateral displacement.
// Uniform bytes: nine float4s followed by extent=(width,height,0,0).
constexpr char kShader[] = R"SKSL(
uniform shader inLayer;
uniform float4 mesh[9];
uniform float4 extent;
float3 weights(float2 p, float2 a, float2 b, float2 c) {
    float2 e = b-a, f = c-a, d = p-a;
    float determinant = e.x*f.y-e.y*f.x;
    if (abs(determinant)<1e-8) return float3(-1);
    float v = (d.x*f.y-d.y*f.x)/determinant;
    float w = (e.x*d.y-e.y*d.x)/determinant;
    return float3(1-v-w,v,w);
}
bool cell(float2 p, float4 a, float4 b, float4 c, float4 d, out float2 uv) {
    float3 w = weights(p,a.xy,b.xy,d.xy);
    if (min(min(w.x,w.y),w.z) >= -0.00001) {
        uv = a.zw*w.x+b.zw*w.y+d.zw*w.z;
        return true;
    }
    w = weights(p,a.xy,d.xy,c.xy);
    if (min(min(w.x,w.y),w.z) >= -0.00001) {
        uv = a.zw*w.x+d.zw*w.y+c.zw*w.z;
        return true;
    }
    return false;
}
half4 main(float2 pixel) {
    float2 p = pixel/extent.xy;
    float2 uv = float2(0);
    // This ROM compiles runtime effects without dynamic uniform-array indexing.
    if (cell(p,mesh[0],mesh[1],mesh[3],mesh[4],uv) ||
        cell(p,mesh[1],mesh[2],mesh[4],mesh[5],uv) ||
        cell(p,mesh[3],mesh[4],mesh[6],mesh[7],uv) ||
        cell(p,mesh[4],mesh[5],mesh[7],mesh[8],uv)) {
        return inLayer.eval(clamp(uv,float2(0),float2(1))*extent.xy);
    }
    return half4(0);
}
)SKSL";

int64_t NowNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

struct Session {
    void* surface = nullptr;
    int32_t layer = -1;
    int width = 0;
    int height = 0;
    int64_t started_ns = 0;
    double elapsed = 0;
    uint64_t frames = 0;
    bool attached = false;
    hha::genie::Animation animation;

    ~Session() {
        if (surface != nullptr) g_dec_strong(surface, this);
    }
};

struct Controller {
    std::mutex mutex;
    std::unique_ptr<Session> pending;
    bool command = false;
    ALooper* looper = nullptr;  // Owned by the persistent rendering thread.
    AChoreographer* choreographer = nullptr;
    bool callback_pending = false;  // Rendering-thread fields below.
    std::unique_ptr<Session> active;
    void* shader = nullptr;  // One process-lifetime registration, reused per open.
};
std::mutex g_initialize_mutex;
std::atomic<Controller*> g_controller{nullptr};

bool Submit(Controller& controller, Session& session, bool detach) {
    ASurfaceTransaction* transaction = ASurfaceTransaction_create();
    if (transaction == nullptr) return false;
    if (detach) {
        // A null shader clears ePostProcessChanged's token and uniforms. Identity
        // alone would leave the effect attached and can keep this layer CLIENT.
        void* empty = nullptr;
        std::shared_ptr<std::vector<unsigned char>> uniforms;
        g_post(transaction, &session.surface, &empty, &uniforms, 0);
        g_rotate(transaction, &session.surface, 0, 0, 0, 0, 0, 0, 0, 0, 45, 1600);
    } else {
        const auto vertices = session.animation.Mesh();
        double left = vertices[0].x, right = left;
        double top = vertices[0].y, bottom = top;
        for (const auto& p : vertices) {
            left = std::min(left, p.x);
            right = std::max(right, p.x);
            top = std::min(top, p.y);
            bottom = std::max(bottom, p.y);
        }
        if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) ||
            !std::isfinite(bottom) || right-left < 1e-6 || bottom-top < 1e-6) {
            ASurfaceTransaction_delete(transaction);
            return false;
        }
        // The stock parent currently owns overall position/scale. Remove that
        // affine component from the local model while preserving row timing and
        // deformation. This is deliberately not the final iOS parent trajectory.
        std::array<float, 40> values{};
        for (size_t i = 0; i < vertices.size(); ++i) {
            values[i*4] = static_cast<float>((vertices[i].x-left)/(right-left));
            values[i*4+1] = static_cast<float>((vertices[i].y-top)/(bottom-top));
            values[i*4+2] = static_cast<float>(i%3)/2;
            values[i*4+3] = static_cast<float>(i/3)/2;
        }
        values[36] = static_cast<float>(session.width);
        values[37] = static_cast<float>(session.height);
        auto uniforms = std::make_shared<std::vector<unsigned char>>(sizeof(values));
        std::memcpy(uniforms->data(), values.data(), sizeof(values));
        g_post(transaction, &session.surface, &controller.shader, &uniforms, 0);
        g_rotate(transaction, &session.surface, 0, 0, 1e-6f, 0, 0, 0, 0, 0, 45, 1600);
        session.attached = true;
        if (session.frames == 2) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "mesh layer=%d elapsed=%.4f rowSpans=%.5f/%.5f/%.5f", session.layer,
                session.elapsed, values[8]-values[0], values[20]-values[12],
                values[32]-values[24]);
        }
    }
    ASurfaceTransaction_apply(transaction);
    ASurfaceTransaction_delete(transaction);
    return true;
}

void Finish(Controller& controller, const char* reason) {
    if (!controller.active) return;
    auto& session = *controller.active;
    const bool detached = !session.attached || Submit(controller, session, true);
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "finish layer=%d reason=%s callbacks=%llu elapsed=%.4f detachSubmitted=%d",
        session.layer, reason, static_cast<unsigned long long>(session.frames),
        session.elapsed, detached);
    controller.active.reset();
}

void ConsumeCommand(Controller& controller) {
    std::unique_ptr<Session> next;
    {
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (!controller.command) return;
        next = std::move(controller.pending);
        controller.command = false;
    }
    Finish(controller, next ? "replaced" : "cancelled");
    controller.active = std::move(next);
    if (controller.active) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "start layer=%d extent=%dx%d",
            controller.active->layer, controller.active->width, controller.active->height);
    }
}

bool ParseSwitch(const char* value, bool fallback) {
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0) return true;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0) return false;
    return fallback;
}

bool ContentEnabled() {
    FILE* file = std::fopen("/data/local/tmp/hha.conf", "re");
    if (file == nullptr) {
        file = std::fopen("/data/user/0/com.android.settings/files/hha-content.conf", "re");
    }
    if (file == nullptr) return false;
    bool enabled = false, content = false;
    char line[256], value[16];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (char* comment = std::strchr(line, '#')) *comment = '\0';
        if (std::sscanf(line, " enabled = %15s", value) == 1) {
            enabled = ParseSwitch(value, enabled);
        } else if (std::sscanf(line, " content_enabled = %15s", value) == 1) {
            content = ParseSwitch(value, content);
        }
    }
    std::fclose(file);
    return enabled && content;
}

void Frame(int64_t frame_time, void* context) {
    auto& controller = *static_cast<Controller*>(context);
    controller.callback_pending = false;
    ConsumeCommand(controller);
    if (!controller.active) return;
    auto& session = *controller.active;
    if (session.frames % 4 == 0 && !ContentEnabled()) {
        Finish(controller, "disabled");
        return;
    }
    const double elapsed = std::max(0.0, static_cast<double>(frame_time-session.started_ns)/1e9);
    // Advance by actual display time, including missed frames. The analytic
    // solver does not accumulate a variable-Euler-step integration error.
    session.animation.Advance(std::max(0.0, elapsed-session.elapsed));
    session.elapsed = elapsed;
    if (!Submit(controller, session, false)) {
        Finish(controller, "submit-failed");
        return;
    }
    ++session.frames;
    if (session.animation.Settled()) Finish(controller, "settled");
    else if (elapsed >= 1.5) Finish(controller, "deadline");
}

void Worker(Controller* controller) {
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    AChoreographer* choreographer = AChoreographer_getInstance();
    if (looper == nullptr || choreographer == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native display-event loop unavailable");
        std::lock_guard<std::mutex> lock(controller->mutex);
        controller->pending.reset();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(controller->mutex);
        controller->looper = looper;
        controller->choreographer = choreographer;
    }
    for (;;) {
        ConsumeCommand(*controller);
        if (controller->active && !controller->callback_pending) {
            controller->callback_pending = true;
            AChoreographer_postFrameCallback64(choreographer, Frame, controller);
        }
        ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
    }
}

bool Initialize() {
    std::lock_guard<std::mutex> lock(g_initialize_mutex);
    if (g_controller.load(std::memory_order_acquire) != nullptr) return true;
    std::string fingerprint;
    const prop_info* property = __system_property_find("ro.build.fingerprint");
    if (property != nullptr) {
        __system_property_read_callback(property,
            [](void* context, const char*, const char* value, uint32_t) {
                *static_cast<std::string*>(context) = value;
            }, &fingerprint);
    }
    if (android_get_device_api_level() != 37 || fingerprint != kFingerprint) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "unverified ROM; content ABI disabled: %s", fingerprint.c_str());
        return false;
    }
    void* gui = dlopen("libgui.so", RTLD_NOW | RTLD_LOCAL);
    void* utils = dlopen("libutils.so", RTLD_NOW | RTLD_LOCAL);
    // A module ClassLoader has an isolated linker namespace. The public NDK
    // libandroid handle also searches its already loaded platform dependencies,
    // including the exact libgui/libutils that own VRI SurfaceControl objects.
    void* platform = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (gui == nullptr) gui = platform;
    if (utils == nullptr) utils = platform;
    if (gui == nullptr || utils == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "platform libraries unavailable: %s", dlerror());
        return false;
    }
    auto reg = reinterpret_cast<RegisterFn>(dlsym(gui, kRegister));
    g_post = reinterpret_cast<PostFn>(dlsym(gui, kPost));
    g_rotate = reinterpret_cast<RotateFn>(dlsym(gui, kRotate));
    g_layer_id = reinterpret_cast<LayerIdFn>(dlsym(gui, "_ZNK7android14SurfaceControl10getLayerIdEv"));
    g_inc_strong = reinterpret_cast<StrongFn>(dlsym(utils, "_ZNK7android7RefBase9incStrongEPKv"));
    g_dec_strong = reinterpret_cast<StrongFn>(dlsym(utils, "_ZNK7android7RefBase9decStrongEPKv"));
    if (!reg || !g_post || !g_rotate || !g_layer_id || !g_inc_strong || !g_dec_strong) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "required ROM symbols missing; content animation disabled");
        return false;
    }
    BinderResult token = reg(std::string("hha_content_genie"), std::string(kShader));
    if (token.ptr == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "SELF shader registration rejected");
        return false;
    }
    // The controller and one display-event thread live for the process. Pin this
    // library so libxposed hot reload cannot unload code under the worker thread.
    Dl_info own{};
    if (dladdr(reinterpret_cast<const void*>(&Initialize), &own) == 0 ||
        own.dli_fname == nullptr || dlopen(own.dli_fname, RTLD_NOW | RTLD_NODELETE) == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "cannot retain content renderer library");
        return false;
    }
    auto* controller = new Controller;
    controller->shader = token.ptr;
    token.ptr = nullptr;
    g_controller.store(controller, std::memory_order_release);
    std::thread(Worker, controller).detach();
    __android_log_print(ANDROID_LOG_INFO, kTag, "SELF renderer ready pid=%d", getpid());
    return true;
}
}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeInitialize(JNIEnv*, jclass) {
    return Initialize() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeStart(JNIEnv*, jclass, jlong surface, jint width,
                                          jint height, jfloat amount, jfloat scale) {
    // Callers hold SurfaceControl.mLock until this method has retained its own
    // RefBase reference. No Java pointer is stored without that ownership.
    auto* target = g_controller.load(std::memory_order_acquire);
    if (target == nullptr || surface == 0 || width <= 0 || height <= 0 ||
        width > 16384 || height > 16384 || !std::isfinite(amount) ||
        !std::isfinite(scale) || std::abs(amount) > 1 || scale <= 0 || scale >= 1) return JNI_FALSE;
    auto session = std::make_unique<Session>();
    hha::genie::Parameters parameters;
    parameters.width = width;
    parameters.height = height;
    parameters.minimized_width = width;
    parameters.minimized_height = height;
    parameters.window_x = parameters.icon_x = width/2.0;
    parameters.window_y = parameters.icon_y = height/2.0;
    parameters.genie_amount_y = amount;
    parameters.genie_scale = scale;
    if (!session->animation.Reset(parameters, true) || !session->animation.Retarget(false)) return JNI_FALSE;
    session->surface = reinterpret_cast<void*>(static_cast<uintptr_t>(surface));
    g_inc_strong(session->surface, session.get());
    session->layer = g_layer_id(session->surface);
    session->width = width;
    session->height = height;
    session->started_ns = NowNs();
    auto& controller = *target;
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.pending = std::move(session);
    controller.command = true;
    if (controller.looper != nullptr) ALooper_wake(controller.looper);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_ng_lst_hha_ContentAnimator_nativeStop(JNIEnv*, jclass) {
    auto* target = g_controller.load(std::memory_order_acquire);
    if (target == nullptr) return;
    auto& controller = *target;
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.pending.reset();
    controller.command = true;
    if (controller.looper != nullptr) ALooper_wake(controller.looper);
}
