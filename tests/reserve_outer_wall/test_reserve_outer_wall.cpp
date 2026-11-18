#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {

constexpr const char *kWallOrderWarning =
    "Reserve outer wall is ignored unless the order of walls is outer/inner";
constexpr const char *kGeneratorWarning =
    "Reserve outer wall is ignored unless the wall generator is Classic";

struct ValidateResult
{
    StringObjectException error;
    StringObjectException warning;
};

ValidateResult validate_with(std::initializer_list<ConfigBase::SetDeserializeItem> items,
                             StringObjectException                  warning = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"printable_area", "0x0,200x0,200x200,0x200"},
        {"printable_height", 200},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"line_width", 0.4},
        {"enable_support", false},
        {"enable_prime_tower", false},
        {"enable_wrapping_detection", false},
        {"skirt_loops", 0},
        {"brim_width", 0},
        {"spiral_mode", false},
        {"precise_outer_wall", false},
        {"reserve_outer_wall", true},
        {"wall_generator", "classic"},
        {"wall_sequence", "outer wall/inner wall"},
    });
    config.set_deserialize_strict(items);

    Model        model;
    Print        print;
    ModelObject *object = model.add_object();
    object->name        = "cube.stl";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);

    ValidateResult result;
    result.warning = std::move(warning);
    result.error   = print.validate(&result.warning);
    return result;
}

void require_no_reserve_warning(const ValidateResult &result)
{
    INFO(result.error.string);
    INFO(result.warning.string);
    INFO(result.warning.opt_key);
    REQUIRE(result.error.string.empty());
    REQUIRE(result.warning.opt_key != "reserve_outer_wall");
    REQUIRE(result.warning.string != kWallOrderWarning);
    REQUIRE(result.warning.string != kGeneratorWarning);
}

void require_reserve_warning(const ValidateResult &result, const char *expected)
{
    INFO(result.error.string);
    INFO(result.warning.string);
    REQUIRE(result.error.string.empty());
    REQUIRE(result.warning.string == expected);
    REQUIRE(result.warning.opt_key == "reserve_outer_wall");
}

} // namespace

TEST_CASE("reserve_outer_wall defaults to false", "[ReserveOuterWall]")
{
    const PrintRegionConfig cfg;
    REQUIRE(cfg.reserve_outer_wall.value == false);
}

TEST_CASE("Classic outer/inner with reserve outer wall on does not warn", "[ReserveOuterWall]")
{
    const ValidateResult result = validate_with({});
    require_no_reserve_warning(result);
    REQUIRE(result.warning.string.empty());
}

TEST_CASE("Classic inner/outer with reserve outer wall on warns about wall order", "[ReserveOuterWall]")
{
    const ValidateResult result = validate_with({{"wall_sequence", "inner wall/outer wall"}});
    require_reserve_warning(result, kWallOrderWarning);
}

TEST_CASE("Arachne outer/inner with reserve outer wall on warns about the Classic generator", "[ReserveOuterWall]")
{
    const ValidateResult result = validate_with({{"wall_generator", "arachne"}});
    require_reserve_warning(result, kGeneratorWarning);
}

TEST_CASE("Arachne and inner/outer together store the wall-order warning", "[ReserveOuterWall]")
{
    const ValidateResult result = validate_with({
        {"wall_generator", "arachne"},
        {"wall_sequence", "inner wall/outer wall"},
    });
    require_reserve_warning(result, kWallOrderWarning);
}

TEST_CASE("A non-empty warning string is left unchanged", "[ReserveOuterWall]")
{
    StringObjectException earlier;
    earlier.string  = "already stored";
    earlier.opt_key = "enable_prime_tower";

    const ValidateResult result = validate_with({{"wall_sequence", "inner wall/outer wall"}}, earlier);
    INFO(result.error.string);
    INFO(result.warning.string);
    REQUIRE(result.error.string.empty());
    REQUIRE(result.warning.string == "already stored");
    REQUIRE(result.warning.opt_key == "enable_prime_tower");
}

TEST_CASE("Spiral vase does not warn about reserve outer wall", "[ReserveOuterWall]")
{
    const ValidateResult result = validate_with({
        {"spiral_mode", true},
        {"wall_sequence", "inner wall/outer wall"},
        {"wall_generator", "arachne"},
    });
    require_no_reserve_warning(result);
}

namespace {

TriangleMesh wall_with_wedge()
{
    TriangleMesh wall = make_cube(30., 1., 10.);
    const TriangleMesh wedge(
        std::vector<Vec3f>{
            Vec3f(15.f, 1.f, 4.f),
            Vec3f(14.f, 1.f, 6.f),
            Vec3f(16.f, 1.f, 6.f),
            Vec3f(15.f, 2.f, 6.f),
        },
        std::vector<Vec3i>{
            Vec3i(0, 2, 1),
            Vec3i(0, 1, 3),
            Vec3i(0, 3, 2),
            Vec3i(1, 2, 3),
        });
    MeshBoolean::cgal::plus(wall, wedge);
    return wall;
}

// The 1.35 mm bar is wide enough for a center gap thicker than the 0.45 mm inner
// wall, and that gap sits on the outer bead. The 2 mm bar fits a normal inner
// wall plus a middle gap that stays clear of the outer bead.
TriangleMesh thick_gap_beside_middle_gap()
{
    TriangleMesh thick = make_cube(12., 1.35, 10.);
    TriangleMesh middle = make_cube(12., 2., 10.);
    middle.translate(20.f, 0.f, 0.f);
    MeshBoolean::cgal::plus(thick, middle);
    return thick;
}

DynamicPrintConfig wall_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"printable_area", "0x0,200x0,200x200,0x200"},
        {"printable_height", 200},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"line_width", 0.42},
        {"outer_wall_line_width", 0.42},
        {"inner_wall_line_width", 0.45},
        {"wall_loops", 2},
        {"wall_generator", "classic"},
        {"wall_sequence", "outer wall/inner wall"},
        {"min_bead_width", 85},
        {"nozzle_diameter", 0.4},
        {"sparse_infill_density", 15},
        {"infill_wall_overlap", 15},
        {"ensure_vertical_shell_thickness", "enabled"},
        {"detect_thin_wall", false},
        {"top_one_wall_type", "all top"},
        {"enable_support", false},
        {"brim_type", "no_brim"},
        {"brim_width", 0},
        {"skirt_loops", 0},
        {"enable_prime_tower", false},
        {"enable_wrapping_detection", false},
        {"spiral_mode", false},
        {"reserve_outer_wall", true},
    });
    config.set_deserialize_strict(extra);
    return config;
}

