#ifndef slic3r_InsetStart_hpp_
#define slic3r_InsetStart_hpp_

#include "ExPolygon.hpp"
#include "Polyline.hpp"

namespace Slic3r {

// First-layer in-wall lead-in: a short extrusion from inside the part to the
// first wall start, so leftover pressure dumps in the material instead of on
// a visible outline.
//
// Returns an empty polyline when the island is only one line thick (inward
// offset by line_width is empty) or a lead-in cannot be placed.

Polyline make_inset_lead_in(
    const ExPolygon &island,
    const Point     &wall_start,
    coord_t          line_width,
    coord_t          preferred_inset);

// Capsule covering an already-extruded lead-in (half line width).
Polygons inset_start_exclusion(const Polyline &lead, coord_t line_width);

// Remove pieces of a later path that would overprint the lead-in.
Polylines clip_against_inset_start(const Polyline &subject, const Polygons &exclusion);

} // namespace Slic3r

#endif
