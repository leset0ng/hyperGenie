// SPDX-License-Identifier: Apache-2.0
// Real content-layer renderer owned by system_server on the verified ROM.
#include "hha_genie.h"
#include "hha_viewport.h"
#include "hha_renderer.h"

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
#include <cstddef>
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
constexpr char kBorder[] =
    "_ZN7android21SurfaceComposerClient11Transaction17setBorderSettingsERKNS_2spINS_14SurfaceControlEEENS_3gui14BorderSettingsE";
// Both snapshot paths preserve CLIENT for a positive border width, even with
// fully transparent color. Keep standard shadows at their original zero: a
// positive shadow radius draws the unwarped host and darkens the mesh exterior.
constexpr float kClientBorderWidth = 1e-6f;
// The verified nontrivial by-value setter receives an indirect argument and
// only copies width/color at +8. It never reads the vptr or destroys this view.
struct BorderSettingsAbi {
    void* vptr = nullptr;
    float width = 0;
    uint32_t color = 0;
};
static_assert(sizeof(BorderSettingsAbi) == 16);
static_assert(offsetof(BorderSettingsAbi, width) == 8);
static_assert(offsetof(BorderSettingsAbi, color) == 12);

using StrongFn = void (*)(const void*, const void*);
using PostFn = void* (*)(void*, const void*, const void*, const void*, int32_t);
using BorderFn = void* (*)(void*, const void*, const BorderSettingsAbi*);
using LayerIdFn = int32_t (*)(const void*);
StrongFn g_inc_strong = nullptr;
StrongFn g_dec_strong = nullptr;
PostFn g_post = nullptr;
BorderFn g_border = nullptr;
LayerIdFn g_layer_id = nullptr;
using CommitFn = void (*)(ASurfaceTransaction*,void*,ASurfaceTransaction_OnCommit);
CommitFn g_on_commit = nullptr;
using CompleteFn = void (*)(ASurfaceTransaction*,void*,ASurfaceTransaction_OnComplete);
CompleteFn g_on_complete = ASurfaceTransaction_setOnComplete;

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
// Uniform bytes: nine float4s, virtual extent/corner metrics, then the valid
// source footprint. SF's icon host extends its draw crop beyond the square
// texture; that padded virtual region must not be squeezed into the final icon.
constexpr char kShader[] = R"SKSL(
uniform shader inLayer;
uniform float4 mesh[9];
uniform float4 extent;
uniform float4 sourceExtent;
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
        float radius=extent.z;
        float2 metric=float2(1,extent.w);
        float2 edge=abs((uv-float2(0.5))*metric)-(metric*0.5-radius);
        float distance=length(max(edge,float2(0)))+min(max(edge.x,edge.y),0)-radius;
        float coverage=clamp(0.5-distance*min(extent.x,extent.y),0.0,1.0);
        return inLayer.eval(clamp(uv,float2(0),float2(1))*sourceExtent.xy)*coverage;
    }
    return half4(0);
}
)SKSL";

int64_t NowNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

struct Companion {
    void* surface=nullptr;
    int32_t layer=-1;
};

struct Session {
    void* surface = nullptr;
    void* parent = nullptr;
    std::vector<Companion> companions;
    int32_t layer = -1;
    int32_t parent_layer = -1;
    int width = 0;
    int height = 0;
    int texture_width = 0;
    int texture_height = 0;
    int64_t started_ns = 0;
    int64_t request_ns = 0;  // Submission identity, independent of the preserved motion clock.
    int64_t last_ns = 0;
    double elapsed = 0;
    uint64_t frames = 0;
    uint64_t generation = 1;
    bool attached = false;
    bool primed_continuation = false;
    bool closing = false;
    bool starting = false;
    hha::genie::Parameters parameters;
    hha::genie::Animation animation;
    hha::genie::Scalar corner;
    std::shared_ptr<std::atomic<bool>> committed;

    ~Session() {
        if (surface != nullptr) g_dec_strong(surface, this);
        if (parent != nullptr) g_dec_strong(parent, this);
        for(const auto& companion:companions) g_dec_strong(companion.surface,this);
    }
};

