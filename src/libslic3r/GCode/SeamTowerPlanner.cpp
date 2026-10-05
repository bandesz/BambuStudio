#include "SeamTowerPlanner.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SeamTower.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace Slic3r {

static Point scaled_xy(const Vec3f &p)
{
    return Point::new_scale(double(p.x()), double(p.y()));
}

static bool hop_target_matches(const Point &hop_target, const Point &loop_start, coord_t max_distance)
{
    const double max2 = double(max_distance) * double(max_distance);
    return (hop_target - loop_start).cast<double>().squaredNorm() <= max2;
}

static void nearest_layer_contour(const Layer *layer, const Point &seam, Polygon &contour, bool &is_hole, int &contour_id)
{
    is_hole    = false;
    contour_id = 0;
    contour    = {};
    if (layer == nullptr || layer->lslices.empty())
        return;

    double        best      = std::numeric_limits<double>::max();
    const Polygon *best_poly = nullptr;
    int            best_id   = 0;
    bool           best_hole = false;
    int            id        = 0;
    for (const ExPolygon &island : layer->lslices) {
        const double d_outer = (island.contour.point_projection(seam) - seam).cast<double>().squaredNorm();
        if (d_outer < best) {
            best      = d_outer;
            best_poly = &island.contour;
            best_id   = id;
            best_hole = false;
        }
        ++id;
        for (const Polygon &hole : island.holes) {
            const double d_hole = (hole.point_projection(seam) - seam).cast<double>().squaredNorm();
            if (d_hole < best) {
                best      = d_hole;
                best_poly = &hole;
                best_id   = id;
                best_hole = true;
            }
            ++id;
        }
    }
    if (best_poly == nullptr)
        return;
    contour    = *best_poly;
    is_hole    = best_hole;
    contour_id = best_id;
}

// The closest perimeter wins. A fixed radius tags the outer seam as a hole
// whenever the shell is thinner than that radius, and the outer tower is dropped.
static bool closest_perimeter_is_hole(const Layer *layer, const Point &seam, bool &found)
{
    found = false;
    if (layer == nullptr)
        return false;
    double best = std::numeric_limits<double>::max();
    bool   hole = false;
    auto consider = [&](const ExtrusionEntity *entity) {
        if (entity == nullptr || !entity->is_loop())
            return;
        const auto *loop = static_cast<const ExtrusionLoop *>(entity);
        const Point proj = loop->polygon().point_projection(seam);
        const double d2  = (proj - seam).cast<double>().squaredNorm();
        if (d2 >= best)
            return;
        best  = d2;
        hole  = (loop->loop_role() & elrPerimeterHole) != 0;
        found = true;
    };
    for (const LayerRegion *region : layer->regions()) {
        for (const ExtrusionEntity *ex : region->perimeters.entities) {
            consider(ex);
            if (!ex->is_collection())
                continue;
            for (const ExtrusionEntity *perimeter : static_cast<const ExtrusionEntityCollection *>(ex)->entities)
                consider(perimeter);
        }
    }
    return hole;
}

// True when the closest extruded loop is the wall a seam tower is printed
// before. An inner wall of a thin shell is not that wall.
static bool closest_loop_owns_tower(const Layer *layer, const Point &seam)
{
    if (layer == nullptr)
        return false;
    double best = std::numeric_limits<double>::max();
    bool   owns = false;
    auto consider = [&](const ExtrusionEntity *entity) {
        if (entity == nullptr || !entity->is_loop())
            return;
        const auto *loop = static_cast<const ExtrusionLoop *>(entity);
        const Point proj = loop->polygon().point_projection(seam);
        const double d2  = (proj - seam).cast<double>().squaredNorm();
        if (d2 >= best)
            return;
        best = d2;
        bool facing = false;
        for (const ExtrusionPath &path : loop->paths) {
            if (path.role() == erExternalPerimeter || path.role() == erOverhangPerimeter)
                facing = true;
        }
        const bool hole   = (loop->loop_role() & elrPerimeterHole) != 0;
        const bool second = (loop->loop_role() & elrSecondPerimeter) != 0;
        owns = facing && !hole && !second;
    };
    for (const LayerRegion *region : layer->regions()) {
        for (const ExtrusionEntity *ex : region->perimeters.entities) {
            consider(ex);
            if (!ex->is_collection())
                continue;
            for (const ExtrusionEntity *perimeter : static_cast<const ExtrusionEntityCollection *>(ex)->entities)
                consider(perimeter);
        }
    }
    return owns;
}

static coord_t stack_xy_window(const SeamTowerStack &stack)
{
    coord_t window = 0;
    for (size_t i = 0; i < stack.seams.size(); ++i) {
        for (size_t j = i + 1; j < stack.seams.size(); ++j) {
            const coord_t d = coord_t((stack.seams[i].point - stack.seams[j].point).cast<double>().norm());
            window          = std::max(window, d);
        }
    }
    return window;
}

static Point seam_on_layer(const SeamTowerStack &stack, int layer)
{
    const SeamTowerSeam *best = nullptr;
    int                  best_d = std::numeric_limits<int>::max();
    for (const SeamTowerSeam &seam : stack.seams) {
        if (seam.layer_index == layer)
            return seam.point;
        const int d = std::abs(seam.layer_index - layer);
        if (d < best_d) {
            best_d = d;
            best   = &seam;
        }
    }
    return best != nullptr ? best->point : Point(0, 0);
}

static bool stack_has_seam_on_layer(const SeamTowerStack &stack, int layer)
{
    for (const SeamTowerSeam &seam : stack.seams)
        if (seam.layer_index == layer)
            return true;
    return false;
}

// Several perimeters on one layer can share a contour id when their outlines
// overlap. The map used to keep only the last outline, so a seam on the outer
// wall was offset from an inner island stored later under the same id.
using ContoursByLayer = std::map<std::pair<int, int>, std::vector<Polygon>>;

