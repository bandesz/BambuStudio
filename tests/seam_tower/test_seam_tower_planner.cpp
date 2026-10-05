#include <catch2/catch.hpp>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/SeamTowerPlanner.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

#include <algorithm>
#include <limits>
#include <vector>

using namespace Slic3r;

static Point mm(double x, double y) { return Point::new_scale(x, y); }

static Polygon square_contour_mm(double size)
{
    const coord_t s = scale_(size);
    return Polygon{Points{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)}};
}

static Polygon hole_contour_mm(double outer, double inner)
{
    const coord_t o = scale_((outer - inner) / 2.0);
    const coord_t i = scale_(inner);
    return Polygon{Points{
        Point(o, o),
        Point(o, o + i),
        Point(o + i, o + i),
        Point(o + i, o)
    }}; // CW hole
}

static SeamTowerParams enabled_params()
{
    SeamTowerParams p;
    p.enabled     = true;
    p.gap         = scale_(0.1);
    p.depth       = scale_(2.0);
    p.in_holes    = false;
    p.min_size    = scale_(3.0);
    p.line_width  = scale_(0.4);
    return p;
}

static std::vector<SeamTowerBuildInput> aligned_cube_inputs(int layers)
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    std::vector<SeamTowerBuildInput> inputs;
    inputs.reserve(size_t(layers));
    for (int layer = 0; layer < layers; ++layer)
        inputs.push_back({0, layer, seam, contour, false});
    return inputs;
}

static int seam_tower_layer_count(const SeamTowerPlanner &planner)
{
    int n = 0;
    for (const auto &kv : planner.layer_collections()) {
        if (kv.second.empty())
            continue;
        if (kv.second.role() == erSeamTower)
            ++n;
    }
    return n;
}

TEST_CASE("Planner builds per-layer erSeamTower collections for a cube with aligned seams", "[SeamTower]")
{
    SeamTowerPlanner planner;
    planner.build(aligned_cube_inputs(3), enabled_params());

    REQUIRE_FALSE(planner.empty());
    REQUIRE(seam_tower_layer_count(planner) >= 1);
    for (int layer = 0; layer < 3; ++layer) {
        const ExtrusionEntityCollection *coll = planner.collection_at_layer(layer);
        REQUIRE(coll != nullptr);
        REQUIRE_FALSE(coll->empty());
        REQUIRE(coll->role() == erSeamTower);
    }
}

TEST_CASE("Disabled planner stays empty", "[SeamTower]")
{
    SeamTowerParams params = enabled_params();
    params.enabled = false;

    SeamTowerPlanner planner;
    planner.build(aligned_cube_inputs(3), params);

    REQUIRE(planner.empty());
    REQUIRE(planner.collection_at_layer(0) == nullptr);
}

TEST_CASE("Spiral vase planner stays empty", "[SeamTower]")
{
    SeamTowerParams params = enabled_params();
    params.spiral_vase = true;

    SeamTowerPlanner planner;
    planner.build(aligned_cube_inputs(3), params);

    REQUIRE(planner.empty());
}

TEST_CASE("Hole stacks are omitted when in_holes is false", "[SeamTower]")
{
    const Polygon hole = hole_contour_mm(20.0, 12.0);
    REQUIRE(hole.is_clockwise());
    const Point seam = Point((hole.points.front().x() + hole.points.back().x()) / 2, hole.points.front().y());

    std::vector<SeamTowerBuildInput> inputs{
        {1, 0, seam, hole, true},
        {1, 1, seam, hole, true},
    };

    SeamTowerParams params = enabled_params();
    params.in_holes = false;

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    REQUIRE(planner.empty());
}

static void collect_path_flow(const ExtrusionEntity *entity, std::vector<float> &heights, std::vector<double> &mm3)
{
    if (entity == nullptr)
        return;
    if (entity->is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            collect_path_flow(child, heights, mm3);
        return;
    }
    if (entity->is_loop()) {
        for (const ExtrusionPath &path : static_cast<const ExtrusionLoop *>(entity)->paths) {
            heights.push_back(path.height);
            mm3.push_back(path.mm3_per_mm);
        }
        return;
    }
    const auto *path = static_cast<const ExtrusionPath *>(entity);
    heights.push_back(path->height);
    mm3.push_back(path->mm3_per_mm);
}

