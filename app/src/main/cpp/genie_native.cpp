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
#include <chrono>
#include <cmath>
#include <condition_variable>
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
constexpr char kHomeParams[] =
    "_ZN7android19SFAnimationListener23onSFAnimationHomeParamsENS_3gui14CallAnimParamsEi";
constexpr char kSetCallback[] =
    "_ZN7android14SurfaceControl22setSFAnimationCallbackERKNS_2wpINS_19SFAnimationListener19SFAnimationCallbackEEE";

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
using HomeParamsFn = void (*)(void*, const void*, int32_t);
using SetCallbackFn = void (*)(void*, const void*);
using HostCommitFn = void (*)(ASurfaceTransaction*,void*,ASurfaceTransaction_OnCommit);
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
HomeParamsFn g_home_params = nullptr;
SetCallbackFn g_set_callback = nullptr;
HostCommitFn g_host_commit = nullptr;
const void* g_listener_vtable = nullptr;
const void* g_params_vtable = nullptr;
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
    int32_t source_leash=-1;
    int64_t request_ns=0;
    void* parent_control=nullptr;
    void* readback_child=nullptr;
    void* listener=nullptr;
    bool closing=false;
    bool readback_logged=false;
    // Set when an exact completed closing generation lands over an older
    // binding: the retained child converges to that real landed endpoint.
    bool landed=false;
    // Last generation whose morph mask was re-primed without a full
    // registration; bounds the stop/close-observation re-primes.
    int64_t mask_epoch=0;
};
std::array<IconBuffer,16> g_icons{};
size_t g_next_icon=0;

void* ReadBoundListener(const void* control) {
    if(!control || !g_listener_vtable) return nullptr;
    // Validate this ROM's listener layout only when acquiring a capability.
    // Retaining the parent keeps these objects alive without per-frame maps IO.
    genie::abi::detail::Readable memory;
    const auto address=reinterpret_cast<uintptr_t>(control);
    uintptr_t listener=0,table=0,callback=0,callback_table=0;
    if(!memory.Read(address+0xa8,listener) || !listener ||
       !memory.Read(listener,table) || table!=reinterpret_cast<uintptr_t>(g_listener_vtable) ||
       !memory.Read(listener+0x28,callback) || !callback ||
       !memory.Read(callback,callback_table) || !callback_table) return nullptr;
    return reinterpret_cast<void*>(listener);
}

struct ReadbackEndpoint {
    std::array<float,9> corners{};
    uint16_t present=0;
};
struct AppReadback {
    void* control=nullptr;
    int32_t layer=-1;
    int32_t source_leash=-1;
    int32_t animation_id=-1;
    int64_t request_ns=0;
    void* listener=nullptr;
    bool closing=false;
    bool readback_logged=false;
    ReadbackEndpoint endpoint;
    genie::PublicationKind kind=genie::PublicationKind::None;
    void* owned_host=nullptr;
    int32_t owned_layer=-1;
    bool host_started=false;
    bool host_committed=false;
    uintptr_t host_commit_token=0;
};
std::array<AppReadback,32> g_apps{};
size_t g_next_app=0;
// Tokens are never reused, including after a registry slot is evicted. A
// transaction discarded without apply owns no heap state through its callback.
uintptr_t g_next_host_commit_token=1;

ReadbackEndpoint ReadEndpoint(const void* parameters) {
    ReadbackEndpoint result;
    constexpr const char* keys[]={"radiusRatio","cornerTLX","cornerTLY","cornerTRX","cornerTRY",
        "cornerBRX","cornerBRY","cornerBLX","cornerBLY"};
    for(size_t i=0;i<std::size(keys);++i) {
        const auto property=Spring(parameters,keys[i]);
        if(property.valid) {
            result.corners[i]=property.end;
            result.present|=1u<<i;
        }
    }
    return result;
}

const Last* CurrentReadbackSource(int32_t leash,int32_t animation_id,int64_t request_ns,bool closing) {
    // Caller holds g_mutex. A stopped source retains its exact epoch until a
    // newer request or registry eviction, covering queued takeover callbacks.
    if(leash<0 || request_ns<=0 || !closing) return nullptr;
    const Last* latest=nullptr;
    for(const auto& source:g_last) if(source.transition.leash_layer==leash &&
        (!latest || source.transition.event_ns>latest->transition.event_ns)) latest=&source;
    if(!latest || latest->transition.event_ns!=request_ns ||
       latest->animation_id!=animation_id || !latest->transition.closing ||
       !latest->supported || !latest->admitted || latest->preparing || latest->retry_pending) return nullptr;
    return latest;
}

const Last* CurrentReadbackSource(const IconBuffer& icon) {
    if(!icon.surface || icon.surface!=icon.readback_child || !icon.parent_control || icon.parent<0) return nullptr;
    return CurrentReadbackSource(icon.source_leash,icon.animation_id,icon.request_ns,icon.closing);
}

const Last* LandedReadbackSource(const IconBuffer& icon) {
    // Caller holds g_mutex. An exact completed closing generation owns its
    // true landed endpoint for a bounded window, even when that generation
    // never acquired render admission. The geometry is real, never invented.
    if(!icon.landed || icon.request_ns<=0 || !icon.closing ||
       !icon.surface || icon.surface!=icon.readback_child || !icon.parent_control) return nullptr;
    const Last* latest=nullptr;
    for(const auto& source:g_last) if(source.transition.leash_layer==icon.source_leash &&
        (!latest || source.transition.event_ns>latest->transition.event_ns)) latest=&source;
    if(!latest || latest->transition.event_ns!=icon.request_ns ||
       latest->animation_id!=icon.animation_id || !latest->transition.closing ||
       latest->transition.event_ns>Now() || Now()-latest->transition.event_ns>=1500000000LL) return nullptr;
    return latest;
}

const Last* CurrentReadbackSource(const AppReadback& app) {
    if(!app.control || app.layer<0 || app.layer!=app.source_leash) return nullptr;
    return CurrentReadbackSource(app.source_leash,app.animation_id,app.request_ns,app.closing);
}

const AppReadback* ReadbackApp(const Last& source) {
    for(const auto& app:g_apps) if(app.control && app.source_leash==source.transition.leash_layer &&
        app.animation_id==source.animation_id && app.request_ns==source.transition.event_ns) return &app;
    return nullptr;
}

