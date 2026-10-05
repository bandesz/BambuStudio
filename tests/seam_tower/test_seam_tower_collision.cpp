#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/GCode/SeamTowerPlanner.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SeamTower.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace Slic3r;

static const std::string kSkipMsg = "Seam tower skipped: not enough space.";

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

static Polygon square_from_to_mm(double x0, double y0, double x1, double y1)
{
    return Polygon{Points{mm(x0, y0), mm(x1, y0), mm(x1, y1), mm(x0, y1)}};
}

static SeamTowerParams enabled_params()
{
    SeamTowerParams p;
    p.enabled    = true;
    p.gap        = scale_(0.1);
    p.depth      = scale_(2.0);
    p.in_holes   = false;
    p.min_size   = scale_(3.0);
    p.line_width = scale_(0.4);
    return p;
}

static std::vector<SeamTowerBuildInput> cube_stack(const Point &seam, int contour_id, int layers)
{
    const Polygon contour = square_contour_mm(20.0);
    std::vector<SeamTowerBuildInput> inputs;
    inputs.reserve(size_t(layers));
    for (int layer = 0; layer < layers; ++layer)
        inputs.push_back({contour_id, layer, seam, contour, false});
    return inputs;
}

static bool has_skip_warning(const SeamTowerPlanner &planner)
{
    const auto &w = planner.warnings();
    return std::find(w.begin(), w.end(), kSkipMsg) != w.end();
}

static Points extrusion_points(const SeamTowerPlanner &planner)
{
    Points pts;
    for (const auto &kv : planner.layer_collections())
        kv.second.collect_points(pts);
    return pts;
}

