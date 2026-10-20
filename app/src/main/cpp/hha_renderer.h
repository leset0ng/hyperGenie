// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hha_bridge.h"

namespace hha {
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
}