static const Polygon *closest_contour(const std::vector<Polygon> &polys, const Point &seam)
{
    const Polygon *best   = nullptr;
    double         best_d = std::numeric_limits<double>::max();
    for (const Polygon &poly : polys) {
        if (poly.size() < 3)
            continue;
        const double d = (poly.point_projection(seam) - seam).cast<double>().squaredNorm();
        if (d < best_d) {
            best_d = d;
            best   = &poly;
        }
    }
    return best;
}

// The outline for this stack on this layer. The stack's own contour id wins.
// If that id is missing, use this layer's outline the seam actually sits on.
// Otherwise reuse this stack's own outline from the nearest layer, so an
// overhang can stand from the first layer. Tiny facets are skipped before this.
static const Polygon *contour_on_layer(
    const ContoursByLayer &contours, int contour_id, int layer, const Point &seam)
{
    auto it = contours.find({contour_id, layer});
    if (it != contours.end()) {
        if (const Polygon *poly = closest_contour(it->second, seam))
            return poly;
    }

    const Polygon *near   = nullptr;
    double         near_d = std::numeric_limits<double>::max();
    const double   on_wall = double(scale_(2.0)) * double(scale_(2.0));
    auto consider = [&](const Polygon &poly) {
        if (poly.size() < 3)
            return;
        const double d = (poly.point_projection(seam) - seam).cast<double>().squaredNorm();
        if (d < near_d) {
            near_d = d;
            near   = &poly;
        }
    };
    for (const auto &kv : contours) {
        if (kv.first.second != layer)
            continue;
        for (const Polygon &poly : kv.second)
            consider(poly);
    }
    if (near != nullptr && near_d <= on_wall)
        return near;

    const std::vector<Polygon> *nearest = nullptr;
    int                         nearest_d = std::numeric_limits<int>::max();
    for (const auto &kv : contours) {
        if (kv.first.first != contour_id)
            continue;
        const int d = std::abs(kv.first.second - layer);
        if (d < nearest_d) {
            nearest_d = d;
            nearest   = &kv.second;
        }
    }
    return nearest != nullptr ? closest_contour(*nearest, seam) : nullptr;
}

// The largest outline in this stack whose seam lies outside `foot`. A foot that
// shares the wall's contour id sits under the overhang; supports fill that
// step. The seam on the wall above is still `gap` outside that wall.
static const Polygon *overhanging_wall_contour(
    const ContoursByLayer &contours, const SeamTowerStack &stack, const Polygon &foot, Point &seam_out)
{
    const double apart2   = double(scale_(2.0)) * double(scale_(2.0));
    const double on_wall2 = double(scale_(1.0)) * double(scale_(1.0));
    const Polygon *best   = nullptr;
    double         best_a = std::abs(foot.area());
    for (const SeamTowerSeam &sample : stack.seams) {
        const Polygon *poly = contour_on_layer(contours, stack.contour_id, sample.layer_index, sample.point);
        if (poly == nullptr || poly->is_clockwise())
            continue;
        const double on_poly = (poly->point_projection(sample.point) - sample.point).cast<double>().squaredNorm();
        if (on_poly > on_wall2)
            continue;
        const double area = std::abs(poly->area());
        if (area <= best_a)
            continue;
        const double d2 = (foot.point_projection(sample.point) - sample.point).cast<double>().squaredNorm();
        if (d2 <= apart2)
            continue;
        best_a   = area;
        best     = poly;
        seam_out = sample.point;
    }
    return best;
}

static const char *kSeamTowerSkipMessage = "Seam tower skipped: not enough space.";

static bool island_hits_polygons(const ExPolygon &island, const ExPolygons &polys)
{
    return !polys.empty() && !intersection_ex(island, polys).empty();
}

static bool island_hits_box(const ExPolygon &island, const BoundingBox &box)
{
    if (!box.defined)
        return false;
    if (!get_extents(island).overlap(box))
        return false;
    return !intersection_ex(island, ExPolygon{box.polygon()}).empty();
}

static bool island_collides(const ExPolygon &island, const SeamTowerParams &params, const ExPolygons &accepted)
{
    if (params.plate_bbox.defined && !params.plate_bbox.contains(get_extents(island)))
        return true;
    if (island_hits_box(island, params.wipe_tower_bbox))
        return true;
    if (island_hits_polygons(island, params.collision_polygons))
        return true;
    return island_hits_polygons(island, accepted);
}

static bool outside_plate(const ExPolygon &island, const BoundingBox &plate, const Point &plate_shift)
{
    if (!plate.defined)
        return false;
    ExPolygon local = island;
    local.translate(plate_shift);
    return !plate.contains(get_extents(local));
}

// Sliced contours are already centered. Other objects, supports, siblings, accepted
// towers, and the wipe tower share PrintInstance::shift (the same shift
// Print::first_layer_islands uses). The bed polygon from get_bed_shape is
// plate-local, so it is tested with shift_without_plate_offset.
// blocked receives the world_shifts indices whose copy cannot print this island.
// Returns true only when there is no copy left: the input-only path (no shifts)
// collided, or every instance did. A crowded copy does not cancel a clear one.
static bool island_collides_in_world(const ExPolygon             &island,
                                     const SeamTowerParams       &params,
                                     const ExPolygons            &accepted,
                                     const std::vector<Point>    &world_shifts,
                                     const std::vector<Point>    &plate_shifts,
                                     const ExPolygons            &self_local,
                                     std::vector<size_t>         &blocked)
{
    blocked.clear();
    if (world_shifts.empty())
        return island_collides(island, params, accepted);
    for (size_t i = 0; i < world_shifts.size(); ++i) {
        bool collides = outside_plate(island, params.plate_bbox, plate_shifts[i]);
        ExPolygon world = island;
        world.translate(world_shifts[i]);
        collides = collides || island_hits_box(world, params.wipe_tower_bbox) ||
                   island_hits_polygons(world, params.collision_polygons) ||
                   island_hits_polygons(world, accepted);
        if (!collides && world_shifts.size() > 1 && !self_local.empty()) {
            for (size_t other_i = 0; other_i < world_shifts.size(); ++other_i) {
                if (other_i == i)
                    continue;
                ExPolygons siblings;
                siblings.reserve(self_local.size());
                for (const ExPolygon &ex : self_local) {
                    ExPolygon moved = ex;
                    moved.translate(world_shifts[other_i]);
                    siblings.push_back(std::move(moved));
                }
                if (island_hits_polygons(world, siblings)) {
                    collides = true;
                    break;
                }
            }
        }
        if (collides)
            blocked.push_back(i);
    }
    return blocked.size() == world_shifts.size();
}

