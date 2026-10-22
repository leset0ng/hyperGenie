// SPDX-License-Identifier: Apache-2.0
// Real content-layer renderer owned by system_server on compatible HyperOS.
#include "hha_compat.h"
#include "hha_genie.h"
#include "hha_viewport.h"
#include "hha_renderer.h"

#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <jni.h>
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
struct SurfaceResult {
    void* ptr = nullptr;
    ~SurfaceResult() { if (ptr && g_dec_strong) g_dec_strong(ptr, this); }
};
using ParentingFn = SurfaceResult (*)(const void*);
ParentingFn g_parenting = nullptr; // Optional; only resolves same-process BBQ owners.
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
    // Floating icons share the owner's mesh but sample a square source buffer.
    int texture_width=0;
    int texture_height=0;
    bool starting=false;
};

struct Session {
    void* surface = nullptr;
    void* parent = nullptr;
    std::vector<Companion> companions;
    int32_t layer = -1;
    int32_t parent_layer = -1;
    int32_t unwarped_layer = -1; // A live subtree's source must never be warped twice.
    int width = 0;
    int height = 0;
    int texture_width = 0;
    int texture_height = 0;
    int64_t started_ns = 0;
    int64_t request_ns = 0;  // Submission identity, independent of the preserved motion clock.
    int64_t last_ns = 0;
    int64_t cancel_ns = 0;
    double elapsed = 0;
    uint64_t frames = 0;
    uint64_t generation = 1;
    bool attached = false;
    bool primed_continuation = false;
    bool closing = false;
    bool starting = false;
    bool preserve_preview_masks = true; // OEM preview visibility can outlive its motion owner.
    hha::genie::Parameters parameters;
    hha::genie::Animation animation;
    hha::genie::Scalar corner;
    std::shared_ptr<std::atomic<bool>> committed;
    std::vector<std::shared_ptr<std::atomic<bool>>> commit_dependencies;

    ~Session() {
        if (surface != nullptr) g_dec_strong(surface, this);
        if (parent != nullptr) g_dec_strong(parent, this);
        for(const auto& companion:companions) g_dec_strong(companion.surface,this);
    }
};

struct Controller {
    std::mutex mutex;
    std::unique_ptr<Session> pending;
    std::vector<std::unique_ptr<Session>> queued;
    std::vector<std::unique_ptr<Session>> overlapping;
    std::vector<std::unique_ptr<Session>> retiring;
    // Bounded tombstones prevent delayed imported publications from resurrecting
    // a cancelled/finished owner after its retained surfaces have been released.
    std::vector<int64_t> retired_requests;
    bool stop_requested = false;
    std::atomic<uint64_t> detaches_pending{0};
    ALooper* looper = nullptr;  // Owned by the persistent rendering thread.
    bool callback_pending = false;  // Rendering-thread fields below.
    std::unique_ptr<Session> active;
    void* shader = nullptr;  // One process-lifetime registration, reused per open.
};
std::mutex g_initialize_mutex;
std::atomic<Controller*> g_controller{nullptr};

bool Owns(const Session& session, int32_t layer) {
    return session.layer == layer || std::any_of(session.companions.begin(),session.companions.end(),
        [&](const Companion& companion) {return companion.layer == layer;});
}

bool SameRequestDomain(const Session& a,const Session& b) {
    return a.request_ns == b.request_ns && a.width == b.width && a.height == b.height;
}

bool Related(const Session& a, const Session& b) {
    return a.parent_layer == b.parent_layer || SameRequestDomain(a,b) || Owns(a,b.layer) || Owns(b,a.layer) ||
        std::any_of(a.companions.begin(),a.companions.end(),
            [&](const Companion& companion) {return Owns(b,companion.layer);});
}

bool LiveOwner(const Controller& controller, const Session& retiring, int32_t layer,
               const Session* successor = nullptr) {
    if(successor && successor != &retiring && Owns(*successor,layer)) return true;
    auto owns = [&](const std::unique_ptr<Session>& session) {
        return session && session.get() != &retiring && Owns(*session,layer);
    };
    if(owns(controller.active) || owns(controller.pending)) return true;
    return std::any_of(controller.overlapping.begin(),controller.overlapping.end(),owns) ||
        std::any_of(controller.queued.begin(),controller.queued.end(),owns);
}

bool Committed(const Session& session) {
    return (!session.committed || session.committed->load(std::memory_order_acquire)) &&
        std::all_of(session.commit_dependencies.begin(),session.commit_dependencies.end(),
            [](const std::shared_ptr<std::atomic<bool>>& ready) {return ready->load(std::memory_order_acquire);});
}