struct Controller {
    std::mutex mutex;
    std::unique_ptr<Session> pending;
    std::vector<std::unique_ptr<Session>> retiring;
    bool command = false;
    hha::RenderCancellation cancellation;
    std::atomic<bool> has_active{false};
    std::atomic<bool> working{false};
    std::atomic<uint64_t> submitted_serial{0};
    std::atomic<uint64_t> cleared_serial{0};
    std::atomic<uint64_t> detaches_pending{0};
    ALooper* looper = nullptr;  // Owned by the persistent rendering thread.
    AChoreographer* choreographer = nullptr;
    bool callback_pending = false;  // Rendering-thread fields below.
    std::unique_ptr<Session> active;
    void* shader = nullptr;  // One process-lifetime registration, reused per open.
};
std::mutex g_initialize_mutex;
std::atomic<Controller*> g_controller{nullptr};

void ClearSurface(ASurfaceTransaction* transaction, void* surface) {
    if(!surface) return;
    void* empty=nullptr;
    std::shared_ptr<std::vector<unsigned char>> uniforms;
    g_post(transaction,&surface,&empty,&uniforms,0);
    const BorderSettingsAbi border{};
    g_border(transaction,&surface,&border);
}

bool Submit(Controller& controller, Session& session, bool detach,
            ASurfaceTransaction* supplied = nullptr, const Session* preserve = nullptr) {
    ASurfaceTransaction* transaction = supplied ? supplied : ASurfaceTransaction_create();
    if (transaction == nullptr) return false;
    if (detach) {
        // A null shader clears ePostProcessChanged's token and uniforms. Identity
        // alone would leave the effect attached and can keep this layer CLIENT.
        std::vector<int32_t> kept;
        if(preserve) for(const auto& companion:preserve->companions) kept.push_back(companion.layer);
        unsigned cleared=0;
        auto clear = [&](void* surface, int32_t layer) {
            if(surface && (!preserve || !hha::RetainsLayer(layer,preserve->layer,kept))) {
                ClearSurface(transaction,surface);
                ++cleared;
            }
        };
        clear(session.surface,session.layer);
        for(const auto& companion:session.companions) clear(companion.surface,companion.layer);
        if(!cleared) {
            if(!supplied) ASurfaceTransaction_delete(transaction);
            return true;
        }
        // A serial watermark alone cannot cover multiple independent detaches:
        // every submitted retirement stays outstanding until its own completion.
        struct Completion {Controller* controller;uint64_t serial;bool complete_detach;};
        auto* completion=new Completion{&controller,controller.submitted_serial.load(std::memory_order_acquire),!preserve};
        controller.detaches_pending.fetch_add(1,std::memory_order_release);
        g_on_complete(transaction,completion,
            [](void* context,ASurfaceTransactionStats*) {
                std::unique_ptr<Completion> completion(static_cast<Completion*>(context));
                if(completion->complete_detach) {
                    auto& cleared=completion->controller->cleared_serial;
                    uint64_t old=cleared.load();
                    while(old<completion->serial && !cleared.compare_exchange_weak(old,completion->serial)) {}
                }
                completion->controller->detaches_pending.fetch_sub(1,std::memory_order_release);
            });
    } else {
        const auto vertices=hha::genie::ContourMesh(session.animation,session.parameters);
        std::array<float, 44> values{};
        for (size_t i = 0; i < vertices.size(); ++i) {
            values[i*4] = static_cast<float>(vertices[i].x);
            values[i*4+1] = static_cast<float>(vertices[i].y);
            values[i*4+2] = static_cast<float>(i%3)/2;
            values[i*4+3] = static_cast<float>(i/3)/2;
        }
        values[36] = static_cast<float>(session.texture_width?session.texture_width:session.width);
        values[37] = session.texture_width?values[36]*session.height/session.width:static_cast<float>(session.height);
        // The icon's extra virtual height is a crop/CLAMP extension, not more
        // glyph pixels. Sample only its square source footprint; otherwise the
        // final square silhouette compresses the glyph into its upper half.
        values[40]=static_cast<float>(session.texture_width?session.texture_width:session.width);
        values[41]=static_cast<float>(session.texture_height?session.texture_height:session.height);
        const double visible_width=std::max(1e-6,(vertices[2].x-vertices[0].x)*session.width);
        const double visible_height=std::max(1e-6,(vertices[6].y-vertices[0].y)*session.height);
        values[39]=static_cast<float>(visible_height/visible_width);
        // Both round-rect metric axes use the current visible width as one
        // unit. Endpoint ratios come from the actual icon/device radii, while
        // the independent spring preserves its value and velocity on reversal.
        values[38]=static_cast<float>(std::clamp(session.corner.value,0.0,
            0.5*std::min(1.0,visible_height/visible_width)));
        auto uniforms = std::make_shared<std::vector<unsigned char>>(sizeof(values));
        std::memcpy(uniforms->data(), values.data(), sizeof(values));
        std::vector<void*> surfaces{session.surface};
        for(const auto& companion:session.companions) surfaces.push_back(companion.surface);
        for(void* surface:surfaces) {
            if(!surface) continue;
            g_post(transaction,&surface,&controller.shader,&uniforms,0);
            const BorderSettingsAbi border{nullptr,kClientBorderWidth,0};
            g_border(transaction,&surface,&border);
        }
        session.attached = true;
        controller.submitted_serial.fetch_add(1,std::memory_order_acq_rel);
        if (session.frames == 2) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "mesh layer=%d elapsed=%.4f rowSpans=%.5f/%.5f/%.5f", session.layer,
                session.elapsed, values[8]-values[0], values[20]-values[12],
                values[32]-values[24]);
        }
    }
    if(!supplied) {
        ASurfaceTransaction_apply(transaction);
        ASurfaceTransaction_delete(transaction);
    }
    return true;
}