bool CurrentListener(void* control,int32_t layer,void* listener) {
    void* current=nullptr;
    std::memcpy(&current,static_cast<unsigned char*>(control)+0xa8,sizeof(current));
    return current==listener && g_layer && g_layer(control)==layer;
}

void SetCallback(void* control,const void* callback) {
    g_set_callback(control,callback);
    if(!control || !g_layer) return;
    const int32_t layer=g_layer(control);
    bool bound=false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(const auto& app:g_apps) if(app.control==control && app.layer==layer && CurrentReadbackSource(app)) {
            bound=true;break;
        }
        if(!bound) for(const auto& icon:g_icons) if(icon.parent_control==control && icon.parent==layer &&
            CurrentReadbackSource(icon)) {bound=true;break;}
    }
    if(!bound) return;
    void* listener=ReadBoundListener(control);
    std::lock_guard<std::mutex> lock(g_mutex);
    for(auto& app:g_apps) if(app.control==control && app.layer==layer && CurrentReadbackSource(app)) {
        if(app.listener!=listener) app.readback_logged=false;
        app.listener=listener;
        break;
    }
    for(auto& icon:g_icons) if(icon.parent_control==control && icon.parent==layer &&
        CurrentReadbackSource(icon)) {
        if(icon.listener!=listener) icon.readback_logged=false;
        icon.listener=listener;
        break;
    }
}

void HomeParams(void* listener,const void* parameters,int32_t id) {
    const Config config=ReadConfig();
    const void* table=nullptr;
    if(parameters) std::memcpy(&table,parameters,sizeof(table));
    bool normalize=false,log=false;
    const char* kind="app";
    int32_t parent=-1,source=-1;
    int64_t request=0;
    std::array<float,12> geometry{};
    ReadbackEndpoint endpoint;
    if(config.enabled && config.content && listener && parameters &&
       g_listener_vtable && g_params_vtable && table==g_params_vtable) {
        std::memcpy(&table,listener,sizeof(table));
        if(table==g_listener_vtable) {
            std::lock_guard<std::mutex> lock(g_mutex);
            for(auto& app:g_apps) if(app.listener==listener) {
                const auto* current=CurrentReadbackSource(app);
                if(!current || !CurrentListener(app.control,app.layer,listener)) continue;
                normalize=true;
                parent=app.layer;source=app.source_leash;request=app.request_ns;
                geometry=current->transition.geometry;
                endpoint=app.endpoint;
                log=config.verbose && !app.readback_logged;
                if(log) app.readback_logged=true;
                break;
            }
            if(!normalize) for(auto& icon:g_icons) if(icon.listener==listener) {
                const auto* current=CurrentReadbackSource(icon);
                if(!current) current=LandedReadbackSource(icon);
                if(!current || !CurrentListener(icon.parent_control,icon.parent,listener)) continue;
                normalize=true;kind="icon";
                parent=icon.parent;source=icon.source_leash;request=icon.request_ns;
                geometry=current->transition.geometry;
                if(const auto* app=ReadbackApp(*current)) endpoint=app->endpoint;
                log=config.verbose && !icon.readback_logged;
                if(log) icon.readback_logged=true;
                break;
            }
        }
    }
    if(!normalize) {
        g_home_params(listener,parameters,id);
        return;
    }
    // CallAnimParams is an indirect by-value object with a trivial destructor:
    // vptr followed by 28 floats. Launcher uses this private logical endpoint
    // for its fallback underlay; SF keeps the original animated presentation.
    alignas(8) std::array<unsigned char,0x78> copy{};
    std::memcpy(copy.data(),parameters,copy.size());
    const auto set=[&copy](size_t index,float value) {
        std::memcpy(copy.data()+8+index*sizeof(value),&value,sizeof(value));
    };
    set(0,geometry[4]+geometry[6]/2);
    set(2,geometry[5]+geometry[7]/2);
    set(4,geometry[6]);set(6,geometry[7]);set(8,1);set(10,0);
    set(12,geometry[11]);
    for(size_t index:{1,3,5,7,9,11,13,15}) set(index,0);
    // Preserve OEM endpoint units exactly instead of inferring corner ratios.
    if(endpoint.present&1u) set(14,endpoint.corners[0]);
    for(size_t i=1;i<endpoint.corners.size();++i) if(endpoint.present&(1u<<i)) set(15+i,endpoint.corners[i]);
    if(log) __android_log_print(ANDROID_LOG_INFO,kTag,
        "%s readback listener=%p layer=%d source=%d request=%lld closing=1 callback=%d endpoint=%.2f,%.2f %.2fx%.2f alpha=0",
        kind,listener,parent,source,static_cast<long long>(request),id,
        geometry[4],geometry[5],geometry[6],geometry[7]);
    g_home_params(listener,copy.data(),id);
}

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
                genie::ReleaseIconSurface(entry->parent_control,entry);
                entry->surface=const_cast<void*>(surface);
                entry->animation_id=-1;
                entry->source_leash=-1;
                entry->request_ns=0;
                entry->parent_control=nullptr;
                entry->readback_child=nullptr;
                entry->listener=nullptr;
                entry->closing=false;
                entry->readback_logged=false;
                entry->landed=false;
                entry->mask_epoch=0;
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
        // Every accepted source epoch must acquire its own publication and
        // closing host, including a reversal submitted through adjust.
        previous->admitted=false;
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
    if(request.task_id>=0 ||
       (g_name && g_name(control).find("Pre startup opening leash")==0)) return;
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
void SuppressBoundIconFallback(void* transaction,const void* surface,const void* parameters,const char* reason) {
    const void* control=surface?*static_cast<const void* const*>(surface):nullptr;
    if(!control || !g_layer || !g_name || g_name(control).find("FloatingIcon_all")!=0) return;
    const int32_t layer=g_layer(control);
    Last binding;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(const auto& icon:g_icons) if(icon.surface && icon.parent==layer &&
            icon.source_leash>=0 && icon.request_ns>0) {
            binding.transition.leash_layer=icon.source_leash;
            binding.transition.event_ns=icon.request_ns;
            break;
        }
    }
    if(binding.transition.event_ns<=0) return;
    // A successful registration leaves SELF on this exact buffer until OEM
    // removal. Stock aspect/crop springs cannot reuse that virtual viewport.
    // A replacement child resets its binding and retains ordinary stock motion.
    const uint64_t transaction_id=transaction && g_transaction_id?g_transaction_id(transaction):0;
    SuppressFloatingIcon(transaction,control,parameters,layer,binding,reason,transaction_id);
}
constexpr const char* kViewportSprings[]={"centerX","centerY","width","height","ratio",
    "radius","radiusRatio","cornerTLX","cornerTLY","cornerTRX","cornerTRY",
    "cornerBRX","cornerBRY","cornerBLX","cornerBLY"};