// Partial modifier over the wall, including the wedge, so a merged follower can own fill.
TriangleMesh partial_wall_modifier_mesh()
{
    TriangleMesh mesh = make_cube(20., 3., 10.);
    return mesh;
}

enum class WallModifier { None, InfillDensity, ReserveOff };

void add_wall_object(Model &model, WallModifier modifier)
{
    ModelObject *object = model.add_object();
    object->name        = "wall-wedge";
    object->add_volume(wall_with_wedge());
    if (modifier != WallModifier::None) {
        ModelVolume *volume = object->add_volume(partial_wall_modifier_mesh(), ModelVolumeType::PARAMETER_MODIFIER);
        if (modifier == WallModifier::InfillDensity)
            volume->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
        else
            volume->config.set_key_value("reserve_outer_wall", new ConfigOptionBool(false));
    }
    object->add_instance();
    object->ensure_on_bed();
}

void slice_model(Print &print, Model &model, const DynamicPrintConfig &config, StringObjectException *warning_out)
{
    REQUIRE_FALSE(model.objects.empty());
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);

    StringObjectException local_warning;
    StringObjectException *warning = warning_out != nullptr ? warning_out : &local_warning;
    const StringObjectException error = print.validate(warning);
    INFO(error.string);
    INFO(warning->string);
    REQUIRE(error.string.empty());
    print.set_status_silent();
    print.process();
}

void slice_wall(Print &print, std::initializer_list<ConfigBase::SetDeserializeItem> extra, StringObjectException *warning_out = nullptr)
{
    Model model;
    add_wall_object(model, WallModifier::None);
    slice_model(print, model, wall_config(extra), warning_out);
}

void slice_gap_bars(Print &print, std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "gap-bars";
    object->add_volume(thick_gap_beside_middle_gap());
    object->add_instance();
    object->ensure_on_bed();
    slice_model(print, model, wall_config(extra), nullptr);
}

// Layer::slice_z is the midplane the mesh is sliced on.
const Layer *layer_at_slice(const Print &print, double slice_z)
{
    REQUIRE_FALSE(print.objects().empty());
    const PrintObject *object = print.objects().front();
    const Layer       *found  = nullptr;
    for (const Layer *layer : object->layers()) {
        if (std::abs(layer->slice_z - slice_z) < 1e-6)
            found = layer;
    }
    INFO("slice plane " << slice_z << " layers " << object->layer_count());
    REQUIRE(found != nullptr);
    return found;
}

void collect_paths(const ExtrusionEntity *entity, std::vector<const ExtrusionPath *> &out)
{
    if (entity == nullptr)
        return;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_paths(child, out);
        return;
    }
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        out.push_back(path);
        return;
    }
    if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        for (const ExtrusionPath &path : multi->paths)
            out.push_back(&path);
        return;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        for (const ExtrusionPath &path : loop->paths)
            out.push_back(&path);
    }
}

std::vector<const ExtrusionPath *> layer_paths(const Layer &layer)
{
    std::vector<const ExtrusionPath *> out;
    for (const LayerRegion *region : layer.regions()) {
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            collect_paths(entity, out);
        for (const ExtrusionEntity *entity : region->thin_fills.entities)
            collect_paths(entity, out);
        for (const ExtrusionEntity *entity : region->fills.entities)
            collect_paths(entity, out);
    }
    return out;
}

bool is_outer_bead_role(ExtrusionRole role)
{
    return role == erExternalPerimeter || role == erOverhangPerimeter;
}

bool is_classic_wall(ExtrusionRole role)
{
    return is_outer_bead_role(role) || role == erPerimeter || role == erGapFill;
}

std::vector<const ExtrusionPath *> classic_wall_paths(const std::vector<const ExtrusionPath *> &paths)
{
    std::vector<const ExtrusionPath *> out;
    for (const ExtrusionPath *path : paths)
        if (is_classic_wall(path->role()))
            out.push_back(path);
    return out;
}

// Core of the outer bead, inside the normal spacing overlap of one layer height.
ExPolygons outer_bead_core(const ExPolygons &bead)
{
    const float allowed = float(scale_(0.2 * (1. - 0.25 * 3.141592653589793)));
    return offset_ex(bead, -allowed);
}