static void accept_island_in_world(ExPolygons &accepted, const ExPolygon &island, const std::vector<Point> &world_shifts)
{
    if (world_shifts.empty()) {
        accepted.push_back(island);
        return;
    }
    for (const Point &shift : world_shifts) {
        ExPolygon world = island;
        world.translate(shift);
        accepted.push_back(std::move(world));
    }
}

static ExPolygon largest_expolygon(const ExPolygons &exs);

// A lattice can change contour identity and start a new stack on the column
// already accepted for this seam. That column is the support under the pad.
static ExPolygon tower_under_island(const ExPolygon &island, const ExPolygons &accepted, const std::vector<Point> &world_shifts)
{
    if (accepted.empty())
        return {};
    ExPolygons below;
    auto collect = [&](const ExPolygons &local_accepted) {
        ExPolygons hit = intersection_ex(ExPolygons{island}, local_accepted);
        below.insert(below.end(), hit.begin(), hit.end());
    };
    if (world_shifts.empty())
        collect(accepted);
    else {
        for (const Point &shift : world_shifts) {
            ExPolygons local_accepted;
            local_accepted.reserve(accepted.size());
            for (const ExPolygon &world : accepted) {
                ExPolygon local = world;
                local.translate(Point(-shift.x(), -shift.y()));
                local_accepted.push_back(std::move(local));
            }
            collect(local_accepted);
        }
    }
    return largest_expolygon(below);
}

static coord_t object_line_width(const Print &print, const PrintObject *po)
{
    float lw = print.default_region_config().outer_wall_line_width;
    if (lw <= 0)
        lw = po->config().line_width;
    if (lw <= 0)
        lw = print.config().nozzle_diameter.get_at(0);
    return scale_(lw);
}

static ExPolygons object_local_supports(const PrintObject *po)
{
    ExPolygons local;
    for (const SupportLayer *sl : po->support_layers())
        local.insert(local.end(), sl->support_islands.begin(), sl->support_islands.end());
    return union_ex(local);
}

// Part slices and supports. Used for other objects and for sibling instances,
// which block a tower at every height. This object's own part slices are not
// used this way: each layer is tested on its own, below.
static ExPolygons object_local_slices(const PrintObject *po)
{
    ExPolygons local;
    for (const Layer *layer : po->layers())
        local.insert(local.end(), layer->lslices.begin(), layer->lslices.end());
    ExPolygons supports = object_local_supports(po);
    local.insert(local.end(), supports.begin(), supports.end());
    return union_ex(local);
}

// The island the seam is printed on. A tower may graze this wall when a later
// layer sticks out; it must not enter any other island.
static const ExPolygon *source_slice(const ExPolygons &solid, const Point &seam)
{
    const ExPolygon *nearest = nullptr;
    double           best    = std::numeric_limits<double>::max();
    for (const ExPolygon &ex : solid) {
        if (ex.contains(seam))
            return &ex;
        const double d2 = (ex.contour.point_projection(seam) - seam).cast<double>().squaredNorm();
        if (d2 < best) {
            best    = d2;
            nearest = &ex;
        }
    }
    return nearest;
}

static bool hits_foreign_solid(const ExPolygon &island, const ExPolygons &solid, const Point &seam)
{
    if (solid.size() < 2)
        return false;
    const ExPolygon *source = source_slice(solid, seam);
    ExPolygons       foreign;
    foreign.reserve(solid.size() - 1);
    for (const ExPolygon &ex : solid) {
        if (&ex == source)
            continue;
        foreign.push_back(ex);
    }
    return island_hits_polygons(island, foreign);
}

static ExPolygon largest_expolygon(const ExPolygons &exs)
{
    const ExPolygon *best      = nullptr;
    double           best_area = 0;
    for (const ExPolygon &ex : exs) {
        const double area = std::abs(ex.area());
        if (area > best_area) {
            best_area = area;
            best      = &ex;
        }
    }
    return best != nullptr ? *best : ExPolygon{};
}

static ExPolygons translate_instances_to_world(const PrintObject *po, const ExPolygons &local)
{
    ExPolygons world;
    for (const PrintInstance &inst : po->instances()) {
        const Point shift = inst.shift;
        for (const ExPolygon &ex : local) {
            ExPolygon moved = ex;
            moved.translate(shift);
            world.push_back(std::move(moved));
        }
    }
    return world;
}

// One full-height union per object for this build. Neighbors read the world
// copy; sibling instances read the same local union.
struct ObjectSilhouette
{
    ExPolygons local;
    ExPolygons world;
};

static std::map<const PrintObject *, ObjectSilhouette> cache_object_silhouettes(const Print &print)
{
    std::map<const PrintObject *, ObjectSilhouette> cache;
    for (const PrintObject *po : print.objects()) {
        ObjectSilhouette sil;
        sil.local = object_local_slices(po);
        sil.world = translate_instances_to_world(po, sil.local);
        cache.emplace(po, std::move(sil));
    }
    return cache;
}