void Finish(Controller& controller, const char* reason, const Session* successor = nullptr) {
    if (!controller.active) return;
    auto& session = *controller.active;
    const bool detached = !session.attached || Submit(controller, session, true, nullptr, successor);
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "finish layer=%d request=%lld reason=%s callbacks=%llu elapsed=%.4f detachSubmitted=%d retained=%d",
        session.layer,static_cast<long long>(session.request_ns),reason,
        static_cast<unsigned long long>(session.frames),session.elapsed,detached,
        successor && successor->layer==session.layer);
    controller.active.reset();
    controller.has_active.store(false,std::memory_order_release);
}

void DetachLostSurfaces(const Session& previous,const Session& next) {
    if(!previous.attached) return;
    ASurfaceTransaction* transaction=ASurfaceTransaction_create();
    if(!transaction) return;
    std::vector<Companion> surfaces{{previous.surface,previous.layer}};
    surfaces.insert(surfaces.end(),previous.companions.begin(),previous.companions.end());
    std::vector<int32_t> kept;
    for(const auto& companion:next.companions) kept.push_back(companion.layer);
    for(const auto& item:surfaces) {
        if(item.layer>=0 && !hha::RetainsLayer(item.layer,next.layer,kept))
            ClearSurface(transaction,item.surface);
    }
    ASurfaceTransaction_apply(transaction);
    ASurfaceTransaction_delete(transaction);
}

// A primed external transaction can commit after its pending command was
// superseded or stopped. Retain that session until the worker can retire it.
void RetirePending(Controller& controller) {
    if(controller.pending && controller.pending->attached)
        controller.retiring.push_back(std::move(controller.pending));
    else controller.pending.reset();
}

