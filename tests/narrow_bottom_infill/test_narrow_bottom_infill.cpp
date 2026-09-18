#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

namespace {

void process_print(Print &print, TriangleMesh mesh,
                   std::initializer_list<ConfigBase::SetDeserializeItem> config_items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict(config_items);

    Model model;
    ModelObject *object = model.add_object();
    object->name += "object.stl";
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

double wrapped_line_angle_diff_deg(double a_deg, double b_deg)
{
    double d = std::abs(a_deg - b_deg);
    d = std::fmod(d, 180.0);
    if (d > 90.0)
        d = 180.0 - d;
    return d;
}

void collect_role_polylines(const ExtrusionEntity *entity, ExtrusionRole role, Polylines &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_role_polylines(child, role, out);
        return;
    }
    if (entity->role() == role)
        entity->collect_polylines(out);
}

Polylines first_layer_bottom_fill(const Print &print)
{
    Polylines out;
    const Layer *layer = print.objects().front()->get_layer(0);
    for (const LayerRegion *region : layer->regions())
        for (const ExtrusionEntity *entity : region->fills.entities)
            collect_role_polylines(entity, erBottomSurface, out);
    return out;
}

struct FillDirectionFractions
{
    double near_45{0};
    double axis_aligned{0};
    double total_length{0};
};

FillDirectionFractions direction_fractions(const Polylines &polylines)
{
    FillDirectionFractions fracs;
    const double min_seg = scale_(0.2);
    for (const Polyline &pl : polylines) {
        for (size_t i = 1; i < pl.points.size(); ++i) {
            const Point &a = pl.points[i - 1];
            const Point &b = pl.points[i];
            const double len = (b - a).cast<double>().norm();
            if (len < min_seg)
                continue;
            const double angle_deg = Geometry::rad2deg(std::atan2(double(b.y() - a.y()), double(b.x() - a.x())));
            fracs.total_length += len;
            if (wrapped_line_angle_diff_deg(angle_deg, 45.0) <= 20.0)
                fracs.near_45 += len;
            if (wrapped_line_angle_diff_deg(angle_deg, 0.0) <= 20.0 ||
                wrapped_line_angle_diff_deg(angle_deg, 90.0) <= 20.0)
                fracs.axis_aligned += len;
        }
    }
    return fracs;
}

void collect_role_paths(const ExtrusionEntity *entity, ExtrusionRole role, std::vector<const ExtrusionPath *> &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_role_paths(child, role, out);
        return;
    }
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        if (path->role() == role)
            out.push_back(path);
        return;
    }
    if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        if (multi->role() == role)
            for (const ExtrusionPath &p : multi->paths)
                out.push_back(&p);
        return;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        if (loop->role() == role)
            for (const ExtrusionPath &p : loop->paths)
                out.push_back(&p);
    }
}

struct BottomFillGeom {
    FillDirectionFractions dirs;
    BoundingBox            bbox;
    size_t                 n_points{0};
    double                 width_min{1e9};
    double                 width_max{0};
    size_t                 n_width_samples{0};
};

BottomFillGeom geom_from_polylines_and_paths(const Polylines &polylines, const std::vector<const ExtrusionPath *> &paths)
{
    BottomFillGeom geom;
    geom.dirs = direction_fractions(polylines);
    if (!polylines.empty())
        geom.bbox = get_extents(polylines);
    for (const Polyline &pl : polylines)
        geom.n_points += pl.points.size();
    for (const ExtrusionPath *path : paths) {
        if (path->width <= 0)
            continue;
        geom.width_min = std::min(geom.width_min, double(path->width));
        geom.width_max = std::max(geom.width_max, double(path->width));
        ++geom.n_width_samples;
    }
    if (geom.n_width_samples == 0) {
        geom.width_min = 0;
        geom.width_max = 0;
    }
    return geom;
}

std::vector<const ExtrusionPath *> first_layer_bottom_paths(const Print &print)
{
    std::vector<const ExtrusionPath *> out;
    const Layer *layer = print.objects().front()->get_layer(0);
    for (const LayerRegion *region : layer->regions())
        for (const ExtrusionEntity *entity : region->fills.entities)
            collect_role_paths(entity, erBottomSurface, out);
    return out;
}

BottomFillGeom first_layer_bottom_geom(const Print &print)
{
    return geom_from_polylines_and_paths(first_layer_bottom_fill(print), first_layer_bottom_paths(print));
}

double rel_diff(double a, double b)
{
    const double denom = std::max(std::max(std::abs(a), std::abs(b)), 1.0);
    return std::abs(a - b) / denom;
}