static BoundingBox wipe_tower_world_bbox(const Print &print)
{
    if (!print.has_wipe_tower())
        return {};

    const PrintConfig &config       = print.config();
    const int          plate_index  = print.get_plate_index();
    const Vec3d        plate_origin = print.get_plate_origin();
    float              x            = config.wipe_tower_x.get_at(plate_index) + plate_origin(0);
    float              y            = config.wipe_tower_y.get_at(plate_index) + plate_origin(1);
    float              width        = config.prime_tower_width.value;
    float              a            = config.wipe_tower_rotation_angle.value;
    float              depth        = print.wipe_tower_data(print.extruders().size()).depth;
    if (config.prime_tower_rib_wall.value)
        width = depth;

    // Same transform as get_wipe_tower_extrusions_extents: the footprint is stored
    // in the tower's local frame, then Rotation about that origin, then Translation.
    Polygon poly;
    if (!print.is_step_done(psWipeTower)) {
        poly.points.emplace_back(scale_(0.), scale_(0.));
        poly.points.emplace_back(scale_(width), scale_(0.));
        poly.points.emplace_back(scale_(width), scale_(depth));
        poly.points.emplace_back(scale_(0.), scale_(depth));
    } else if (print.wipe_tower_data().wipe_tower_mesh_data) {
        // Brim and rib offset are already in this local polygon.
        poly = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
    }
    if (poly.size() >= 3) {
        poly.rotate(Geometry::deg2rad(a));
        poly.translate(Point(scale_(x), scale_(y)));
    }
    if (poly.size() < 3)
        return {};
    return get_extents(poly);
}

static void start_polygon_at_closest(Polygon &poly, const Point &target)
{
    if (poly.points.size() < 3)
        return;
    const Point closest = poly.point_projection(target);
    for (size_t i = 0; i < poly.points.size(); ++i) {
        if (poly.points[i] != closest)
            continue;
        if (i != 0)
            std::rotate(poly.points.begin(), poly.points.begin() + std::ptrdiff_t(i), poly.points.end());
        return;
    }

    const size_t n = poly.points.size();
    size_t       insert_after = 0;
    double       best         = std::numeric_limits<double>::max();
    for (size_t i = 0; i < n; ++i) {
        const Line   line(poly.points[i], poly.points[(i + 1) % n]);
        Point        foot;
        const double d2 = line.distance_to_squared(closest, &foot);
        if (d2 < best) {
            best         = d2;
            insert_after = i;
        }
    }
    poly.points.insert(poly.points.begin() + std::ptrdiff_t(insert_after + 1), closest);
    std::rotate(poly.points.begin(), poly.points.begin() + std::ptrdiff_t(insert_after + 1), poly.points.end());
}

static ExtrusionEntityCollection fill_seam_tower_island(
    const ExPolygon &island, const SeamTowerParams &params, float height_mm, const Point &wall_start, bool is_first_layer)
{
    ExtrusionEntityCollection out;
    if (island.empty() || params.line_width <= 0)
        return out;
    if (height_mm <= 0)
        height_mm = 0.2f;

    const float line_width_mm = unscale<float>(params.line_width);
    const float mm3_per_mm    = line_width_mm * height_mm;
    const float step          = float(params.line_width);

    std::vector<Polygons> levels;
    ExPolygons            remaining = offset_ex(island, -0.5f * step);
    while (!remaining.empty()) {
        Polygons loops;
        loops.reserve(remaining.size());
        for (const ExPolygon &ex : remaining)
            if (ex.contour.is_valid())
                loops.push_back(ex.contour);
        if (!loops.empty())
            levels.push_back(std::move(loops));
        remaining = offset_ex(remaining, -step);
    }

    if (!is_first_layer) {
        if (levels.size() > 2)
            levels.resize(2);
    } else if (!levels.empty()) {
        constexpr double extra_cap_mm = 5.0;
        const size_t     wall_count   = std::min(levels.size(), size_t(2));
        const size_t     inner_wall   = wall_count - 1;
        size_t           keep         = wall_count;
        for (size_t i = wall_count; i < levels.size(); ++i) {
            if (double(i - inner_wall) * line_width_mm > extra_cap_mm)
                break;
            keep = i + 1;
        }
        levels.resize(keep);
    }

    for (size_t i = levels.size(); i-- > 0;) {
        if (i == 0) {
            for (Polygon &poly : levels[i])
                start_polygon_at_closest(poly, wall_start);
        }
        extrusion_entities_append_loops(out.entities, std::move(levels[i]), erSeamTower, mm3_per_mm, line_width_mm, height_mm);
    }
    return out;
}

struct ContourTrack
{
    Polygon polygon;
    bool    is_hole = false;
    int     id      = 0;
};

// True when two layer contours are the same wall. A fin inside the outer
// contour is contained by it, so raw overlap is not enough: the shared area
// has to be most of the larger polygon.
static bool same_contour(const Polygon &a, const Polygon &b)
{
    if (a.size() < 3 || b.size() < 3)
        return false;
    Polygon aa = a;
    Polygon bb = b;
    aa.make_counter_clockwise();
    bb.make_counter_clockwise();
    const double denom = std::max(std::abs(aa.area()), std::abs(bb.area()));
    if (denom <= 0)
        return false;
    const ExPolygons hit = intersection_ex(ExPolygon{std::move(aa)}, ExPolygon{std::move(bb)});
    double overlap = 0;
    for (const ExPolygon &ex : hit)
        overlap += std::abs(ex.area());
    return overlap / denom > 0.5;
}