Polygons round_join_coverage(const ExtrusionPath &path)
{
    if (path.width <= 0.f || path.polyline.size() < 2)
        return {};
    Polyline line = path.polyline;
    const bool closed = line.points.front() == line.points.back() && line.points.size() >= 4;
    if (closed)
        line.points.pop_back();
    const float delta = float(scale_(path.width / 2.));
    const auto  end   = closed ? ClipperLib::etClosedLine : ClipperLib::etOpenButt;
    return offset(line, delta, jtRound, scale_(0.01), end);
}

ExPolygons outer_bead_coverage(const std::vector<const ExtrusionPath *> &paths)
{
    Polygons covered;
    for (const ExtrusionPath *path : paths)
        if (is_outer_bead_role(path->role()))
            append(covered, round_join_coverage(*path));
    return union_ex(covered);
}

double overlap_area_mm2(const ExtrusionPath &path, const ExPolygons &outer_bead)
{
    const Polygons covered = round_join_coverage(path);
    if (covered.empty() || outer_bead.empty())
        return 0.;
    return std::abs(area(intersection_ex(union_ex(covered), outer_bead))) * SCALING_FACTOR * SCALING_FACTOR;
}

double max_later_overlap_mm2(const std::vector<const ExtrusionPath *> &paths, const ExPolygons &outer_bead)
{
    double max_area = 0.;
    for (const ExtrusionPath *path : paths) {
        if (is_outer_bead_role(path->role()))
            continue;
        max_area = std::max(max_area, overlap_area_mm2(*path, outer_bead));
    }
    return max_area;
}

Vec2d mesh_xy(const PrintObject &object, const Point &point)
{
    const Point &origin = object.center_offset();
    return Vec2d(unscale<double>(point.x() + origin.x()), unscale<double>(point.y() + origin.y()));
}

std::vector<double> flat_face_ys(const PrintObject &object, const std::vector<const ExtrusionPath *> &paths)
{
    std::vector<double> ys;
    for (const ExtrusionPath *path : paths) {
        if (path->role() != erExternalPerimeter)
            continue;
        for (const Point &point : path->polyline.points) {
            const Vec2d xy = mesh_xy(object, point);
            if (xy.x() >= 0. && xy.x() <= 30. && std::abs(xy.y() - 0.21) <= 0.3)
                ys.push_back(xy.y());
        }
    }
    return ys;
}

bool x_interval_overlaps(const PrintObject &object, const ExtrusionPath &path, double lo, double hi)
{
    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    for (const Point &point : path.polyline.points) {
        const double x = mesh_xy(object, point).x();
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
    }
    return max_x >= lo && min_x <= hi;
}

bool has_role(const std::vector<const ExtrusionPath *> &paths, ExtrusionRole role)
{
    return std::any_of(paths.begin(), paths.end(), [role](const ExtrusionPath *path) { return path->role() == role; });
}

bool interior_nonempty(const Layer &layer)
{
    return std::any_of(layer.regions().begin(), layer.regions().end(), [](const LayerRegion *region) {
        return !region->reserved_outer_wall_interior.empty();
    });
}

bool is_fill_role(ExtrusionRole role)
{
    return role != erExternalPerimeter && role != erOverhangPerimeter && role != erPerimeter && role != erGapFill;
}

bool has_fill_role(const std::vector<const ExtrusionPath *> &paths)
{
    return std::any_of(paths.begin(), paths.end(), [](const ExtrusionPath *path) { return is_fill_role(path->role()); });
}

double max_fill_overlap_mm2(const std::vector<const ExtrusionPath *> &paths, const ExPolygons &bead)
{
    double max_area = 0.;
    for (const ExtrusionPath *path : paths) {
        if (!is_fill_role(path->role()))
            continue;
        max_area = std::max(max_area, overlap_area_mm2(*path, bead));
    }
    return max_area;
}

double fill_outside_interior_mm2(const Layer &layer)
{
    double outside = 0.;
    for (const LayerRegion *region : layer.regions()) {
        if (region->reserved_outer_wall_interior.empty())
            continue;
        ExPolygons fill;
        fill.reserve(region->fill_surfaces.surfaces.size());
        for (const Surface &surface : region->fill_surfaces.surfaces)
            fill.emplace_back(surface.expolygon);
        if (fill.empty())
            continue;
        outside += std::abs(area(diff_ex(fill, region->reserved_outer_wall_interior))) * SCALING_FACTOR * SCALING_FACTOR;
    }
    return outside;
}

double fill_outside_region_interior_mm2(const LayerRegion &region)
{
    ExPolygons fill;
    fill.reserve(region.fill_surfaces.surfaces.size());
    for (const Surface &surface : region.fill_surfaces.surfaces)
        fill.emplace_back(surface.expolygon);
    if (fill.empty() || region.reserved_outer_wall_interior.empty())
        return 0.;
    return std::abs(area(diff_ex(fill, region.reserved_outer_wall_interior))) * SCALING_FACTOR * SCALING_FACTOR;
}

double expolygon_overlap_mm2(const ExPolygons &subject, const ExPolygons &clip)
{
    if (subject.empty() || clip.empty())
        return 0.;
    return std::abs(area(intersection_ex(subject, clip))) * SCALING_FACTOR * SCALING_FACTOR;
}

ExPolygons fill_surfaces_of(const LayerRegion &region, bool solid)
{
    ExPolygons out;
    for (const Surface &surface : region.fill_surfaces.surfaces)
        if (surface.is_solid() == solid)
            out.emplace_back(surface.expolygon);
    return out;
}