bool IndependentParameters(const void* source,const void* copy) {
    if(!copy || !*static_cast<const void* const*>(copy) ||
       *static_cast<const void* const*>(copy)==*static_cast<const void* const*>(source) ||
       !Spring(copy,"alpha").valid) return false;
    for(const char* key:kViewportSprings) {
        const auto* original=Find(source,0x18,key);
        const auto* cloned=Find(copy,0x18,key);
        if((original!=nullptr)!=(cloned!=nullptr)) return false;
        if(!original) continue;
        const auto* a=*reinterpret_cast<const unsigned char* const*>(original);
        const auto* b=*reinterpret_cast<const unsigned char* const*>(cloned);
        if(!a || !b || a==b || std::memcmp(a+0x10,b+0x10,0x18)!=0) return false;
    }
    const auto* alpha=Find(source,0x18,"alpha");
    const auto* cloned_alpha=Find(copy,0x18,"alpha");
    return alpha && cloned_alpha && *reinterpret_cast<const void* const*>(alpha)!=
        *reinterpret_cast<const void* const*>(cloned_alpha);
}

void TuneOwnedParameters(const void* source,const void* copy,bool starting) {
    for(const char* key:kViewportSprings) {
        const auto* original=Find(source,0x18,key);
        const auto* cloned=Find(copy,0x18,key);
        if(!original || !cloned) continue;
        const auto* a=*reinterpret_cast<const unsigned char* const*>(original);
        auto* b=*reinterpret_cast<unsigned char* const*>(cloned);
        if(a && b) std::memcpy(b+0x10,a+0x10,0x18);
    }
    SetFixed(copy,"needDrawWindowMask",0);
    // The independent host keeps the cloned OEM alpha curve even after the
    // source Task stops; no visibility override extends that fade.
    if(starting) {
        // An OEM adjust may omit the App type because its own host already
        // exists. A fresh owned host needs the ordinary App start contract.
        auto* object=*static_cast<unsigned char* const*>(copy);
        const int32_t app_type=2,adjust_type=0;
        std::memcpy(object+0x40,&app_type,sizeof(app_type));
        std::memcpy(object+0x44,&adjust_type,sizeof(adjust_type));
    }
}

bool ValidPublication(const genie::GroupPublication& publication,const Last& source,const void* control) {
    switch(publication.kind) {
        case genie::PublicationKind::NormalGroup:
            return !source.transition.closing && !publication.host;
        case genie::PublicationKind::RegisteredPreview:
            return !publication.host;
        case genie::PublicationKind::OwnedClosingGroup:
            return source.transition.closing && publication.host && publication.release && g_layer &&
                g_layer(publication.host)>=0 && g_layer(publication.host)!=g_layer(control);
        default:
            return false;
    }
}