// A seam on an outline that sits inside a larger one is not the part's outer
// wall. The hexagon's inner boundary is close to its own polygon and was
// given the outer wall's identity, so a tower grew beside a wall with no
// outer seam.
static bool seam_lies_inside_larger_outline(const Layer *layer, const Polygon &contour, const Point &seam)
{
    if (layer == nullptr || contour.size() < 3)
        return false;
    const double mine = std::abs(contour.area());
    if (mine <= 0)
        return false;
    const double apart = double(scale_(1.0)) * double(scale_(1.0));
    for (const ExPolygon &island : layer->lslices) {
        if (island.contour.size() < 3)
            continue;
        if (std::abs(island.contour.area()) <= mine * 1.05)
            continue;
        if (!island.contour.contains(seam))
            continue;
        const double d2 = (island.contour.point_projection(seam) - seam).cast<double>().squaredNorm();
        if (d2 > apart)
            return true;
    }
    return false;
}

// An inactive layer keeps the column only while the seam is still on the
// outside of this layer. A seam that shows up on the top, several millimetres
// inside the hexagon, must not grow a column down the outside wall.
static bool inactive_seam_is_inside_the_part(const Layer *layer, const Point &seam)
{
    if (layer == nullptr)
        return false;
    const Polygon *largest = nullptr;
    double         largest_area = 0;
    for (const ExPolygon &island : layer->lslices) {
        if (island.contour.size() < 3)
            continue;
        const double area = std::abs(island.contour.area());
        if (area > largest_area) {
            largest_area = area;
            largest      = &island.contour;
        }
    }
    if (largest == nullptr || !largest->contains(seam))
        return false;
    const double apart = double(scale_(1.0)) * double(scale_(1.0));
    const double d2    = (largest->point_projection(seam) - seam).cast<double>().squaredNorm();
    return d2 > apart;
}

// The hole wall sits inside the outer contour, so the check above is true for
// every hole seam. A through hole still has that wall on layers whose seam
// sample joined another stack. Those layers keep the column.
static bool seam_lies_on_a_hole(const Layer *layer, const Point &seam)
{
    if (layer == nullptr)
        return false;
    const double near = double(scale_(1.0)) * double(scale_(1.0));
    for (const ExPolygon &island : layer->lslices) {
        for (const Polygon &hole : island.holes) {
            if (hole.size() < 3)
                continue;
            const double d2 = (hole.point_projection(seam) - seam).cast<double>().squaredNorm();
            if (d2 <= near)
                return true;
        }
    }
    return false;
}

// Island order in lslices changes when a fin appears. Match contours to the
// previous layer by overlap so one wall keeps one stack for its whole height.
static int match_contour_id(const Polygon &poly, bool is_hole, const std::vector<ContourTrack> &previous,
                            std::vector<ContourTrack> &current, int &next_id)
{
    for (const ContourTrack &track : current)
        if (track.is_hole == is_hole && same_contour(track.polygon, poly))
            return track.id;

    int    best_id   = -1;
    double best_area = 0;
    for (const ContourTrack &track : previous) {
        if (track.is_hole != is_hole || !same_contour(track.polygon, poly))
            continue;
        const double area = std::abs(track.polygon.area());
        if (area > best_area) {
            best_area = area;
            best_id   = track.id;
        }
    }
    const int id = best_id >= 0 ? best_id : next_id++;
    current.push_back(ContourTrack{poly, is_hole, id});
    return id;
}

static void collect_inputs_from_object(
    const PrintObject *po, const PrintObjectSeamData &seam_data, int object_base, std::vector<SeamTowerBuildInput> &out)
{
    std::vector<ContourTrack> previous;
    int                       next_id = 0;
    for (size_t layer_idx = 0; layer_idx < seam_data.layers.size(); ++layer_idx) {
        const PrintObjectSeamData::LayerSeams &layer_seams = seam_data.layers[layer_idx];
        const Layer *layer = (po != nullptr && layer_idx < po->layer_count()) ? po->get_layer(int(layer_idx)) : nullptr;
        std::vector<ContourTrack> current;
        for (const SeamPlacerImpl::Perimeter &perim : layer_seams.perimeters) {
            Point seam;
            if (perim.finalized)
                seam = scaled_xy(perim.final_seam_position);
            else if (perim.seam_index < layer_seams.points.size())
                seam = scaled_xy(layer_seams.points[perim.seam_index].position);
            else if (perim.start_index < layer_seams.points.size())
                seam = scaled_xy(layer_seams.points[perim.start_index].position);
            else
                continue;

            // The stored seam is where the wall starts. When that point is the
            // external wall, classify from a point on the wall. An aligned seam
            // can sit in the gap of a thin shell, closer to the hole behind the
            // wall, and the tower would then be printed after the wall.
            Point on_perimeter = seam;
            if (perim.seam_index < layer_seams.points.size())
                on_perimeter = scaled_xy(layer_seams.points[perim.seam_index].position);
            else if (perim.start_index < layer_seams.points.size())
                on_perimeter = scaled_xy(layer_seams.points[perim.start_index].position);
            // Only the external wall is reclassified, and only when the aligned
            // seam itself was about to be called a hole. Other seams keep the
            // point they are stored at, so a speck beside the wall stays a speck.
            bool seam_found = false;
            const bool seam_is_hole = closest_perimeter_is_hole(layer, seam, seam_found);
            const Point classify_at = (seam_found && seam_is_hole && closest_loop_owns_tower(layer, on_perimeter))
                                          ? on_perimeter : seam;

            Polygon contour;
            bool    is_hole    = false;
            int     contour_id = 0;
            nearest_layer_contour(layer, classify_at, contour, is_hole, contour_id);
            if (contour.empty()) {
                for (size_t i = perim.start_index; i <= perim.end_index && i < layer_seams.points.size(); ++i)
                    contour.points.push_back(scaled_xy(layer_seams.points[i].position));
                is_hole = contour.is_clockwise();
            }
            bool loop_found = false;
            if (const bool loop_hole = closest_perimeter_is_hole(layer, classify_at, loop_found); loop_found)
                is_hole = loop_hole;
            if (is_hole != contour.is_clockwise() && layer != nullptr) {
                double        best = std::numeric_limits<double>::max();
                const Polygon *match = nullptr;
                for (const ExPolygon &island : layer->lslices) {
                    auto consider = [&](const Polygon &poly, bool hole) {
                        if (hole != is_hole)
                            return;
                        const double d2 = (poly.point_projection(classify_at) - classify_at).cast<double>().squaredNorm();
                        if (d2 < best) {
                            best  = d2;
                            match = &poly;
                        }
                    };
                    consider(island.contour, false);
                    for (const Polygon &hole : island.holes)
                        consider(hole, true);
                }
                if (match != nullptr)
                    contour = *match;
            }
            if (!contour.empty())
                contour_id = match_contour_id(contour, is_hole, previous, current, next_id);

            if (!is_hole && seam_lies_inside_larger_outline(layer, contour, seam))
                is_hole = true;

            const float height_mm = (layer != nullptr) ? float(layer->height) : 0.2f;
            out.push_back({object_base + contour_id, int(layer_idx), seam, std::move(contour), is_hole, height_mm});
        }
        if (!current.empty())
            previous = std::move(current);
    }
}