TEST_CASE("Planner warns when a tower intersects another object", "[SeamTower]")
{
    const Point   seam    = mm(10.0, 0.0);
    const Polygon contour = square_contour_mm(20.0);
    SeamTowerParams params = enabled_params();
    const ExPolygon island = make_seam_tower_island(contour, seam, params.gap, params.depth);
    REQUIRE_FALSE(island.empty());

    params.collision_polygons = ExPolygons{ExPolygon{get_extents(island).polygon()}};

    SeamTowerPlanner planner;
    planner.build(cube_stack(seam, 0, 2), params);

    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("Planner warns when a tower leaves the plate", "[SeamTower]")
{
    const Point seam = mm(10.0, 0.0);
    SeamTowerParams params = enabled_params();
    params.plate_bbox = BoundingBox(mm(0.0, 0.0), mm(256.0, 256.0));

    SeamTowerPlanner planner;
    planner.build(cube_stack(seam, 0, 2), params);

    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("Planner warns when a tower intersects the prime-tower box", "[SeamTower]")
{
    const Point   seam    = mm(10.0, 0.0);
    const Polygon contour = square_contour_mm(20.0);
    SeamTowerParams params = enabled_params();
    const ExPolygon island = make_seam_tower_island(contour, seam, params.gap, params.depth);
    REQUIRE_FALSE(island.empty());

    params.wipe_tower_bbox = get_extents(island);

    SeamTowerPlanner planner;
    planner.build(cube_stack(seam, 0, 2), params);

    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("Planner warns when a hole is below min_size", "[SeamTower]")
{
    const Polygon hole = hole_contour_mm(20.0, 8.0);
    REQUIRE(hole.is_clockwise());
    const Point seam = Point((hole.points.front().x() + hole.points.back().x()) / 2, hole.points.front().y());

    SeamTowerParams params = enabled_params();
    params.in_holes = true;
    params.min_size = scale_(10.0);

    const ExPolygon island = make_seam_tower_island(hole, seam, params.gap, params.depth, scale_(4), 0, params.min_size);
    REQUIRE(island.empty());

    std::vector<SeamTowerBuildInput> inputs{{1, 0, seam, hole, true}, {1, 1, seam, hole, true}};

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("Planner warns when overhang clip leaves an empty island", "[SeamTower]")
{
    SeamTowerParams params = enabled_params();
    params.depth = scale_(0.3);

    const Polygon contour0 = square_contour_mm(20.0);
    const Point   seam0    = mm(10.0, 0.0);
    const ExPolygon t0     = make_seam_tower_island(contour0, seam0, params.gap, params.depth);
    REQUIRE_FALSE(t0.empty());

    const Polygon contour1 = square_from_to_mm(2.0, 2.0, 18.0, 18.0);
    const Point   seam1    = mm(10.0, 2.0);
    const ExPolygon clipped = make_seam_tower_island(
        contour1, seam1, params.gap, params.depth, scale_(4), 0, 0, &t0, params.line_width);
    REQUIRE(clipped.empty());

    std::vector<SeamTowerBuildInput> inputs{
        {0, 0, seam0, contour0, false},
        {0, 1, seam1, contour1, false},
    };

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("A colliding stack does not drop the other tower on the same object", "[SeamTower]")
{
    const Point bottom = mm(10.0, 0.0);
    const Point top    = mm(10.0, 20.0);
    const Polygon contour = square_contour_mm(20.0);
    SeamTowerParams params = enabled_params();

    const ExPolygon bottom_island = make_seam_tower_island(contour, bottom, params.gap, params.depth);
    REQUIRE_FALSE(bottom_island.empty());
    params.collision_polygons = ExPolygons{ExPolygon{get_extents(bottom_island).polygon()}};

    auto inputs = cube_stack(bottom, 0, 2);
    auto top_inputs = cube_stack(top, 1, 2);
    inputs.insert(inputs.end(), top_inputs.begin(), top_inputs.end());

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    REQUIRE_FALSE(planner.empty());
    REQUIRE(has_skip_warning(planner));

    const Points pts = extrusion_points(planner);
    REQUIRE_FALSE(pts.empty());
    const bool any_top = std::any_of(pts.begin(), pts.end(), [](const Point &p) { return p.y() > scale_(20); });
    const bool any_bottom = std::any_of(pts.begin(), pts.end(), [](const Point &p) { return p.y() < 0; });
    REQUIRE(any_top);
    REQUIRE_FALSE(any_bottom);
}

static Polygon hexagon_contour_mm(double radius)
{
    Points pts;
    pts.reserve(6);
    for (int i = 0; i < 6; ++i) {
        const double a = double(i) * (M_PI / 3.0);
        pts.push_back(mm(radius * std::cos(a), radius * std::sin(a)));
    }
    Polygon p{std::move(pts)};
    p.make_counter_clockwise();
    return p;
}

static std::vector<SeamTowerBuildInput> hexagon_stack(const Polygon &contour, const Point &seam, int layers)
{
    std::vector<SeamTowerBuildInput> inputs;
    inputs.reserve(size_t(layers));
    for (int layer = 0; layer < layers; ++layer)
        inputs.push_back({0, layer, seam, contour, false});
    return inputs;
}

TEST_CASE("A hexagon on a large plate builds without a skip warning", "[SeamTower]")
{
    const Polygon contour = hexagon_contour_mm(10.0);
    const Point   vertex  = contour.points.front();
    const Point   mid     = (contour.points[0] + contour.points[1]) / 2;
    SeamTowerParams params = enabled_params();
    params.plate_bbox = BoundingBox(mm(-50.0, -50.0), mm(256.0, 256.0));

    SECTION("seam at a vertex") {
        SeamTowerPlanner planner;
        planner.build(hexagon_stack(contour, vertex, 3), params);
        REQUIRE_FALSE(planner.empty());
        REQUIRE(planner.warnings().empty());
    }

    SECTION("seam at mid-edge") {
        SeamTowerPlanner planner;
        planner.build(hexagon_stack(contour, mid, 3), params);
        REQUIRE_FALSE(planner.empty());
        REQUIRE(planner.warnings().empty());
    }

    SECTION("small 8 mm hexagon, seam at a vertex") {
        const Polygon small = hexagon_contour_mm(4.0);
        SeamTowerPlanner planner;
        planner.build(hexagon_stack(small, small.points.front(), 3), params);
        REQUIRE_FALSE(planner.empty());
        REQUIRE(planner.warnings().empty());
    }
}

TEST_CASE("A hexagon with a seam facing off the plate is skipped", "[SeamTower]")
{
    const Polygon contour = hexagon_contour_mm(10.0);
    SeamTowerParams params = enabled_params();
    params.plate_bbox      = BoundingBox(mm(0.0, 0.0), mm(256.0, 256.0));

    SeamTowerPlanner planner;
    planner.build(hexagon_stack(contour, contour.points[3], 2), params); // vertex at -X
    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("Unstable hexagon seams still keep one tower on a large plate", "[SeamTower]")
{
    const Polygon contour = hexagon_contour_mm(4.0);
    SeamTowerParams params = enabled_params();
    params.plate_bbox      = BoundingBox(mm(-50.0, -50.0), mm(256.0, 256.0));

    std::vector<SeamTowerBuildInput> inputs;
    for (int layer = 0; layer < 6; ++layer)
        inputs.push_back({0, layer, contour.points[size_t(layer) % 6], contour, false});

    SeamTowerPlanner planner;
    planner.build(inputs, params);
    REQUIRE_FALSE(planner.empty());
}

TEST_CASE("A cube inside a large plate still builds without a skip warning", "[SeamTower]")
{
    SeamTowerParams params = enabled_params();
    params.plate_bbox = BoundingBox(mm(-50.0, -50.0), mm(256.0, 256.0));

    SeamTowerPlanner planner;
    planner.build(cube_stack(mm(10.0, 0.0), 0, 2), params);

    REQUIRE_FALSE(planner.empty());
    REQUIRE(planner.warnings().empty());
}

TEST_CASE("Planner skips a tower when depth is too thin for one line width", "[SeamTower]")
{
    SECTION("depth zero") {
        SeamTowerParams params = enabled_params();
        params.depth           = 0;

        SeamTowerPlanner planner;
        planner.build(cube_stack(mm(10.0, 0.0), 0, 2), params);

        REQUIRE(planner.empty());
        REQUIRE(has_skip_warning(planner));
    }

    SECTION("depth thinner than one line width") {
        SeamTowerParams params = enabled_params();
        params.depth           = scale_(0.3);

        SeamTowerPlanner planner;
        planner.build(cube_stack(mm(10.0, 0.0), 0, 2), params);

        REQUIRE(planner.empty());
        REQUIRE(has_skip_warning(planner));
    }
}

TEST_CASE("Arrange inflation helper is zero when seam tower is off", "[SeamTower]")
{
    REQUIRE(seam_tower_arrange_inflation_mm(false, 0.1, 2.0) == 0.0);
}

TEST_CASE("Arrange inflation helper is gap plus depth when seam tower is on", "[SeamTower]")
{
    REQUIRE(seam_tower_arrange_inflation_mm(true, 0.1, 2.0) == Approx(2.1));
}

static coord_t arrange_inflation_mm(bool enabled, double brim_mm, double gap_mm, double depth_mm)
{
    arrangement::ArrangePolygon ap;
    ap.brim_width = brim_mm;
    arrangement::ArrangePolygons selected{ap};
    DynamicPrintConfig cfg;
    cfg.set_key_value("seam_tower", new ConfigOptionBool(enabled));
    cfg.set_key_value("seam_tower_gap", new ConfigOptionFloat(gap_mm));
    cfg.set_key_value("seam_tower_depth", new ConfigOptionFloat(depth_mm));
    arrangement::ArrangeParams params;
    arrangement::update_selected_items_inflation(selected, cfg, params);
    return selected.front().inflation;
}

TEST_CASE("Arrange inflation is at least gap plus depth larger when seam tower is on", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    const coord_t off  = arrange_inflation_mm(false, brim, gap, depth);
    const coord_t on   = arrange_inflation_mm(true, brim, gap, depth);
    REQUIRE(on >= off + scaled(gap + depth));
}

static coord_t unselected_arrange_inflation(
    const DynamicPrintConfig &cfg, double brim_mm, bool is_virt_object, bool is_wipe_tower)
{
    arrangement::ArrangePolygon ap;
    ap.brim_width     = brim_mm;
    ap.is_virt_object = is_virt_object;
    ap.is_wipe_tower  = is_wipe_tower;
    arrangement::ArrangePolygons unselected{ap};
    arrangement::ArrangeParams params;
    arrangement::update_unselected_items_inflation(unselected, cfg, params);
    return unselected.front().inflation;
}

static DynamicPrintConfig seam_tower_arrange_config(bool enabled, double gap_mm, double depth_mm)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("seam_tower", new ConfigOptionBool(enabled));
    cfg.set_key_value("seam_tower_gap", new ConfigOptionFloat(gap_mm));
    cfg.set_key_value("seam_tower_depth", new ConfigOptionFloat(depth_mm));
    return cfg;
}

TEST_CASE("Unselected arrange inflation is at least gap plus depth larger when seam tower is on", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    const coord_t off  = unselected_arrange_inflation(seam_tower_arrange_config(false, gap, depth), brim, false, false);
    const coord_t on   = unselected_arrange_inflation(seam_tower_arrange_config(true, gap, depth), brim, false, false);
    REQUIRE(on >= off + scaled(gap + depth));
}

TEST_CASE("Unselected virtual wipe tower does not gain seam tower gap plus depth", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    const coord_t off  = unselected_arrange_inflation(seam_tower_arrange_config(false, gap, depth), brim, true, true);
    const coord_t on   = unselected_arrange_inflation(seam_tower_arrange_config(true, gap, depth), brim, true, true);
    REQUIRE(on == off);
}

TEST_CASE("Unselected arrange inflation adds nothing when seam tower keys are missing", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    const coord_t missing = unselected_arrange_inflation(DynamicPrintConfig{}, brim, false, false);
    const coord_t off     = unselected_arrange_inflation(seam_tower_arrange_config(false, gap, depth), brim, false, false);
    REQUIRE(missing == off);
}

static coord_t selected_arrange_inflation(
    const DynamicPrintConfig &cfg, double brim_mm, bool is_virt_object, bool is_wipe_tower)
{
    arrangement::ArrangePolygon ap;
    ap.brim_width     = brim_mm;
    ap.is_virt_object = is_virt_object;
    ap.is_wipe_tower  = is_wipe_tower;
    arrangement::ArrangePolygons selected{ap};
    arrangement::ArrangeParams params;
    arrangement::update_selected_items_inflation(selected, cfg, params);
    return selected.front().inflation;
}

TEST_CASE("Selected virtual wipe tower does not gain seam tower gap plus depth", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    const coord_t off = selected_arrange_inflation(seam_tower_arrange_config(false, gap, depth), brim, true, true);
    const coord_t on  = selected_arrange_inflation(seam_tower_arrange_config(true, gap, depth), brim, true, true);
    REQUIRE(on == off);
}

TEST_CASE("Selected arrange inflation adds nothing when seam tower keys are missing", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    const coord_t missing = selected_arrange_inflation(DynamicPrintConfig{}, brim, false, false);
    const coord_t off     = selected_arrange_inflation(seam_tower_arrange_config(false, gap, depth), brim, false, false);
    REQUIRE(missing == off);
}

static arrangement::ArrangePolygon polygon_with_seam_tower_margin(double brim_mm, bool enabled, double gap_mm, double depth_mm)
{
    arrangement::ArrangePolygon ap;
    ap.brim_width = brim_mm;
    ap.seam_tower_margin = arrangement::SeamTowerMargin{enabled, gap_mm, depth_mm};
    return ap;
}

TEST_CASE("Selected arrange inflation uses each polygon's own seam tower margin", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    // Process preset is on, with a different gap and depth than either object.
    const DynamicPrintConfig preset = seam_tower_arrange_config(true, 9.0, 9.0);

    arrangement::ArrangePolygons selected{
        polygon_with_seam_tower_margin(brim, true, gap, depth),
        polygon_with_seam_tower_margin(brim, false, gap, depth),
    };
    arrangement::ArrangeParams params;
    arrangement::update_selected_items_inflation(selected, preset, params);

    const coord_t brim_inflation = scaled(brim);
    REQUIRE(selected[0].inflation == brim_inflation + scaled(seam_tower_arrange_inflation_mm(true, gap, depth)));
    REQUIRE(selected[1].inflation == brim_inflation);
}

TEST_CASE("Selected object override on grows when the process preset has seam tower off", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    arrangement::ArrangePolygons selected{polygon_with_seam_tower_margin(brim, true, gap, depth)};
    arrangement::ArrangeParams params;
    arrangement::update_selected_items_inflation(selected, seam_tower_arrange_config(false, gap, depth), params);
    REQUIRE(selected.front().inflation == scaled(brim) + scaled(seam_tower_arrange_inflation_mm(true, gap, depth)));
}

TEST_CASE("Unselected arrange inflation uses each polygon's own seam tower margin", "[SeamTower]")
{
    const double brim  = 5.0;
    const double gap   = 0.1;
    const double depth = 2.0;
    const DynamicPrintConfig preset = seam_tower_arrange_config(true, 9.0, 9.0);

    arrangement::ArrangePolygons unselected{
        polygon_with_seam_tower_margin(brim, true, gap, depth),
        polygon_with_seam_tower_margin(brim, false, gap, depth),
    };
    arrangement::ArrangeParams params;
    arrangement::update_unselected_items_inflation(unselected, preset, params);

    const coord_t brim_inflation = scaled(brim);
    REQUIRE(unselected[0].inflation == brim_inflation + scaled(seam_tower_arrange_inflation_mm(true, gap, depth)));
    REQUIRE(unselected[1].inflation == brim_inflation);
}

static DynamicPrintConfig arrange_instance_config(bool seam_tower_on, double gap_mm, double depth_mm)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    cfg.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    cfg.set_key_value("seam_tower", new ConfigOptionBool(seam_tower_on));
    cfg.set_key_value("seam_tower_gap", new ConfigOptionFloat(gap_mm));
    cfg.set_key_value("seam_tower_depth", new ConfigOptionFloat(depth_mm));
    return cfg;
}

TEST_CASE("get_instance_arrange_poly stores the object seam tower margin over the process preset", "[SeamTower]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "cube";
    object->add_volume(make_cube(20, 20, 20));
    ModelInstance *instance = object->add_instance();

    object->config.set_key_value("seam_tower", new ConfigOptionBool(true));
    object->config.set_key_value("seam_tower_gap", new ConfigOptionFloat(0.4));
    object->config.set_key_value("seam_tower_depth", new ConfigOptionFloat(3.0));

    arrangement::ArrangePolygon on_override = get_instance_arrange_poly(instance, arrange_instance_config(false, 0.1, 2.0));
    REQUIRE(on_override.seam_tower_margin.has_value());
    REQUIRE(on_override.seam_tower_margin->enabled);
    REQUIRE(on_override.seam_tower_margin->gap == Approx(0.4));
    REQUIRE(on_override.seam_tower_margin->depth == Approx(3.0));

    object->config.set_key_value("seam_tower", new ConfigOptionBool(false));
    arrangement::ArrangePolygon off_override = get_instance_arrange_poly(instance, arrange_instance_config(true, 0.1, 2.0));
    REQUIRE(off_override.seam_tower_margin.has_value());
    REQUIRE_FALSE(off_override.seam_tower_margin->enabled);

    object->config.erase("seam_tower");
    object->config.erase("seam_tower_gap");
    object->config.erase("seam_tower_depth");
    arrangement::ArrangePolygon from_preset = get_instance_arrange_poly(instance, arrange_instance_config(true, 0.1, 2.0));
    REQUIRE(from_preset.seam_tower_margin.has_value());
    REQUIRE(from_preset.seam_tower_margin->enabled);
    REQUIRE(from_preset.seam_tower_margin->gap == Approx(0.1));
    REQUIRE(from_preset.seam_tower_margin->depth == Approx(2.0));

    DynamicPrintConfig bare;
    bare.set_key_value("enable_support", new ConfigOptionBool(false));
    bare.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    arrangement::ArrangePolygon missing = get_instance_arrange_poly(instance, bare);
    REQUIRE(missing.seam_tower_margin.has_value());
    REQUIRE_FALSE(missing.seam_tower_margin->enabled);
    REQUIRE(missing.seam_tower_margin->gap == 0.0);
    REQUIRE(missing.seam_tower_margin->depth == 0.0);
}

// Same square on every layer; seam samples only from layer 2 upward.
static std::vector<SeamTowerBuildInput> seams_from_layer(int first_seam, int last_seam)
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    std::vector<SeamTowerBuildInput> inputs;
    // The wall is on the bed even where the seam has not started.
    for (int layer = 0; layer < first_seam; ++layer)
        inputs.push_back({0, layer, seam, contour, false, 0.2f, nullptr, false});
    for (int layer = first_seam; layer <= last_seam; ++layer)
        inputs.push_back({0, layer, seam, contour, false});
    return inputs;
}

TEST_CASE("A seam above the bed still builds a tower from the first layer", "[SeamTower]")
{
    const Point seam = mm(10.0, 0.0);
    SeamTowerPlanner planner;
    planner.build(seams_from_layer(3, 6), enabled_params());

    REQUIRE_FALSE(planner.empty());
    REQUIRE(planner.warnings().empty());
    for (int layer = 0; layer <= 6; ++layer) {
        const ExtrusionEntityCollection *coll = planner.collection_at_layer(layer);
        REQUIRE(coll != nullptr);
        REQUIRE_FALSE(coll->empty());
    }
    // Layers under the first seam are inactive: they are not paired with a wall.
    REQUIRE(planner.take_matching_tower(0, seam, planner.match_distance()) == nullptr);
    REQUIRE_FALSE(planner.has_tower_for(0, seam, planner.match_distance()));
    REQUIRE_FALSE(planner.take_unprinted_towers(0, nullptr, Point(0, 0)).empty());
    // The layer that has the seam is printed with that wall.
    REQUIRE(planner.has_tower_for(3, seam, planner.match_distance()));
    REQUIRE(planner.take_matching_tower(3, seam, planner.match_distance()) != nullptr);
    REQUIRE(planner.take_unprinted_towers(3, nullptr, Point(0, 0)).empty());
}

TEST_CASE("A stack builds from the first layer through its last seam", "[SeamTower]")
{
    SeamTowerPlanner planner;
    planner.build(seams_from_layer(2, 4), enabled_params());

    REQUIRE_FALSE(planner.empty());
    REQUIRE(planner.warnings().empty());
    for (int layer = 0; layer <= 4; ++layer) {
        const ExtrusionEntityCollection *coll = planner.collection_at_layer(layer);
        REQUIRE(coll != nullptr);
        REQUIRE_FALSE(coll->empty());
    }
}

TEST_CASE("A speck seam beside the outer wall does not grow its own tower", "[SeamTower]")
{
    // The hexagon's outer wall has one seam on every layer. A voronoi facet
    // adds a second external loop on one upper layer, 1.5 mm outside that wall.
    // That facet is not the wall, so it must not become a column on the bed.
    const Point outer_seam = mm(10.0, 0.0);
    std::vector<SeamTowerBuildInput> inputs = cube_stack(outer_seam, 0, 6);
    const Polygon speck = square_from_to_mm(9.0, -2.5, 11.0, -1.5);
    const Point   speck_seam = mm(10.0, -1.5);
    inputs.push_back({9, 5, speck_seam, speck, false});

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());

    for (int layer = 0; layer <= 5; ++layer) {
        REQUIRE(planner.take_matching_tower(layer, outer_seam, planner.match_distance()) != nullptr);
        REQUIRE(planner.take_unprinted_towers(layer, nullptr, Point(0, 0)).empty());
    }
}

TEST_CASE("A wall that changes contour id still stands from the first layer", "[SeamTower]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    std::vector<SeamTowerBuildInput> inputs;
    for (int layer = 0; layer <= 3; ++layer)
        inputs.push_back({0, layer, seam, contour, false});
    for (int layer = 4; layer <= 6; ++layer)
        inputs.push_back({7, layer, seam, contour, false});

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());

    REQUIRE(planner.warnings().empty());
    for (int layer = 0; layer <= 6; ++layer) {
        const ExtrusionEntityCollection *coll = planner.collection_at_layer(layer);
        REQUIRE(coll != nullptr);
        REQUIRE_FALSE(coll->empty());
    }
}

TEST_CASE("A column whose first layer cannot be built emits nothing on the upper layers", "[SeamTower]")
{
    // Layer 0 is a hole below min_size, so its island is empty. Layers 2..4 are
    // a square that would build on its own. Same contour and seam, so one stack.
    // A collision box is not used: that would also reject the upper layers.
    const Polygon hole = hole_contour_mm(20.0, 8.0);
    const Point   seam = Point((hole.points.front().x() + hole.points.back().x()) / 2, hole.points.front().y());
    const Polygon square = square_contour_mm(20.0);

    SeamTowerParams params = enabled_params();
    params.in_holes = true;
    params.min_size = scale_(10.0);

    REQUIRE(make_seam_tower_island(hole, seam, params.gap, params.depth, scale_(4), 0, params.min_size).empty());
    REQUIRE_FALSE(make_seam_tower_island(square, seam, params.gap, params.depth, scale_(4), 0, params.min_size).empty());

    std::vector<SeamTowerBuildInput> inputs{{0, 0, seam, hole, true}};
    for (int layer = 2; layer <= 4; ++layer)
        inputs.push_back({0, layer, seam, square, false});

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    REQUIRE(planner.empty());
    for (int layer = 0; layer <= 4; ++layer)
        REQUIRE(planner.collection_at_layer(layer) == nullptr);
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("An unmatched tower on the seam's first layer is still available to print", "[SeamTower]")
{
    SeamTowerPlanner planner;
    planner.build(seams_from_layer(2, 4), enabled_params());
    // The grounded layers have no seam, so they are flushed as unmatched pads.
    REQUIRE_FALSE(planner.take_unprinted_towers(0, nullptr, Point(0, 0)).empty());
    const Point seam = mm(10.0, 0.0);
    REQUIRE(planner.take_matching_tower(2, seam, planner.match_distance()) != nullptr);
    REQUIRE(planner.take_unprinted_towers(2, nullptr, Point(0, 0)).empty());
}

TEST_CASE("Flushing unprinted towers before matching leaves no hop candidate", "[SeamTower]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    SeamTowerPlanner planner;
    planner.build({{0, 0, seam, contour, false}}, enabled_params());
    REQUIRE_FALSE(planner.take_unprinted_towers(0, nullptr, Point(0, 0)).empty());
    REQUIRE(planner.take_matching_tower(0, seam, planner.match_distance()) == nullptr);
}

TEST_CASE("Overhanging wall keeps a seam tower when the gap clears support expansion", "[SeamTower]")
{
    // The foot is 4 mm inside the wall, and the two outlines share a contour id
    // because most of their area overlaps. Supports fill that overhang and stop
    // 1 mm outside the wall. The tower gap is 2 mm, so a pad beside the wall is
    // clear; a pad beside the foot is not.
    const Polygon foot = square_contour_mm(40.0);
    const Polygon wall = square_from_to_mm(-4.0, -4.0, 44.0, 44.0);
    const Point   foot_seam = mm(20.0, 0.0);
    const Point   wall_seam = mm(20.0, -4.0);
    const ExPolygon support{square_from_to_mm(-5.0, -5.0, 45.0, 45.0)};

    SeamTowerParams params = enabled_params();
    params.gap   = scale_(2.0);
    params.depth = scale_(5.0);
    params.length = scale_(5.0);
    params.collision_polygons = ExPolygons{support};

    const ExPolygon beside_foot = make_seam_tower_island(foot, foot_seam, params.gap, params.depth, params.length);
    const ExPolygon beside_wall = make_seam_tower_island(wall, wall_seam, params.gap, params.depth, params.length);
    REQUIRE_FALSE(beside_foot.empty());
    REQUIRE_FALSE(beside_wall.empty());
    REQUIRE_FALSE(intersection_ex(ExPolygons{beside_foot}, ExPolygons{support}).empty());
    REQUIRE(intersection_ex(ExPolygons{beside_wall}, ExPolygons{support}).empty());

    std::vector<SeamTowerBuildInput> inputs;
    for (int layer = 0; layer < 4; ++layer)
        inputs.push_back({0, layer, foot_seam, foot, false});
    for (int layer = 4; layer < 12; ++layer)
        inputs.push_back({0, layer, wall_seam, wall, false});

    SeamTowerPlanner planner;
    planner.build(inputs, params);

    REQUIRE_FALSE(planner.empty());
    REQUIRE_FALSE(has_skip_warning(planner));
    const Points pts = extrusion_points(planner);
    REQUIRE_FALSE(pts.empty());
    for (const Point &pt : pts)
        REQUIRE_FALSE(support.contains(pt));
}

TEST_CASE("Many separated towers are planned without a per-layer collision blow-up", "[SeamTower]")
{
    // Each stack is a vertical column. Collision is 2D, so cost must stay flat
    // as layer count grows instead of intersecting every layer with every other.
    constexpr int objects = 16;
    constexpr int layers  = 100;
    std::vector<SeamTowerBuildInput> inputs;
    inputs.reserve(size_t(objects * layers));
    for (int object = 0; object < objects; ++object) {
        const Polygon contour = square_from_to_mm(object * 40.0, 0.0, object * 40.0 + 20.0, 20.0);
        const Point   seam    = mm(object * 40.0 + 10.0, 0.0);
        for (int layer = 0; layer < layers; ++layer)
            inputs.push_back({object, layer, seam, contour, false});
    }

    const auto t0 = std::chrono::steady_clock::now();
    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    REQUIRE(planner.warnings().empty());
    REQUIRE(planner.layer_collections().size() == size_t(layers));
    REQUIRE(ms < 200.0);
}
