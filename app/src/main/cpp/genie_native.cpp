// SPDX-License-Identifier: Apache-2.0
// Native-only HyperOS launcher integration. Target Apps install no hooks.
#include "genie_bridge.h"
#include "genie_viewport.h"
#include "genie_renderer.h"
#include "genie_compat.h"
#include "genie_abi.h"

#include <android/log.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr char kTag[] = "Genie";
constexpr char kStart[] =
    "_ZN7android21SurfaceComposerClient11Transaction16startSFAnimationERKNS_2spINS_14SurfaceControlEEERKNS2_INS_3gui17SfAnimationParamsEEE";
constexpr char kAdjust[] =
    "_ZN7android21SurfaceComposerClient11Transaction17adjustSFAnimationERKNS_2spINS_14SurfaceControlEEERKNS2_INS_3gui17SfAnimationParamsEEE";
constexpr char kBlastUpdate[] =
    "_ZN7android16BLASTBufferQueue6updateERKNS_2spINS_14SurfaceControlEEEjji";
constexpr char kReparent[] =
    "_ZN7android21SurfaceComposerClient11Transaction8reparentERKNS_2spINS_14SurfaceControlEEES6_";
constexpr char kStop[] =
    "_ZN7android21SurfaceComposerClient11Transaction15stopSFAnimationERKNS_2spINS_14SurfaceControlEEEi";

struct NativeAPIEntries {
    uint32_t version;
    int (*hookFunc)(void*, void*, void**);
    int (*unhookFunc)(void*);
};
using Loaded = void (*)(const char*, void*);
using AnimationFn = void* (*)(void*, const void*, const void*);
using StopFn = void* (*)(void*, const void*, int32_t);
using BlastUpdateFn = void* (*)(void*,const void*,uint32_t,uint32_t,int32_t);
using LayerFn = int32_t (*)(const void*);
using NameFn = const std::string& (*)(const void*);
using TransactionIdFn = uint64_t (*)(void*);
AnimationFn g_start = nullptr;
AnimationFn g_adjust = nullptr;
AnimationFn g_reparent = nullptr;
StopFn g_stop = nullptr;
BlastUpdateFn g_blast_update = nullptr;
std::atomic<bool> g_icon_ready{false};
std::atomic<bool> g_native_entered{false};
LayerFn g_layer = nullptr;
NameFn g_name = nullptr;
TransactionIdFn g_transaction_id = nullptr; // Optional transaction diagnostics.
const NativeAPIEntries* g_api = nullptr;
std::atomic<bool> g_installed{false};
std::mutex g_mutex;

int64_t Now() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<int64_t>(value.tv_sec)*1000000000LL+value.tv_nsec;
}
struct Config { bool enabled = true; bool content = true; bool verbose = true; };
Config g_config;
int64_t g_next_config = 0;

Config ReadConfig() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Now() < g_next_config) return g_config;
    g_next_config = Now()+250000000;
    FILE* file = std::fopen("/data/local/tmp/genie.conf", "re");
    if (!file) file = std::fopen("/data/adb/genie.conf", "re");
    if (!file) return g_config;
    Config next;
    char line[256], key[64], value[32];
    while (std::fgets(line, sizeof(line), file)) {
        if (char* comment = std::strchr(line, '#')) *comment = '\0';
        if (std::sscanf(line, " %63[^= \t] = %31s", key, value) != 2) continue;
        const bool on = std::strcmp(value,"1")==0 || std::strcmp(value,"true")==0;
        const bool off = std::strcmp(value,"0")==0 || std::strcmp(value,"false")==0;
        if (!on && !off) continue;
        if (!std::strcmp(key,"enabled")) next.enabled = on;
        else if (!std::strcmp(key,"content_enabled")) next.content = on;
        else if (!std::strcmp(key,"verbose")) next.verbose = on;
    }
    std::fclose(file);
    g_config = next;
    return next;
}

// Verified ROM libc++ map and SpringProperty layouts. No foreign allocations or
// ownership changes; only the submitted parameters' scalar values are inspected.
const unsigned char* Find(const void* reference, size_t root, const char* key) {
    if (!reference) return nullptr;
    const auto* object = *static_cast<const unsigned char* const*>(reference);
    if (!object) return nullptr;
    const auto* node = *reinterpret_cast<const unsigned char* const*>(object+root);
    const size_t length = std::strlen(key);
    for (unsigned depth=0; node && depth<64; ++depth) {
        const unsigned tag = node[0x20];
        const size_t size = tag&1 ? *reinterpret_cast<const size_t*>(node+0x28) : tag>>1;
        if (size>256) return nullptr;
        const char* data = tag&1 ? *reinterpret_cast<const char* const*>(node+0x30) :
            reinterpret_cast<const char*>(node+0x21);
        if (!data) return nullptr;
        int comparison = std::memcmp(key,data,length<size?length:size);
        if (!comparison) comparison=length<size?-1:length>size?1:0;
        if (!comparison) return node+0x38;
        node=*reinterpret_cast<const unsigned char* const*>(node+(comparison>0?8:0));
    }
    return nullptr;
}
float Fixed(const void* parameters,const char* key) {
    const auto* value=Find(parameters,0x30,key);
    float result=0;
    if(value) std::memcpy(&result,value,sizeof(result));
    return result;
}