// While the App's synchronous prepare owns this generation, freeze the host
// at its presented viewport/crop. The incoming OEM springs start at the gesture
// presentation: pinning end=start and clearing motion keeps the live mesh
// exactly where it is. Never touch alpha here. The hold is only the prepare
// gap: once the generation is older than that, the normal closing springs run
// from the same presented start so the motion continues without a hard cut.
void HoldPresentedSprings(void* parameters,bool gap_expired=false,
                          const genie::model::Rect* motion_end=nullptr) {
    for(const char* key:kViewportSprings) {
        const auto spring=Spring(parameters,key);
        if(!spring.valid || !std::isfinite(spring.start)) continue;
        SetSpring(parameters,key,spring.start,spring.start);
        SetVelocity(parameters,key,0);
        SetDelay(parameters,key,0);
    }
    if(gap_expired && motion_end && motion_end->width>0 && motion_end->height>0) {
        const auto width=Spring(parameters,"width"),height=Spring(parameters,"height");
        if(width.valid && height.valid) {
            SetSpring(parameters,"width",width.start,motion_end->width);
            SetSpring(parameters,"height",height.start,motion_end->height);
            const auto x=Spring(parameters,"centerX"),y=Spring(parameters,"centerY");
            SetSpring(parameters,"centerX",x.start,motion_end->left+motion_end->width/2);
            SetSpring(parameters,"centerY",y.start,motion_end->top+motion_end->height/2);
        }
    }
}
// The real prepare gap is tens of milliseconds; a failed generation must not
// freeze the host for the whole close.
constexpr int64_t kHoldGapNs=150000000LL;
// The OEM animation stop drops the custom morph mask from its surfaces. When a
// close is observed but its registration cannot arrive, re-prime the mask for
// the established custom icon in this same transaction: the presented morph
// survives the close and animates toward the landed square instead of exposing
// the raw CLAMP plate. The icon keeps its existing binding; this is a mask
// continuity action, not a new registration.
// Only the canonical BBQ child of this parent may be re-primed; a malformed
// or foreign buffer entry must never reach registration.
bool ValidIconChild(const IconBuffer& icon) {
    int child_parent=-1;
    return g_name && std::sscanf(g_name(icon.surface).c_str(),"[BBQ] FloatingIcon_all#%d",&child_parent)==1 &&
        child_parent==icon.parent;
}
void ReprimeIconMask(void* transaction,const Last& snapshot,int32_t animation_id) {
    if(!transaction || !g_icon_ready.load(std::memory_order_acquire) ||
       !snapshot.transition.closing) return;
    IconBuffer lease;void* parent=nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(auto& icon:g_icons) if(icon.surface && icon.surface==icon.readback_child &&
            icon.parent_control && ValidIconChild(icon) && icon.source_leash==snapshot.transition.leash_layer &&
            icon.request_ns>0 && icon.width>0 && icon.height>0 &&
            icon.mask_epoch!=snapshot.transition.event_ns &&
            g_layer(icon.parent_control)==icon.parent && g_layer(icon.surface)>=0) {
            lease=icon;parent=icon.parent_control;
            genie::RetainIconSurface(lease.surface,&lease);
            break;
        }
    }
    if(!lease.surface) return;
    const auto& geometry=snapshot.transition.geometry;
    const genie::model::Rect window=snapshot.window.width>0?snapshot.window:
        genie::model::Rect{geometry[0],geometry[1],geometry[2],geometry[3]};
    genie::PrepareIcon(transaction,lease.surface,parent,lease.width,lease.height,
        static_cast<int>(window.width),static_cast<int>(window.height),snapshot.transition);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(auto& current:g_icons) if(current.surface==lease.surface &&
            current.parent==lease.parent && current.mask_epoch!=snapshot.transition.event_ns) {
            current.mask_epoch=snapshot.transition.event_ns;
            current.animation_id=animation_id;
        }
    }
    genie::ReleaseIconSurface(lease.surface,&lease);
}
// Failed-generation hold: freeze inside the real prepare gap, otherwise let
// the closing springs run from the same presented start toward the landed
// viewport so the motion never freezes for the whole close.
void HoldForGeneration(void* parameters,const Last& snapshot) {
    const auto& geometry=snapshot.transition.geometry;
    const size_t small=snapshot.transition.closing?4:0;
    const size_t full=snapshot.transition.closing?0:4;
    const genie::model::Rect icon{geometry[small],geometry[small+1],geometry[small+2],geometry[small+3]};
    const genie::model::Rect window=snapshot.window.width>0?snapshot.window:
        genie::model::Rect{geometry[full],geometry[full+1],geometry[full+2],geometry[full+3]};
    genie::model::Rect minimized;
    if(Now()-snapshot.transition.event_ns>=kHoldGapNs &&
       genie::model::HostViewportForIcon(icon,window,&minimized))
        // Release toward the content's own icon-side endpoint (the landed
        // square), never the virtual-crop fit.
        HoldPresentedSprings(parameters,true,&icon);
    else HoldPresentedSprings(parameters,false);
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
        SuppressBoundIconFallback(transaction,surface,parameters,"unsupported-host");
        return;
    }
    const bool closing=end_width*end_height<start_width*start_height;
    // Replaces the former 0.8/1.25 aspect window: endpoints must be portrait.
    // Preserve native listeners/IDs while replacing the app's crop viewport.
    // The visible contour is produced by the same mesh as the content.
    genie::GroupPublication owned;
    genie::AnimationParameters owned_parameters;
    genie::Transition publication_request;
    bool start_owned=false,dispatch_publication=false;
    uintptr_t owned_commit_token=0;
    int32_t owned_layer=-1,owned_source=-1;
    uint64_t owned_transaction=0;
    if(surface && g_layer) {
        const void* control=*static_cast<const void* const*>(surface);
        const int32_t layer=control?g_layer(control):-1;
        const bool is_icon=type==1 || (control && g_name && g_name(control).find("FloatingIcon_all")==0);
        Last snapshot;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if(is_icon) {
                const IconBuffer* bound=nullptr;
                for(const auto& icon:g_icons) if(icon.surface && icon.parent==layer && icon.source_leash>=0) {
                    bound=&icon;break;
                }
                if(bound) {
                    // OEM IDs are reused by independent Apps. A late callback
                    // for an existing icon must keep its established App leash;
                    // only a newer epoch on that same leash can retarget it.
                    for(const auto& source:g_last) if(source.valid && source.supported &&
                        source.transition.leash_layer==bound->source_leash && source.animation_id==animation_id &&
                        source.transition.event_ns>=bound->request_ns && source.transition.event_ns<=Now()) {
                        snapshot=source;break;
                    }
                } else if(const auto* source=genie::FindIconSource(g_last,animation_id,Now())) snapshot=*source;
            } else {
                for(const auto& entry:g_last) if(entry.valid && entry.transition.leash_layer==layer) {
                    snapshot=entry;break;
                }
            }
        }
        if(snapshot.valid && snapshot.preparing) {
            // Another callback owns this generation's synchronous prepare.
            // Hold the presented parent/icon viewport; a bare OEM close spring
            // must never drive a host whose mesh is not re-registered yet.
            ReprimeIconMask(transaction,snapshot,animation_id);
            HoldPresentedSprings(const_cast<void*>(parameters));
            return;
        }
        if(!snapshot.valid ||
           (!snapshot.supported && !snapshot.retry_pending) ||
           (!is_icon && snapshot.animation_id!=animation_id)) {
            // A failed close generation keeps an already customized icon on
            // its presented viewport instead of the alpha0 clone suppression.
            // A stopped App's old clone still retires through that suppression.
            bool hold_generation=!is_icon && snapshot.valid && snapshot.transition.closing &&
                (!snapshot.supported && !snapshot.retry_pending);
            if(is_icon && !hold_generation) {
                std::lock_guard<std::mutex> lock(g_mutex);
                for(const auto& icon:g_icons) {
                    if(!(icon.surface && icon.surface==icon.readback_child && icon.parent==layer &&
                         icon.request_ns>0 && icon.source_leash>=0)) continue;
                    for(const auto& source:g_last) if(source.valid &&
                        source.transition.leash_layer==icon.source_leash &&
                        source.transition.event_ns>=icon.request_ns && source.transition.event_ns<=Now() &&
                        source.transition.closing &&
                        (source.preparing || (!source.supported && !source.retry_pending))) {
                        hold_generation=true;break;
                    }
                    if(hold_generation) break;
                }
            }
            if(hold_generation) {
                ReprimeIconMask(transaction,snapshot,animation_id);
                HoldForGeneration(const_cast<void*>(parameters),snapshot);
                return;
            }
            SuppressBoundIconFallback(transaction,surface,parameters,"source-not-ready");
            return;
        }
        // A deferred publication has no renderer session to keep alive. Do not
        // revive its original motion after the renderer's ordinary deadline.
        if(snapshot.retry_pending && Now()-snapshot.transition.event_ns>=1500000000LL) {
            SuppressBoundIconFallback(transaction,surface,parameters,"source-expired");
            return;
        }
        const auto endpoint=is_icon?ReadbackEndpoint{}:ReadEndpoint(parameters);
        const auto& geometry=snapshot.transition.geometry;
        const size_t small=snapshot.transition.closing?4:0;
        const size_t full=snapshot.transition.closing?0:4;
        const genie::model::Rect icon{geometry[small],geometry[small+1],geometry[small+2],geometry[small+3]};
        genie::model::Rect window=snapshot.window.width>0?snapshot.window:
            genie::model::Rect{geometry[full],geometry[full+1],geometry[full+2],geometry[full+3]};
        const bool prestart=g_name && g_name(control).find("Pre startup opening leash")==0;
        bool prepared_now=false;
        genie::GroupPublication publication;
        // Clone before asking the server to reparent its output. If this ABI is
        // unavailable, the original App keeps its entire stock transaction.
        if(!is_icon && snapshot.transition.closing && (snapshot.transition.task_id>=0 || prestart) &&
           (!genie::CloneAnimationParameters(parameters,&owned_parameters) ||
            !IndependentParameters(parameters,owned_parameters.Reference()))) {
            // This epoch cannot publish its closing host. Keep a later icon
            // callback from admitting it through the previous opening owner.
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for(auto& source:g_last) if(source.valid &&
                    source.transition.event_ns==snapshot.transition.event_ns &&
                    source.transition.leash_layer==layer && source.animation_id==animation_id) {
                    source.supported=false;
                    source.retry_pending=false;
                    break;
                }
            }
            // This failed generation keeps its presented host frozen too;
            // the bare OEM close spring must not drive it on this callback.
            ReprimeIconMask(transaction,snapshot,animation_id);
            HoldForGeneration(const_cast<void*>(parameters),snapshot);
            // An unanchored pre-start close has no Task event to supersede the
            // live opening on the server. Cancel is ordering only: it stops the
            // stale opening session without dispatching any render admission.
            if(snapshot.transition.task_id<0)
                genie::QueueCancel(layer,snapshot.transition.event_ns);
            return;
        }
        if(!is_icon && !snapshot.admitted && (snapshot.transition.task_id>=0 || prestart)) {
            if(!g_icon_ready.load(std::memory_order_acquire) || !g_start || !g_adjust || !g_host_commit) return;
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
            auto result=genie::GroupPreparation::Rejected;
            if(!snapshot.transition.closing && prestart) {
                if(genie::PrepareOpeningPreview(transaction,const_cast<void*>(control),
                    static_cast<int>(window.width),static_cast<int>(window.height),snapshot.transition)) {
                    publication.kind=genie::PublicationKind::RegisteredPreview;
                    result=genie::GroupPreparation::Ready;
                }
            } else {
                result=genie::PrepareContentGroupStatus(transaction,const_cast<void*>(control),&window,
                    snapshot.transition,&publication);
            }
            if(result==genie::GroupPreparation::Ready && !ValidPublication(publication,snapshot,control)) {
                genie::QueueCancel(snapshot.transition.leash_layer,snapshot.transition.event_ns);
                result=genie::GroupPreparation::Rejected;
            } else if(result!=genie::GroupPreparation::Ready && snapshot.transition.closing &&
                      snapshot.transition.task_id<0) {
                // See the clone-failure path above: without a Task anchor the
                // server never learns this close exists, so its stale opening
                // would keep publishing. Cancel carries ordering only.
                genie::QueueCancel(snapshot.transition.leash_layer,snapshot.transition.event_ns);
            }
            bool hold_pending=false,abort_generation=false;
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
                hold_pending=current && snapshot.transition.closing && result!=genie::GroupPreparation::Ready;
                abort_generation=!current || result!=genie::GroupPreparation::Ready;
            }
            if(abort_generation) {
                // A pending close keeps its presented viewport until its
                // publication lands; stock close springs must not run it.
                if(hold_pending) {
                    ReprimeIconMask(transaction,snapshot,animation_id);
                    HoldForGeneration(const_cast<void*>(parameters),snapshot);
                }
                return;
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
                const bool floating=g_name && g_name(control).find("FloatingIcon_all")==0;
                void* listener=floating?ReadBoundListener(control):nullptr;
                std::lock_guard<std::mutex> lock(g_mutex);
                for(auto& source:g_icons) if(source.surface==lease.surface && source.parent==layer) {
                    // A nested callback may have replaced or advanced this icon.
                    // Do not let an older registration retag that newer epoch.
                    if(source.request_ns<=snapshot.transition.event_ns) {
                        const bool changed=source.request_ns!=snapshot.transition.event_ns ||
                            source.parent_control!=control || source.listener!=listener ||
                            source.closing!=snapshot.transition.closing;
                        if(source.parent_control!=control) {
                            genie::ReleaseIconSurface(source.parent_control,&source);
                            source.parent_control=floating?const_cast<void*>(control):nullptr;
                            genie::RetainIconSurface(source.parent_control,&source);
                        }
                        source.animation_id=animation_id;
                        source.source_leash=snapshot.transition.leash_layer;
                        source.request_ns=snapshot.transition.event_ns;
                        source.readback_child=floating?source.surface:nullptr;
                        source.listener=listener;
                        source.closing=snapshot.transition.closing;
                        if(changed) source.readback_logged=false;
                        if(changed && config.verbose && floating) __android_log_print(ANDROID_LOG_INFO,kTag,
                            "icon readback bound parent=%d source=%d request=%lld closing=%d listener=%p",
                            source.parent,source.source_leash,static_cast<long long>(source.request_ns),
                            source.closing?1:0,source.listener);
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
        const genie::model::Rect original_end{geometry[4],geometry[5],geometry[6],geometry[7]};
        // The icon face must morph along the content's exact rect path: the
        // request's declared start/end rects ARE the content's endpoints. The
        // icon's virtual crop is texture padding, never a morph endpoint.
        // Both faces are drawn by the same SF routine from their AnimTarget's
        // live rect springs, so the icon's springs mirror the content's choice
        // exactly — including the preserved presentation on an adjust.
        const auto start=snapshot.adjusting?snapshot.presentation_start:original_start;
        const auto end=snapshot.transition.closing?original_end:window;
        // The icon's rect centre composes as centerY + centerYOffset, and that
        // offset keeps the glyph aligned inside the face. Compensate the
        // spring instead of clearing the offset: the face lands on the
        // content's rect while the glyph placement stays intact.
        const float center_x_offset=Fixed(parameters,"centerXOffset");
        const float center_y_offset=Fixed(parameters,"centerYOffset");
        SetSpring(parameters,"centerX",start.left+start.width/2-center_x_offset,
                                   end.left+end.width/2-center_x_offset);
        SetSpring(parameters,"centerY",start.top+start.height/2-center_y_offset,
                                   end.top+end.height/2-center_y_offset);
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
            void* listener=ReadBoundListener(control);
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
                // The asynchronous warm-up installs RefBase ownership helpers.
                // Never retain a raw App capability before that completes.
                if(g_icon_ready.load(std::memory_order_acquire)) {
                    AppReadback* entry=nullptr;
                    for(auto& app:g_apps) if(app.control==control && app.layer==layer) {entry=&app;break;}
                    if(!entry) entry=&g_apps[g_next_app++%g_apps.size()];
                    const bool new_epoch=entry->control!=control || entry->request_ns!=source.transition.event_ns ||
                        entry->source_leash!=source.transition.leash_layer || entry->animation_id!=source.animation_id;
                    const bool changed=new_epoch || entry->listener!=listener || entry->closing!=source.transition.closing;
                    if(new_epoch || publication.kind!=genie::PublicationKind::None) {
                        const bool new_host=new_epoch || entry->owned_host!=publication.host;
                        if(entry->owned_host!=publication.host) {
                            genie::ReleaseIconSurface(entry->owned_host,&entry->owned_host);
                            entry->owned_host=publication.host;
                            genie::RetainIconSurface(entry->owned_host,&entry->owned_host);
                        }
                        entry->kind=publication.kind;
                        entry->owned_layer=entry->owned_host && g_layer?g_layer(entry->owned_host):-1;
                        if(new_host) {
                            entry->host_started=false;
                            entry->host_committed=false;
                            entry->host_commit_token=0;
                        }
                    }
                    if(entry->control!=control) {
                        genie::ReleaseIconSurface(entry->control,entry);
                        entry->control=const_cast<void*>(control);
                        genie::RetainIconSurface(entry->control,entry);
                    }
                    entry->layer=layer;
                    entry->source_leash=source.transition.leash_layer;
                    entry->animation_id=source.animation_id;
                    entry->request_ns=source.transition.event_ns;
                    entry->listener=listener;
                    entry->closing=source.transition.closing;
                    if(new_epoch) entry->endpoint=endpoint;
                    if(changed) entry->readback_logged=false;
                    if(entry->kind==genie::PublicationKind::OwnedClosingGroup && entry->owned_host &&
                       entry->owned_layer>=0 && entry->closing && owned_parameters.object && g_layer && g_host_commit &&
                       g_layer(entry->owned_host)==entry->owned_layer && CurrentReadbackSource(*entry) &&
                       (entry->host_started?entry->host_committed:g_next_host_commit_token!=0)) {
                        start_owned=!entry->host_started;
                        if(start_owned) {
                            entry->host_started=true;
                            entry->host_committed=false;
                            entry->host_commit_token=g_next_host_commit_token++;
                        }
                        owned_commit_token=entry->host_commit_token;
                        owned.host=entry->owned_host;
                        genie::RetainIconSurface(owned.host,&owned.host);
                        owned.release=[](const void* surface,const void* owner) {
                            genie::ReleaseIconSurface(const_cast<void*>(surface),owner);
                        };
                        owned_layer=entry->owned_layer;
                    }
                    if(changed && config.verbose) __android_log_print(ANDROID_LOG_INFO,kTag,
                        "app readback bound parent=%d source=%d request=%lld closing=%d listener=%p",
                        entry->layer,entry->source_leash,static_cast<long long>(entry->request_ns),
                        entry->closing?1:0,entry->listener);
                }
                break;
            }
        }
        publication_request=snapshot.transition;
        dispatch_publication=prepared_now;
        owned_source=layer;
        owned_transaction=transaction_id;
        __android_log_print(ANDROID_LOG_INFO,kTag,"mesh viewport layer=%d animation=%d source=%d epoch=%lld hostTxn=%llu start=%.1fx%.1f end=%.1fx%.1f",layer,
            animation_id,snapshot.transition.leash_layer,static_cast<long long>(snapshot.transition.event_ns),
            static_cast<unsigned long long>(transaction_id),start.width,start.height,end.width,end.height);
    }
    for (const char* key:kViewportSprings) {
        const auto* value=Find(parameters,0x18,key);
        auto* property=value?*reinterpret_cast<unsigned char* const*>(value):nullptr;
        if (!property) continue;
        const float damping=1.0f, response=closing?0.336f:0.24f;
        std::memcpy(property+0x20,&damping,4);
        std::memcpy(property+0x24,&response,4);
    }
    if(owned.host) {
        TuneOwnedParameters(parameters,owned_parameters.Reference(),start_owned);
        const void* host=owned.host;
        (start_owned?g_start:g_adjust)(transaction,&host,owned_parameters.Reference());
        if(start_owned) {
            // No other transaction may adjust this host before its initial
            // SELF, reparent and start have committed together. Register out
            // of lock because a callback may run immediately. An obsolete
            // token can never unlock a newer host or a reused registry slot.
            g_host_commit(static_cast<ASurfaceTransaction*>(transaction),
                reinterpret_cast<void*>(owned_commit_token),
                [](void* context,ASurfaceTransactionStats*) {
                    const auto token=reinterpret_cast<uintptr_t>(context);
                    if(!token) return;
                    std::lock_guard<std::mutex> lock(g_mutex);
                    for(auto& app:g_apps) if(app.owned_host && app.host_started &&
                        app.host_commit_token==token) {
                        app.host_committed=true;
                        break;
                    }
                });
        }
        if(config.verbose && start_owned) {
            const auto alpha=Spring(owned_parameters.Reference(),"alpha");
            __android_log_print(ANDROID_LOG_INFO,kTag,
                "owned closing host source=%d request=%lld animation=%d host=%d transaction=%llu alpha=%.2f->%.2f",
                owned_source,static_cast<long long>(publication_request.event_ns),animation_id,owned_layer,
                static_cast<unsigned long long>(owned_transaction),alpha.start,alpha.end);
        }
    }
    if(dispatch_publication) genie::QueueTransition(publication_request);
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
void SuppressStoppedIcons(void* transaction,const void* control,int32_t animation_id) {
    if(!transaction || !control || animation_id<0 || !g_layer || !g_name ||
       !g_icon_ready.load(std::memory_order_acquire)) return;
    const auto config=ReadConfig();
    if(!config.enabled || !config.content) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for(const auto& icon:g_icons) {
        const auto* source=CurrentReadbackSource(icon);
        if(!source || icon.animation_id!=animation_id || icon.width<=0 || icon.height<=0 ||
           g_layer(icon.parent_control)!=icon.parent || g_layer(icon.surface)<0 ||
           g_layer(icon.surface)==icon.parent ||
           g_name(icon.parent_control).find("FloatingIcon_all")!=0) continue;
        int child_parent=-1;
        if(std::sscanf(g_name(icon.surface).c_str(),"[BBQ] FloatingIcon_all#%d",&child_parent)!=1 ||
           child_parent!=icon.parent) continue;
        const auto* app=ReadbackApp(*source);
        if(!app || CurrentReadbackSource(*app)!=source ||
           app->kind!=genie::PublicationKind::OwnedClosingGroup || !app->owned_host ||
           !app->host_started || !app->host_committed || !app->host_commit_token ||
           app->owned_layer<0 || g_layer(app->owned_host)!=app->owned_layer ||
           g_layer(app->control)!=app->layer ||
           g_layer(icon.surface)==app->layer || g_layer(icon.surface)==app->owned_layer ||
           (control!=icon.parent_control && control!=app->control)) continue;
        // Stock Stop freezes the icon's last SELF frame while the independent
        // closing host keeps moving. Hide only these exact temporary controls
        // in that caller transaction; the shared mesh and OEM Stop stay intact.
        ASurfaceTransaction_setBufferAlpha(static_cast<ASurfaceTransaction*>(transaction),
            reinterpret_cast<ASurfaceControl*>(icon.parent_control),0);
        ASurfaceTransaction_setBufferAlpha(static_cast<ASurfaceTransaction*>(transaction),
            reinterpret_cast<ASurfaceControl*>(icon.surface),0);
        if(config.verbose) __android_log_print(ANDROID_LOG_INFO,kTag,
            "stopped icon hidden parent=%d buffer=%d source=%d request=%lld animation=%d",
            icon.parent,g_layer(icon.surface),icon.source_leash,
            static_cast<long long>(icon.request_ns),animation_id);
    }
}
// Deferred mask re-assertion. The OEM stop only flags the layer state; SF
// tears the mask down inside its own snapshot passes (the stop flag even
// overwrites the mask fields from the parent snapshot), so a re-prime merged
// into the stop's own transaction survives only when the pass order happens
// to favor it. A batch stop or an asynchronous stop transaction can land
// later and wipe it. Re-assert the mask from standalone transactions after
// the caller's stop transaction has applied: the deferred write lands in a
// later SF pass than every teardown of that batch. The window is bounded and
// the action is idempotent; a newer generation's registration retires the
// keeper immediately. This is a kill switch, not a correctness knob: the
// deferred path must stay inert only when the whole module is not installed.
constexpr int64_t kReprimeKeeperWindowNs=150000000LL;
constexpr int64_t kReprimeKeeperTickNs=30000000LL;
bool g_keeper_enabled=false;
struct KeeperRequest {
    bool active=false;
    void* surface=nullptr;
    void* parent_control=nullptr;
    int32_t parent=-1;
    int32_t source_leash=-1;
    int width=0,height=0;
    genie::model::Rect window{};
    genie::Transition transition{};
    int64_t event_ns=0;
    int64_t deadline_ns=0;
};
std::mutex g_keeper_mutex;
std::condition_variable g_keeper_cv;
KeeperRequest g_keeper;

