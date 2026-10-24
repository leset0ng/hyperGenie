// SPDX-License-Identifier: Apache-2.0
#include "genie_bridge.h"
#include "genie_renderer.h"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <time.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace genie {
namespace {
using Command=BridgeCommand;
struct Dispatcher {
    std::mutex mutex;
    std::condition_variable condition;
    PendingTransitions pending;
};

void* Create(void*) { return nullptr; }
void Destroy(void*) {}
binder_status_t Transact(AIBinder*, transaction_code_t, const AParcel*, AParcel*) {
    return STATUS_UNKNOWN_TRANSACTION;
}

AIBinder_Class* BridgeClass() {
    static AIBinder_Class* value=AIBinder_Class_define("android.view.IWindowManager", Create, Destroy, Transact);
    return value;
}

struct IconPayload {
    void* surface;
    void* parent;
    int32_t texture_width,texture_height,window_width,window_height;
    void* (*view)(AParcel*);
    int32_t (*write_surface)(void*,void*);
};

// Prepare operations read additional reply payloads, so the caller owns output.
binder_status_t Send(AIBinder* binder, const Transition& transition, int32_t operation,
                     AParcel** output, int32_t* accepted, const IconPayload* icon = nullptr) {
    AParcel* input = nullptr;
    *accepted = 0;
    binder_status_t status = AIBinder_prepareTransaction(binder, &input);
    auto integer = [&](int32_t value) {
        if (status == STATUS_OK) status = AParcel_writeInt32(input, value);
    };
    integer(kBridgeMagic);
    integer(kBridgeVersion);
    integer(operation);
    integer(transition.leash_layer);
    integer(transition.container_layer);
    integer(transition.task_id);
    integer(transition.closing ? 1 : 0);
    if (status == STATUS_OK) status = AParcel_writeInt64(input, transition.event_ns);
    for (float value : transition.geometry) {
        if (status == STATUS_OK) status = AParcel_writeFloat(input, value);
    }
    if (icon) {
        integer(icon->texture_width);
        integer(icon->texture_height);
        integer(icon->window_width);
        integer(icon->window_height);
        // SurfaceControl.CREATOR reads Java width/height/presence before the
        // native capability payload. Native writeToParcel alone omits these.
        integer(icon->texture_width);
        integer(icon->texture_height);
        integer(1);
        if (status == STATUS_OK) status = icon->write_surface(icon->surface, icon->view(input));
        integer(0);
        integer(0);
        integer(1);
        if (status == STATUS_OK) status = icon->write_surface(icon->parent, icon->view(input));
    }
    if (status == STATUS_OK) status = AIBinder_transact(binder, kBridgeCode, &input, output, 0);
    if (status == STATUS_OK) {
        AStatus* result = nullptr;
        status = AParcel_readStatusHeader(*output, &result);
        if (status == STATUS_OK && result && AStatus_isOk(result))
            status = AParcel_readInt32(*output, accepted);
        if (result) AStatus_delete(result);
    }
    if (input) AParcel_delete(input);
    return status;
}

void Run(Dispatcher* dispatcher) {
    // The service-manager NDK API is a platform export on the verified ROM. It
    // is resolved at runtime so an unavailable bridge keeps stock animation.
    void* library = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    using CheckService = AIBinder* (*)(const char*);
    auto check = library ? reinterpret_cast<CheckService>(
        dlsym(library, "AServiceManager_checkService")) : nullptr;
    if (!check) {
        __android_log_print(ANDROID_LOG_WARN, "Genie.Bridge", "NDK service lookup unavailable");
        return;
    }
    AIBinder_Class* type = BridgeClass();
    AIBinder* binder = nullptr;
    Command last;
    bool active = false;
    auto deadline=std::chrono::steady_clock::now();
    for (;;) {
        Command command;
        bool poll=false;
        {
            std::unique_lock<std::mutex> lock(dispatcher->mutex);
            if(active) dispatcher->condition.wait_for(lock,std::chrono::milliseconds(50),
                [&] {return dispatcher->pending.Ready();});
            else dispatcher->condition.wait(lock,[&] {return dispatcher->pending.Ready();});
            if(auto queued=dispatcher->pending.Take()) {
                command=*queued;
            } else poll=true;
        }
        if(poll) {
            if(std::chrono::steady_clock::now()>deadline) {active=false;continue;}
            if(LauncherAnimationEnabled()) continue;
            CancelIconRenderer();
            command=last;
            command.cancel=true;
            command.transition.leash_layer=-1;
            timespec timestamp{};
            clock_gettime(CLOCK_MONOTONIC,&timestamp);
            command.transition.event_ns=static_cast<int64_t>(timestamp.tv_sec)*1000000000LL+timestamp.tv_nsec;
        }
        last=command;
        active=!command.cancel;
        deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        if (!binder) {
            binder = check("window");
            if (binder && !AIBinder_associateClass(binder, type)) {
                AIBinder_decStrong(binder);
                binder = nullptr;
            }
        }
        if (!binder) continue;
        AParcel* output = nullptr;
        int32_t accepted = 0;
        const binder_status_t status = Send(binder, command.transition, command.cancel ? 2 : 1,
                                             &output, &accepted);
        if (output) AParcel_delete(output);
        if (status == STATUS_DEAD_OBJECT) {
            AIBinder_decStrong(binder);
            binder = nullptr;
        }
        __android_log_print(accepted ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG, "Genie.Bridge",
            "dispatch layer=%d direction=%s status=%d accepted=%d",
            command.transition.leash_layer, command.cancel ? "cancel" :
                command.transition.closing ? "close" : "open", status, accepted);
    }
}

Dispatcher& GetDispatcher() {
    static Dispatcher* dispatcher = [] {
        auto* value = new Dispatcher;
        std::thread(Run, value).detach();
        return value;
    }();
    return *dispatcher;
}
void Queue(Command command) {
    auto& dispatcher = GetDispatcher();
    {
        std::lock_guard<std::mutex> lock(dispatcher.mutex);
        if(!dispatcher.pending.Post(command)) return;
    }
    dispatcher.condition.notify_one();
}
}  // namespace