// Compatible wall settings keep perimeter entities on the first region. A follower's
// perimeters and thin fills are cleared.
const LayerRegion *merged_follower(const Layer &layer, bool solid_fill)
{
    bool leader = false;
    const LayerRegion *follower = nullptr;
    for (const LayerRegion *region : layer.regions()) {
        if (!region->perimeters.empty())
            leader = true;
        if (!region->perimeters.empty() || !region->thin_fills.empty())
            continue;
        if (!fill_surfaces_of(*region, solid_fill).empty())
            follower = region;
    }
    return leader ? follower : nullptr;
}

Layer *mutable_layer_at_slice(Print &print, double slice_z)
{
    REQUIRE_FALSE(print.objects_mutable().empty());
    PrintObject *object = print.objects_mutable().front();
    Layer       *found  = nullptr;
    for (Layer *layer : object->layers()) {
        if (std::abs(layer->slice_z - slice_z) < 1e-6)
            found = layer;
    }
    INFO("slice plane " << slice_z << " layers " << object->layer_count());
    REQUIRE(found != nullptr);
    return found;
}

void collect_object_paths(const Print &print, std::vector<const ExtrusionPath *> &out)
{
    for (const Layer *layer : print.objects().front()->layers()) {
        const std::vector<const ExtrusionPath *> paths = layer_paths(*layer);
        out.insert(out.end(), paths.begin(), paths.end());
    }
}

struct NormLine
{
    std::vector<std::pair<double, double>> pts;
    bool operator<(const NormLine &other) const { return pts < other.pts; }
};

NormLine normalize_polyline(const Polyline &line)
{
    NormLine out;
    out.pts.reserve(line.points.size());
    for (const Point &point : line.points)
        out.pts.emplace_back(unscale<double>(point.x()), unscale<double>(point.y()));
    if (out.pts.size() >= 2 && out.pts.front() == out.pts.back())
        out.pts.pop_back();
    if (out.pts.size() >= 2) {
        std::vector<std::pair<double, double>> reversed = out.pts;
        std::reverse(reversed.begin(), reversed.end());
        if (reversed < out.pts)
            out.pts.swap(reversed);
    }
    return out;
}

std::vector<NormLine> normalized_lines(const std::vector<const ExtrusionPath *> &paths)
{
    std::vector<NormLine> lines;
    lines.reserve(paths.size());
    for (const ExtrusionPath *path : paths) {
        if (path->polyline.size() < 2)
            continue;
        lines.push_back(normalize_polyline(path->polyline));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

double polyline_deviation_mm(const std::vector<const ExtrusionPath *> &left, const std::vector<const ExtrusionPath *> &right)
{
    const std::vector<NormLine> a = normalized_lines(left);
    const std::vector<NormLine> b = normalized_lines(right);
    if (a.size() != b.size())
        return 1e6;
    double deviation = 0.;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].pts.size() != b[i].pts.size())
            return 1e6;
        for (size_t p = 0; p < a[i].pts.size(); ++p) {
            deviation = std::max(deviation, std::abs(a[i].pts[p].first - b[i].pts[p].first));
            deviation = std::max(deviation, std::abs(a[i].pts[p].second - b[i].pts[p].second));
        }
    }
    return deviation;
}

void require_flat_face_width(const PrintObject &object, const std::vector<const ExtrusionPath *> &paths)
{
    size_t samples = 0;
    for (const ExtrusionPath *path : paths) {
        if (path->role() != erExternalPerimeter)
            continue;
        bool on_flat_face = false;
        for (const Point &point : path->polyline.points) {
            const Vec2d xy = mesh_xy(object, point);
            if (xy.x() >= 0. && xy.x() <= 30. && std::abs(xy.y() - 0.21) <= 0.3)
                on_flat_face = true;
        }
        if (!on_flat_face)
            continue;
        ++samples;
        REQUIRE(double(path->width) == Approx(0.42).margin(0.001));
    }
    REQUIRE(samples > 0);
}

std::vector<const ExtrusionPath *> paths_of_role_overlapping_x(const PrintObject &object,
                                                               const std::vector<const ExtrusionPath *> &paths,
                                                               ExtrusionRole role, double lo, double hi)
{
    std::vector<const ExtrusionPath *> out;
    for (const ExtrusionPath *path : paths)
        if (path->role() == role && x_interval_overlaps(object, *path, lo, hi))
            out.push_back(path);
    return out;
}

std::vector<const ExtrusionPath *> sorted_paths(std::vector<const ExtrusionPath *> paths)
{
    std::sort(paths.begin(), paths.end(), [](const ExtrusionPath *a, const ExtrusionPath *b) {
        const NormLine left = normalize_polyline(a->polyline);
        const NormLine right = normalize_polyline(b->polyline);
        if (left < right)
            return true;
        if (right < left)
            return false;
        return a->width < b->width;
    });
    return paths;
}

void require_paths_unchanged(const std::vector<const ExtrusionPath *> &left, const std::vector<const ExtrusionPath *> &right)
{
    const double deviation = polyline_deviation_mm(left, right);
    INFO("polyline deviation " << deviation);
    REQUIRE(deviation <= 0.01);
    const std::vector<const ExtrusionPath *> a = sorted_paths(left);
    const std::vector<const ExtrusionPath *> b = sorted_paths(right);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        REQUIRE(double(a[i]->width) == Approx(double(b[i]->width)).margin(0.001));
}

} // namespace