void GateCommit(Controller& controller, Session& session, ASurfaceTransaction* transaction) {
    session.commit_dependencies.erase(std::remove_if(session.commit_dependencies.begin(),session.commit_dependencies.end(),
        [](const std::shared_ptr<std::atomic<bool>>& ready) {return ready->load(std::memory_order_acquire);}),
        session.commit_dependencies.end());
    if(session.committed && !session.committed->load(std::memory_order_acquire))
        session.commit_dependencies.push_back(session.committed);
    session.committed=std::make_shared<std::atomic<bool>>(false);
    struct Commit {Controller* controller;std::shared_ptr<std::atomic<bool>> ready;};
    auto* commit=new Commit{&controller,session.committed};
    g_on_commit(transaction,commit,[](void* context,ASurfaceTransactionStats*) {
        std::unique_ptr<Commit> commit(static_cast<Commit*>(context));
        commit->ready->store(true,std::memory_order_release);
        std::lock_guard<std::mutex> lock(commit->controller->mutex);
        if(commit->controller->looper) ALooper_wake(commit->controller->looper);
    });
}

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
        unsigned cleared=0;
        auto clear = [&](void* surface, int32_t layer) {
            if(surface && !LiveOwner(controller,session,layer,preserve)) {
                ClearSurface(transaction,surface);
                ++cleared;
            }
        };
        // Composed outputs can remain visible after worker retirement. Keep
        // their last masked state until the bridge hides them and restores the
        // source in one transaction; clearing SELF alone exposes a rectangle.
        if(session.unwarped_layer<0 && !(session.preserve_preview_masks && session.starting))
            clear(session.surface,session.layer);
        // Retiring motion ownership does not end an OEM preview/color's
        // visibility under its mask-free host. Freeze its last mask until an
        // authenticated successor overwrites it or platform removal ends it.
        for(const auto& companion:session.companions)
            if(!(session.preserve_preview_masks && companion.starting))
                clear(companion.surface,companion.layer);
        if(!cleared) {
            if(!supplied) ASurfaceTransaction_delete(transaction);
            return true;
        }
        // Every submitted retirement stays outstanding until its own completion.
        // The persistent controller outlives every transaction callback.
        controller.detaches_pending.fetch_add(1,std::memory_order_release);
        g_on_complete(transaction,&controller,
            [](void* context,ASurfaceTransactionStats*) {
                static_cast<Controller*>(context)->detaches_pending.fetch_sub(1,std::memory_order_release);
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
        if(supplied) {
            double left=vertices[0].x,top=vertices[0].y,right=left,bottom=top;
            for(const auto& vertex:vertices) {
                left=std::min(left,vertex.x);top=std::min(top,vertex.y);
                right=std::max(right,vertex.x);bottom=std::max(bottom,vertex.y);
            }
            __android_log_print(ANDROID_LOG_INFO,kTag,
                "publication layer=%d parent=%d epoch=%lld closing=%d continuation=%d txn=%p domain=%dx%d "
                "corner=%.6f appliedCorner=%.6f cornerVelocity=%.6f target=%.6f "
                "bounds=(%.6f,%.6f)-(%.6f,%.6f) tl=(%.6f,%.6f) br=(%.6f,%.6f) "
                "rowSpans=%.6f/%.6f/%.6f",
                session.layer,session.parent_layer,static_cast<long long>(session.request_ns),
                session.closing,session.primed_continuation,static_cast<void*>(supplied),session.width,session.height,
                session.corner.value,static_cast<double>(values[38]),session.corner.velocity,session.corner.target,
                left,top,right,bottom,vertices[0].x,vertices[0].y,vertices[8].x,vertices[8].y,
                vertices[2].x-vertices[0].x,vertices[5].x-vertices[3].x,vertices[8].x-vertices[6].x);
        }
        std::vector<Companion> surfaces{{session.surface,session.layer,session.texture_width,session.texture_height}};
        surfaces.insert(surfaces.end(),session.companions.begin(),session.companions.end());
        for(const auto& item:surfaces) {
            void* surface=item.surface;
            if(!surface) continue;
            auto surface_values=values;
            if(item.texture_width>0) {
                surface_values[36]=static_cast<float>(item.texture_width);
                surface_values[37]=surface_values[36]*session.height/session.width;
                surface_values[40]=static_cast<float>(item.texture_width);
                surface_values[41]=static_cast<float>(item.texture_height);
            } else {
                surface_values[36]=surface_values[40]=static_cast<float>(session.width);
                surface_values[37]=surface_values[41]=static_cast<float>(session.height);
            }
            auto uniforms=std::make_shared<std::vector<unsigned char>>(sizeof(surface_values));
            std::memcpy(uniforms->data(),surface_values.data(),sizeof(surface_values));
            g_post(transaction,&surface,&controller.shader,&uniforms,0);
            const BorderSettingsAbi border{nullptr,kClientBorderWidth,0};
            g_border(transaction,&surface,&border);
        }
        session.attached = true;
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

void RememberRetired(Controller& controller,int64_t request_ns) {
    if(std::find(controller.retired_requests.begin(),controller.retired_requests.end(),request_ns)!=
       controller.retired_requests.end()) return;
    controller.retired_requests.push_back(request_ns);
    if(controller.retired_requests.size()>128) controller.retired_requests.erase(controller.retired_requests.begin());
}

void Finish(Controller& controller, const char* reason, const Session* successor = nullptr) {
    if (!controller.active) return;
    auto& session = *controller.active;
    if(!successor || successor->request_ns!=session.request_ns)
        RememberRetired(controller,session.request_ns);
    const bool detached = !session.attached || Submit(controller, session, true, nullptr, successor);
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "finish layer=%d request=%lld reason=%s callbacks=%llu elapsed=%.4f detachSubmitted=%d retained=%d",
        session.layer,static_cast<long long>(session.request_ns),reason,
        static_cast<unsigned long long>(session.frames),session.elapsed,detached,
        successor && successor->layer==session.layer);
    controller.active.reset();
}

void DetachLostSurfaces(Controller& controller,const Session& previous,const Session& next) {
    if(!previous.attached) return;
    ASurfaceTransaction* transaction=ASurfaceTransaction_create();
    if(!transaction) return;
    std::vector<Companion> surfaces{{previous.surface,previous.layer,0,0,previous.starting}};
    surfaces.insert(surfaces.end(),previous.companions.begin(),previous.companions.end());
    for(const auto& item:surfaces) {
        if(item.layer==previous.layer && (previous.unwarped_layer>=0 ||
           (previous.preserve_preview_masks && previous.starting))) continue;
        if(previous.preserve_preview_masks && item.starting) continue;
        if(item.layer>=0 && !LiveOwner(controller,previous,item.layer,&next))
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
    // Each retirement preserves the union of independent live and pending owners.
    for(auto it=controller.retiring.begin();it!=controller.retiring.end();) {
        auto& retired=**it;
        if(!Committed(retired)) {
            ++it;
            continue;
        }
        auto incoming=[&](const std::unique_ptr<Session>& pending) {
            return pending && Related(*pending,retired) && !Committed(*pending);
        };
        if(incoming(controller.pending) || std::any_of(controller.queued.begin(),controller.queued.end(),incoming)) {
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

// Revocation ends ownership, unlike an OEM cancel that can leave its closing
// silhouette visible. Remove every matching writer before retiring any surfaces:
// independently live/primed successors keep their capabilities, and composed
// outputs keep their last mask until the bridge hides/restores them atomically.
void RevokeRequest(Controller& controller, hha::RenderIdentity identity) {
    auto revoke=[&](std::unique_ptr<Session>& session) {
        if(!session || !hha::SameRender(identity,{session->parent_layer,session->request_ns})) return false;
        RememberRetired(controller,session->request_ns);
        session->preserve_preview_masks=true;
        if(session->attached) controller.retiring.push_back(std::move(session));
        else session.reset();
        return true;
    };
    revoke(controller.active);revoke(controller.pending);
    auto remove=[&](std::vector<std::unique_ptr<Session>>& sessions) {
        sessions.erase(std::remove_if(sessions.begin(),sessions.end(),revoke),sessions.end());
    };
    remove(controller.overlapping);remove(controller.queued);
    // A superseded borrowed publication may already be waiting in retirement.
    // Tombstone it too, so it cannot be rediscovered after its successor ends.
    for(const auto& session:controller.retiring)
        if(hha::SameRender(identity,{session->parent_layer,session->request_ns})) {
            RememberRetired(controller,session->request_ns);
            session->preserve_preview_masks=true;
        }
    RetireCommitted(controller);
}

// Keep icon capabilities on the same motion owner through starting/App handoff,
// including closing. They must never become a second independently clocked session.
void PreserveLinkedIcons(Session& next,const Session& previous) {
    if(next.width!=previous.width || next.height!=previous.height ||
       (!SameRequestDomain(next,previous) && next.parent_layer!=previous.parent_layer &&
        next.layer!=previous.layer)) return;
    auto retain=[&](const Companion& item) {
        if(!item.surface || item.texture_width<=0 || item.layer==next.layer ||
           item.layer==next.unwarped_layer || Owns(next,item.layer)) return;
        next.companions.push_back(item);
        g_inc_strong(item.surface,&next);
    };
    retain({previous.surface,previous.layer,previous.texture_width,previous.texture_height});
    for(const auto& item:previous.companions) retain(item);
    std::sort(next.companions.begin(),next.companions.end(),
        [](const Companion& a,const Companion& b) {return a.layer<b.layer;});
}

// Removing an OEM preview from its registry does not remove its visible SF
// layer. Keep each verified opening capability under the same mesh until this
// request finishes; otherwise a still-opaque preview becomes a full rectangle
// behind the animated App. The immutable request, parent and extent bound reuse.
void PreserveOpeningPreviews(Session& next, const Session& previous) {
    PreserveLinkedIcons(next,previous);
    if(next.width!=previous.width || next.height!=previous.height) return;
    const bool same_request=next.request_ns==previous.request_ns;
    auto retain=[&](void* surface,int32_t layer,int texture_width,int texture_height,bool starting) {
        if(!surface || layer<0 || layer==next.layer || layer==next.unwarped_layer ||
           std::any_of(next.companions.begin(),next.companions.end(),
               [&](const Companion& item) {return item.layer==layer;})) return;
        next.companions.push_back({surface,layer,texture_width,texture_height,starting});
        g_inc_strong(surface,&next);
    };
    for(const auto& item:previous.companions)
        if(same_request || item.starting)
            retain(item.surface,item.layer,item.texture_width,item.texture_height,item.starting);
    // A composed output has its own visibility owner. Carrying it as an
    // ordinary preview loses its masked-retirement contract and can clear SELF
    // while a newer group is visible. The bridge hides it on group replacement.
    if(previous.unwarped_layer<0 && (same_request || previous.starting))
        retain(previous.surface,previous.layer,previous.texture_width,previous.texture_height,previous.starting);
    std::sort(next.companions.begin(),next.companions.end(),
        [](const Companion& a,const Companion& b) {return a.layer<b.layer;});
}

// Caller holds controller.mutex throughout command adoption and all SF writes.
void AdoptCommand(Controller& controller) {
    if (controller.pending && !Committed(*controller.pending)) return;
    auto next = std::move(controller.pending);
    // QueueSession already bounds request age. Even an unprimed worker command
    // can own earlier, visible show publications coalesced into its companions;
    // dropping it here would let their pending retirement clear those buffers.
    if(next && controller.active) PreserveOpeningPreviews(*next,*controller.active);
    if (next && controller.active && next->layer==controller.active->layer &&
        next->width==controller.active->width && next->height==controller.active->height) {
        auto& current=*controller.active;
        if (next->request_ns<current.request_ns) return;
        current.request_ns=next->request_ns;
        current.cancel_ns=next->cancel_ns;
        current.starting=next->starting;
        current.unwarped_layer=next->unwarped_layer;
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
                [](const Companion& a,const Companion& b) {
                    return a.layer==b.layer && a.texture_width==b.texture_width &&
                        a.texture_height==b.texture_height;
                });
        if(companions_changed) {
            DetachLostSurfaces(controller,current,*next);
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
       (next->parent_layer==controller.active->parent_layer ||
        SameRequestDomain(*next,*controller.active)) &&
       next->width==controller.active->width && next->height==controller.active->height &&
       (next->request_ns==controller.active->request_ns || next->starting || controller.active->starting)) {
        const auto& previous=*controller.active;
        const auto requested=next->parameters;
        if(!next->primed_continuation) {
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
            if(next->closing!=previous.closing) {
                next->parameters.response=requested.response;
                next->parameters.damping_ratio=requested.damping_ratio;
                next->animation.Retarget(next->parameters,next->closing);
            }
        }
        next->attached=previous.attached || next->attached;
        DetachLostSurfaces(controller,previous,*next);
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

void ConsumeCommand(Controller& controller) {
    if(controller.stop_requested) {
        controller.stop_requested=false;
        auto remember=[&](const std::unique_ptr<Session>& session) {
            if(session) {
                RememberRetired(controller,session->request_ns);
                session->preserve_preview_masks=false; // Explicit disable may release previews too.
            }
        };
        for(const auto& session:controller.retiring) remember(session);
        remember(controller.active);remember(controller.pending);
        for(const auto& session:controller.queued) remember(session);
        for(const auto& session:controller.overlapping) remember(session);
        while(!controller.queued.empty()) {
            auto session=std::move(controller.queued.back());
            controller.queued.pop_back();
            if(session->attached) controller.retiring.push_back(std::move(session));
        }
        RetirePending(controller);
        if(controller.active && !Committed(*controller.active))
            controller.retiring.push_back(std::move(controller.active));
        else Finish(controller,"cancelled");
        for(auto& session:controller.overlapping) {
            controller.active=std::move(session);
            if(!Committed(*controller.active)) controller.retiring.push_back(std::move(controller.active));
            else Finish(controller,"cancelled");
        }
        controller.overlapping.clear();
        return;
    }
    // An unrelated show transaction must not hold up an already-committed App.
    // Keep all uncommitted primes owned and select ready commands independently.
    for(;;) {
        if(!controller.pending || !Committed(*controller.pending)) {
            auto ready=std::find_if(controller.queued.begin(),controller.queued.end(),
                [](const std::unique_ptr<Session>& session) {return Committed(*session);});
            if(ready==controller.queued.end()) break;
            if(controller.pending) std::swap(controller.pending,*ready);
            else {controller.pending=std::move(*ready);controller.queued.erase(ready);}
        }
        if(!controller.pending) break;
        auto& next=*controller.pending;
        if(!controller.active || !Related(*controller.active,next)) {
            auto previous=std::find_if(controller.overlapping.begin(),controller.overlapping.end(),
                [&](const std::unique_ptr<Session>& session) {return Related(*session,next);});
            if(previous!=controller.overlapping.end()) {
                std::swap(controller.active,*previous);
                if(!*previous) controller.overlapping.erase(previous);
            } else if(controller.active) {
                controller.overlapping.push_back(std::move(controller.active));
            }
        }
        AdoptCommand(controller);
    }
}

bool PublicationPending(const Controller& controller,const Session& session) {
    if(!Committed(session)) return true;
    auto blocked=[&](const std::unique_ptr<Session>& pending) {
        return pending && Related(*pending,session) && !Committed(*pending);
    };
    return blocked(controller.pending) ||
        std::any_of(controller.queued.begin(),controller.queued.end(),blocked);
}

void FrameCurrent(Controller& controller,int64_t frame_time) {
    if (!controller.active || PublicationPending(controller,*controller.active)) return;
    auto& session = *controller.active;
    // Stopping an SF animation does not hide its buffer. In particular an old
    // closing icon can remain opaque after another App has started. Keep that
    // silhouette until the existing deadline; explicit global disable may clear.
    if(session.cancel_ns && NowNs()>=session.cancel_ns && !session.closing) {
        Finish(controller,"icon-cancelled");
        return;
    }
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
    // A composed output remains visible until the bridge restores its source.
    // Its host has no OEM mask, so it needs SELF even after opening settles.
    if (!session.closing && !session.starting && session.unwarped_layer<0 &&
        session.animation.Settled() && corner_settled)
        Finish(controller, "settled");
    else if (elapsed >= 1.5) Finish(controller, "deadline");
}

void Frame(int64_t frame_time, void* context) {
    auto& controller=*static_cast<Controller*>(context);
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.callback_pending=false;
    ConsumeCommand(controller);
    RetireCommitted(controller);
    FrameCurrent(controller,frame_time);
    for(auto& session:controller.overlapping) {
        std::swap(controller.active,session);
        FrameCurrent(controller,frame_time);
        std::swap(controller.active,session);
    }
    controller.overlapping.erase(std::remove(controller.overlapping.begin(),controller.overlapping.end(),nullptr),
        controller.overlapping.end());
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
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(controller->mutex);
            ConsumeCommand(*controller);
            RetireCommitted(*controller);
            if ((controller->active || !controller->overlapping.empty()) && !controller->callback_pending) {
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
    if (!hha::CheckHyperOS(kTag)) return false;
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
    g_parenting = reinterpret_cast<ParentingFn>(dlsym(gui, "_ZN7android14SurfaceControl17getParentingLayerEv"));
    g_inc_strong = reinterpret_cast<StrongFn>(dlsym(utils, "_ZNK7android7RefBase9incStrongEPKv"));
    g_dec_strong = reinterpret_cast<StrongFn>(dlsym(utils, "_ZNK7android7RefBase9decStrongEPKv"));
    g_on_commit = reinterpret_cast<CommitFn>(dlsym(platform, "ASurfaceTransaction_setOnCommit"));
    if (!reg || !g_post || !g_border || !g_layer_id || !g_inc_strong || !g_dec_strong || !g_on_commit) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "required SELF interfaces missing; content animation disabled");
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

extern "C" JNIEXPORT jlong JNICALL
Java_ng_lst_hha_ContentAnimator_nativeBufferSurfaceCopy(JNIEnv* env, jclass, jlong source) {
    if (!source || !g_parenting || !g_controller.load(std::memory_order_acquire)) return 0;
    void* original = reinterpret_cast<void*>(static_cast<uintptr_t>(source));
    // The caller holds the original Java SC lock. Its local +0x58 owner is lost
    // by the regular SC copy constructor and cannot be recovered by layer ID.
    SurfaceResult buffer = g_parenting(original);
    if (!buffer.ptr || buffer.ptr == original || g_layer_id(buffer.ptr) == g_layer_id(original)) return 0;
    jclass surface_class = env->FindClass("android/view/SurfaceControl");
    if (!surface_class) return 0;
    jmethodID copy = env->GetStaticMethodID(surface_class, "nativeCopyFromSurfaceControl", "(J)J");
    const jlong result = copy ? env->CallStaticLongMethod(surface_class, copy,
        static_cast<jlong>(reinterpret_cast<uintptr_t>(buffer.ptr))) : 0;
    env->DeleteLocalRef(surface_class);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeInitialize(JNIEnv*, jclass) {
    return Initialize() ? JNI_TRUE : JNI_FALSE;
}

static bool PrimeLinkedIcon(void* transaction,jlong surface,jlong parent,int texture_width,
                            int texture_height,int width,int height,
                            const std::array<float,12>& geometry,bool closing,int64_t event_ns,
                            int64_t source_request_ns=0,int32_t source_parent_layer=-1);

static bool QueueSession(jlong surface,jlong parent,int width,int height,
    const std::array<float,12>& values,bool closing,int64_t event_ns,const std::vector<jlong>& companions,bool starting,
    int texture_width=0,int texture_height=0,void* initial_transaction=nullptr,void* unwarped_source=nullptr,
    bool linked_icon=false) {
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
    next->unwarped_layer=unwarped_source?g_layer_id(unwarped_source):-1;
    for(jlong companion:companions) {
        if(!companion) continue;
        void* overlay=reinterpret_cast<void*>(static_cast<uintptr_t>(companion));
        const int32_t layer=g_layer_id(overlay);
        if(layer!=next->layer && layer!=next->unwarped_layer && std::none_of(next->companions.begin(),next->companions.end(),
            [&](const Companion& existing) {return existing.layer==layer;})) {
            next->companions.push_back({overlay,layer,0,0,true});
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
    std::unique_lock<std::mutex> lock(controller->mutex);
    if((linked_icon && controller->stop_requested) ||
       std::find(controller->retired_requests.begin(),controller->retired_requests.end(),event_ns)!=
           controller->retired_requests.end()) return false;
    Session* previous=nullptr;
    auto inspect=[&](const std::unique_ptr<Session>& session) {
        if(session && Related(*session,*next) && (!previous ||
           session->request_ns>previous->request_ns ||
           (session->request_ns==previous->request_ns && session->last_ns>=previous->last_ns)))
            previous=session.get();
    };
    inspect(controller->active);inspect(controller->pending);
    for(const auto& session:controller->overlapping) inspect(session);
    for(const auto& session:controller->queued) inspect(session);
    if(previous && previous->layer==next->layer && next->unwarped_layer<0)
        next->unwarped_layer=previous->unwarped_layer;
    if(linked_icon && previous) {
        // A content publication may have won the race while this provisional
        // icon was built. Join that owner instead of replacing its App buffer.
        if(!SameRequestDomain(*previous,*next) || previous->closing!=closing) return false;
        lock.unlock();
        return PrimeLinkedIcon(initial_transaction,surface,parent,texture_width,texture_height,
            width,height,values,closing,event_ns);
    }
    // Ordering is local to the actual stream. A delayed App request cannot be
    // discarded merely because another App has a newer event timestamp.
    if(previous && event_ns<previous->request_ns) return false;
    // Motion comes from the newest compatible state, but ownership must be the
    // union of every same-request live/pending publication. Multiple show paths
    // can publish different buffers before the worker adopts any one of them.
    auto dependency=[&](const std::shared_ptr<std::atomic<bool>>& ready) {
        if(ready && !ready->load(std::memory_order_acquire) &&
           std::find(next->commit_dependencies.begin(),next->commit_dependencies.end(),ready)==next->commit_dependencies.end())
            next->commit_dependencies.push_back(ready);
    };
    auto await_publication=[&](const Session& session) {
        for(const auto& ready:session.commit_dependencies) dependency(ready);
        dependency(session.committed);
    };
    auto preserve=[&](const std::unique_ptr<Session>& session) {
        if(!session || !Related(*session,*next)) return;
        PreserveOpeningPreviews(*next,*session);
        await_publication(*session);
    };
    preserve(controller->active);preserve(controller->pending);
    for(const auto& session:controller->overlapping) preserve(session);
    for(const auto& session:controller->queued) preserve(session);
    // Revocation cannot retract a borrowed host transaction that SF has not
    // committed yet. Wait for its possible late write without inheriting its
    // revoked geometry or previews into the successor.
    for(const auto& session:controller->retiring)
        if(Related(*session,*next)) await_publication(*session);
    if(!initial_transaction && previous && previous->attached &&
       SameRequestDomain(*previous,*next) && previous->closing==next->closing) {
        // A worker discovery can replace a correctly primed pending command
        // before either host commits. Icon publication may read that command
        // immediately, so preserve its published snapshot now, not at adoption.
        next->animation=previous->animation;
        next->parameters=previous->parameters;
        next->corner=previous->corner;
        next->started_ns=previous->started_ns;
        next->last_ns=previous->last_ns;
        next->elapsed=previous->elapsed;
        next->frames=previous->frames;
        next->generation=previous->generation;
        next->primed_continuation=true;
        next->attached=true;
    }
    if(initial_transaction) {
        // Queue the first SELF state in the exact transaction that installs the
        // virtual icon host. No frame may expose the unwarped tall backing plate.
        const double delta=static_cast<double>(now-event_ns)/1e9;
        if(previous && previous->width==width && previous->height==height &&
           (previous->layer==next->layer || previous->request_ns==event_ns ||
            previous->starting || next->starting)) {
            const auto& current=*previous;
            const auto requested_corner=next->corner;
            next->animation=current.animation;
            next->parameters=current.parameters;
            next->corner=current.corner;
            next->last_ns=current.last_ns;
            next->started_ns=current.started_ns;
            next->generation=current.generation;
            const bool frozen=PublicationPending(*controller,current);
            const double advance=frozen?0.0:std::max(0.0,static_cast<double>(now-current.last_ns)/1e9);
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
        // An older borrowed publication can commit after this transaction.
        // Keep their common snapshot frozen until every dependency is ready.
        const bool frozen=previous && PublicationPending(*controller,*previous);
        if(!frozen) next->last_ns=std::max(now,event_ns);
        next->elapsed=std::max(0.0,static_cast<double>(next->last_ns-next->started_ns)/1e9);
        auto* transaction=static_cast<ASurfaceTransaction*>(initial_transaction);
        // Transfer away from direct SELF in the publication that reveals the
        // grouped buffer. Pending same-request adoption blocks further old writes.
        if(unwarped_source) ClearSurface(transaction,unwarped_source);
        if(!Submit(*controller,*next,false,transaction)) return false;
        GateCommit(*controller,*next,transaction);
    }
    if(controller->pending) {
        if(Related(*controller->pending,*next)) RetirePending(*controller);
        else controller->queued.push_back(std::move(controller->pending));
    }
    controller->pending=std::move(next);
    if(controller->looper) ALooper_wake(controller->looper);
    return JNI_TRUE;
}

static bool AttachSession(void* transaction,jlong surface,jlong parent,int width,int height,int64_t event_ns,
                          bool starting=false) {
    auto* controller=g_controller.load(std::memory_order_acquire);
    if(!controller || !transaction || !surface || !parent || width<=0 || height<=0) return false;
    std::lock_guard<std::mutex> lock(controller->mutex);
    if(controller->stop_requested ||
       std::find(controller->retired_requests.begin(),controller->retired_requests.end(),event_ns)!=
           controller->retired_requests.end()) return false;
    const int32_t parent_layer=g_layer_id(reinterpret_cast<void*>(static_cast<uintptr_t>(parent)));
    Session* current=nullptr;
    auto find=[&](const std::unique_ptr<Session>& session) {
        if(session && session->parent_layer==parent_layer && session->request_ns==event_ns &&
           session->width==width && session->height==height) current=session.get();
    };
    find(controller->active);find(controller->pending);
    for(const auto& session:controller->overlapping) find(session);
    for(const auto& session:controller->queued) find(session);
    if(!current) return false;
    void* control=reinterpret_cast<void*>(static_cast<uintptr_t>(surface));
    const int32_t layer=g_layer_id(control);
    if (layer == current->unwarped_layer) return false;
    // The caller has already proved ownership and the output domain. A buffer
    // currently owned by another stream cannot be adopted by this attachment.
    auto conflicting=[&](const std::unique_ptr<Session>& owner) {
        return owner && owner.get()!=current && Owns(*owner,layer) &&
            !(owner->parent_layer==current->parent_layer && owner->request_ns==current->request_ns);
    };
    if(conflicting(controller->active) || conflicting(controller->pending) ||
       std::any_of(controller->overlapping.begin(),controller->overlapping.end(),conflicting) ||
       std::any_of(controller->queued.begin(),controller->queued.end(),conflicting)) return false;
    if(!Owns(*current,layer)) {
        current->companions.push_back({control,layer,0,0,starting});
        g_inc_strong(control,current);
        std::sort(current->companions.begin(),current->companions.end(),
            [](const Companion& a,const Companion& b) {return a.layer<b.layer;});
    } else if(starting) {
        if(current->layer==layer) current->starting=true;
        for(auto& companion:current->companions)
            if(companion.layer==layer) companion.starting=true;
    }
    // Publish only this buffer into its real show transaction; do not replace
    // the request, reset springs, or apply/close the OEM-owned transaction.
    Session publication;
    publication.surface=control;
    g_inc_strong(control,&publication);
    publication.layer=layer;
    publication.width=width;publication.height=height;
    publication.texture_width=current->texture_width;publication.texture_height=current->texture_height;
    publication.animation=current->animation;publication.parameters=current->parameters;
    publication.corner=current->corner;publication.frames=current->frames;
    if(!Submit(*controller,publication,false,static_cast<ASurfaceTransaction*>(transaction))) return false;
    current->attached=true;
    GateCommit(*controller,*current,static_cast<ASurfaceTransaction*>(transaction));
    if(controller->looper) ALooper_wake(controller->looper);
    return true;
}

// The authenticated bridge imports only the launcher's real floating buffer.
// Its first write travels back in the host transaction; every later write uses
// the App owner's exact mesh and transaction, including publication barriers.
static bool PrimeLinkedIcon(void* transaction,jlong surface,jlong parent,int texture_width,
                            int texture_height,int width,int height,
                            const std::array<float,12>& geometry,bool closing,int64_t event_ns,
                            int64_t source_request_ns,int32_t source_parent_layer) {
    auto* controller=g_controller.load(std::memory_order_acquire);
    const int64_t now=NowNs();
    if(!controller || !transaction || !surface || !parent || texture_width<=0 ||
       texture_height<=0 || texture_width>1024 || texture_height>1024 || width<=0 ||
       height<=width || width>16384 || height>16384 || event_ns<=0 ||
       event_ns>now+100000000LL || now-event_ns>1500000000LL) return false;
    for(float value:geometry) if(!std::isfinite(value)) return false;
    {
        std::lock_guard<std::mutex> lock(controller->mutex);
        if(controller->stop_requested ||
           std::find(controller->retired_requests.begin(),controller->retired_requests.end(),event_ns)!=
               controller->retired_requests.end()) return false;
        Session* current=nullptr;
        auto find=[&](const std::unique_ptr<Session>& owner) {
            if(owner && owner->request_ns==event_ns && owner->width==width &&
               owner->height==height && owner->closing==closing) current=owner.get();
        };
        find(controller->active);
        for(const auto& owner:controller->overlapping) find(owner);
        for(const auto& owner:controller->queued) find(owner);
        find(controller->pending);
        bool advancing=false;
        if(!current && source_request_ns>0 && source_request_ns<=event_ns) {
            const int32_t icon_layer=g_layer_id(reinterpret_cast<void*>(static_cast<uintptr_t>(surface)));
            auto predecessor=[&](const std::unique_ptr<Session>& owner) {
                if(!owner) return;
                const bool source=owner->request_ns==source_request_ns ||
                    (source_parent_layer>=0 && owner->parent_layer==source_parent_layer &&
                     owner->request_ns<source_request_ns);
                if(source && owner->request_ns<event_ns && owner->width==width &&
                   owner->height==height && Owns(*owner,icon_layer) &&
                   (!current || owner->request_ns>=current->request_ns)) current=owner.get();
            };
            predecessor(controller->active);
            for(const auto& owner:controller->overlapping) predecessor(owner);
            for(const auto& owner:controller->queued) predecessor(owner);
            predecessor(controller->pending);
            advancing=current!=nullptr;
            if(!current && source_request_ns<event_ns) return false;
        }
        if(current) {
            void* control=reinterpret_cast<void*>(static_cast<uintptr_t>(surface));
            const int32_t layer=g_layer_id(control);
            if(layer<0 || layer==current->unwarped_layer) return false;
            auto conflicting=[&](const std::unique_ptr<Session>& owner) {
                if(!owner || owner.get()==current || !Owns(*owner,layer) ||
                   SameRequestDomain(*owner,*current)) return false;
                // Shared icon ownership alone cannot prove a predecessor: use
                // the actual App primary/parent and strictly ordered epochs.
                const bool predecessor=owner->request_ns<current->request_ns &&
                    owner->width==width && owner->height==height &&
                    (owner->layer==current->layer || owner->parent_layer==current->parent_layer);
                return !predecessor;
            };
            if(conflicting(controller->active) || conflicting(controller->pending) ||
               std::any_of(controller->overlapping.begin(),controller->overlapping.end(),conflicting) ||
               std::any_of(controller->queued.begin(),controller->queued.end(),conflicting)) return false;
            if(current->layer==layer) {
                if(current->texture_width!=texture_width || current->texture_height!=texture_height)
                    return false;
            } else {
                auto item=std::find_if(current->companions.begin(),current->companions.end(),
                    [&](const Companion& companion) {return companion.layer==layer;});
                if(item!=current->companions.end()) {
                    if(item->texture_width!=texture_width || item->texture_height!=texture_height)
                        return false;
                } else {
                    current->companions.push_back({control,layer,texture_width,texture_height});
                    g_inc_strong(control,current);
                    std::sort(current->companions.begin(),current->companions.end(),
                        [](const Companion& a,const Companion& b) {return a.layer<b.layer;});
                }
            }
            if(advancing) {
                // The bridge proved an ordered adjust of the same App stream.
                // Preserve presentation and velocity while moving its epoch;
                // the asynchronous App lookup subsequently joins this state.
                if(current->closing!=closing) {
                    auto parameters=current->parameters;
                    parameters.response=closing?hha::genie::kClosingResponse:hha::genie::kOpeningResponse;
                    if(!current->animation.Retarget(parameters,closing)) return false;
                    current->parameters=parameters;
                    current->closing=closing;
                    current->started_ns=event_ns;
                    ++current->generation;
                }
                const size_t end=4;
                const hha::genie::Rect target{geometry[end],geometry[end+1],geometry[end+2],geometry[end+3]};
                current->corner.target=hha::genie::CornerRatio(geometry[11],target);
                current->corner.response=current->parameters.response;
                current->request_ns=event_ns;
                current->cancel_ns=0;
            }
            // Publish the entire shared snapshot. A pending App transaction
            // can arrive after this icon host transaction; writing only the
            // new icon here would expose different snapshots in the meantime.
            if(!Submit(*controller,*current,false,static_cast<ASurfaceTransaction*>(transaction)))
                return false;
            current->attached=true;
            GateCommit(*controller,*current,static_cast<ASurfaceTransaction*>(transaction));
            if(controller->looper) ALooper_wake(controller->looper);
            return true;
        }
    }
    // A published starting request can precede worker adoption of App content.
    // Seed a provisional owner; the real App inherits it and retains this icon.
    return QueueSession(surface,parent,width,height,geometry,closing,event_ns,{},true,
        texture_width,texture_height,transaction,nullptr,true);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativePrimeIcon(JNIEnv* env,jclass,jlong transaction,jlong surface,
    jlong parent,jint texture_width,jint texture_height,jint width,jint height,
    jfloatArray geometry,jboolean closing,jlong event_ns,jlong source_request_ns,jint source_parent_layer) {
    if(!geometry || env->GetArrayLength(geometry)!=12) return JNI_FALSE;
    std::array<float,12> values{};
    env->GetFloatArrayRegion(geometry,0,values.size(),values.data());
    if(env->ExceptionCheck()) return JNI_FALSE;
    return PrimeLinkedIcon(reinterpret_cast<void*>(static_cast<uintptr_t>(transaction)),surface,parent,
        texture_width,texture_height,width,height,values,closing,event_ns,source_request_ns,source_parent_layer)?JNI_TRUE:JNI_FALSE;
}

static jboolean TransitionJni(JNIEnv* env,jlong transaction,jlong surface,jlong parent,
    jint width,jint height,jfloatArray geometry,jboolean closing,jlong event_ns,
    jlongArray companion_array,jboolean starting,jlong unwarped_source=0) {
    if(!geometry || env->GetArrayLength(geometry)!=12 || !companion_array ||
       env->GetArrayLength(companion_array)>8) return JNI_FALSE;
    std::array<float,12> values{};
    std::vector<jlong> companions(env->GetArrayLength(companion_array));
    env->GetLongArrayRegion(companion_array,0,companions.size(),companions.data());
    env->GetFloatArrayRegion(geometry,0,values.size(),values.data());
    if(env->ExceptionCheck()) return JNI_FALSE;
    return QueueSession(surface,parent,width,height,values,closing,event_ns,companions,starting,
        0,0,reinterpret_cast<void*>(static_cast<uintptr_t>(transaction)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(unwarped_source)))?JNI_TRUE:JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeTransition(JNIEnv* env,jclass,jlong surface,
    jlong parent,jint width,jint height,jfloatArray geometry,jboolean closing,
    jlong event_ns,jlongArray companion_array,jboolean starting) {
    return TransitionJni(env,0,surface,parent,width,height,geometry,closing,event_ns,companion_array,starting);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativePrime(JNIEnv* env,jclass,jlong transaction,jlong surface,
    jlong parent,jint width,jint height,jfloatArray geometry,jboolean closing,
    jlong event_ns,jlongArray companion_array,jboolean starting,jlong unwarped_source) {
    if(!transaction) return JNI_FALSE;
    return TransitionJni(env,transaction,surface,parent,width,height,geometry,closing,event_ns,companion_array,starting,
        unwarped_source);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeAttach(JNIEnv*,jclass,jlong transaction,jlong surface,
    jlong parent,jint width,jint height,jlong event_ns,jboolean starting) {
    return AttachSession(reinterpret_cast<void*>(static_cast<uintptr_t>(transaction)),surface,
        parent,width,height,event_ns,starting)?JNI_TRUE:JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeClearRetiredGroup(JNIEnv*,jclass,jlong transaction,jlong surface) {
    auto* controller=g_controller.load(std::memory_order_acquire);
    if(!controller || !transaction || !surface) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(controller->mutex);
    void* control=reinterpret_cast<void*>(static_cast<uintptr_t>(surface));
    const int layer=g_layer_id(control);
    auto owns=[&](const std::unique_ptr<Session>& session) {return session && Owns(*session,layer);};
    if(owns(controller->active) || owns(controller->pending) ||
       std::any_of(controller->queued.begin(),controller->queued.end(),owns) ||
       std::any_of(controller->overlapping.begin(),controller->overlapping.end(),owns) ||
       std::any_of(controller->retiring.begin(),controller->retiring.end(),owns)) return JNI_FALSE;
    ClearSurface(reinterpret_cast<ASurfaceTransaction*>(static_cast<uintptr_t>(transaction)),control);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_ng_lst_hha_ContentAnimator_nativeStopRequest(JNIEnv*,jclass,jlong parent,jlong event_ns) {
    if(parent<=0 || parent>INT32_MAX) return;
    hha::CancelIconRenderer({static_cast<int32_t>(parent),event_ns});
}

extern "C" JNIEXPORT void JNICALL
Java_ng_lst_hha_ContentAnimator_nativeRevokeRequest(JNIEnv*,jclass,jlong parent,jlong event_ns) {
    if(parent<=0 || parent>INT32_MAX || event_ns<=0) return;
    auto* controller=g_controller.load(std::memory_order_acquire);
    if(!controller) return;
    std::lock_guard<std::mutex> lock(controller->mutex);
    RevokeRequest(*controller,{static_cast<int32_t>(parent),event_ns});
    if(controller->looper) ALooper_wake(controller->looper);
}

namespace hha {
bool WarmIconRenderer() {return Initialize();}
void RetainIconSurface(void* surface,const void* owner) {if(surface && g_inc_strong) g_inc_strong(surface,owner);}
void ReleaseIconSurface(void* surface,const void* owner) {if(surface && g_dec_strong) g_dec_strong(surface,owner);}
bool PrimeContentPublication(void* transaction,void* surface,void* parent,int width,int height,
                             const Transition& request) {
    auto* controller=g_controller.load(std::memory_order_acquire);
    if(!controller || !transaction || !surface || !parent ||
       width<=0 || height<=0 || width>16384 || height>16384) return false;
    const auto& values=request.geometry;
    for(float value:values) if(!std::isfinite(value)) return false;
    const int64_t now=NowNs();
    const size_t full=request.closing?0:4;
    const size_t small=request.closing?4:0;
    const bool full_endpoint=std::abs(values[full+2]-width)<2 &&
                             std::abs(values[full+3]-height)<2;
    const bool scaled_close=request.closing && values[full+2]>0 && values[full+3]>0 &&
        values[full+2]<=width+2.0f && values[full+3]<=height+2.0f &&
        std::abs(values[full+3]-values[full+2]*height/width)<2;
    if(request.event_ns<=0 || request.event_ns>now+100000000LL ||
       now-request.event_ns>1500000000LL || (!full_endpoint && !scaled_close)) return false;
    const genie::Rect icon{values[small],values[small+1],values[small+2],values[small+3]};
    // Match QueueSession: presentation coordinates are not the full buffer's
    // origin when a gesture has already translated and scaled its parent.
    const genie::Rect window{full_endpoint?values[full]:0.0,full_endpoint?values[full+1]:0.0,
        static_cast<double>(width),static_cast<double>(height)};
    const genie::Rect start{values[0],values[1],values[2],values[3]};
    const genie::Rect end{values[4],values[5],values[6],values[7]};
    Session publication;
    if(!genie::ParametersForIcon(icon,window,&publication.parameters)) return false;
    publication.parameters.response=request.closing?genie::kClosingResponse:genie::kOpeningResponse;
    if(!publication.animation.Reset(publication.parameters,!request.closing) ||
       !publication.animation.Retarget(request.closing)) return false;
    publication.corner={genie::CornerRatio(values[10],start),0.0,
        genie::CornerRatio(values[11],end),publication.parameters.response};
    const double elapsed=std::max(0.0,static_cast<double>(now-request.event_ns)/1e9);
    publication.animation.Advance(elapsed);
    genie::AdvanceScalar(&publication.corner,elapsed,1.0);
    publication.width=width;publication.height=height;
    publication.surface=surface;publication.parent=parent;
    g_inc_strong(surface,&publication);g_inc_strong(parent,&publication);
    // Transaction setters retain their SC capabilities. This local publication
    // owns no launcher session/callback; system_server owns subsequent updates
    // and retirement of the actual preview returned by the authenticated bridge.
    std::lock_guard<std::mutex> lock(controller->mutex);
    if(!controller->shader) return false;
    return Submit(*controller,publication,false,static_cast<ASurfaceTransaction*>(transaction));
}
bool PrimeOpeningPreview(void* transaction,void* surface,void* parent,int width,int height,
                         const Transition& request) {
    return !request.closing && PrimeContentPublication(transaction,surface,parent,width,height,request);
}
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
        controller->stop_requested=true;
        if(controller->looper) ALooper_wake(controller->looper);
    }
}
void CancelIconRenderer(RenderIdentity identity) {
    if(auto* controller=g_controller.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(controller->mutex);
        auto stop=[&](const std::unique_ptr<Session>& session) {
            if(session && SameRender(identity,{session->parent_layer,session->request_ns})) {
                RememberRetired(*controller,session->request_ns);
                session->cancel_ns=NowNs()+48000000LL;
            }
        };
        stop(controller->active);stop(controller->pending);
        for(const auto& session:controller->overlapping) stop(session);
        for(const auto& session:controller->queued) stop(session);
        if(controller->looper) ALooper_wake(controller->looper);
    }
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_ng_lst_hha_ContentAnimator_nativeRunning(JNIEnv*,jclass) {
    auto* controller=g_controller.load(std::memory_order_acquire);
    if(!controller) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(controller->mutex);
    return controller->pending || !controller->queued.empty() || !controller->overlapping.empty() ||
        !controller->retiring.empty() || controller->detaches_pending.load(std::memory_order_acquire)>0 ||
        controller->active?JNI_TRUE:JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_ng_lst_hha_ContentAnimator_nativeStop(JNIEnv*, jclass) {
    hha::CancelIconRenderer();
}
