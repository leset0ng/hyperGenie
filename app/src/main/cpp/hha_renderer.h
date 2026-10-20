// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hha_bridge.h"
#include <algorithm>
#include <vector>

namespace hha {

struct RenderIdentity {
    int32_t parent = -1;
    int64_t request_ns = 0;
};

inline bool SameRender(RenderIdentity a, RenderIdentity b) {
    return a.parent == b.parent && a.request_ns == b.request_ns;
}

// Access is serialized with the renderer's writes and command publication.
class RenderCancellation {
public:
    void Request(RenderIdentity identity, int64_t deadline_ns) {
        identity_ = identity;
        deadline_ns_ = deadline_ns;
    }
    bool Due(RenderIdentity current, int64_t now_ns) const {
        return deadline_ns_ > 0 && now_ns >= deadline_ns_ && SameRender(identity_, current);
    }
    void Clear() { deadline_ns_ = 0; }
private:
    RenderIdentity identity_;
    int64_t deadline_ns_ = 0;
};

inline bool RetainsLayer(int32_t layer, int32_t primary,
                         const std::vector<int32_t>& companions) {
    return layer >= 0 && (layer == primary ||
        std::find(companions.begin(), companions.end(), layer) != companions.end());
}
// The launcher and system_server compile the same mesh shader/solver. Only the
// launcher uses these entry points, on its own retained BBQ icon capabilities.
bool WarmIconRenderer();
void RetainIconSurface(void* surface,const void* owner);
void ReleaseIconSurface(void* surface,const void* owner);
// The first icon shader is appended to the launcher's own host transaction.
// Subsequent vsync writes wait for that transaction's on-commit callback.
bool RenderIcon(void* transaction,void* surface,void* parent,int texture_width,int texture_height,
                int window_width,int window_height,const Transition& transition);
void CancelIconRenderer();
void CancelIconRenderer(RenderIdentity identity);
}