TEST_CASE("Reserve outer wall keeps the flat face and skips beads in the narrow channel", "[ReserveOuterWall]")
{
    Print print;
    slice_wall(print, {});
    const Layer *z2 = layer_at_slice(print, 1.9);
    const Layer *z4 = layer_at_slice(print, 4.1);
    const Layer *z5 = layer_at_slice(print, 5.3);

    const PrintObject                       *object = print.objects().front();
    const std::vector<const ExtrusionPath *> paths2 = layer_paths(*z2);
    const std::vector<const ExtrusionPath *> paths5 = layer_paths(*z5);
    const std::vector<double>                y2      = flat_face_ys(*object, paths2);
    const std::vector<double>                y5      = flat_face_ys(*object, paths5);
    REQUIRE_FALSE(y2.empty());
    REQUIRE_FALSE(y5.empty());
    const double mean2 = std::accumulate(y2.begin(), y2.end(), 0.) / double(y2.size());
    const double mean5 = std::accumulate(y5.begin(), y5.end(), 0.) / double(y5.size());
    INFO("flat face Y at 1.9 " << mean2);
    INFO("flat face Y at 5.3 " << mean5);
    REQUIRE(std::abs(mean5 - mean2) <= 0.01);
    REQUIRE(std::abs(mean5 - 0.21) <= 0.01);
    for (double y : y5)
        REQUIRE(std::abs(y - 0.21) <= 0.01);

    require_flat_face_width(*object, paths5);

    for (const Layer *layer : {z2, z4}) {
        const std::vector<const ExtrusionPath *> channel = layer_paths(*layer);
        INFO("slice_z " << layer->slice_z);
        REQUIRE_FALSE(has_role(channel, erPerimeter));
        REQUIRE(has_role(channel, erGapFill));
        for (const ExtrusionPath *path : channel)
            if (path->role() == erGapFill)
                REQUIRE(double(path->width) <= 0.45);
    }

    for (const Layer *layer : print.objects().front()->layers())
        REQUIRE(interior_nonempty(*layer));
}

TEST_CASE("Reserve outer wall leaves classic wall lines in place", "[ReserveOuterWall]")
{
    Print on;
    Print off;
    slice_wall(on, {});
    slice_wall(off, {{"reserve_outer_wall", false}});

    std::vector<const ExtrusionPath *> on_paths;
    std::vector<const ExtrusionPath *> off_paths;
    collect_object_paths(on, on_paths);
    collect_object_paths(off, off_paths);
    const double deviation = polyline_deviation_mm(classic_wall_paths(on_paths), classic_wall_paths(off_paths));
    INFO("wall polyline deviation " << deviation);
    REQUIRE(deviation <= 0.01);

    for (const Layer *layer : off.objects().front()->layers()) {
        for (const ExtrusionPath *path : layer_paths(*layer))
            if (path->role() == erGapFill)
                REQUIRE(double(path->width) <= 0.45);
    }
}

TEST_CASE("Reserve outer wall off overlaps the outer bead on the wedge", "[ReserveOuterWall]")
{
    Print print;
    slice_wall(print, {{"reserve_outer_wall", false}});
    const Layer *z5 = layer_at_slice(print, 5.3);
    const std::vector<const ExtrusionPath *> paths = layer_paths(*z5);
    const ExPolygons bead = outer_bead_coverage(paths);
    REQUIRE_FALSE(bead.empty());
    const double overlap = max_later_overlap_mm2(paths, bead);
    INFO("later overlap at 5.3 " << overlap);
    REQUIRE(overlap > 0.02);
    for (const Layer *layer : print.objects().front()->layers())
        REQUIRE_FALSE(interior_nonempty(*layer));
}

TEST_CASE("Classic inner/outer ignores reserve outer wall", "[ReserveOuterWall]")
{
    Print on;
    Print off;
    slice_wall(on, {{"wall_sequence", "inner wall/outer wall"}});
    slice_wall(off, {{"wall_sequence", "inner wall/outer wall"}, {"reserve_outer_wall", false}});
    REQUIRE(layer_at_slice(on, 5.3) != nullptr);
    REQUIRE(layer_at_slice(off, 5.3) != nullptr);

    std::vector<const ExtrusionPath *> on_paths;
    std::vector<const ExtrusionPath *> off_paths;
    collect_object_paths(on, on_paths);
    collect_object_paths(off, off_paths);
    const double deviation = polyline_deviation_mm(on_paths, off_paths);
    INFO("polyline deviation " << deviation);
    REQUIRE(deviation <= 0.01);
    for (const Layer *layer : on.objects().front()->layers())
        REQUIRE_FALSE(interior_nonempty(*layer));
}

TEST_CASE("Arachne outer/inner ignores reserve outer wall", "[ReserveOuterWall]")
{
    Print on;
    Print off;
    slice_wall(on, {{"wall_generator", "arachne"}});
    slice_wall(off, {{"wall_generator", "arachne"}, {"reserve_outer_wall", false}});
    REQUIRE(layer_at_slice(on, 5.3) != nullptr);
    REQUIRE(layer_at_slice(off, 5.3) != nullptr);

    std::vector<const ExtrusionPath *> on_paths;
    std::vector<const ExtrusionPath *> off_paths;
    collect_object_paths(on, on_paths);
    collect_object_paths(off, off_paths);
    const double deviation = polyline_deviation_mm(on_paths, off_paths);
    INFO("polyline deviation " << deviation);
    REQUIRE(deviation <= 0.01);
    for (const Layer *layer : on.objects().front()->layers())
        REQUIRE_FALSE(interior_nonempty(*layer));
}