struct Property {
    float start = 0;
    float end = 0;
    float velocity = 0;
    float delay = 0;
    bool valid = false;
};
Property Spring(const void* parameters, const char* key) {
    const auto* value = Find(parameters,0x18,key);
    const auto* property = value ? *reinterpret_cast<const unsigned char* const*>(value) : nullptr;
    if (!property) return {};
    Property result;
    std::memcpy(&result.start,property+0x10,4);
    std::memcpy(&result.end,property+0x14,4);
    std::memcpy(&result.velocity,property+0x1c,4);
    std::memcpy(&result.delay,property+0x18,4);
    result.valid=std::isfinite(result.start)&&std::isfinite(result.end)&&
        std::isfinite(result.velocity)&&std::isfinite(result.delay);
    return result;
}

std::array<float, 2> FitSize(float width, float height, float ratio) {
    return {std::min(width, height / ratio), std::min(height, width * ratio)};
}

using Last = genie::AnimationSource;
std::array<Last,32> g_last{};
size_t g_next = 0;

struct IconBuffer {
    void* surface=nullptr;
    int32_t parent=-1;
    int width=0,height=0;
    int32_t animation_id=-1;
    int64_t request_ns=0;
};
std::array<IconBuffer,16> g_icons{};
size_t g_next_icon=0;

void* BlastUpdate(void* queue,const void* reference,uint32_t width,uint32_t height,int32_t format) {
    void* result=g_blast_update(queue,reference,width,height,format);
    const void* surface=reference?*static_cast<const void* const*>(reference):nullptr;
    if(g_icon_ready && surface && g_name && width>0 && height>0 && width<=1024 && height<=1024) {
        const std::string& name=g_name(surface);
        int parent=-1;
        if(std::sscanf(name.c_str(),"[BBQ] FloatingIcon_all#%d",&parent)==1 && parent>=0) {
            std::lock_guard<std::mutex> lock(g_mutex);
            IconBuffer* entry=nullptr;
            for(auto& icon:g_icons) if(icon.parent==parent) {entry=&icon;break;}
            if(!entry) entry=&g_icons[g_next_icon++%g_icons.size()];
            if(entry->surface!=surface) {
                genie::ReleaseIconSurface(entry->surface,entry);
                entry->surface=const_cast<void*>(surface);
                entry->animation_id=-1;
                entry->request_ns=0;
                genie::RetainIconSurface(entry->surface,entry);
            }
            entry->parent=parent;entry->width=width;entry->height=height;
            __android_log_print(ANDROID_LOG_INFO,kTag,"icon BBQ parent=%d layer=%d extent=%ux%u",parent,
                g_layer(surface),width,height);
        }
    }
    return result;
}

struct Anchor { int32_t leash=-1,container=-1,task=-1; };
std::array<Anchor,32> g_anchors{};
size_t g_next_anchor=0;

Anchor ReadAnchor(const void* control) {
    Anchor result;
    if(!g_name || !control) return result;
    const std::string& name=g_name(control);
    if(name.size()>512) return result;
    int task=-1,container=-1;
    if(std::sscanf(name.c_str(),"Surface(name=Task=%d#%d)",&task,&container)==2 ||
       std::sscanf(name.c_str(),"Task=%d#%d",&task,&container)==2) {
        if(task>=0 && container>=0) {result.task=task;result.container=container;}
    }
    return result;
}

