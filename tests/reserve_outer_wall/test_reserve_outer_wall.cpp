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

void slice_wall(Print &print, std::initializer_list<ConfigBase::SetDeserializeItem> extra)
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

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "wall-wedge";
    object->add_volume(wall_with_wedge());
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);

    StringObjectException warning;
    const StringObjectException error = print.validate(&warning);
    INFO(error.string);
    INFO(warning.string);
    REQUIRE(error.string.empty());
    print.set_status_silent();
    print.process();
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

bool has_inner_bead_on_wedge(const PrintObject &object, const std::vector<const ExtrusionPath *> &paths)
{
    for (const ExtrusionPath *path : paths) {
        if (path->role() == erPerimeter && std::abs(path->width - 0.45) <= 0.001 &&
            x_interval_overlaps(object, *path, 14.5, 15.5))
            return true;
    }
    return false;
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

} // namespace

TEST_CASE("Reserve outer wall keeps the flat face and skips beads in the narrow channel", "[ReserveOuterWall]")
{
    Print print;
    slice_wall(print, {});
    const Layer *z2  = layer_at_slice(print, 1.9);
    const Layer *z42 = layer_at_slice(print, 4.1);
    const Layer *z5  = layer_at_slice(print, 5.3);

    const PrintObject                       *object  = print.objects().front();
    const std::vector<const ExtrusionPath *> paths2  = layer_paths(*z2);
    const std::vector<const ExtrusionPath *> paths42 = layer_paths(*z42);
    const std::vector<const ExtrusionPath *> paths5  = layer_paths(*z5);
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

    const ExPolygons bead5 = outer_bead_coverage(paths5);
    REQUIRE_FALSE(bead5.empty());
    const double overlap5 = max_later_overlap_mm2(paths5, bead5);
    INFO("later overlap at 5.3 " << overlap5);
    REQUIRE(overlap5 <= 0.01);
    REQUIRE(has_inner_bead_on_wedge(*object, paths5));

    REQUIRE_FALSE(has_role(paths2, erPerimeter));
    REQUIRE_FALSE(has_role(paths2, erGapFill));
    REQUIRE_FALSE(has_role(paths42, erPerimeter));
    REQUIRE_FALSE(has_role(paths42, erGapFill));

    for (const Layer *layer : print.objects().front()->layers())
        REQUIRE(interior_nonempty(*layer));
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
