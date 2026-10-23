// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "genie_bridge.h"

namespace genie {

struct RenderIdentity {
    int32_t parent = -1;
    int64_t request_ns = 0;
};

inline bool SameRender(RenderIdentity a, RenderIdentity b) {
    return a.parent == b.parent && a.request_ns == b.request_ns;
}

// The launcher and system_server compile the same mesh shader/solver. Only the
// launcher uses these entry points, on its own retained BBQ icon capabilities.
bool WarmIconRenderer();
void RetainIconSurface(void* surface,const void* owner);
void ReleaseIconSurface(void* surface,const void* owner);
// The first icon shader is appended to the launcher's own host transaction.
// Subsequent vsync writes wait for that transaction's on-commit callback.
// One publication in the supplied first-show transaction. The server owns
// subsequent preview frames and retirement, so this creates no icon session.
bool PrimeOpeningPreview(void* transaction,void* surface,void* parent,int width,int height,
                         const Transition& transition);
// Return the server-verified full content domain independently of a gesture's
// current presentation. No cached opening is required for a closing request.
bool PrepareContentGroup(void* transaction,void* parent,genie::model::Rect* window,const Transition& transition);
bool PrimeContentPublication(void* transaction,void* surface,void* parent,int width,int height,
                             const Transition& transition);
bool PrepareOpeningPreview(void* transaction,void* parent,int width,int height,
                           const Transition& transition);
// Register the actual floating icon with the central renderer. The returned
// server transaction (including its commit listener) joins this host publication.
bool PrepareIcon(void* host_transaction,void* icon_surface,void* icon_parent,
                 int texture_width,int texture_height,int window_width,int window_height,
                 const Transition& transition);
bool RenderIcon(void* transaction,void* surface,void* parent,int texture_width,int texture_height,
                int window_width,int window_height,const Transition& transition);
void CancelIconRenderer();
void CancelIconRenderer(RenderIdentity identity);
}