bool CloneAnimationParameters(const void* reference,AnimationParameters* output) {
    if(!reference || !output) return false;
    output->Reset();
    const void* source=*static_cast<const void* const*>(reference);
    if(!source) return false;
    static void* binder=dlopen("libbinder_ndk.so",RTLD_NOW|RTLD_LOCAL);
    static void* gui=dlopen("libgui.so",RTLD_NOW|RTLD_LOCAL);
    static void* utils=dlopen("libutils.so",RTLD_NOW|RTLD_LOCAL);
    using CreateParcel=AParcel* (*)();
    using ViewParcel=void* (*)(AParcel*);
    using Write=int32_t (*)(const void*,void*);
    using Read=int32_t (*)(const void*,void**);
    using Release=void (*)(const void*,const void*);
    static auto create=binder?reinterpret_cast<CreateParcel>(dlsym(binder,"AParcel_create")):nullptr;
    static auto view=binder?reinterpret_cast<ViewParcel>(dlsym(binder,
        "_Z26AParcel_viewPlatformParcelP7AParcel")):nullptr;
    static auto write=gui?reinterpret_cast<Write>(dlsym(gui,
        "_ZN7android3gui17SfAnimationParams13writeToParcelEPNS_6ParcelE")):nullptr;
    static auto read=gui?reinterpret_cast<Read>(dlsym(gui,
        "_ZN7android3gui17SfAnimationParams14readFromParcelEPKNS_6ParcelEPNS_2spIS1_EE")):nullptr;
    static auto release=utils?reinterpret_cast<Release>(dlsym(utils,"_ZNK7android7RefBase9decStrongEPKv")):nullptr;
    static const auto* table=gui?static_cast<const unsigned char*>(dlsym(gui,
        "_ZTVN7android3gui17SfAnimationParamsE")):nullptr;
    if(!create || !view || !write || !read || !release || !table ||
       *static_cast<const void* const*>(source)!=table+0x10) return false;
    AParcel* parcel=create();
    if(!parcel) return false;
    void* platform=view(parcel);
    output->release=release;
    const bool copied=platform && write(source,platform)==0 && AParcel_setDataPosition(parcel,0)==STATUS_OK &&
        read(platform,&output->object)==0 && output->object && output->object!=source &&
        *static_cast<const void* const*>(output->object)==table+0x10;
    AParcel_delete(parcel);
    if(!copied) output->Reset();
    return copied;
}