TEST_CASE("Reserve outer wall keeps infill inside the interior near the wedge top", "[ReserveOuterWall]")
{
    Print print;
    slice_wall(print, {});
    const Layer                             *top   = layer_at_slice(print, 5.9);
    const std::vector<const ExtrusionPath *> paths = layer_paths(*top);
    REQUIRE(interior_nonempty(*top));

    const ExPolygons outer = outer_bead_coverage(paths);
    REQUIRE_FALSE(outer.empty());
    const ExPolygons core = outer_bead_core(outer);
    const double core_overlap = max_fill_overlap_mm2(paths, core);
    INFO("fill overlap with outer bead core at 5.9 " << core_overlap);
    REQUIRE(core_overlap <= 0.02);

    for (const Layer *layer : print.objects().front()->layers()) {
        const double outside = fill_outside_interior_mm2(*layer);
        INFO("slice_z " << layer->slice_z << " fill surface outside interior " << outside);
        REQUIRE(outside <= 0.01);
    }
}

TEST_CASE("Reserve outer wall off does not clip infill near the wedge top", "[ReserveOuterWall]")
{
    Print print;
    slice_wall(print, {{"reserve_outer_wall", false}});
    const Layer                             *layer = layer_at_slice(print, 5.9);
    const std::vector<const ExtrusionPath *> paths = layer_paths(*layer);
    REQUIRE(has_fill_role(paths));
    REQUIRE_FALSE(interior_nonempty(*layer));

    const ExPolygons outer = outer_bead_coverage(paths);
    REQUIRE_FALSE(outer.empty());
    const double outer_overlap = max_fill_overlap_mm2(paths, outer);
    INFO("fill overlap with outer bead at 5.9 off " << outer_overlap);
    REQUIRE(outer_overlap > 0.01);
    const double core_overlap = max_fill_overlap_mm2(paths, outer_bead_core(outer));
    INFO("fill overlap with outer bead core at 5.9 off " << core_overlap);
    REQUIRE(core_overlap > 0.05);
}

TEST_CASE("Spiral vase ignores reserve outer wall on every layer including bottom shells", "[ReserveOuterWall]")
{
    Print on;
    Print off;
    StringObjectException on_warning;
    StringObjectException off_warning;
    slice_wall(on, {{"spiral_mode", true}}, &on_warning);
    slice_wall(off, {{"spiral_mode", true}, {"reserve_outer_wall", false}}, &off_warning);

    INFO(on_warning.string);
    INFO(on_warning.opt_key);
    INFO(off_warning.string);
    INFO(off_warning.opt_key);
    REQUIRE(on_warning.string == off_warning.string);
    REQUIRE(on_warning.opt_key == off_warning.opt_key);
    REQUIRE(on_warning.opt_key != "reserve_outer_wall");

    REQUIRE(layer_at_slice(on, 0.1) != nullptr);
    REQUIRE(layer_at_slice(off, 0.1) != nullptr);
    std::vector<const ExtrusionPath *> on_paths;
    std::vector<const ExtrusionPath *> off_paths;
    collect_object_paths(on, on_paths);
    collect_object_paths(off, off_paths);
    const double deviation = polyline_deviation_mm(on_paths, off_paths);
    INFO("polyline deviation " << deviation);
    REQUIRE(deviation <= 0.01);
    for (const Layer *layer : on.objects().front()->layers())
        REQUIRE_FALSE(interior_nonempty(*layer));
}

TEST_CASE("Infill density modifier keeps each region's fill inside its own reserved interior", "[ReserveOuterWall]")
{
    Model model;
    add_wall_object(model, WallModifier::InfillDensity);

    Print print;
    slice_model(print, model, wall_config({}), nullptr);
    const Layer *top = layer_at_slice(print, 5.9);
    REQUIRE(top->regions().size() >= 2);

    size_t filled = 0;
    for (const Layer *layer : print.objects().front()->layers()) {
        for (const LayerRegion *region : layer->regions()) {
            if (region->fill_surfaces.empty())
                continue;
            ++filled;
            INFO("slice_z " << layer->slice_z << " region " << region->region().print_object_region_id());
            REQUIRE_FALSE(region->reserved_outer_wall_interior.empty());
            const double outside = fill_outside_region_interior_mm2(*region);
            INFO("fill outside own interior " << outside);
            REQUIRE(outside <= 0.01);
        }
    }
    REQUIRE(filled > 0);

    const std::vector<const ExtrusionPath *> top_paths = layer_paths(*top);
    const ExPolygons                         top_outer = outer_bead_coverage(top_paths);
    REQUIRE_FALSE(top_outer.empty());
    const double top_overlap = max_fill_overlap_mm2(top_paths, outer_bead_core(top_outer));
    INFO("fill overlap with outer bead core at 5.9 " << top_overlap);
    REQUIRE(top_overlap <= 0.02);
}