void SeamTowerPlanner::append_stacks(const std::vector<SeamTowerBuildInput> &inputs,
                                     const SeamTowerParams                  &params,
                                     ExPolygons                             &accepted,
                                     const std::vector<Point>               &world_shifts,
                                     const std::vector<Point>               &plate_shifts,
                                     const PrintObject                      *po,
                                     const ExPolygons                       &self_local)
{
    m_match_distance = std::max(m_match_distance, params.gap + params.line_width);
    if (!params.enabled || params.spiral_vase)
        return;

    std::vector<SeamTowerSample>       samples;
    ContoursByLayer                    contours;
    std::map<int, float>                   layer_heights;
    std::map<int, const PrintObject *>     objects_by_contour;
    samples.reserve(inputs.size());
    for (const SeamTowerBuildInput &in : inputs) {
        if (in.is_hole && !params.in_holes)
            continue;
        if (!in.contour.empty())
            contours[{in.contour_id, in.layer_index}].push_back(in.contour);
        layer_heights[in.layer_index] = (in.height_mm > 0) ? in.height_mm : 0.2f;
        if (in.object != nullptr)
            objects_by_contour[in.contour_id] = in.object;
        if (in.has_seam)
            samples.push_back({in.contour_id, in.layer_index, in.point});
    }
    if (samples.empty())
        return;

    std::vector<SeamTowerStack> stacks = cluster_seam_tower_stacks(samples, params.depth, params.line_width);

    double bed_area = 0;
    for (const auto &kv : contours) {
        if (kv.first.second != 0)
            continue;
        for (const Polygon &poly : kv.second)
            bed_area = std::max(bed_area, std::abs(poly.area()));
    }

    for (const SeamTowerStack &stack : stacks) {
        if (stack.seams.empty())
            continue;
        // A one-layer facet beside the outer wall is not that wall's seam. It is
        // absent from the first layer and tiny next to the part, so grounding it
        // would plant an extra column on the bed. A real overhang is a large
        // share of the first-layer wall and is still built.
        if (bed_area > 0 && contours.find({stack.contour_id, 0}) == contours.end()) {
            double ref_area = 0;
            for (const auto &kv : contours) {
                if (kv.first.first != stack.contour_id)
                    continue;
                for (const Polygon &poly : kv.second)
                    ref_area = std::max(ref_area, std::abs(poly.area()));
            }
            if (ref_area < 0.05 * bed_area) {
                m_warnings.emplace_back(kSeamTowerSkipMessage);
                continue;
            }
        }
        const coord_t stack_window = stack_xy_window(stack);
        const coord_t min_size     = params.min_size;
        ExPolygon     previous;
        std::vector<std::pair<int, ExtrusionEntityCollection>> filled_layers;
        // Upper layers are clipped inside the first island, so the column's
        // 2D footprint is that island. Testing and storing every layer makes
        // collision quadratic in objects × layers.
        ExPolygon footprint;
        bool      have_footprint = false;
        bool      ok             = true;
        std::set<Point> skipped_shifts;
        // The column stands from the object's first layer. Layers below the
        // first seam are still extruded so the pad is not floating when the
        // seam appears. A layer that cannot be built cancels the whole column.
        // Set when the foot outline is buried in supports and the wall above
        // still has its gap clear. Later layers of the foot keep this outline
        // so the column is not pulled back into the overhang.
        const Polygon *wall_contour = nullptr;
        Point          wall_seam;
        for (int layer = 0; layer <= stack.last_layer; ++layer) {
            Point      seam     = seam_on_layer(stack, layer);
            if (!stack_has_seam_on_layer(stack, layer) && po != nullptr && layer >= 0 && size_t(layer) < po->layer_count()
                && inactive_seam_is_inside_the_part(po->get_layer(layer), seam)
                && !seam_lies_on_a_hole(po->get_layer(layer), seam)) {
                ok = false;
                break;
            }
            const Polygon   *contour  = contour_on_layer(contours, stack.contour_id, layer, seam);
            if (wall_contour != nullptr && contour != nullptr && !contour->is_clockwise()
                && std::abs(wall_contour->area()) > std::abs(contour->area())
                && !contour->contains(wall_seam)) {
                const double d2 = (contour->point_projection(wall_seam) - wall_seam).cast<double>().squaredNorm();
                const double apart2 = double(scale_(2.0)) * double(scale_(2.0));
                if (d2 > apart2) {
                    contour = wall_contour;
                    seam    = wall_seam;
                }
            }
            if (contour == nullptr || contour->empty()) {
                ok = false;
                break;
            }
            const ExPolygon *prev_ptr = have_footprint ? &previous : nullptr;
            ExPolygon island = make_seam_tower_island(
                *contour, seam, params.gap, params.depth, params.length, stack_window, min_size, prev_ptr, params.line_width);
            if (island.empty()) {
                ok = false;
                break;
            }
            auto trim_to_slices = [&](ExPolygon &pad) {
                if (po == nullptr || layer < 0 || size_t(layer) >= po->layer_count())
                    return true;
                const ExPolygons &slices = po->get_layer(layer)->lslices;
                if (hits_foreign_solid(pad, slices, seam))
                    return false;
                if (!slices.empty())
                    pad = largest_expolygon(diff_ex(pad, slices));
                return !pad.empty();
            };
            // Own part slices are per layer. A textured wall's peaks live on
            // other layers; folding every layer into one polygon cancels a
            // tower that is clear of the wall it is printed next to. Entering
            // a different island (an internal fin aimed at the outer wall)
            // still cancels the tower. Grazing this same wall is trimmed.
            if (!trim_to_slices(island)) {
                ok = false;
                break;
            }
            if (!have_footprint) {
                std::vector<size_t> blocked;
                auto place_beside_overhanging_wall = [&]() -> bool {
                    Point          relocated_seam;
                    const Polygon *relocated = overhanging_wall_contour(contours, stack, *contour, relocated_seam);
                    if (relocated == nullptr)
                        return false;
                    ExPolygon pad = make_seam_tower_island(
                        *relocated, relocated_seam, params.gap, params.depth, params.length, stack_window, min_size,
                        nullptr, params.line_width);
                    if (pad.empty())
                        return false;
                    const Point saved = seam;
                    seam = relocated_seam;
                    if (!trim_to_slices(pad)) {
                        seam = saved;
                        return false;
                    }
                    std::vector<size_t> relocated_blocked;
                    if (island_collides_in_world(pad, params, accepted, world_shifts, plate_shifts, self_local, relocated_blocked)) {
                        seam = saved;
                        return false;
                    }
                    island       = std::move(pad);
                    blocked      = std::move(relocated_blocked);
                    contour      = relocated;
                    wall_contour = relocated;
                    wall_seam    = relocated_seam;
                    return true;
                };
                if (island_collides_in_world(island, params, accepted, world_shifts, plate_shifts, self_local, blocked)) {
                    const ExPolygons no_towers;
                    const bool elsewhere = island_collides_in_world(
                        island, params, no_towers, world_shifts, plate_shifts, self_local, blocked);
                    const ExPolygon support = elsewhere ? ExPolygon{} :
                        tower_under_island(island, accepted, world_shifts);
                    bool placed = false;
                    if (!support.empty()) {
                        island = make_seam_tower_island(
                            *contour, seam, params.gap, params.depth, params.length, stack_window, min_size, &support, params.line_width);
                        placed = !island.empty() && trim_to_slices(island) &&
                            !island_collides_in_world(island, params, no_towers, world_shifts, plate_shifts, self_local, blocked);
                    }
                    if (!placed && !place_beside_overhanging_wall()) {
                        ok = false;
                        break;
                    }
                }
                for (size_t index : blocked)
                    skipped_shifts.insert(world_shifts[index]);
                footprint      = island;
                have_footprint = true;
            }
            auto height_it = layer_heights.find(layer);
            float height_mm = 0.2f;
            if (height_it != layer_heights.end())
                height_mm = height_it->second;
            else if (po != nullptr && layer >= 0 && size_t(layer) < po->layer_count())
                height_mm = float(po->get_layer(layer)->height);
            // The 5 mm brim is bed adhesion on the object's first layer.
            ExtrusionEntityCollection filled = fill_seam_tower_island(island, params, height_mm, seam,
                                                                      layer == 0);
            if (filled.empty()) {
                ok = false;
                break;
            }
            filled_layers.emplace_back(layer, std::move(filled));
            previous = std::move(island);
        }
        if (!ok) {
            m_warnings.emplace_back(kSeamTowerSkipMessage);
            continue;
        }
        if (!skipped_shifts.empty())
            m_warnings.emplace_back(kSeamTowerSkipMessage);
        for (auto &item : filled_layers) {
            EmitItem emit;
            emit.collection     = item.second;
            emit.hop_target     = seam_on_layer(stack, item.first);
            emit.object         = po;
            emit.skipped_shifts = skipped_shifts;
            emit.active         = stack_has_seam_on_layer(stack, item.first);
            if (emit.object == nullptr) {
                auto oit = objects_by_contour.find(stack.contour_id);
                if (oit != objects_by_contour.end())
                    emit.object = oit->second;
            }
            m_emits[item.first].push_back(std::move(emit));
        }
        if (have_footprint) {
            std::vector<Point> clear_shifts;
            clear_shifts.reserve(world_shifts.size());
            for (const Point &shift : world_shifts) {
                if (!skipped_shifts.count(shift))
                    clear_shifts.push_back(shift);
            }
            accept_island_in_world(accepted, footprint, world_shifts.empty() ? world_shifts : clear_shifts);
        }
    }
}

