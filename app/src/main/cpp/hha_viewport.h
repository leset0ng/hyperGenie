// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "hha_genie.h"
#include <cmath>

namespace hha::genie {

// Android's SF parent exposes one uniform scale. Adapt the document's changing
// local B aspect into that host, preserving raw row differences and centre lag.
// Endpoints are exact: minimized content has icon aspect, full content identity.
inline std::array<Point,9> ContourMesh(const Animation& animation,const Parameters& p) {
    auto mesh=animation.Mesh();
    const double q=1-p.genie_scale;
    if(q<=0) return mesh;
    const double seed_w=p.width+q*(p.minimized_width-p.width);
    const double seed_h=p.height+q*(p.minimized_height-p.height);
    const double seed_factor=(p.minimized_height/p.minimized_width)*seed_w/seed_h;
    const double factor=1+animation.Scalars()[18].value/q*(seed_factor-1);
    const double center_y=mesh[4].y;
    for(auto& point:mesh) point.y=center_y+(point.y-center_y)*factor;
    return mesh;
}

// Uniform host transform which places the recovered raw icon-end mesh exactly
// on the physical icon. The full endpoint is the identity viewport. This frees
// the outer silhouette from SF's animated aspect crop and rounded rectangle.
inline bool HostViewportForIcon(const Rect& icon,const Rect& window,Rect* host) {
    if(!host) return false;
    Parameters parameters;
    Animation seed;
    if(!ParametersForIcon(icon,window,&parameters) || !seed.Reset(parameters,true)) return false;
    const auto mesh=ContourMesh(seed,parameters);
    const double span_x=mesh[2].x-mesh[0].x;
    const double span_y=mesh[6].y-mesh[0].y;
    if(span_x<=0 || span_y<=0) return false;
    const double sx=icon.width/(span_x*window.width);
    const double sy=icon.height/(span_y*window.height);
    if(!std::isfinite(sx) || !std::isfinite(sy) || std::abs(sx-sy)>1e-6) return false;
    *host={icon.left-mesh[0].x*window.width*sx,
           icon.top-mesh[0].y*window.height*sy,window.width*sx,window.height*sy};
    return true;
}

}  // namespace hha::genie