TEST_CASE("A merged follower cuts solid infill to the outer bead and keeps sparse infill out of the core", "[ReserveOuterWall]")
{
    Model model;
    add_wall_object(model, WallModifier::InfillDensity);

    Print print;
    slice_model(print, model, wall_config({}), nullptr);
    const Layer *top = layer_at_slice(print, 5.9);
    REQUIRE(top->regions().size() >= 2);

    const LayerRegion *solid_follower = merged_follower(*top, true);
    REQUIRE(solid_follower != nullptr);
    REQUIRE(solid_follower->perimeters.empty());
    REQUIRE(solid_follower->thin_fills.empty());

    const ExPolygons bead = outer_bead_coverage(layer_paths(*top));
    REQUIRE_FALSE(bead.empty());
    const ExPolygons solid = fill_surfaces_of(*solid_follower, true);
    const double solid_on_bead = expolygon_overlap_mm2(solid, bead);
    INFO("follower solid fill on outer bead " << solid_on_bead);
    REQUIRE(solid_on_bead <= 0.01);

    REQUIRE_FALSE(solid_follower->reserved_outer_wall_bead.empty());
    const double solid_on_stored = expolygon_overlap_mm2(solid, solid_follower->reserved_outer_wall_bead);
    INFO("follower solid fill on stored bead " << solid_on_stored);
    REQUIRE(solid_on_stored <= 0.01);

    // The 1 mm wall has no sparse surface on the follower. A wider cube does.
    Model        sparse_model;
    ModelObject *sparse_object = sparse_model.add_object();
    sparse_object->name        = "sparse-cube";
    sparse_object->add_volume(make_cube(20., 20., 4.));
    ModelVolume *sparse_modifier = sparse_object->add_volume(make_cube(10., 20., 4.), ModelVolumeType::PARAMETER_MODIFIER);
    sparse_modifier->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    sparse_object->add_instance();
    sparse_object->ensure_on_bed();

    Print sparse_print;
    slice_model(sparse_print, sparse_model, wall_config({
        {"top_shell_layers", 2},
        {"bottom_shell_layers", 2},
        {"top_shell_thickness", 0},
        {"bottom_shell_thickness", 0},
        {"ensure_vertical_shell_thickness", "disabled"},
    }), nullptr);

    bool   found_sparse     = false;
    double max_sparse_core  = 0.;
    for (const Layer *layer : sparse_print.objects().front()->layers()) {
        const LayerRegion *sparse_follower = merged_follower(*layer, false);
        if (sparse_follower == nullptr)
            continue;
        found_sparse = true;
        REQUIRE(sparse_follower->perimeters.empty());
        REQUIRE(sparse_follower->thin_fills.empty());
        const ExPolygons sparse_bead = outer_bead_coverage(layer_paths(*layer));
        REQUIRE_FALSE(sparse_bead.empty());
        const double sparse_on_core = expolygon_overlap_mm2(fill_surfaces_of(*sparse_follower, false), outer_bead_core(sparse_bead));
        INFO("slice_z " << layer->slice_z << " follower sparse fill on outer bead core " << sparse_on_core);
        max_sparse_core = std::max(max_sparse_core, sparse_on_core);
    }
    REQUIRE(found_sparse);
    REQUIRE(max_sparse_core <= 0.02);
}

TEST_CASE("A modifier with reserve outer wall off does not share the part interior", "[ReserveOuterWall]")
{
    Model model;
    add_wall_object(model, WallModifier::ReserveOff);

    Print print;
    slice_model(print, model, wall_config({}), nullptr);
    const Layer *top = layer_at_slice(print, 5.9);
    REQUIRE(top->regions().size() >= 2);

    const LayerRegion *part     = nullptr;
    const LayerRegion *modifier = nullptr;
    for (const LayerRegion *region : top->regions()) {
        if (region->region().config().reserve_outer_wall.value)
            part = region;
        else
            modifier = region;
    }
    REQUIRE(part != nullptr);
    REQUIRE(modifier != nullptr);
    REQUIRE(modifier->reserved_outer_wall_interior.empty());
    REQUIRE_FALSE(part->reserved_outer_wall_interior.empty());
    REQUIRE_FALSE(modifier->perimeters.empty());
}

TEST_CASE("A region with empty slices clears its reserved outer wall interior", "[ReserveOuterWall]")
{
    Model model;
    add_wall_object(model, WallModifier::InfillDensity);

    Print print;
    slice_model(print, model, wall_config({}), nullptr);
    Layer *layer = mutable_layer_at_slice(print, 1.9);
    REQUIRE_FALSE(layer->regions().empty());
    LayerRegion *cleared = layer->get_region(0);
    cleared->slices.clear();
    cleared->reserved_outer_wall_interior = {
        ExPolygon{Point::new_scale(0., 0.), Point::new_scale(1., 0.), Point::new_scale(0., 1.)}};
    cleared->reserved_outer_wall_bead = {
        ExPolygon{Point::new_scale(0., 0.), Point::new_scale(1., 0.), Point::new_scale(0., 1.)}};
    REQUIRE_FALSE(cleared->reserved_outer_wall_interior.empty());
    REQUIRE_FALSE(cleared->reserved_outer_wall_bead.empty());
    layer->make_perimeters();
    REQUIRE(cleared->reserved_outer_wall_interior.empty());
    REQUIRE(cleared->reserved_outer_wall_bead.empty());
}

TEST_CASE("Re-slicing a density modifier with the checkbox off leaves every interior empty", "[ReserveOuterWall]")
{
    Model              model;
    DynamicPrintConfig config = wall_config({});
    add_wall_object(model, WallModifier::InfillDensity);

    Print print;
    slice_model(print, model, config, nullptr);
    Layer *top = mutable_layer_at_slice(print, 5.9);
    REQUIRE(top->regions().size() >= 2);
    REQUIRE(interior_nonempty(*top));
    const ExPolygon stale{Point::new_scale(0., 0.), Point::new_scale(2., 0.), Point::new_scale(0., 2.)};
    for (Layer *layer : print.objects_mutable().front()->layers()) {
        for (LayerRegion *region : layer->regions()) {
            region->reserved_outer_wall_interior = {stale};
            region->reserved_outer_wall_bead     = {stale};
        }
    }

    config.set_deserialize_strict({{"reserve_outer_wall", false}});
    StringObjectException warning;
    slice_model(print, model, config, &warning);
    INFO(warning.string);
    INFO(warning.opt_key);
    REQUIRE(warning.opt_key != "reserve_outer_wall");
    for (const Layer *sliced : print.objects().front()->layers()) {
        for (const LayerRegion *region : sliced->regions()) {
            REQUIRE(region->reserved_outer_wall_interior.empty());
            REQUIRE(region->reserved_outer_wall_bead.empty());
        }
    }
}