TEST_CASE("Planner fill uses each input's layer height", "[SeamTower]")
{
    auto inputs = aligned_cube_inputs(1);
    inputs.front().height_mm = 0.3f;

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());
    REQUIRE_FALSE(planner.empty());

    const ExtrusionEntityCollection *coll = planner.collection_at_layer(0);
    REQUIRE(coll != nullptr);

    std::vector<float>  heights;
    std::vector<double> mm3;
    for (const ExtrusionEntity *entity : coll->entities)
        collect_path_flow(entity, heights, mm3);

    REQUIRE_FALSE(heights.empty());
    for (float h : heights)
        REQUIRE(h == Approx(0.3f));
    for (double v : mm3)
        REQUIRE(v == Approx(0.4 * 0.3));
}

TEST_CASE("Hole stacks are built when in_holes is true", "[SeamTower]")
{
    const Polygon hole = hole_contour_mm(20.0, 12.0);
    REQUIRE(hole.is_clockwise());
    const Point seam = Point((hole.points.front().x() + hole.points.back().x()) / 2, hole.points.front().y());

    std::vector<SeamTowerBuildInput> inputs{
        {1, 0, seam, hole, true},
        {1, 1, seam, hole, true},
    };

    SeamTowerParams params = enabled_params();
    params.in_holes = true;

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    REQUIRE_FALSE(planner.empty());
    REQUIRE(seam_tower_layer_count(planner) >= 1);
    const ExtrusionEntityCollection *coll = planner.collection_at_layer(0);
    REQUIRE(coll != nullptr);
    REQUIRE(coll->role() == erSeamTower);
}

static double along_wall_extent_mm(const Polygon &contour, const Point &seam, const ExtrusionEntityCollection &coll)
{
    double best = std::numeric_limits<double>::max();
    Vec2d  tangent = Vec2d::Zero();
    for (const Line &line : contour.lines()) {
        Point closest;
        const double d2 = line.distance_to_squared(seam, &closest);
        if (d2 > best)
            continue;
        const Vec2d  dxy = (line.b - line.a).cast<double>();
        const double len = dxy.norm();
        if (len < 1.0)
            continue;
        best    = d2;
        tangent = dxy / len;
    }

    Points pts;
    coll.collect_points(pts);
    double tmin = std::numeric_limits<double>::max();
    double tmax = std::numeric_limits<double>::lowest();
    for (const Point &p : pts) {
        const double t = (p - seam).cast<double>().dot(tangent);
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }
    return unscale<double>(tmax - tmin);
}

TEST_CASE("A stack missing the middle sample still emits a tower on that layer", "[SeamTower]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    const std::vector<SeamTowerBuildInput> inputs{
        {0, 0, seam, contour, false},
        {0, 1, seam, contour, false, 0.2f, nullptr, false},
        {0, 2, seam, contour, false},
    };

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());

    for (int layer = 0; layer <= 2; ++layer) {
        const ExtrusionEntityCollection *coll = planner.collection_at_layer(layer);
        REQUIRE(coll != nullptr);
        REQUIRE_FALSE(coll->empty());
        REQUIRE(coll->role() == erSeamTower);
    }
}

TEST_CASE("Planned island grows past seam tower length when two seams on one edge are farther apart", "[SeamTower]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam_a  = mm(7.0, 0.0);
    const Point   seam_b  = mm(13.0, 0.0);

    SeamTowerParams params = enabled_params();
    params.length = scale_(4.0);
    // Jump threshold is max(depth, 4 * line_width). 8 mm keeps a 6 mm drift in one stack.
    params.depth = scale_(8.0);

    const std::vector<SeamTowerBuildInput> inputs{
        {0, 0, seam_a, contour, false},
        {0, 1, seam_b, contour, false},
    };

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    const ExtrusionEntityCollection *coll = planner.collection_at_layer(0);
    REQUIRE(coll != nullptr);
    REQUIRE_FALSE(coll->empty());
    REQUIRE(along_wall_extent_mm(contour, seam_a, *coll) > 4.0);
}