void SeamTowerPlanner::build(const Print &print, const SeamPlacer &seam_placer, const SeamTowerParams &params)
{
    m_emits.clear();
    m_warnings.clear();
    m_match_distance = 0;
    if (params.spiral_vase || print.config().spiral_mode.value)
        return;

    // Disabled neighbors are collected only once some object actually builds
    // towers. With the option off everywhere, skip the full-height unions.
    bool any_seam_tower = false;
    for (const PrintObject *po : print.objects()) {
        if (po->config().seam_tower) {
            any_seam_tower = true;
            break;
        }
    }
    if (!any_seam_tower)
        return;

    SeamTowerParams shared = params;
    shared.enabled         = true;
    const Points bed       = get_bed_shape(print.config());
    if (bed.size() >= 3)
        shared.plate_bbox = BoundingBox(bed);
    shared.wipe_tower_bbox = wipe_tower_world_bbox(print);

    // Includes objects with seam_tower off: they still block neighbors.
    const std::map<const PrintObject *, ObjectSilhouette> silhouettes = cache_object_silhouettes(print);

    ExPolygons accepted;
    int        object_base = 0;
    for (const PrintObject *po : print.objects()) {
        if (!po->config().seam_tower) {
            object_base += 1000000;
            continue;
        }

        SeamTowerParams op = shared;
        op.gap             = scale_(po->config().seam_tower_gap);
        op.depth           = scale_(po->config().seam_tower_depth);
        op.length          = scale_(po->config().seam_tower_length);
        op.in_holes        = po->config().seam_tower_in_holes;
        op.min_size        = scale_(po->config().seam_tower_min_size);
        op.line_width      = object_line_width(print, po);
        for (const PrintObject *other : print.objects()) {
            if (other == po)
                continue;
            const ExPolygons &part = silhouettes.at(other).world;
            op.collision_polygons.insert(op.collision_polygons.end(), part.begin(), part.end());
        }
        // Supports block at every height. Part slices are tested per layer in
        // append_stacks, so a bump on one layer does not occupy the tower on another.
        ExPolygons supports = translate_instances_to_world(po, object_local_supports(po));
        op.collision_polygons.insert(op.collision_polygons.end(), supports.begin(), supports.end());

        std::vector<Point> shifts;
        std::vector<Point> plate_shifts;
        shifts.reserve(po->instances().size());
        plate_shifts.reserve(po->instances().size());
        for (const PrintInstance &inst : po->instances()) {
            shifts.push_back(inst.shift);
            plate_shifts.push_back(inst.shift_without_plate_offset());
        }

        std::vector<SeamTowerBuildInput> inputs;
        auto it = seam_placer.m_seam_per_object.find(po);
        if (it != seam_placer.m_seam_per_object.end())
            collect_inputs_from_object(po, it->second, object_base, inputs);
        append_stacks(inputs, op, accepted, shifts, plate_shifts, po, silhouettes.at(po).local);
        object_base += 1000000;
    }
}