GroupPreparation PrepareContentGroupStatus(void* transaction,void* parent,genie::model::Rect* window,
                                          const Transition& transition,GroupPublication* publication) {
    GroupPublication local;
    auto* prepared=publication?publication:&local;
    prepared->Reset();
    static void* library=dlopen("libbinder_ndk.so",RTLD_NOW|RTLD_LOCAL);
    using CheckService=AIBinder* (*)(const char*);
    static auto check=library?reinterpret_cast<CheckService>(dlsym(library,"AServiceManager_checkService")):nullptr;
    static void* platform=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
    using ViewParcel=const void* (*)(const AParcel*);
    using ReadSurface=int32_t (*)(const void*,void**);
    using Release=void (*)(const void*,const void*);
    using Layer=int32_t (*)(const void*);
    using ReadTransaction=int32_t (*)(void*,const void*);
    using MergeTransaction=void* (*)(void*,void*);
    auto symbol=[&](const char* name) {return platform?dlsym(platform,name):nullptr;};
    static auto view=library?reinterpret_cast<ViewParcel>(dlsym(library,"_Z26AParcel_viewPlatformParcelPK7AParcel")):nullptr;
    static auto read=reinterpret_cast<ReadSurface>(symbol("_ZN7android14SurfaceControl14readFromParcelERKNS_6ParcelEPNS_2spIS0_EE"));
    static auto release=reinterpret_cast<Release>(symbol("_ZNK7android7RefBase9decStrongEPKv"));
    static auto layer=reinterpret_cast<Layer>(symbol("_ZNK7android14SurfaceControl10getLayerIdEv"));
    static auto read_transaction=reinterpret_cast<ReadTransaction>(symbol("_ZN7android21SurfaceComposerClient11Transaction14readFromParcelEPKNS_6ParcelE"));
    static auto merge_transaction=reinterpret_cast<MergeTransaction>(symbol("_ZN7android21SurfaceComposerClient11Transaction5mergeEOS1_"));
    if(!check || !transaction || !parent || !window || !view || !read || !release || !layer || !read_transaction ||
       !merge_transaction || transition.task_id<0) return GroupPreparation::Rejected;
    AIBinder* binder=check("window");
    if(!binder) return GroupPreparation::Rejected;
    if(!AIBinder_associateClass(binder,BridgeClass())) {
        AIBinder_decStrong(binder);
        return GroupPreparation::Rejected;
    }
    AParcel* output=nullptr;
    int32_t accepted=0;
    binder_status_t status=Send(binder,transition,4,&output,&accepted);
    int32_t kind=0,width=0,height=0;
    if(status==STATUS_OK && accepted==1) {
        status=AParcel_readInt32(output,&kind);
        if(status==STATUS_OK) status=AParcel_readInt32(output,&width);
        if(status==STATUS_OK) status=AParcel_readInt32(output,&height);
    }
    const auto type=static_cast<PublicationKind>(kind);
    const bool owned=type==PublicationKind::OwnedClosingGroup;
    const bool recognized=type==PublicationKind::NormalGroup || type==PublicationKind::RegisteredPreview || owned;
    const bool direction=owned?transition.closing && publication:
        type!=PublicationKind::NormalGroup || !transition.closing;
    void* output_surface=nullptr;
    ASurfaceTransaction* visibility=ASurfaceTransaction_create();
    bool primed=false;
    if(status==STATUS_OK && accepted==1 && recognized && direction && visibility &&
       width>0 && height>width && width<=16384 && height<=16384) {
        const void* parcel=view(output);
        bool capabilities=parcel && read(parcel,&output_surface)==0 && output_surface;
        if(capabilities && owned) {
            prepared->release=release;
            capabilities=read(parcel,&prepared->host)==0 && prepared->host;
            if(capabilities) {
                const int32_t id=layer(prepared->host);
                capabilities=id>=0 && id!=layer(parent) && id!=layer(output_surface);
            }
        }
        if(capabilities && read_transaction(visibility,parcel)==0) {
            // The server supplies shader state, visibility and host topology in
            // this transaction. The caller adds the owned host's first motion.
            merge_transaction(transaction,visibility);
            *window={0,0,static_cast<double>(width),static_cast<double>(height)};
            prepared->kind=type;
            primed=true;
        }
    }
    if(!primed) prepared->Reset();
    if(output_surface) release(output_surface,&output_surface);
    if(visibility) ASurfaceTransaction_delete(visibility);
    if(output) AParcel_delete(output);
    AIBinder_decStrong(binder);
    __android_log_print(primed?ANDROID_LOG_INFO:ANDROID_LOG_DEBUG,"Genie.Bridge",
        "group before host leash=%d accepted=%d kind=%d primed=%d status=%d domain=%dx%d",transition.leash_layer,accepted,kind,primed,status,width,height);
    if(accepted==1 && !primed) QueueCancel(transition.leash_layer,transition.event_ns);
    if(primed) return GroupPreparation::Ready;
    return status==STATUS_OK && accepted==2 ? GroupPreparation::Deferred : GroupPreparation::Rejected;
}