void Observe(const void* surface, const void* parameters, bool adjust) {
    if (!parameters || !surface || !g_layer) return;
    const auto* object = *static_cast<const unsigned char* const*>(parameters);
    const void* control = *static_cast<const void* const*>(surface);
    if (!object || !control) return;
    int32_t type,adjust_type,animation_id;
    std::memcpy(&type,object+0x40,4);
    std::memcpy(&adjust_type,object+0x44,4);
    std::memcpy(&animation_id,object+0x48,4);
    const int32_t id = g_layer(control);
    Last cached;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(const auto& slot:g_last) if(slot.valid && slot.transition.leash_layer==id) {cached=slot;break;}
    }
    if (type!=2 && !(adjust && type==-1 && adjust_type==1 && cached.valid)) return;
    if(Fixed(parameters,"isNoSourceAnim")!=0 || Fixed(parameters,"bigIcon")!=0) return;
    // Match SF's target-ID ordering before assigning a new local timestamp.
    const bool continuing=adjust && cached.valid && cached.supported && cached.admitted;
    if (adjust && cached.valid && animation_id<=cached.animation_id) return;
    const Config config = ReadConfig();
    if (!config.enabled || !config.content) {
        genie::QueueCancel(-1,Now());
        genie::CancelIconRenderer();
        return;
    }
    const auto x=Spring(parameters,"centerX"), y=Spring(parameters,"centerY");
    const auto w=Spring(parameters,"width"), h=Spring(parameters,"height");
    const auto ratio=Spring(parameters,"ratio");
    const auto radius=Spring(parameters,"radius");
    const auto corner=Spring(parameters,"cornerTLX");
    const auto radius_ratio=Spring(parameters,"radiusRatio");
    if (!x.valid || !y.valid || !w.valid || !h.valid ||
        !ratio.valid || ratio.start<=0 || ratio.end<=0 ||
        w.start<=0 || w.end<=0 || h.start<=0 || h.end<=0) return;
    const auto [start_width,start_height]=FitSize(w.start,h.start,ratio.start);
    const auto [end_width,end_height]=FitSize(w.end,h.end,ratio.end);
    // Portrait-only admission: a landscape host keeps its complete OEM motion.
    if(!genie::model::PortraitWindow(start_width,start_height,end_width,end_height)) {
        if(config.verbose) __android_log_print(ANDROID_LOG_INFO,kTag,
            "portrait guard keeps stock animation start=%.1fx%.1f end=%.1fx%.1f",
            start_width,start_height,end_width,end_height);
        return;
    }
    const float start_area=start_width*start_height, end_area=end_width*end_height;
    // The OEM can start a reversal with the same ID. A same-direction repeat,
    // however, must preserve its epoch and never reread our rewritten springs.
    if(!adjust && cached.valid && (animation_id<cached.animation_id ||
       (animation_id==cached.animation_id && (end_area<start_area)==cached.transition.closing))) return;
    if (!std::isfinite(start_area) || !std::isfinite(end_area) ||
        (!(adjust && cached.valid) && std::max(start_area,end_area)<4*std::min(start_area,end_area))) return;
    genie::Transition request;
    request.leash_layer=id;
    request.container_layer=id;
    if (g_name) {
        const std::string& name=g_name(control);
        int task=-1,container=-1;
        // Shell wraps the actual task in an external transition leash which is
        // absent from WMS's Java ancestor tree. Its verified name carries the
        // wrapped Task SC identity; WMS validates BOTH against its own objects.
        if (name.size()<512 && name.find("_transition-leash")!=std::string::npos &&
            std::sscanf(name.c_str(),"Surface(name=Task=%d",&task)==1 && task>=0) {
            request.task_id=task;
            request.container_layer=-1;
            if(std::sscanf(name.c_str(),"Surface(name=Task=%d#%d)",&task,&container)==2 && container>=0)
                request.container_layer=container;
        }
        if(config.verbose) __android_log_print(ANDROID_LOG_INFO,kTag,
            "source leash=%d task=%d container=%d name=%.200s",id,request.task_id,request.container_layer,name.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(const auto& anchor:g_anchors) if(anchor.leash==id && anchor.task>=0) {
            request.container_layer=anchor.container;
            request.task_id=anchor.task;
            break;
        }
    }
    request.closing=end_area<start_area;
    request.event_ns=Now();
    request.geometry={x.start-start_width/2,y.start-start_height/2,start_width,start_height,
                      x.end-end_width/2,y.end-end_height/2,end_width,end_height,
                      x.velocity,y.velocity,corner.valid?corner.start*std::hypot(start_width,start_height):radius.start,
                      corner.valid?corner.end*std::hypot(end_width,end_height):radius.end};
    if(config.verbose) __android_log_print(ANDROID_LOG_INFO,kTag,
        "corner layer=%d radius=%.2f->%.2f corner=%.2f->%.2f ratio=%.3f->%.3f",
        id,radius.start,radius.end,corner.start,corner.end,radius_ratio.start,radius_ratio.end);
    const genie::model::Rect presentation_start{request.geometry[0],request.geometry[1],
        request.geometry[2],request.geometry[3]};
    if (adjust && cached.valid) {
        // A live host is already expanded to the window aspect. Keep the true
        // icon endpoint separate from that current presentation on a reversal.
        const double full_area=cached.window.width*cached.window.height;
        if(full_area>0) request.closing=end_area<0.5*full_area;
        if(!request.closing) {
            const size_t icon=cached.transition.closing?4:0;
            std::copy_n(cached.transition.geometry.begin()+icon,4,request.geometry.begin());
            request.geometry[10]=cached.transition.geometry[cached.transition.closing?11:10];
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Last* previous=nullptr;
        for(auto& slot:g_last) if(slot.valid && slot.transition.leash_layer==id) {previous=&slot;break;}
        if (previous && (animation_id<previous->animation_id ||
            (animation_id==previous->animation_id && (adjust || request.closing==previous->transition.closing)))) return;
        if (!previous) previous=&g_last[g_next++%g_last.size()];
        previous->window=request.closing?cached.window:
            genie::model::Rect{request.geometry[4],request.geometry[5],request.geometry[6],request.geometry[7]};
        previous->presentation_start=presentation_start;
        previous->host_velocity={x.velocity,y.velocity,w.velocity};
        previous->host_delay={x.delay,y.delay,w.delay};
        previous->host_transaction_id=0;
        previous->adjusting=continuing;
        previous->transition=request;
        previous->animation_id=animation_id;
        previous->valid=true;
        previous->supported=true;
        previous->admitted=continuing;
        previous->retry_pending=false;
        previous->preparing=false;
    }
    if (config.verbose) {
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "%s layer=%d direction=%s rect=(%.1f,%.1f %.1fx%.1f)->(%.1f,%.1f %.1fx%.1f) velocity=(%.2f,%.2f)",
            adjust?"adjust":"start",id,request.closing?"close":"open",
            request.geometry[0],request.geometry[1],w.start,h.start,
            request.geometry[4],request.geometry[5],w.end,h.end,x.velocity,y.velocity);
    }
    // Pre-start's exact preview is primed in the same Launcher transaction
    // before the custom viewport. The restricted prepare RPC publishes this
    // geometry to the central worker without taking the WM lock.
    // A rejected initial publication may become ready before a later adjust.
    // Retry its first-frame proof instead of permanently leaving it on stock.
    if(!continuing && (request.task_id>=0 ||
       (g_name && g_name(control).find("Pre startup opening leash")==0))) return;
    genie::QueueTransition(request);
}
void SetSpring(const void* parameters,const char* key,float start,float end) {
    const auto* value=Find(parameters,0x18,key);
    auto* property=value?*reinterpret_cast<unsigned char* const*>(value):nullptr;
    if(!property) return;
    std::memcpy(property+0x10,&start,4);
    std::memcpy(property+0x14,&end,4);
}
void SetVelocity(const void* parameters,const char* key,float velocity) {
    const auto* value=Find(parameters,0x18,key);
    auto* property=value?*reinterpret_cast<unsigned char* const*>(value):nullptr;
    if(property) std::memcpy(property+0x1c,&velocity,4);
}
void SetDelay(const void* parameters,const char* key,float delay) {
    const auto* value=Find(parameters,0x18,key);
    auto* property=value?*reinterpret_cast<unsigned char* const*>(value):nullptr;
    if(property) std::memcpy(property+0x18,&delay,4);
}
void SetFixed(const void* parameters,const char* key,float number) {
    auto* value=const_cast<unsigned char*>(Find(parameters,0x30,key));
    if(value) std::memcpy(value,&number,4);
}
void SuppressFloatingIcon(void* transaction,const void* control,const void* parameters,
                          int32_t layer,const Last& snapshot,const char* reason,uint64_t transaction_id) {
    if(!control || !g_name || g_name(control).find("FloatingIcon_all")!=0) return;
    // Suppress only the temporary clone. Zero both the initial alpha and its
    // stock spring so a later SF animation update cannot reveal it again.
    SetSpring(parameters,"alpha",0,0);
    SetVelocity(parameters,"alpha",0);
    SetDelay(parameters,"alpha",0);
    if(transaction) ASurfaceTransaction_setBufferAlpha(
        static_cast<ASurfaceTransaction*>(transaction),
        reinterpret_cast<ASurfaceControl*>(const_cast<void*>(control)),0);
    __android_log_print(ANDROID_LOG_WARN,kTag,
        "icon suppressed parent=%d source=%d epoch=%lld reason=%s hostTxn=%llu sourceTxn=%llu",layer,
        snapshot.transition.leash_layer,static_cast<long long>(snapshot.transition.event_ns),reason,
        static_cast<unsigned long long>(transaction_id),
        static_cast<unsigned long long>(snapshot.host_transaction_id));
}
void TuneParent(void* transaction,const void* surface,const void* parameters) {
    const Config config=ReadConfig();
    if (!config.enabled || !config.content || !parameters) return;
    const auto* object=*static_cast<const unsigned char* const*>(parameters);
    if (!object) return;
    int32_t type=0,animation_id=0;
    std::memcpy(&type,object+0x40,4);
    std::memcpy(&animation_id,object+0x48,4);
    if (type!=1 && type!=2 && type!=-1) return;
    if(type==1) {
        if(surface && g_name) {
            const void* control=*static_cast<const void* const*>(surface);
            const auto x=Spring(parameters,"centerX"),y=Spring(parameters,"centerY");
            const auto alpha=Spring(parameters,"alpha");
            __android_log_print(ANDROID_LOG_INFO,kTag,
                "icon source=%s center=(%.1f,%.1f)->(%.1f,%.1f) alpha=%.2f->%.2f",control?g_name(control).c_str():"?",
                x.start,y.start,x.end,y.end,alpha.start,alpha.end);
        }
    }
    if(Fixed(parameters,"isNoSourceAnim")!=0 || Fixed(parameters,"bigIcon")!=0) return;
    const auto w=Spring(parameters,"width"),h=Spring(parameters,"height");
    const auto ratio=Spring(parameters,"ratio");
    if (!w.valid || !h.valid || !ratio.valid || ratio.start<=0 || ratio.end<=0 ||
        w.start<=0 || h.start<=0 || w.end<=0 || h.end<=0) return;
    const auto [start_width,start_height]=FitSize(w.start,h.start,ratio.start);
    const auto [end_width,end_height]=FitSize(w.end,h.end,ratio.end);
    // Portrait-only admission: a landscape host keeps its complete OEM motion.
    if(!genie::model::PortraitWindow(start_width,start_height,end_width,end_height)) {
        if(config.verbose) __android_log_print(ANDROID_LOG_INFO,kTag,
            "portrait guard keeps stock host animation start=%.1fx%.1f end=%.1fx%.1f type=%d",
            start_width,start_height,end_width,end_height,type);
        return;
    }
    const bool closing=end_width*end_height<start_width*start_height;
    // Replaces the former 0.8/1.25 aspect window: endpoints must be portrait.
    // Preserve native listeners/IDs while replacing the app's crop viewport.
    // The visible contour is produced by the same mesh as the content.
    if(surface && g_layer) {
        const void* control=*static_cast<const void* const*>(surface);
        const int32_t layer=control?g_layer(control):-1;
        const bool is_icon=type==1 || (control && g_name && g_name(control).find("FloatingIcon_all")==0);
        Last snapshot;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if(is_icon) {
                if(const auto* source=genie::FindIconSource(g_last,animation_id,Now())) snapshot=*source;
            } else {
                for(const auto& entry:g_last) if(entry.valid && entry.transition.leash_layer==layer) {
                    snapshot=entry;break;
                }
            }
        }
        if(!snapshot.valid || snapshot.preparing ||
           (!snapshot.supported && !snapshot.retry_pending) ||
           (!is_icon && snapshot.animation_id!=animation_id)) return;
        // A deferred publication has no renderer session to keep alive. Do not
        // revive its original motion after the renderer's ordinary deadline.
        if(snapshot.retry_pending && Now()-snapshot.transition.event_ns>=1500000000LL) return;
        const auto& geometry=snapshot.transition.geometry;
        const size_t small=snapshot.transition.closing?4:0;
        const size_t full=snapshot.transition.closing?0:4;
        const genie::model::Rect icon{geometry[small],geometry[small+1],geometry[small+2],geometry[small+3]};
        genie::model::Rect window=snapshot.window.width>0?snapshot.window:
            genie::model::Rect{geometry[full],geometry[full+1],geometry[full+2],geometry[full+3]};
        const bool prestart=g_name && g_name(control).find("Pre startup opening leash")==0;
        bool prepared_now=false;
        if(!is_icon && !snapshot.admitted && (snapshot.transition.task_id>=0 || prestart)) {
            // Reserve this generation across the synchronous Binder call. An
            // icon or nested host callback must not publish unprepared geometry.
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                bool reserved=false;
                for(auto& source:g_last) if(source.valid &&
                    source.transition.event_ns==snapshot.transition.event_ns &&
                    source.transition.leash_layer==layer && source.animation_id==animation_id &&
                    !source.preparing && !source.admitted &&
                    (source.supported || source.retry_pending)) {
                    source.preparing=true;
                    source.supported=false;
                    reserved=true;
                    break;
                }
                if(!reserved) return;
            }
            const auto result=!snapshot.transition.closing && prestart ?
                (genie::PrepareOpeningPreview(transaction,const_cast<void*>(control),
                    static_cast<int>(window.width),static_cast<int>(window.height),snapshot.transition) ?
                    genie::GroupPreparation::Ready : genie::GroupPreparation::Rejected) :
                genie::PrepareContentGroupStatus(transaction,const_cast<void*>(control),&window,snapshot.transition);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                bool current=false;
                for(auto& source:g_last) if(source.valid &&
                    source.transition.event_ns==snapshot.transition.event_ns &&
                    source.transition.leash_layer==layer && source.animation_id==animation_id) {
                    if(result!=genie::GroupPreparation::Ready) {
                        source.preparing=false;
                        source.retry_pending=result==genie::GroupPreparation::Deferred;
                    }
                    current=true;
                    break;
                }
                // A nested newer generation owns the host now. Its publication
                // guards also reject the obsolete transaction merged by Binder.
                if(!current || result!=genie::GroupPreparation::Ready) return;
            }
            prepared_now=true;
        }
        const uint64_t transaction_id=transaction && g_transaction_id?g_transaction_id(transaction):0;
        genie::model::Rect minimized;
        if(!genie::model::HostViewportForIcon(icon,window,&minimized)) {
            if(prepared_now) {
                std::lock_guard<std::mutex> lock(g_mutex);
                for(auto& source:g_last) if(source.valid &&
                    source.transition.event_ns==snapshot.transition.event_ns &&
                    source.transition.leash_layer==layer) source.preparing=false;
            }
            return;
        }
        if(is_icon) {
            // OEM can build App and icon in separate transactions and merge
            // them before presentation. Construction IDs are diagnostic only;
            // the server's exact request and capability proof authorize joining.
            // Keep an independent capability lease while the server registers
            // this buffer. Never hold the launcher registry lock across Binder.
            IconBuffer lease;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for(const auto& source:g_icons) if(source.surface && source.parent==layer) {
                    lease=source;
                    genie::RetainIconSurface(lease.surface,&lease);
                    break;
                }
            }
            const bool rendering=lease.surface && genie::PrepareIcon(transaction,lease.surface,
                const_cast<void*>(control),lease.width,lease.height,
                static_cast<int>(window.width),static_cast<int>(window.height),snapshot.transition);
            if(rendering) {
                std::lock_guard<std::mutex> lock(g_mutex);
                for(auto& source:g_icons) if(source.surface==lease.surface && source.parent==layer) {
                    // A nested callback may have replaced or advanced this icon.
                    // Do not let an older registration retag that newer epoch.
                    if(source.request_ns<=snapshot.transition.event_ns) {
                        source.animation_id=animation_id;
                        source.request_ns=snapshot.transition.event_ns;
                    }
                    break;
                }
            }
            genie::ReleaseIconSurface(lease.surface,&lease);
            if(!rendering) {
                SuppressFloatingIcon(transaction,control,parameters,layer,snapshot,
                    lease.surface?"registration-rejected":"missing-child",transaction_id);
                return;
            }
        }
        const genie::model::Rect original_start{geometry[0],geometry[1],geometry[2],geometry[3]};
        const auto start=snapshot.adjusting?snapshot.presentation_start:
            (snapshot.transition.closing?original_start:minimized);
        const auto end=snapshot.transition.closing?minimized:window;
        SetSpring(parameters,"centerX",start.left+start.width/2,end.left+end.width/2);
        SetSpring(parameters,"centerY",start.top+start.height/2,end.top+end.height/2);
        SetSpring(parameters,"width",start.width,end.width);
        SetSpring(parameters,"height",start.height,end.height);
        const float aspect=window.height/window.width;
        SetVelocity(parameters,"centerX",snapshot.host_velocity[0]);
        SetVelocity(parameters,"centerY",snapshot.host_velocity[1]);
        SetVelocity(parameters,"width",snapshot.host_velocity[2]);
        SetVelocity(parameters,"height",snapshot.host_velocity[2]*aspect);
        SetDelay(parameters,"centerX",snapshot.host_delay[0]);
        SetDelay(parameters,"centerY",snapshot.host_delay[1]);
        SetDelay(parameters,"width",snapshot.host_delay[2]);
        SetDelay(parameters,"height",snapshot.host_delay[2]);
        SetSpring(parameters,"ratio",aspect,aspect);
        SetVelocity(parameters,"ratio",0);
        SetDelay(parameters,"ratio",0);
        SetFixed(parameters,"needDrawWindowMask",0);
        for(const char* key:{"radius","radiusRatio","cornerTLX","cornerTLY","cornerTRX","cornerTRY",
                            "cornerBRX","cornerBRY","cornerBLX","cornerBLY"}) {
            SetSpring(parameters,key,0,0);
            SetVelocity(parameters,key,0);
            SetDelay(parameters,key,0);
        }
        if(!is_icon) {
            std::lock_guard<std::mutex> lock(g_mutex);
            for(auto& source:g_last) if(source.valid &&
                source.transition.event_ns==snapshot.transition.event_ns &&
                source.transition.leash_layer==snapshot.transition.leash_layer) {
                source.host_transaction_id=transaction_id;
                source.window=window;
                source.admitted=true;
                source.retry_pending=false;
                source.preparing=false;
                source.supported=true;
                break;
            }
        }
        if(prepared_now) genie::QueueTransition(snapshot.transition);
        __android_log_print(ANDROID_LOG_INFO,kTag,"mesh viewport layer=%d animation=%d source=%d epoch=%lld hostTxn=%llu start=%.1fx%.1f end=%.1fx%.1f",layer,
            animation_id,snapshot.transition.leash_layer,static_cast<long long>(snapshot.transition.event_ns),
            static_cast<unsigned long long>(transaction_id),start.width,start.height,end.width,end.height);
    }
    constexpr const char* keys[]={"centerX","centerY","width","height","ratio",
        "radius","radiusRatio","cornerTLX","cornerTLY","cornerTRX","cornerTRY",
        "cornerBRX","cornerBRY","cornerBLX","cornerBLY"};
    for (const char* key:keys) {
        const auto* value=Find(parameters,0x18,key);
        auto* property=value?*reinterpret_cast<unsigned char* const*>(value):nullptr;
        if (!property) continue;
        const float damping=1.0f, response=closing?0.336f:0.24f;
        std::memcpy(property+0x20,&damping,4);
        std::memcpy(property+0x24,&response,4);
    }
}
void* Start(void* transaction,const void* surface,const void* parameters) {
    Observe(surface,parameters,false);
    TuneParent(transaction,surface,parameters);
    return g_start(transaction,surface,parameters);
}
void* Adjust(void* transaction,const void* surface,const void* parameters) {
    Observe(surface,parameters,true);
    TuneParent(transaction,surface,parameters);
    return g_adjust(transaction,surface,parameters);
}
void* Reparent(void* transaction,const void* child,const void* parent) {
    if(child && parent && g_name && g_layer) {
        const void* child_control=*static_cast<const void* const*>(child);
        const void* parent_control=*static_cast<const void* const*>(parent);
        if(child_control && parent_control &&
           g_name(parent_control).find("Pre startup opening leash") == 0) {
            const Anchor anchor=ReadAnchor(child_control);
            if(anchor.task>=0 && ReadConfig().enabled && ReadConfig().content) {
                const int32_t parent_id=g_layer(parent_control);
                genie::Transition updated;
                bool dispatch=false;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_anchors[g_next_anchor++%g_anchors.size()]={parent_id,anchor.container,anchor.task};
                    for(auto& last:g_last) {
                        if(last.valid && last.supported && last.transition.leash_layer==parent_id &&
                           Now()-last.transition.event_ns<1500000000LL) {
                            last.transition.container_layer=anchor.container;
                            last.transition.task_id=anchor.task;
                            updated=last.transition;
                            dispatch=true;
                            break;
                        }
                    }
                }
                __android_log_print(ANDROID_LOG_INFO,kTag,
                    "prestart anchor leash=%d task=%d container=%d dispatch=%d",
                    parent_id,anchor.task,anchor.container,dispatch);
                if(dispatch) genie::QueueTransition(updated);
            }
        }
    }
    return g_reparent(transaction,child,parent);
}
void* Stop(void* transaction,const void* surface,int32_t animation_id) {
    if (surface && g_layer) {
        const void* control=*static_cast<const void* const*>(surface);
        const int32_t id=control?g_layer(control):-1;
        bool cancel=false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            // Icon buffers share the central App owner's lifetime. An icon's
            // stock completion must not cancel or detach that shared mesh.
            for(auto& slot:g_last) {
                if(slot.valid && slot.transition.leash_layer==id && slot.animation_id==animation_id) {
                    slot.valid=false;
                    cancel=true;
                    break;
                }
            }
        }
        if(cancel) genie::QueueCancel(id,Now());
    }
    return g_stop(transaction,surface,animation_id);
}
void Install(void* handle) {
    if (!handle || g_installed.exchange(true)) return;
    void* start=dlsym(handle,kStart);
    void* adjust=dlsym(handle,kAdjust);
    void* stop=dlsym(handle,kStop);
    void* reparent=dlsym(handle,kReparent);
    void* blast=dlsym(handle,kBlastUpdate);
    g_layer=reinterpret_cast<LayerFn>(dlsym(handle,"_ZNK7android14SurfaceControl10getLayerIdEv"));
    g_name=reinterpret_cast<NameFn>(dlsym(handle,"_ZNK7android14SurfaceControl7getNameEv"));
    g_transaction_id=reinterpret_cast<TransactionIdFn>(dlsym(handle,
        "_ZN7android21SurfaceComposerClient11Transaction5getIdEv"));
    if (!start || !adjust || !stop || !reparent || !blast || !g_layer || !g_name) {
        __android_log_print(ANDROID_LOG_WARN,kTag,"required launcher interfaces missing; keeping stock");
        return;
    }
    void* utils=dlopen("libutils.so",RTLD_NOW|RTLD_NOLOAD);
    const char* reason=nullptr;
    const bool compatible=genie::abi::ProbeAnimation(handle,utils?utils:handle,&reason);
    if(utils) dlclose(utils);
    if(!compatible) {
        __android_log_print(ANDROID_LOG_WARN,kTag,"%s; keeping stock",reason);
        return;
    }
    int status=g_api->hookFunc(start,reinterpret_cast<void*>(Start),reinterpret_cast<void**>(&g_start));
    if (status || !g_start) {
        if (!status) g_api->unhookFunc(start);
        return;
    }
    status=g_api->hookFunc(adjust,reinterpret_cast<void*>(Adjust),reinterpret_cast<void**>(&g_adjust));
    if (status || !g_adjust) {
        if (!status) g_api->unhookFunc(adjust);
        g_api->unhookFunc(start);
        return;
    }
    status=g_api->hookFunc(stop,reinterpret_cast<void*>(Stop),reinterpret_cast<void**>(&g_stop));
    if(status || !g_stop) {
        if(!status) g_api->unhookFunc(stop);
        g_api->unhookFunc(adjust);
        g_api->unhookFunc(start);
        return;
    }
    status=g_api->hookFunc(reparent,reinterpret_cast<void*>(Reparent),reinterpret_cast<void**>(&g_reparent));
    if(status || !g_reparent) {
        if(!status) g_api->unhookFunc(reparent);
        g_api->unhookFunc(stop);
        g_api->unhookFunc(adjust);
        g_api->unhookFunc(start);
        return;
    }
    status=g_api->hookFunc(blast,reinterpret_cast<void*>(BlastUpdate),reinterpret_cast<void**>(&g_blast_update));
    if(status || !g_blast_update) {
        if(!status) g_api->unhookFunc(blast);
        g_api->unhookFunc(reparent);
        g_api->unhookFunc(stop);
        g_api->unhookFunc(adjust);
        g_api->unhookFunc(start);
        return;
    }
    // Warm only after all launcher capabilities have been accepted. native_init
    // must return before shader Binder registration and the renderer's self-load.
    std::thread([] {
        g_icon_ready.store(genie::WarmIconRenderer(),std::memory_order_release);
    }).detach();
    __android_log_print(ANDROID_LOG_INFO,kTag,"central Genie launcher hooks installed pid=%d uid=%d",getpid(),getuid());
}
void OnLoaded(const char* name,void* handle) {
    if (name && std::strstr(name,"libgui.so")) Install(handle);
}
}  // namespace

bool genie::LauncherAnimationEnabled() {
    const auto config=ReadConfig();
    return config.enabled && config.content;
}

extern "C" __attribute__((visibility("default"),unused))
Loaded native_init(const NativeAPIEntries* entries) {
    if (!entries || !entries->hookFunc || !entries->unhookFunc) return nullptr;
    // LSPosed can reenter native_init when the renderer pins its own library.
    // Never recurse into renderer initialization while its mutex is held.
    if(g_native_entered.exchange(true,std::memory_order_acq_rel)) return nullptr;
    // Native init also runs for a module scoped to system_server. The Java
    // central entry owns that process; never intercept its libgui transactions.
    if (getuid()<10000) return nullptr;
    if (!genie::CheckHyperOS(kTag)) return nullptr;
    g_api=entries;
    void* handle=dlopen("libgui.so",RTLD_NOW|RTLD_NOLOAD);
    if (handle) {Install(handle);dlclose(handle);}
    return OnLoaded;
}
