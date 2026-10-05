#ifndef slic3r_SeamTower_hpp_
#define slic3r_SeamTower_hpp_

#include "ExPolygon.hpp"
#include "Polygon.hpp"

#include <vector>

namespace Slic3r {

// Closed strip beside a wall seam: offset the contour away from the solid by
// `gap`, then by `depth`, and cap the ends. Arc length is
// max(length, stack_window), centred on `seam`. Length defaults to 4 mm.
//
// On a clockwise hole, the strip is built into the hole. `min_size` is the
// inscribed opening that must remain after `gap` + `depth`; smaller holes
// return empty. Outer contours ignore `min_size`.
//
// If `previous` is non-empty, the island is clipped to that footprint.
// Parts of the strip that would hang are replaced by a rectangle of size
// depth × max(length, remaining length) clipped to `previous`. If the result
// is empty or narrower than one `line_width`, returns empty.
// Layer 0: pass nullptr or an empty ExPolygon (no clip).
//
// Returns an empty ExPolygon when the strip cannot be built.

ExPolygon make_seam_tower_island(
    const Polygon    &contour,
    const Point      &seam,
    coord_t           gap,
    coord_t           depth,
    coord_t           length = scale_(4),
    coord_t           stack_window = 0,
    coord_t           min_size = 0,
    const ExPolygon  *previous = nullptr,
    coord_t           line_width = 0);

// One seam sample on a contour at a layer. Clustering walks these bottom-up.
struct SeamTowerSample {
    int   contour_id;
    int   layer_index;
    Point point;
};

struct SeamTowerSeam {
    int   layer_index;
    Point point;
};

// Stable stack of seams on one contour. first_layer is the first assigned
// seam layer. last_layer is the last assigned seam layer.
struct SeamTowerStack {
    int                         contour_id;
    int                         first_layer;
    int                         last_layer;
    std::vector<SeamTowerSeam>  seams;
};

// Group per-layer seam points into stacks. A sample joins the open stack on
// the same contour whose last XY is within max(depth, 4 * line_width);
// otherwise it opens a new stack.
std::vector<SeamTowerStack> cluster_seam_tower_stacks(
    const std::vector<SeamTowerSample> &samples,
    coord_t                             depth,
    coord_t                             line_width);

// Extra arrange clearance (mm) so two objects keep gap+depth between walls.
// Zero when the feature is off. Conservative: exact tower polygons are unknown
// at arrange time.
coordf_t seam_tower_arrange_inflation_mm(bool enabled, double gap_mm, double depth_mm);

} // namespace Slic3r

#endif