bool PrepareContentGroup(void* transaction,void* parent,genie::model::Rect* window,const Transition& transition) {
    return PrepareContentGroupStatus(transaction,parent,window,transition)==GroupPreparation::Ready;
}

bool PrepareOpeningPreview(void* transaction,void* parent,int width,int height,
                           const Transition& transition) {
    // The server resolves only the exact pre-start capability and primes it
    // without entering WMS.mGlobalLock or waiting for its worker/commit.
    if(!transaction || !parent || transition.closing || width<=0 || height<=width ||
       width>16384 || height>16384) return false;
    static void* binder_library=dlopen("libbinder_ndk.so",RTLD_NOW|RTLD_LOCAL);
    static void* platform=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
    static void* gui=dlopen("libgui.so",RTLD_NOW|RTLD_LOCAL);
    static void* utils=dlopen("libutils.so",RTLD_NOW|RTLD_LOCAL);
    using CheckService=AIBinder* (*)(const char*);
    using ViewParcel=const void* (*)(const AParcel*);
    using ReadSurface=int32_t (*)(const void*,void**);
    using Release=void (*)(const void*,const void*);
    using ReadTransaction=int32_t (*)(void*,const void*);
    using MergeTransaction=void* (*)(void*,void*);
    auto symbol=[](void* library,void* fallback,const char* name) {
        void* value=library?dlsym(library,name):nullptr;
        return value?value:(fallback?dlsym(fallback,name):nullptr);
    };
    static auto check=reinterpret_cast<CheckService>(symbol(binder_library,nullptr,"AServiceManager_checkService"));
    static auto view=reinterpret_cast<ViewParcel>(symbol(binder_library,nullptr,"_Z26AParcel_viewPlatformParcelPK7AParcel"));
    static auto read=reinterpret_cast<ReadSurface>(symbol(gui,platform,
        "_ZN7android14SurfaceControl14readFromParcelERKNS_6ParcelEPNS_2spIS0_EE"));
    static auto release=reinterpret_cast<Release>(symbol(utils,platform,"_ZNK7android7RefBase9decStrongEPKv"));
    static auto read_transaction=reinterpret_cast<ReadTransaction>(symbol(gui,platform,"_ZN7android21SurfaceComposerClient11Transaction14readFromParcelEPKNS_6ParcelE"));
    static auto merge_transaction=reinterpret_cast<MergeTransaction>(symbol(gui,platform,"_ZN7android21SurfaceComposerClient11Transaction5mergeEOS1_"));
    if(!check || !view || !read || !release || !read_transaction || !merge_transaction) return false;
    AIBinder* binder=check("window");
    if(!binder) return false;
    if(!AIBinder_associateClass(binder,BridgeClass())) {AIBinder_decStrong(binder);return false;}
    AParcel* output=nullptr;
    int32_t accepted=0;
    const binder_status_t status=Send(binder,transition,3,&output,&accepted);
    void* surface=nullptr; // ABI mirror of the output sp<SurfaceControl>.
    bool primed=false;
    ASurfaceTransaction* publication=nullptr;
    if(status==STATUS_OK && accepted==1) {
        const void* parcel=view(output);
        publication=ASurfaceTransaction_create();
        if(publication && parcel && read(parcel,&surface)==0 && surface &&
           read_transaction(publication,parcel)==0) {
            merge_transaction(transaction,publication);
            primed=true;
        }
    }
    if(publication) ASurfaceTransaction_delete(publication);
    if(surface) release(surface,&surface);
    if(output) AParcel_delete(output);
    AIBinder_decStrong(binder);
    if(accepted && !primed) {
        timespec timestamp{};clock_gettime(CLOCK_MONOTONIC,&timestamp);
        QueueCancel(transition.leash_layer,static_cast<int64_t>(timestamp.tv_sec)*1000000000LL+timestamp.tv_nsec);
    }
    __android_log_print(primed?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"Genie.Bridge",
        "preview first transaction leash=%d accepted=%d primed=%d status=%d",transition.leash_layer,accepted,primed,status);
    return primed;
}