void RetireCommitted(Controller& controller) {
    // The active session alone is a sufficient preserve set after adoption.
    // Wait while an incoming initial transaction still owns a pending handoff.
    if(controller.pending) return;
    for(auto it=controller.retiring.begin();it!=controller.retiring.end();) {
        auto& retired=**it;
        if(retired.committed && !retired.committed->load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        if(!Submit(controller,retired,true,nullptr,controller.active.get())) {
            ++it;
            continue;
        }
        __android_log_print(ANDROID_LOG_INFO,kTag,"retire pending layer=%d request=%lld",
            retired.layer,static_cast<long long>(retired.request_ns));
        it=controller.retiring.erase(it);
    }
}

// Removing an OEM preview from its registry does not remove its visible SF
// layer. Keep each verified opening capability under the same mesh until this
// request finishes; otherwise a still-opaque preview becomes a full rectangle
// behind the animated App. The immutable request, parent and extent bound reuse.
void PreserveOpeningPreviews(Session& next, const Session& previous) {
    if(next.closing || previous.closing || next.request_ns!=previous.request_ns ||
       next.parent_layer!=previous.parent_layer || next.width!=previous.width ||
       next.height!=previous.height) return;
    auto retain=[&](void* surface,int32_t layer) {
        if(!surface || layer<0 || layer==next.layer ||
           std::any_of(next.companions.begin(),next.companions.end(),
               [&](const Companion& item) {return item.layer==layer;})) return;
        next.companions.push_back({surface,layer});
        g_inc_strong(surface,&next);
    };
    for(const auto& item:previous.companions) retain(item.surface,item.layer);
    if(previous.starting) retain(previous.surface,previous.layer);
    std::sort(next.companions.begin(),next.companions.end(),
        [](const Companion& a,const Companion& b) {return a.layer<b.layer;});
}

// Caller holds controller.mutex throughout command adoption and all SF writes.
void ConsumeCommand(Controller& controller) {
    if (!controller.command) return;
    if (controller.pending && controller.pending->committed &&
        !controller.pending->committed->load(std::memory_order_acquire)) return;
    controller.working.store(true,std::memory_order_release);
    auto next = std::move(controller.pending);
    controller.command = false;
    struct Working {
        Controller& controller;
        ~Working() {controller.working.store(false,std::memory_order_release);}
    } working{controller};
    // A primed icon transaction already owns a visible effect, even if commit
    // delivery was late. Adopt it so the normal frame/deadline path detaches it.
    if(next && !controller.active && !next->attached &&
       NowNs()-next->started_ns>500000000LL) return;
    if(next && controller.active) PreserveOpeningPreviews(*next,*controller.active);
    if (next && controller.active && next->layer==controller.active->layer &&
        next->width==controller.active->width && next->height==controller.active->height) {
        auto& current=*controller.active;
        if (next->request_ns<current.request_ns) return;
        current.request_ns=next->request_ns;
        if(next->primed_continuation) {
            // Adopt the exact state already published with the parent viewport.
            // Replaying a fresh endpoint for one transaction causes a flash
            // before this normal same-layer continuation can retarget it.
            current.animation=next->animation;
            current.parameters=next->parameters;
            current.corner=next->corner;
            current.closing=next->closing;
            current.started_ns=next->started_ns;
            current.last_ns=next->last_ns;
            current.elapsed=next->elapsed;
            current.generation=next->generation;
        }
        const bool companions_changed=current.companions.size()!=next->companions.size() ||
            !std::equal(current.companions.begin(),current.companions.end(),next->companions.begin(),
                [](const Companion& a,const Companion& b) {return a.layer==b.layer;});
        if(companions_changed) {
            DetachLostSurfaces(current,*next);
            for(const auto& companion:current.companions) g_dec_strong(companion.surface,&current);
            current.companions=next->companions;
            for(const auto& companion:current.companions) g_inc_strong(companion.surface,&current);
            // Cover newly discovered buffers immediately, before another SF
            // frame can draw them without the shared contour.
            Submit(controller,current,false);
        }
        if (next->parent_layer!=current.parent_layer) {
            if (current.parent) g_dec_strong(current.parent,&current);
            current.parent=next->parent;
            current.parent_layer=next->parent_layer;
            g_inc_strong(current.parent,&current);
        }
        if (current.closing!=next->closing) {
            if (next->started_ns>current.last_ns) {
                const double delta=static_cast<double>(next->started_ns-current.last_ns)/1e9;
                current.animation.Advance(delta);
                hha::genie::AdvanceScalar(&current.corner,delta,1.0);
                current.last_ns=next->started_ns;
            }
            // Reuse the same geometry basis on a reversal. Only dynamics and
            // endpoint change: all 19 positions and velocities remain intact.
            auto parameters=current.parameters;
            parameters.response=next->parameters.response;
            parameters.damping_ratio=next->parameters.damping_ratio;
            current.animation.Retarget(parameters,next->closing);
            current.parameters=parameters;
            current.closing=next->closing;
            current.corner.target=next->corner.target;
            current.corner.response=next->corner.response;
            current.started_ns=next->started_ns;
            current.elapsed=0;
            ++current.generation;
            __android_log_print(ANDROID_LOG_INFO,kTag,
                "retarget layer=%d parent=%d closing=%d generation=%llu",current.layer,
                current.parent_layer,current.closing,static_cast<unsigned long long>(current.generation));
        }
        return;
    }
    if(next && controller.active &&
       next->parent_layer==controller.active->parent_layer &&
       next->width==controller.active->width && next->height==controller.active->height &&
       (next->starting || controller.active->starting)) {
        const auto& previous=*controller.active;
        const auto requested=next->parameters;
        next->animation=previous.animation;
        const auto requested_corner=next->corner;
        next->corner=previous.corner;
        next->corner.target=requested_corner.target;
        next->corner.response=requested_corner.response;
        next->parameters=previous.parameters;
        next->last_ns=previous.last_ns;
        next->started_ns=previous.started_ns;
        next->elapsed=previous.elapsed;
        next->frames=previous.frames;
        next->generation=previous.generation+1;
        next->attached=previous.attached;
        if(next->closing!=previous.closing) {
            next->parameters.response=requested.response;
            next->parameters.damping_ratio=requested.damping_ratio;
            next->animation.Retarget(next->parameters,next->closing);
        }
        DetachLostSurfaces(previous,*next);
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "handoff old=%d content=%d companions=%zu starting=%d generation=%llu",
            previous.layer,next->layer,next->companions.size(),next->starting,
            static_cast<unsigned long long>(next->generation));
        controller.active=std::move(next);
        // The live App may already have a queued buffer. Publish its inherited
        // state during adoption rather than waiting for the next vsync callback.
        Submit(controller,*controller.active,false);
        return;
    }
    Finish(controller, next ? "replaced" : "cancelled", next.get());
    controller.active = std::move(next);
    controller.has_active.store(controller.active!=nullptr,std::memory_order_release);
    if (controller.active) {
        auto& current=*controller.active;
        const int64_t now=NowNs();
        if(now>current.last_ns) {
            const double delta=static_cast<double>(now-current.last_ns)/1e9;
            current.animation.Advance(delta);
            hha::genie::AdvanceScalar(&current.corner,delta,1.0);
            current.last_ns=now;
            current.elapsed=static_cast<double>(now-current.started_ns)/1e9;
        }
        // Prime the contour immediately; waiting for the next callback lets a
        // newly posted Splash buffer expose one unmasked frame.
        Submit(controller,current,false);
        __android_log_print(ANDROID_LOG_INFO, kTag, "start layer=%d extent=%dx%d",
            controller.active->layer, controller.active->width, controller.active->height);
    }
}

