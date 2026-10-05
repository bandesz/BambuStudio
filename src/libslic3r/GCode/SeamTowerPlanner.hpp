#ifndef slic3r_SeamTowerPlanner_hpp_
#define slic3r_SeamTowerPlanner_hpp_

#include "../BoundingBox.hpp"
#include "../ExPolygon.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
class SeamPlacer;

// GCode.cpp assigns spiral_vase only. SeamTowerPlanner::build(const Print&)
// sets shared.enabled, skips objects whose seam_tower config is off, and
// applies per-object keys (gap, depth, length, in_holes, min_size, collision).
struct SeamTowerParams {
    bool    enabled     = false;
    bool    spiral_vase = false;
    coord_t gap         = 0;
    coord_t depth       = 0;
    coord_t length      = scale_(4);
    bool    in_holes    = false;
    coord_t min_size    = 0;
    coord_t line_width  = 0;
    // Other objects / supports. Undefined plate / wipe-tower bboxes are ignored.
    ExPolygons  collision_polygons;
    BoundingBox plate_bbox;
    BoundingBox wipe_tower_bbox;
};

// One seam sample plus the wall contour it sits on. Used by tests and by the
// Print/SeamPlacer path after extracting perimeters.
struct SeamTowerBuildInput {
    int     contour_id   = 0;
    int     layer_index  = 0;
    Point   point;
    Polygon contour;
    bool    is_hole      = false;
    float   height_mm    = 0.2f;
    const PrintObject *object = nullptr;
};

class SeamTowerPlanner
{
public:
    void build(const Print &print, const SeamPlacer &seam_placer, const SeamTowerParams &params);
    void build(const std::vector<SeamTowerBuildInput> &inputs, const SeamTowerParams &params);

    bool empty() const { return m_emits.empty(); }

    const ExtrusionEntityCollection *collection_at_layer(int layer_index) const;
    const ExtrusionEntityCollection *take_matching_tower(int layer_index,
                                                         const Point        &loop_start,
                                                         coord_t             max_distance,
                                                         const PrintObject  *object = nullptr,
                                                         const Point        &instance_shift = Point(0, 0));
    // Towers on seam layers that take_matching_tower did not pair. Marks them printed.
    std::vector<const ExtrusionEntityCollection *> take_unprinted_towers(int layer_index,
                                                                         const PrintObject *object,
                                                                         const Point       &instance_shift);
    bool has_tower_for(int layer_index, const Point &loop_start, coord_t max_distance, const PrintObject *object = nullptr,
                       const Point &instance_shift = Point(0, 0)) const;
    bool has_tower_for(int layer_index, const ExPolygon &island, const PrintObject *object = nullptr,
                       const Point &instance_shift = Point(0, 0)) const;
    coord_t match_distance() const { return m_match_distance; }
    std::map<int, ExtrusionEntityCollection> layer_collections() const;
    const std::vector<std::string> &warnings() const { return m_warnings; }

private:
    struct EmitItem
    {
        ExtrusionEntityCollection collection;
        Point                     hop_target;
        std::set<std::pair<const PrintObject *, Point>> printed;
        // PrintInstance::shift values that must not print this tower.
        std::set<Point>           skipped_shifts;
        const PrintObject        *object  = nullptr;
    };

    void append_stacks(const std::vector<SeamTowerBuildInput> &inputs,
                       const SeamTowerParams                  &params,
                       ExPolygons                             &accepted,
                       const std::vector<Point>               &world_shifts,
                       const std::vector<Point>               &plate_shifts = {},
                       const PrintObject                      *po = nullptr,
                       const ExPolygons                       &self_local = {});

    std::map<int, std::vector<EmitItem>>     m_emits;
    std::vector<std::string>                 m_warnings;
    coord_t                                  m_match_distance = 0;
};

} // namespace Slic3r

#endif