bool PrepareIcon(void* host_transaction,void* icon_surface,void* icon_parent,
                 int texture_width,int texture_height,int window_width,int window_height,
                 const Transition& transition) {
    static void* binder_library=dlopen("libbinder_ndk.so",RTLD_NOW|RTLD_LOCAL);
    static void* platform=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
    static void* gui=dlopen("libgui.so",RTLD_NOW|RTLD_LOCAL);
    using CheckService=AIBinder* (*)(const char*);
    using MutableParcel=void* (*)(AParcel*);
    using ConstParcel=const void* (*)(const AParcel*);
    using WriteSurface=int32_t (*)(void*,void*);
    using ReadTransaction=int32_t (*)(void*,const void*);
    using MergeTransaction=void* (*)(void*,void*);
    auto symbol=[](void* library,void* fallback,const char* name) {
        void* value=library?dlsym(library,name):nullptr;
        return value?value:(fallback?dlsym(fallback,name):nullptr);
    };
    static auto check=reinterpret_cast<CheckService>(symbol(binder_library,nullptr,"AServiceManager_checkService"));
    // Both overloads are exported by the verified ROM's libbinder_ndk. The
    // mutable overload preserves the parcel's current write position.
    static auto mutable_view=reinterpret_cast<MutableParcel>(symbol(binder_library,nullptr,"_Z26AParcel_viewPlatformParcelP7AParcel"));
    static auto const_view=reinterpret_cast<ConstParcel>(symbol(binder_library,nullptr,"_Z26AParcel_viewPlatformParcelPK7AParcel"));
    // ROM libgui: non-const member, Parcel&, status_t return (x0=this,x1=parcel).
    static auto write_surface=reinterpret_cast<WriteSurface>(symbol(gui,platform,"_ZN7android14SurfaceControl13writeToParcelERNS_6ParcelE"));
    static auto read_transaction=reinterpret_cast<ReadTransaction>(symbol(gui,platform,"_ZN7android21SurfaceComposerClient11Transaction14readFromParcelEPKNS_6ParcelE"));
    static auto merge_transaction=reinterpret_cast<MergeTransaction>(symbol(gui,platform,"_ZN7android21SurfaceComposerClient11Transaction5mergeEOS1_"));
    if(!host_transaction || !icon_surface || !icon_parent ||
       texture_width<=0 || texture_height<=0 || texture_width>1024 || texture_height>1024 ||
       window_width<=0 || window_height<=window_width || window_width>16384 || window_height>16384 ||
       !check || !mutable_view || !const_view || !write_surface || !read_transaction || !merge_transaction) return false;
    AIBinder* binder=check("window");
    if(!binder) return false;
    if(!AIBinder_associateClass(binder,BridgeClass())) {AIBinder_decStrong(binder);return false;}
    const IconPayload payload{icon_surface,icon_parent,texture_width,texture_height,window_width,window_height,
                              mutable_view,write_surface};
    AParcel* output=nullptr;
    int32_t accepted=0;
    const binder_status_t status=Send(binder,transition,5,&output,&accepted,&payload);
    ASurfaceTransaction* publication=nullptr;
    bool primed=false;
    if(status==STATUS_OK && accepted==1 && output) {
        publication=ASurfaceTransaction_create();
        const void* parcel=const_view(output);
        if(publication && parcel && read_transaction(publication,parcel)==0) {
            // The native server commit listener travels with this merge. Never
            // apply here: only the caller's first host publication releases it.
            merge_transaction(host_transaction,publication);
            primed=true;
        }
    }
    if(publication) ASurfaceTransaction_delete(publication);
    if(output) AParcel_delete(output);
    AIBinder_decStrong(binder);
    __android_log_print(primed?ANDROID_LOG_INFO:ANDROID_LOG_WARN,"Genie.Bridge",
        "icon before host leash=%d accepted=%d primed=%d status=%d texture=%dx%d domain=%dx%d",
        transition.leash_layer,accepted,primed,status,texture_width,texture_height,window_width,window_height);
    return primed;
}

void QueueTransition(const Transition& transition) { Queue({transition, false}); }
void QueueCancel(int32_t layer, int64_t event_ns) {
    Command command;
    command.cancel = true;
    command.transition.leash_layer = layer;
    command.transition.event_ns = event_ns;
    Queue(command);
}
}  // namespace genie