void Frame(int64_t frame_time, void* context) {
    auto& controller = *static_cast<Controller*>(context);
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.callback_pending = false;
    ConsumeCommand(controller);
    if(controller.pending && controller.pending->committed &&
       !controller.pending->committed->load(std::memory_order_acquire)) return;
    RetireCommitted(controller);
    if (!controller.active) return;
    const auto& active=*controller.active;
    if(controller.cancellation.Due({active.parent_layer,active.request_ns},NowNs())) {
        controller.cancellation.Clear();
        Finish(controller,"icon-cancelled");
        return;
    }
    auto& session = *controller.active;
    // Enable/configuration changes arrive through the authenticated bridge.
    // system_server must not read shell_data_file configuration paths.
    const int64_t timestamp=std::max(frame_time,session.last_ns);
    const double delta=static_cast<double>(timestamp-session.last_ns)/1e9;
    session.animation.Advance(delta);
    hha::genie::AdvanceScalar(&session.corner,delta,1.0);
    session.last_ns=timestamp;
    const double elapsed=std::max(0.0,static_cast<double>(timestamp-session.started_ns)/1e9);
    session.elapsed = elapsed;
    if (!Submit(controller, session, false)) {
        Finish(controller, "submit-failed");
        return;
    }
    ++session.frames;
    const bool corner_settled=std::abs(session.corner.value-session.corner.target)<1e-4 &&
        std::abs(session.corner.velocity)<1e-3;
    // A minimized shader is still the visible silhouette. Clearing it as soon
    // as springs settle exposes the rectangular backing canvas before SF has
    // removed/faded the task. Keep it until platform ownership ends.
    if (!session.closing && !session.starting && session.animation.Settled() && corner_settled)
        Finish(controller, "settled");
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
        {
            std::lock_guard<std::mutex> lock(controller->mutex);
            ConsumeCommand(*controller);
            RetireCommitted(*controller);
            if (controller->active && !controller->callback_pending) {
                controller->callback_pending = true;
                AChoreographer_postFrameCallback64(choreographer, Frame, controller);
            }
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
    g_border = reinterpret_cast<BorderFn>(dlsym(gui, kBorder));
    g_layer_id = reinterpret_cast<LayerIdFn>(dlsym(gui, "_ZNK7android14SurfaceControl10getLayerIdEv"));
    g_inc_strong = reinterpret_cast<StrongFn>(dlsym(utils, "_ZNK7android7RefBase9incStrongEPKv"));
    g_dec_strong = reinterpret_cast<StrongFn>(dlsym(utils, "_ZNK7android7RefBase9decStrongEPKv"));
    g_on_commit = reinterpret_cast<CommitFn>(dlsym(platform, "ASurfaceTransaction_setOnCommit"));
    if (!reg || !g_post || !g_border || !g_layer_id || !g_inc_strong || !g_dec_strong || !g_on_commit) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "required ROM symbols missing; content animation disabled");
        return false;
    }
    BinderResult token = reg(std::string("hha_genie_")+std::to_string(getpid()), std::string(kShader));
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