bool bottom_fill_geom_matches(const BottomFillGeom &a, const BottomFillGeom &b)
{
    return rel_diff(a.dirs.total_length, b.dirs.total_length) < 0.05 &&
           rel_diff(a.dirs.near_45, b.dirs.near_45) < 0.05 &&
           rel_diff(a.dirs.axis_aligned, b.dirs.axis_aligned) < 0.05 &&
           rel_diff(a.bbox.area(), b.bbox.area()) < 0.05 &&
           a.n_points == b.n_points &&
           rel_diff(a.width_min, b.width_min) < 0.05 &&
           rel_diff(a.width_max, b.width_max) < 0.05;
}

bool bottom_fill_geom_differs(const BottomFillGeom &a, const BottomFillGeom &b)
{
    return rel_diff(a.dirs.total_length, b.dirs.total_length) > 0.03 ||
           rel_diff(a.dirs.near_45, b.dirs.near_45) > 0.03 ||
           rel_diff(a.dirs.axis_aligned, b.dirs.axis_aligned) > 0.03 ||
           rel_diff(a.bbox.area(), b.bbox.area()) > 0.03 ||
           a.n_points != b.n_points ||
           rel_diff(a.width_min, b.width_min) > 0.03 ||
           rel_diff(a.width_max, b.width_max) > 0.03;
}

void require_internal_not_controls(const BottomFillGeom &rewritten,
                                   const BottomFillGeom &monotonic_45,
                                   const BottomFillGeom &monotonic_0,
                                   const BottomFillGeom &zigzag_0,
                                   const BottomFillGeom &user_concentric)
{
    REQUIRE(rewritten.dirs.total_length > scale_(10.0));
    REQUIRE(monotonic_45.dirs.near_45 > monotonic_45.dirs.axis_aligned);
    REQUIRE(bottom_fill_geom_differs(rewritten, monotonic_45));
    REQUIRE(bottom_fill_geom_differs(rewritten, monotonic_0));
    REQUIRE(bottom_fill_geom_differs(rewritten, zigzag_0));
    REQUIRE(bottom_fill_geom_differs(rewritten, user_concentric));
}

TriangleMesh mixed_narrow_and_wide_bottom_mesh()
{
    TriangleMesh mixed = make_cube(30, 5, 2);
    TriangleMesh wide  = make_cube(20, 20, 2);
    wide.translate(50.f, 0.f, 0.f);
    mixed.merge(wide);
    return mixed;
}

TriangleMesh body_with_narrow_cantilever()
{
    TriangleMesh body  = make_cube(20, 20, 8);
    TriangleMesh shelf = make_cube(22, 5, 1.2);
    // Overlap the body so the merge stays watertight; ~17 mm of 5 mm-wide shelf hangs in air.
    shelf.translate(15.f, 7.5f, 4.f);
    body.merge(shelf);
    return body;
}

void process_detect_narrow_bottom(Print              &print,
                                  TriangleMesh        mesh,
                                  bool                detect_narrow_bottom,
                                  const char         *bottom_pattern = "monotonic",
                                  const char         *bridge_bottom_pattern = "monotonic",
                                  int                 infill_direction = 45)
{
    process_print(print, std::move(mesh), {
        { "printable_area", "0x0,200x0,200x200,0x200" },
        { "printable_height", 200 },
        { "wall_loops", 2 },
        { "bottom_surface_pattern", bottom_pattern },
        { "bridge_bottom_surface_pattern", bridge_bottom_pattern },
        { "infill_direction", infill_direction },
        { "sparse_infill_density", 0 },
        { "detect_narrow_internal_solid_infill", false },
        { "detect_narrow_bottom_surface_infill", detect_narrow_bottom },
        { "bottom_shell_layers", 3 },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "line_width", 0.4 },
        { "skirt_loops", 0 },
        { "brim_width", 0 },
        { "elefant_foot_compensation", 0 },
        { "enable_support", false }
    });
}

void process_internal_pin_controls(const TriangleMesh &mesh,
                                   Print              &monotonic_45,
                                   Print              &monotonic_0,
                                   Print              &zigzag_0,
                                   Print              &user_concentric)
{
    process_detect_narrow_bottom(monotonic_45, mesh, false, "monotonic", "monotonic", 45);
    process_detect_narrow_bottom(monotonic_0, mesh, false, "monotonic", "monotonic", 0);
    process_detect_narrow_bottom(zigzag_0, mesh, false, "zig-zag", "monotonic", 0);
    process_detect_narrow_bottom(user_concentric, mesh, false, "concentric");
}