void SeamTowerPlanner::build(const std::vector<SeamTowerBuildInput> &inputs, const SeamTowerParams &params)
{
    m_emits.clear();
    m_warnings.clear();
    m_match_distance = 0;
    ExPolygons accepted;
    append_stacks(inputs, params, accepted, {});
}

std::map<int, ExtrusionEntityCollection> SeamTowerPlanner::layer_collections() const
{
    std::map<int, ExtrusionEntityCollection> out;
    for (const auto &kv : m_emits) {
        ExtrusionEntityCollection &coll = out[kv.first];
        for (const EmitItem &item : kv.second)
            coll.append(item.collection);
    }
    return out;
}

const ExtrusionEntityCollection *SeamTowerPlanner::collection_at_layer(int layer_index) const
{
    auto it = m_emits.find(layer_index);
    if (it == m_emits.end() || it->second.empty())
        return nullptr;
    return &it->second.front().collection;
}

const ExtrusionEntityCollection *SeamTowerPlanner::take_matching_tower(int layer_index,
                                                                       const Point       &loop_start,
                                                                       coord_t            max_distance,
                                                                       const PrintObject *object,
                                                                       const Point       &instance_shift)
{
    auto it = m_emits.find(layer_index);
    if (it == m_emits.end())
        return nullptr;
    for (EmitItem &item : it->second) {
        if (object != nullptr && item.object != object)
            continue;
        if (item.skipped_shifts.count(instance_shift))
            continue;
        const std::pair<const PrintObject *, Point> instance_key{item.object, instance_shift};
        if (item.printed.count(instance_key))
            continue;
        if (!item.active)
            continue;
        if (hop_target_matches(item.hop_target, loop_start, max_distance)) {
            item.printed.insert(instance_key);
            return &item.collection;
        }
    }
    return nullptr;
}

std::vector<const ExtrusionEntityCollection *> SeamTowerPlanner::take_unprinted_towers(
    int layer_index, const PrintObject *object, const Point &instance_shift)
{
    std::vector<const ExtrusionEntityCollection *> out;
    auto it = m_emits.find(layer_index);
    if (it == m_emits.end())
        return out;
    for (EmitItem &item : it->second) {
        if (object != nullptr && item.object != object)
            continue;
        if (item.skipped_shifts.count(instance_shift))
            continue;
        const std::pair<const PrintObject *, Point> instance_key{item.object, instance_shift};
        if (item.printed.count(instance_key))
            continue;
        item.printed.insert(instance_key);
        if (!item.collection.empty())
            out.push_back(&item.collection);
    }
    return out;
}

bool SeamTowerPlanner::has_tower_for(int layer_index, const Point &loop_start, coord_t max_distance, const PrintObject *object,
                                     const Point &instance_shift) const
{
    auto it = m_emits.find(layer_index);
    if (it == m_emits.end())
        return false;
    for (const EmitItem &item : it->second) {
        if (object != nullptr && item.object != object)
            continue;
        if (item.skipped_shifts.count(instance_shift))
            continue;
        if (!item.active)
            continue;
        if (hop_target_matches(item.hop_target, loop_start, max_distance))
            return true;
    }
    return false;
}

bool SeamTowerPlanner::has_tower_for(int layer_index, const ExPolygon &island, const PrintObject *object,
                                     const Point &instance_shift) const
{
    auto it = m_emits.find(layer_index);
    if (it == m_emits.end())
        return false;
    for (const EmitItem &item : it->second) {
        if (object != nullptr && item.object != object)
            continue;
        if (item.skipped_shifts.count(instance_shift))
            continue;
        if (!item.active)
            continue;
        if (island.contains(item.hop_target))
            return true;
    }
    return false;
}

} // namespace Slic3r