static bool QueueSession(jlong surface,jlong parent,int width,int height,
    const std::array<float,12>& values,bool closing,int64_t event_ns,const std::vector<jlong>& companions,bool starting,
    int texture_width=0,int texture_height=0,void* initial_transaction=nullptr) {
    auto* controller=g_controller.load(std::memory_order_acquire);
    if (!controller || !surface || !parent || width<=0 || height<=0 || width>16384 || height>16384) return false;
    for(float value:values) if(!std::isfinite(value)) return JNI_FALSE;
    const int64_t now=NowNs();
    if (event_ns<=0 || event_ns>now+100000000LL || now-event_ns>1500000000LL) return JNI_FALSE;
    const size_t icon=closing?4:0;
    const size_t full=closing?0:4;
    const hha::genie::Rect icon_rect{values[icon],values[icon+1],values[icon+2],values[icon+3]};
    // Current WindowState dimensions define the full content domain even when
    // a close request starts from a gesture's partially scaled presentation.
    const bool full_endpoint=std::abs(values[full+2]-width)<2 &&
                             std::abs(values[full+3]-height)<2;
    const hha::genie::Rect window_rect{
        full_endpoint?values[full]:0.0,full_endpoint?values[full+1]:0.0,
        static_cast<double>(width),static_cast<double>(height)};
    hha::genie::Parameters parameters;
    // SF SpringProperty.velocity units have not been equated to the source
    // UIKit gesture units. Do not inject an assumed velocity into the mesh.
    if(!hha::genie::ParametersForIcon(icon_rect,window_rect,&parameters)) return JNI_FALSE;
    parameters.response=closing?hha::genie::kClosingResponse:hha::genie::kOpeningResponse;
    auto next=std::make_unique<Session>();
    next->parameters=parameters;
    next->closing=closing;
    next->starting=starting;
    const hha::genie::Rect start_rect{values[0],values[1],values[2],values[3]};
    const hha::genie::Rect end_rect{values[4],values[5],values[6],values[7]};
    next->corner={hha::genie::CornerRatio(values[10],start_rect),0.0,
        hha::genie::CornerRatio(values[11],end_rect),parameters.response};
    if(!next->animation.Reset(parameters,!closing)||!next->animation.Retarget(closing)) return JNI_FALSE;
    next->surface=reinterpret_cast<void*>(static_cast<uintptr_t>(surface));
    next->parent=reinterpret_cast<void*>(static_cast<uintptr_t>(parent));
    g_inc_strong(next->surface,next.get());
    g_inc_strong(next->parent,next.get());
    next->layer=g_layer_id(next->surface);
    next->parent_layer=g_layer_id(next->parent);
    for(jlong companion:companions) {
        if(!companion) continue;
        void* overlay=reinterpret_cast<void*>(static_cast<uintptr_t>(companion));
        const int32_t layer=g_layer_id(overlay);
        if(layer!=next->layer && std::none_of(next->companions.begin(),next->companions.end(),
            [&](const Companion& existing) {return existing.layer==layer;})) {
            next->companions.push_back({overlay,layer});
            g_inc_strong(overlay,next.get());
        }
    }
    next->width=width;
    next->height=height;
    next->texture_width=texture_width;
    next->texture_height=texture_height;
    next->started_ns=next->last_ns=next->request_ns=event_ns;
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "transition content=%d parent=%d close=%d icon=(%.1f,%.1f %.1fx%.1f) I=(%.1f,%.1f) G=%.4f scale=%.4f lagMs=%.1f cornerPx=%.2f->%.2f cornerRatio=%.5f->%.5f",
        next->layer,next->parent_layer,closing,values[icon],values[icon+1],values[icon+2],values[icon+3],
        parameters.icon_x,parameters.icon_y,parameters.genie_amount_y,parameters.minimized_scale,
        static_cast<double>(now-event_ns)/1e6,values[10],values[11],next->corner.value,next->corner.target);
    // Initial transaction construction/publication and worker writes share one
    // lock. A frame cannot retire an old session after its successor is primed.
    std::lock_guard<std::mutex> lock(controller->mutex);
    if((controller->pending && event_ns<controller->pending->request_ns) ||
       (controller->active && event_ns<controller->active->request_ns)) return false;
    if(initial_transaction) {
        // Queue the first SELF state in the exact transaction that installs the
        // virtual icon host. No frame may expose the unwarped tall backing plate.
        const double delta=static_cast<double>(now-event_ns)/1e9;
        if(controller->active && controller->active->layer==next->layer &&
           controller->active->width==width && controller->active->height==height) {
            const auto& current=*controller->active;
            const auto requested_corner=next->corner;
            next->animation=current.animation;
            next->parameters=current.parameters;
            next->corner=current.corner;
            next->last_ns=current.last_ns;
            next->started_ns=current.started_ns;
            next->generation=current.generation;
            const double advance=std::max(0.0,static_cast<double>(now-current.last_ns)/1e9);
            next->animation.Advance(advance);
            hha::genie::AdvanceScalar(&next->corner,advance,1.0);
            next->corner.target=requested_corner.target;
            next->corner.response=requested_corner.response;
            if(current.closing!=next->closing) {
                next->parameters.response=closing?hha::genie::kClosingResponse:hha::genie::kOpeningResponse;
                next->animation.Retarget(next->parameters,next->closing);
                next->started_ns=event_ns;
                ++next->generation;
            }
            next->primed_continuation=true;
        } else {
            next->animation.Advance(std::max(0.0,delta));
            hha::genie::AdvanceScalar(&next->corner,std::max(0.0,delta),1.0);
        }
        next->last_ns=std::max(now,event_ns);
        next->elapsed=std::max(0.0,static_cast<double>(now-next->started_ns)/1e9);
        auto* transaction=static_cast<ASurfaceTransaction*>(initial_transaction);
        if(!Submit(*controller,*next,false,transaction)) return false;
        next->committed=std::make_shared<std::atomic<bool>>(false);
        struct Commit {Controller* controller;std::shared_ptr<std::atomic<bool>> ready;};
        auto* commit=new Commit{controller,next->committed};
        g_on_commit(transaction,commit,[](void* context,ASurfaceTransactionStats*) {
            std::unique_ptr<Commit> commit(static_cast<Commit*>(context));
            commit->ready->store(true,std::memory_order_release);
            std::lock_guard<std::mutex> lock(commit->controller->mutex);
            if(commit->controller->looper) ALooper_wake(commit->controller->looper);
        });
    }
    controller->cancellation.Clear();
    RetirePending(*controller);
    controller->pending=std::move(next);
    controller->command=true;
    if(controller->looper) ALooper_wake(controller->looper);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeTransition(JNIEnv* env,jclass,jlong surface,
    jlong parent,jint width,jint height,jfloatArray geometry,jboolean closing,
    jlong event_ns,jlongArray companion_array,jboolean starting) {
    if(!geometry || env->GetArrayLength(geometry)!=12 || !companion_array ||
       env->GetArrayLength(companion_array)>8) return JNI_FALSE;
    std::array<float,12> values{};
    std::vector<jlong> companions(env->GetArrayLength(companion_array));
    env->GetLongArrayRegion(companion_array,0,companions.size(),companions.data());
    env->GetFloatArrayRegion(geometry,0,values.size(),values.data());
    if(env->ExceptionCheck()) return JNI_FALSE;
    return QueueSession(surface,parent,width,height,values,closing,event_ns,companions,starting)?JNI_TRUE:JNI_FALSE;
}

