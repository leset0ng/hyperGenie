// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "genie_model.h"

#include <array>
#include <cstdint>
#include <optional>
#include <deque>
#include <algorithm>

namespace genie {

// IWindowManager private extension; mirrored by CentralBridge.kt. Ordinary App
// capabilities stay in system_server. Restricted prepare operations return
// verified preview/output capabilities and an optional owned closing host.
constexpr int32_t kBridgeCode = 0x484841;
constexpr int32_t kBridgeMagic = 0x48484131;
constexpr int32_t kBridgeVersion = 5;

struct Transition {
    int32_t leash_layer = -1;
    int32_t container_layer = -1;
    int32_t task_id = -1;
    bool closing = false;
    int64_t event_ns = 0;
    // start rect, end rect, velocity x/y, corner start/end; physical pixels.
    std::array<float, 12> geometry{};
};

struct AnimationSource {
    Transition transition;
    int32_t animation_id = -1;
    genie::model::Rect window{0,0,0,0};
    genie::model::Rect presentation_start{0,0,0,0};
    // Source centerX/centerY/width dynamics, shared with the paired icon host.
    std::array<float,3> host_velocity{};
    std::array<float,3> host_delay{};
    uint64_t host_transaction_id = 0;
    bool adjusting = false;
    bool valid = false;
    bool supported = true;
    // Only an admitted host may skip first-frame preparation. Deferred groups
    // remain icon-ineligible and can retry in a later real host transaction.
    bool admitted = false;
    bool retry_pending = false;
    bool preparing = false;
};

// SF animation IDs are reused on different leashes. Pair an icon only with the
// newest live submission, never the first historical entry with the same ID.
template <size_t N>
const AnimationSource* FindIconSource(const std::array<AnimationSource,N>& sources,
                                    int32_t animation_id,int64_t now) {
    const AnimationSource* result=nullptr;
    for(const auto& source:sources) {
        const int64_t age=now-source.transition.event_ns;
        // The source stays eligible until its exact animation is stopped. A
        // stalled callback must not put only the icon back onto stock geometry.
        if(!source.valid || source.animation_id!=animation_id || age<0) continue;
        if(!result || source.transition.event_ns>result->transition.event_ns) result=&source;
    }
    return result && result->supported ? result : nullptr;
}

struct BridgeCommand {
    Transition transition;
    bool cancel = false;
};

// Caller serializes access. The pure queue is also exercised by host tests so
// old-leash stops cannot silently swallow a replacement while Binder is busy.
class PendingTransitions {
public:
    bool Post(const BridgeCommand& next) {
        if(next.cancel && next.transition.leash_layer<0) {
            pending_.clear();
            pending_.push_back(next);
            return true;
        }
        auto existing=std::find_if(pending_.begin(),pending_.end(),[&](const BridgeCommand& item) {
            return item.transition.leash_layer==next.transition.leash_layer;
        });
        if(existing!=pending_.end()) {
            if(next.transition.event_ns<existing->transition.event_ns) return false;
            *existing=next;
            return true;
        }
        // Different App clocks are independent. A delayed Task anchor must not
        // be discarded merely because another leash submitted a newer event.
        if(pending_.size()>=32) return false;
        pending_.push_back(next);
        return true;
    }
    bool Ready() const {return !pending_.empty();}
    std::optional<BridgeCommand> Take() {
        if(pending_.empty()) return std::nullopt;
        const auto value=pending_.front();
        pending_.pop_front();
        return value;
    }
private:
    std::deque<BridgeCommand> pending_;
};

// Nonblocking, coalesced dispatch. Never make a synchronous WMS call from a
// launcher animation callback, which can be part of a WMS lock dependency.
enum class GroupPreparation { Rejected, Deferred, Ready };
enum class PublicationKind : int32_t { None=0, NormalGroup=1, RegisteredPreview=2, OwnedClosingGroup=3 };

// readFromParcel creates an sp<SurfaceControl> using &host as its owner tag.
// Keep that address stable until the exact parcel reference has been released.
struct GroupPublication {
    PublicationKind kind=PublicationKind::None;
    void* host=nullptr;
    void (*release)(const void*,const void*)=nullptr;
    GroupPublication()=default;
    GroupPublication(const GroupPublication&)=delete;
    GroupPublication& operator=(const GroupPublication&)=delete;
    GroupPublication(GroupPublication&&)=delete;
    GroupPublication& operator=(GroupPublication&&)=delete;
    ~GroupPublication() {Reset();}
    void Reset() {
        if(host && release) release(host,&host);
        host=nullptr;release=nullptr;kind=PublicationKind::None;
    }
};

// A private parameter object keeps the owned host's alpha independent from
// the OEM App springs. Its parcel-owned sp reference also needs a stable tag.
struct AnimationParameters {
    void* object=nullptr;
    void (*release)(const void*,const void*)=nullptr;
    AnimationParameters()=default;
    AnimationParameters(const AnimationParameters&)=delete;
    AnimationParameters& operator=(const AnimationParameters&)=delete;
    AnimationParameters(AnimationParameters&&)=delete;
    AnimationParameters& operator=(AnimationParameters&&)=delete;
    ~AnimationParameters() {Reset();}
    const void* Reference() const {return &object;}
    void Reset() {
        if(object && release) release(object,&object);
        object=nullptr;release=nullptr;
    }
};
bool CloneAnimationParameters(const void* reference,AnimationParameters* output);

// PREPARE_GROUP accepted=2 means the exact group is awaiting its first buffer.
// No host transaction is changed until a typed Ready reply has been validated.
GroupPreparation PrepareContentGroupStatus(void* transaction,void* parent,genie::model::Rect* window,
                                          const Transition& transition,GroupPublication* publication=nullptr);

void QueueTransition(const Transition& transition);
void QueueCancel(int32_t leash_layer, int64_t event_ns);
bool LauncherAnimationEnabled();

}  // namespace genie