TEST_CASE("Reserve outer wall off leaves the stored bead empty and does not cut follower solid fill", "[ReserveOuterWall]")
{
    Model model;
    add_wall_object(model, WallModifier::InfillDensity);

    Print print;
    slice_model(print, model, wall_config({{"reserve_outer_wall", false}}), nullptr);
    const Layer *top = layer_at_slice(print, 5.9);
    const LayerRegion *follower = merged_follower(*top, true);
    REQUIRE(follower != nullptr);

    for (const Layer *layer : print.objects().front()->layers()) {
        for (const LayerRegion *region : layer->regions())
            REQUIRE(region->reserved_outer_wall_bead.empty());
    }

    const ExPolygons bead = outer_bead_coverage(layer_paths(*top));
    REQUIRE_FALSE(bead.empty());
    const double solid_on_bead = expolygon_overlap_mm2(fill_surfaces_of(*follower, true), bead);
    INFO("follower solid fill on outer bead with reserve off " << solid_on_bead);
    REQUIRE(solid_on_bead > 0.01);
}

TEST_CASE("Reserve outer wall narrows a thick gap line and keeps the middle gap", "[ReserveOuterWall]")
{
    Print on;
    Print off;
    slice_gap_bars(on, {});
    slice_gap_bars(off, {{"reserve_outer_wall", false}});
    const Layer *on_layer = layer_at_slice(on, 1.9);
    const Layer *off_layer = layer_at_slice(off, 1.9);
    const PrintObject *on_object = on.objects().front();
    const PrintObject *off_object = off.objects().front();
    const std::vector<const ExtrusionPath *> on_paths = layer_paths(*on_layer);
    const std::vector<const ExtrusionPath *> off_paths = layer_paths(*off_layer);

    const std::vector<const ExtrusionPath *> outer_on = [&] {
        std::vector<const ExtrusionPath *> out;
        for (const ExtrusionPath *path : on_paths)
            if (is_outer_bead_role(path->role()))
                out.push_back(path);
        return out;
    }();
    const std::vector<const ExtrusionPath *> outer_off = [&] {
        std::vector<const ExtrusionPath *> out;
        for (const ExtrusionPath *path : off_paths)
            if (is_outer_bead_role(path->role()))
                out.push_back(path);
        return out;
    }();
    REQUIRE_FALSE(outer_on.empty());
    require_paths_unchanged(outer_on, outer_off);
    for (const ExtrusionPath *path : outer_on)
        REQUIRE(double(path->width) == Approx(0.42).margin(0.001));

    const std::vector<const ExtrusionPath *> inner_on =
        paths_of_role_overlapping_x(*on_object, on_paths, erPerimeter, 18., 40.);
    const std::vector<const ExtrusionPath *> inner_off =
        paths_of_role_overlapping_x(*off_object, off_paths, erPerimeter, 18., 40.);
    REQUIRE_FALSE(inner_on.empty());
    require_paths_unchanged(inner_on, inner_off);
    for (const ExtrusionPath *path : inner_on)
        REQUIRE(double(path->width) == Approx(0.45).margin(0.001));

    const ExPolygons off_bead = outer_bead_coverage(off_paths);
    const std::vector<const ExtrusionPath *> middle_on =
        paths_of_role_overlapping_x(*on_object, on_paths, erGapFill, 18., 40.);
    const std::vector<const ExtrusionPath *> middle_off =
        paths_of_role_overlapping_x(*off_object, off_paths, erGapFill, 18., 40.);
    REQUIRE_FALSE(middle_off.empty());
    for (const ExtrusionPath *path : middle_off)
        REQUIRE(overlap_area_mm2(*path, off_bead) <= 0.001);
    require_paths_unchanged(middle_on, middle_off);

    const ExPolygons on_core = outer_bead_core(outer_bead_coverage(on_paths));
    const std::vector<const ExtrusionPath *> thick_on =
        paths_of_role_overlapping_x(*on_object, on_paths, erGapFill, 0., 16.);
    const std::vector<const ExtrusionPath *> thick_off =
        paths_of_role_overlapping_x(*off_object, off_paths, erGapFill, 0., 16.);
    REQUIRE_FALSE(thick_off.empty());
    REQUIRE_FALSE(thick_on.empty());
    for (const ExtrusionPath *path : thick_off) {
        INFO("classic thick gap width " << path->width);
        REQUIRE(double(path->width) > 0.45);
        REQUIRE(overlap_area_mm2(*path, off_bead) > 0.01);
    }
    for (const ExtrusionPath *path : thick_on) {
        INFO("retracted gap width " << path->width);
        REQUIRE(double(path->width) == Approx(0.45).margin(0.001));
        const double core_overlap = overlap_area_mm2(*path, on_core);
        INFO("thick gap overlap with outer bead core " << core_overlap);
        REQUIRE(core_overlap <= 0.02);
    }
}

