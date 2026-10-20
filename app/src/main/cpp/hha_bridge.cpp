// SPDX-License-Identifier: Apache-2.0
#include "hha_bridge.h"
#include "hha_renderer.h"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>
#include <dlfcn.h>
#include <time.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hha {
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

void Run(Dispatcher* dispatcher) {
    // The service-manager NDK API is a platform export on the verified ROM. It
    // is resolved at runtime so an unavailable bridge keeps stock animation.
    void* library = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    using CheckService = AIBinder* (*)(const char*);
    auto check = library ? reinterpret_cast<CheckService>(
        dlsym(library, "AServiceManager_checkService")) : nullptr;
    if (!check) {
        __android_log_print(ANDROID_LOG_WARN, "HHA.Bridge", "NDK service lookup unavailable");
        return;
    }
    AIBinder_Class* type = AIBinder_Class_define("android.view.IWindowManager", Create,
                                               Destroy, Transact);
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
        AParcel* input = nullptr;
        AParcel* output = nullptr;
        binder_status_t status = AIBinder_prepareTransaction(binder, &input);
        auto integer = [&](int32_t value) {
            if (status == STATUS_OK) status = AParcel_writeInt32(input, value);
        };
        integer(kBridgeMagic);
        integer(kBridgeVersion);
        integer(command.cancel ? 2 : 1);
        integer(command.transition.leash_layer);
        integer(command.transition.container_layer);
        integer(command.transition.task_id);
        integer(command.transition.closing ? 1 : 0);
        if (status == STATUS_OK) status = AParcel_writeInt64(input, command.transition.event_ns);
        for (float value : command.transition.geometry) {
            if (status == STATUS_OK) status = AParcel_writeFloat(input, value);
        }
        int32_t accepted = 0;
        if (status == STATUS_OK) {
            status = AIBinder_transact(binder, kBridgeCode, &input, &output, 0);
            if (status == STATUS_OK) {
                AStatus* result = nullptr;
                status = AParcel_readStatusHeader(output, &result);
                if (status == STATUS_OK && result && AStatus_isOk(result)) {
                    status = AParcel_readInt32(output, &accepted);
                }
                if (result) AStatus_delete(result);
            }
        }
        if (input) AParcel_delete(input);
        if (output) AParcel_delete(output);
        if (status == STATUS_DEAD_OBJECT) {
            AIBinder_decStrong(binder);
            binder = nullptr;
        }
        __android_log_print(accepted ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG, "HHA.Bridge",
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

void QueueTransition(const Transition& transition) { Queue({transition, false}); }
void QueueCancel(int32_t layer, int64_t event_ns) {
    Command command;
    command.cancel = true;
    command.transition.leash_layer = layer;
    command.transition.event_ns = event_ns;
    Queue(command);
}
}  // namespace hha