Polylines fills_inside_bbox(const Polylines &polylines, const BoundingBox &bb)
{
    Polylines out;
    for (const Polyline &pl : polylines) {
        if (pl.empty())
            continue;
        if (bb.contains(pl.bounding_box().center()))
            out.push_back(pl);
    }
    return out;
}

std::vector<const ExtrusionPath *> paths_inside_bbox(const std::vector<const ExtrusionPath *> &paths, const BoundingBox &bb)
{
    std::vector<const ExtrusionPath *> out;
    for (const ExtrusionPath *path : paths) {
        if (path->polyline.empty())
            continue;
        if (bb.contains(path->polyline.bounding_box().center()))
            out.push_back(path);
    }
    return out;
}

BottomFillGeom geom_inside_bbox(const Print &print, const BoundingBox &bb)
{
    return geom_from_polylines_and_paths(
        fills_inside_bbox(first_layer_bottom_fill(print), bb),
        paths_inside_bbox(first_layer_bottom_paths(print), bb));
}

size_t count_fill_surfaces(const PrintObject &object, SurfaceType type, size_t start_layer = 0)
{
    size_t n = 0;
    for (size_t i = start_layer; i < object.layer_count(); ++i) {
        const Layer *layer = object.get_layer(int(i));
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == type)
                    ++n;
    }
    return n;
}

Polylines bridge_infill_on_bottom_bridge_layers(const PrintObject &object)
{
    Polylines out;
    for (size_t i = 1; i < object.layer_count(); ++i) {
        const Layer *layer = object.get_layer(int(i));
        bool has_bridge = false;
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == stBottomBridge)
                    has_bridge = true;
        if (!has_bridge)
            continue;
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->fills.entities)
                collect_role_polylines(entity, erBridgeInfill, out);
    }
    return out;
}

} // namespace

