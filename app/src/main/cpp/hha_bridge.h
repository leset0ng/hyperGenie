// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "hha_genie.h"

#include <array>
#include <cstdint>
#include <optional>

namespace hha {

// IWindowManager private extension; mirrored by CentralBridge.kt. Only geometry
// crosses the launcher boundary. The server retains all content capabilities.
constexpr int32_t kBridgeCode = 0x484841;
constexpr int32_t kBridgeMagic = 0x48484131;
constexpr int32_t kBridgeVersion = 2;

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
    genie::Rect window{0,0,0,0};
    bool valid = false;
};

// SF animation IDs are reused on different leashes. Pair an icon only with the
// newest live submission, never the first historical entry with the same ID.
template <size_t N>
const AnimationSource* FindIconSource(const std::array<AnimationSource,N>& sources,
                                    int32_t animation_id,int64_t now) {
    const AnimationSource* result=nullptr;
    for(const auto& source:sources) {
        const int64_t age=now-source.transition.event_ns;
        if(!source.valid || source.animation_id!=animation_id || age<0 || age>500000000LL) continue;
        if(!result || source.transition.event_ns>result->transition.event_ns) result=&source;
    }
    return result;
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
        if(pending_) {
            const auto& previous=*pending_;
            if(next.cancel && !previous.cancel && next.transition.leash_layer>=0 &&
               next.transition.leash_layer!=previous.transition.leash_layer) return false;
            if(!next.cancel && !previous.cancel &&
               next.transition.event_ns<previous.transition.event_ns) return false;
        }
        pending_=next;
        return true;
    }
    bool Ready() const {return pending_.has_value();}
    std::optional<BridgeCommand> Take() {
        auto value=pending_;
        pending_.reset();
        return value;
    }
private:
    std::optional<BridgeCommand> pending_;
};

// Nonblocking, coalesced dispatch. Never make a synchronous WMS call from a
// launcher animation callback, which can be part of a WMS lock dependency.
void QueueTransition(const Transition& transition);
void QueueCancel(int32_t leash_layer, int64_t event_ns);
bool LauncherAnimationEnabled();

}  // namespace hha