void DisarmReprimeKeeper() {
    std::lock_guard<std::mutex> lock(g_keeper_mutex);
    g_keeper.active=false;
}
void ArmReprimeKeeper(const IconBuffer& icon,const genie::model::Rect& window,
                      const genie::Transition& transition,int64_t event_ns) {
    if(!g_keeper_enabled) return;
    {
        std::lock_guard<std::mutex> lock(g_keeper_mutex);
        g_keeper.active=true;
        g_keeper.surface=icon.surface;
        g_keeper.parent_control=icon.parent_control;
        g_keeper.parent=icon.parent;
        g_keeper.source_leash=icon.source_leash;
        g_keeper.width=icon.width;
        g_keeper.height=icon.height;
        g_keeper.window=window;
        g_keeper.transition=transition;
        g_keeper.event_ns=event_ns;
        g_keeper.deadline_ns=Now()+kReprimeKeeperWindowNs;
    }
    g_keeper_cv.notify_all();
}
void ReprimeKeeperApply() {
    KeeperRequest request;
    {
        std::lock_guard<std::mutex> lock(g_keeper_mutex);
        if(!g_keeper.active || !g_keeper_enabled) return;
        request=g_keeper;
    }
    if(Now()>=request.deadline_ns) {DisarmReprimeKeeper();return;}
    IconBuffer lease;void* parent=nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(auto& icon:g_icons) if(icon.surface && icon.surface==request.surface &&
            icon.surface==icon.readback_child && icon.parent==request.parent &&
            icon.source_leash==request.source_leash && icon.parent_control && ValidIconChild(icon) &&
            icon.request_ns>0 && icon.request_ns<=request.event_ns && icon.width>0 && icon.height>0 &&
            g_layer(icon.parent_control)==icon.parent && g_layer(icon.surface)>=0) {
            lease=icon;parent=icon.parent_control;
            genie::RetainIconSurface(lease.surface,&lease);
            break;
        }
    }
    // The armed generation is gone or a newer one took over: retire silently.
    if(!lease.surface) {DisarmReprimeKeeper();return;}
    // Standalone transaction: its pass cannot share the OEM stop's teardown
    // ordering, and only the caller of this apply releases the commit listener.
    if(ASurfaceTransaction* standalone=ASurfaceTransaction_create()) {
        genie::PrepareIcon(standalone,lease.surface,parent,lease.width,lease.height,
            static_cast<int>(request.window.width),static_cast<int>(request.window.height),
            request.transition);
        ASurfaceTransaction_apply(standalone);
        ASurfaceTransaction_delete(standalone);
    }
    genie::ReleaseIconSurface(lease.surface,&lease);
}
void ReprimeKeeperThread() {
    for(;;) {
        {
            std::unique_lock<std::mutex> lock(g_keeper_mutex);
            g_keeper_cv.wait(lock,[]{return g_keeper.active;});
        }
        // The first tick waits for the caller's stop transaction to apply; the
        // bounded repeats outlive a late batch member's asynchronous teardown.
        while(true) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(kReprimeKeeperTickNs));
            bool active=false;
            {
                std::lock_guard<std::mutex> lock(g_keeper_mutex);
                active=g_keeper.active && Now()<g_keeper.deadline_ns;
            }
            if(!active) break;
            ReprimeKeeperApply();
        }
    }
}
// Re-prime the established custom icon's morph mask for a generation going
// down. The OEM's animation stop resets that mask; this call must run after
// the reset in the same transaction to survive it. The icon keeps its existing
// binding unless a completed close lands it.
void ReprimeStoppedIcon(void* transaction,const Last& completed,int32_t source_id,bool landed_retarget) {
    // A null transaction still carries the landed retarget semantics; the
    // previous in-completion re-prime accepted it and PrepareIcon passes it
    // through unchanged.
    if(!g_icon_ready.load(std::memory_order_acquire)) return;
    IconBuffer lease;void* parent=nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(auto& icon:g_icons) if(icon.surface && icon.surface==icon.readback_child &&
            icon.parent_control && ValidIconChild(icon) && icon.source_leash==source_id && icon.request_ns>0 &&
            icon.width>0 && icon.height>0 &&
            icon.request_ns<=completed.transition.event_ns &&
            g_layer(icon.parent_control)==icon.parent && g_layer(icon.surface)>=0) {
            lease=icon;parent=icon.parent_control;
            genie::RetainIconSurface(lease.surface,&lease);
            break;
        }
    }
    if(!lease.surface) return;
    const auto& geometry=completed.transition.geometry;
    const size_t full=completed.transition.closing?0:4;
    const genie::model::Rect window=completed.window.width>0?completed.window:
        genie::model::Rect{geometry[full],geometry[full+1],geometry[full+2],geometry[full+3]};
    genie::PrepareIcon(transaction,lease.surface,parent,lease.width,lease.height,
        static_cast<int>(window.width),static_cast<int>(window.height),completed.transition);
    if(landed_retarget && completed.transition.closing) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for(auto& current:g_icons) if(current.surface==lease.surface &&
            current.parent==lease.parent && current.request_ns<completed.transition.event_ns) {
            current.request_ns=completed.transition.event_ns;
            current.animation_id=completed.animation_id;
            current.closing=true;
            current.readback_logged=false;
            current.landed=true;
        }
    }
    ArmReprimeKeeper(lease,window,completed.transition,completed.transition.event_ns);
    genie::ReleaseIconSurface(lease.surface,&lease);
}
void* Stop(void* transaction,const void* surface,int32_t animation_id) {
    // The OEM stop resets the custom morph mask on the animation's surfaces,
    // so run it first and let every retained re-prime land on top of that
    // reset. The OEM may stop the source and its icon parent in either order;
    // covering both surfaces means the last reset in the batch cannot expose
    // the raw CLAMP plate before the next generation takes over.
    void* result=g_stop(transaction,surface,animation_id);
    if (surface && g_layer) {
        const void* control=*static_cast<const void* const*>(surface);
        SuppressStoppedIcons(transaction,control,animation_id);
        const int32_t id=control?g_layer(control):-1;
        bool cancel=false;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            // Icon buffers share the central App owner's lifetime. An icon's
            // stock completion must not cancel or detach that shared mesh.
            for(auto& slot:g_last) {
                if(slot.valid && slot.transition.leash_layer==id && slot.animation_id==animation_id) {
                    const Last completed=slot;
                    slot.valid=false;
                    cancel=true;
                    lock.unlock();
                    ReprimeStoppedIcon(transaction,completed,id,true);
                    lock.lock();
                    break;
                }
            }
            if(!cancel) for(auto& icon:g_icons) {
                if(!(icon.surface && icon.surface==icon.readback_child && icon.parent==id &&
                     icon.source_leash>=0 && icon.request_ns>0)) continue;
                for(const auto& slot:g_last) if(slot.transition.leash_layer==icon.source_leash &&
                    slot.animation_id==animation_id && slot.transition.event_ns>=icon.request_ns &&
                    slot.transition.event_ns>0) {
                    const Last completed=slot;
                    lock.unlock();
                    ReprimeStoppedIcon(transaction,completed,icon.source_leash,false);
                    lock.lock();
                    break;
                }
            }
        }
        if(cancel) genie::QueueCancel(id,Now());
    }
    return result;
}
void Install(void* handle) {
    if (!handle || g_installed.exchange(true)) return;
    void* start=dlsym(handle,kStart);
    void* adjust=dlsym(handle,kAdjust);
    void* stop=dlsym(handle,kStop);
    void* reparent=dlsym(handle,kReparent);
    void* blast=dlsym(handle,kBlastUpdate);
    void* home_params=dlsym(handle,kHomeParams);
    void* set_callback=dlsym(handle,kSetCallback);
    static void* platform=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
    g_host_commit=platform?reinterpret_cast<HostCommitFn>(dlsym(platform,"ASurfaceTransaction_setOnCommit")):nullptr;
    const auto* listener_table=static_cast<const unsigned char*>(dlsym(handle,
        "_ZTVN7android19SFAnimationListenerE"));
    const auto* params_table=static_cast<const unsigned char*>(dlsym(handle,
        "_ZTVN7android3gui14CallAnimParamsE"));
    g_layer=reinterpret_cast<LayerFn>(dlsym(handle,"_ZNK7android14SurfaceControl10getLayerIdEv"));
    g_name=reinterpret_cast<NameFn>(dlsym(handle,"_ZNK7android14SurfaceControl7getNameEv"));
    g_transaction_id=reinterpret_cast<TransactionIdFn>(dlsym(handle,
        "_ZN7android21SurfaceComposerClient11Transaction5getIdEv"));
    if (!start || !adjust || !stop || !reparent || !blast || !home_params || !set_callback || !g_host_commit ||
        !listener_table || !params_table || !g_layer || !g_name) {
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
    g_listener_vtable=listener_table+0x18;
    g_params_vtable=params_table+0x10;
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
    status=g_api->hookFunc(home_params,reinterpret_cast<void*>(HomeParams),reinterpret_cast<void**>(&g_home_params));
    if(status || !g_home_params) {
        if(!status) g_api->unhookFunc(home_params);
        g_api->unhookFunc(blast);
        g_api->unhookFunc(reparent);
        g_api->unhookFunc(stop);
        g_api->unhookFunc(adjust);
        g_api->unhookFunc(start);
        return;
    }
    status=g_api->hookFunc(set_callback,reinterpret_cast<void*>(SetCallback),reinterpret_cast<void**>(&g_set_callback));
    if(status || !g_set_callback) {
        if(!status) g_api->unhookFunc(set_callback);
        g_api->unhookFunc(home_params);
        g_api->unhookFunc(blast);
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
    // The deferred mask keeper exists only in the hooked launcher process;
    // its worker is started here so unhooked test builds stay deterministic.
    g_keeper_enabled=true;
    std::thread(ReprimeKeeperThread).detach();
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
