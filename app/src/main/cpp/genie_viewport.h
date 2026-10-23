// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "genie_model.h"
#include <algorithm>
#include <cmath>

namespace genie::model {

// This integration is verified only on portrait fullscreen transitions. A
// landscape or unusually wide presentation keeps the complete stock animation.
// The eased motion has two endpoints; the expanded one is the App window and
// the shrunken one is the icon, which may legitimately be square. Only the
// expanded endpoint must be strictly taller than wide, so a landscape window,
// a square window, a rotation artefact or a degenerate FitSize result is never
// admitted by rounding.
inline bool PortraitWindow(double start_width,double start_height,
                           double end_width,double end_height) {
    for(double value:{start_width,start_height,end_width,end_height}) {
        if(!std::isfinite(value) || value<=0) return false;
    }
    const double start_area=start_width*start_height, end_area=end_width*end_height;
    if(!std::isfinite(start_area) || !std::isfinite(end_area)) return false;
    return start_area<=end_area ? end_height>end_width : start_height>start_width;
}

// The shader's round-rect metric measures both axes in visible-width units.
// Convert each physical endpoint independently: icon and window widths differ,
// and a gesture release can begin from a partially scaled presentation.
inline double CornerRatio(double radius,const Rect& rect) {
    if(!std::isfinite(radius) || !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
       rect.width<=0 || rect.height<=0) return 0.0;
    return std::clamp(radius,0.0,0.5*std::min(rect.width,rect.height))/rect.width;
}

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

}  // namespace genie::model