TEST_CASE("Detect narrow bottom surface infill", "[Fill]") {
    auto run = [](TriangleMesh mesh, bool detect_narrow_bottom) {
        Print print;
        process_detect_narrow_bottom(print, std::move(mesh), detect_narrow_bottom);
        return direction_fractions(first_layer_bottom_fill(print));
    };

    SECTION("narrow first layer uses concentric when enabled") {
        const TriangleMesh mesh = make_cube(30, 5, 2);

        Print rewritten;
        process_detect_narrow_bottom(rewritten, mesh, true, "monotonic");

        Print monotonic_45, monotonic_0, zigzag_0, user_concentric;
        process_internal_pin_controls(mesh, monotonic_45, monotonic_0, zigzag_0, user_concentric);

        require_internal_not_controls(first_layer_bottom_geom(rewritten),
                                      first_layer_bottom_geom(monotonic_45),
                                      first_layer_bottom_geom(monotonic_0),
                                      first_layer_bottom_geom(zigzag_0),
                                      first_layer_bottom_geom(user_concentric));
    }

    SECTION("narrow first layer stays monotonic when disabled") {
        FillDirectionFractions fracs = run(make_cube(30, 5, 2), false);
        REQUIRE(fracs.total_length > scale_(10.0));
        REQUIRE(fracs.near_45 > fracs.axis_aligned);
    }

    SECTION("wide first layer stays monotonic when enabled") {
        FillDirectionFractions fracs = run(make_cube(20, 20, 20), true);
        REQUIRE(fracs.total_length > scale_(10.0));
        REQUIRE(fracs.near_45 > fracs.axis_aligned);
    }

    SECTION("mixed narrow and wide first-layer islands split independently") {
        const TriangleMesh mesh = mixed_narrow_and_wide_bottom_mesh();

        Print print;
        process_detect_narrow_bottom(print, mesh, true, "monotonic");
        REQUIRE(print.objects().size() == 1);
        const PrintObject &object = *print.objects().front();
        const Layer *layer = object.get_layer(0);
        REQUIRE(layer->lslices.size() == 2);

        BoundingBox narrow_bb, wide_bb;
        bool have_narrow = false, have_wide = false;
        for (const ExPolygon &island : layer->lslices) {
            BoundingBox bb = get_extents(island);
            const auto size = bb.size();
            const double min_mm = unscale<double>(std::min(size.x(), size.y()));
            if (min_mm < 10.0) {
                narrow_bb = bb;
                have_narrow = true;
            } else {
                wide_bb = bb;
                have_wide = true;
            }
        }
        REQUIRE(have_narrow);
        REQUIRE(have_wide);
        narrow_bb.offset(scale_(1.0));
        wide_bb.offset(scale_(1.0));

        Print monotonic_45, monotonic_0, zigzag_0, user_concentric;
        process_internal_pin_controls(mesh, monotonic_45, monotonic_0, zigzag_0, user_concentric);

        require_internal_not_controls(geom_inside_bbox(print, narrow_bb),
                                      geom_inside_bbox(monotonic_45, narrow_bb),
                                      geom_inside_bbox(monotonic_0, narrow_bb),
                                      geom_inside_bbox(zigzag_0, narrow_bb),
                                      geom_inside_bbox(user_concentric, narrow_bb));

        const BottomFillGeom wide_geom = geom_inside_bbox(print, wide_bb);
        const BottomFillGeom wide_mono45 = geom_inside_bbox(monotonic_45, wide_bb);
        REQUIRE(wide_geom.dirs.total_length > scale_(10.0));
        REQUIRE(wide_geom.dirs.near_45 > wide_geom.dirs.axis_aligned);
        REQUIRE(bottom_fill_geom_matches(wide_geom, wide_mono45));
    }

    SECTION("already concentric bottom pattern is not rewritten") {
        const TriangleMesh mesh = make_cube(30, 5, 2);

        Print concentric_on;
        process_detect_narrow_bottom(concentric_on, mesh, true, "concentric");
        const BottomFillGeom on_geom = first_layer_bottom_geom(concentric_on);

        Print concentric_off;
        process_detect_narrow_bottom(concentric_off, mesh, false, "concentric");
        const BottomFillGeom off_geom = first_layer_bottom_geom(concentric_off);

        Print rewritten_internal;
        process_detect_narrow_bottom(rewritten_internal, mesh, true, "monotonic");
        const BottomFillGeom internal_geom = first_layer_bottom_geom(rewritten_internal);

        Print monotonic_45, monotonic_0, zigzag_0;
        process_detect_narrow_bottom(monotonic_45, mesh, false, "monotonic", "monotonic", 45);
        process_detect_narrow_bottom(monotonic_0, mesh, false, "monotonic", "monotonic", 0);
        process_detect_narrow_bottom(zigzag_0, mesh, false, "zig-zag", "monotonic", 0);

        REQUIRE(on_geom.dirs.total_length > scale_(10.0));
        REQUIRE(on_geom.dirs.axis_aligned > on_geom.dirs.near_45);
        REQUIRE(bottom_fill_geom_matches(on_geom, off_geom));
        REQUIRE(bottom_fill_geom_differs(on_geom, internal_geom));
        require_internal_not_controls(internal_geom,
                                      first_layer_bottom_geom(monotonic_45),
                                      first_layer_bottom_geom(monotonic_0),
                                      first_layer_bottom_geom(zigzag_0),
                                      off_geom);
    }

    SECTION("narrow stBottomBridge overhang is not switched by detect") {
        const TriangleMesh mesh = body_with_narrow_cantilever();
        Print detect_on, detect_off;
        process_detect_narrow_bottom(detect_on, mesh, true, "monotonic", "monotonic");
        process_detect_narrow_bottom(detect_off, mesh, false, "monotonic", "monotonic");
        REQUIRE(detect_on.objects().size() == 1);
        REQUIRE(detect_off.objects().size() == 1);
        const PrintObject &object_on  = *detect_on.objects().front();
        const PrintObject &object_off = *detect_off.objects().front();
        REQUIRE(count_fill_surfaces(object_on, stBottomBridge, 1) > 0);
        REQUIRE(count_fill_surfaces(object_on, stBottom, 1) == 0);
        REQUIRE(count_fill_surfaces(object_off, stBottomBridge, 1) > 0);
        REQUIRE(count_fill_surfaces(object_off, stBottom, 1) == 0);

        const FillDirectionFractions on_fracs  = direction_fractions(bridge_infill_on_bottom_bridge_layers(object_on));
        const FillDirectionFractions off_fracs = direction_fractions(bridge_infill_on_bottom_bridge_layers(object_off));
        REQUIRE(on_fracs.total_length > scale_(10.0));
        REQUIRE(rel_diff(on_fracs.total_length, off_fracs.total_length) < 0.05);
        REQUIRE(rel_diff(on_fracs.near_45, off_fracs.near_45) < 0.05);
        REQUIRE(rel_diff(on_fracs.axis_aligned, off_fracs.axis_aligned) < 0.05);
    }
}