namespace hha {
bool WarmIconRenderer() {return Initialize();}
void RetainIconSurface(void* surface,const void* owner) {if(surface && g_inc_strong) g_inc_strong(surface,owner);}
void ReleaseIconSurface(void* surface,const void* owner) {if(surface && g_dec_strong) g_dec_strong(surface,owner);}
bool RenderIcon(void* transaction,void* surface,void* parent,int texture_width,int texture_height,
                int window_width,int window_height,const Transition& transition) {
    if(texture_width<=0 || texture_height<=0 || !g_controller.load()) return false;
    return QueueSession(reinterpret_cast<uintptr_t>(surface),reinterpret_cast<uintptr_t>(parent),
        window_width,window_height,transition.geometry,transition.closing,transition.event_ns,
        {},false,texture_width,texture_height,transaction);
}
void CancelIconRenderer() {
    if(auto* controller=g_controller.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(controller->mutex);
        controller->cancellation.Clear();
        RetirePending(*controller);
        controller->command=true;
        if(controller->looper) ALooper_wake(controller->looper);
    }
}
void CancelIconRenderer(RenderIdentity identity) {
    if(auto* controller=g_controller.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(controller->mutex);
        auto matches = [&](const std::unique_ptr<Session>& session) {
            return session && SameRender(identity,{session->parent_layer,session->request_ns});
        };
        if(controller->pending && !matches(controller->pending)) return;
        if(!matches(controller->pending) && !matches(controller->active)) return;
        controller->cancellation.Request(identity,NowNs()+48000000LL);
        if(controller->looper) ALooper_wake(controller->looper);
    }
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeRunning(JNIEnv*,jclass) {
    auto* controller=g_controller.load(std::memory_order_acquire);
    if(!controller) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(controller->mutex);
    return controller->pending || !controller->retiring.empty() ||
        controller->detaches_pending.load(std::memory_order_acquire)>0 ||
        controller->has_active.load() || controller->working.load() ||
        controller->submitted_serial.load()>controller->cleared_serial.load()?JNI_TRUE:JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_ng_lst_hha_ContentAnimator_nativeStop(JNIEnv*, jclass) {
    auto* target = g_controller.load(std::memory_order_acquire);
    if (target == nullptr) return;
    auto& controller = *target;
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.cancellation.Clear();
    RetirePending(controller);
    controller.command = true;
    if (controller.looper != nullptr) ALooper_wake(controller.looper);
}
