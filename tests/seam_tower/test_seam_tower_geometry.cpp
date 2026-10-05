#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/SeamTower.hpp"

#include <cmath>
#include <limits>

using namespace Slic3r;

static Point mm(double x, double y) { return Point::new_scale(x, y); }

static Polygon square_contour_mm(double size)
{
    const coord_t s = scale_(size);
    return Polygon{Points{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)}};
}

static Vec2d wall_tangent(const Polygon &contour, const Point &seam)
{
    double best = std::numeric_limits<double>::max();
    Vec2d  t    = Vec2d::Zero();
    for (const Line &line : contour.lines()) {
        Point closest;
        const double d2 = line.distance_to_squared(seam, &closest);
        if (d2 > best)
            continue;
        Vec2d dxy = (line.b - line.a).cast<double>();
        const double len = dxy.norm();
        if (len < 1.0)
            continue;
        best = d2;
        t    = dxy / len;
    }
    return t;
}

static double along_contour_mm(const Polygon &contour, const Point &seam, const ExPolygon &island)
{
    const Vec2d tangent = wall_tangent(contour, seam);
    REQUIRE(tangent.squaredNorm() > 0.25);
    double tmin = std::numeric_limits<double>::max();
    double tmax = std::numeric_limits<double>::lowest();
    for (const Point &p : island.contour.points) {
        const double t = (p - seam).cast<double>().dot(tangent);
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }
    return unscale<double>(tmax - tmin);
}

TEST_CASE("Seam tower island is a closed offset strip outside a square", "[SeamTower]")
{
    const coord_t gap        = scale_(0.1);
    const coord_t depth      = scale_(2.0);
    const double  line_width = 0.4;
    const Polygon contour    = square_contour_mm(20.0);
    const Point   seam       = mm(10.0, 0.0);

    const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth);

    REQUIRE_FALSE(island.empty());
    REQUIRE(island.contour.is_valid());
    REQUIRE(island.contour.is_counter_clockwise());
    REQUIRE(island.contour.area() > 0);

    const Point  inner_pt  = island.contour.point_projection(seam);
    const double inner_gap = unscale<double>((inner_pt - seam).cast<double>().norm());
    REQUIRE(inner_gap == Approx(0.1).margin(0.05));

    double max_from_wall = 0;
    for (const Point &p : island.contour.points) {
        const Point proj = contour.point_projection(p);
        max_from_wall = std::max(max_from_wall, unscale<double>((p - proj).cast<double>().norm()));
    }
    const double thickness = max_from_wall - inner_gap;
    REQUIRE(thickness == Approx(2.0).margin(line_width));

    const double along = along_contour_mm(contour, seam, island);
    REQUIRE(along >= 2.0);
}

TEST_CASE("Non-zero stack_window lengthens the island along the contour", "[SeamTower]")
{
    const coord_t gap     = scale_(0.1);
    const coord_t depth   = scale_(2.0);
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);

    const ExPolygon island0 = make_seam_tower_island(contour, seam, gap, depth, scale_(4), 0);
    const ExPolygon island6 = make_seam_tower_island(contour, seam, gap, depth, scale_(4), scale_(6));

    REQUIRE_FALSE(island0.empty());
    REQUIRE_FALSE(island6.empty());
    const double along0 = along_contour_mm(contour, seam, island0);
    const double along6 = along_contour_mm(contour, seam, island6);
    REQUIRE(along0 >= 2.0);
    REQUIRE(along6 >= 2.0);
    REQUIRE(along6 > along0);
}

static double radial_thickness_mm(const Polygon &contour, const Point &seam, const ExPolygon &island)
{
    const Point  inner_pt  = island.contour.point_projection(seam);
    const double inner_gap = unscale<double>((inner_pt - seam).cast<double>().norm());
    double       max_from_wall = 0;
    for (const Point &p : island.contour.points) {
        const Point proj = contour.point_projection(p);
        max_from_wall = std::max(max_from_wall, unscale<double>((p - proj).cast<double>().norm()));
    }
    return max_from_wall - inner_gap;
}

TEST_CASE("Seam tower depth 8 mm and length 4 mm is radial not along-wall", "[SeamTower]")
{
    const coord_t gap        = scale_(0.1);
    const coord_t depth      = scale_(8.0);
    const coord_t length     = scale_(4.0);
    const double  line_width = 0.4;
    const Polygon contour    = square_contour_mm(20.0);
    const Point   seam       = mm(10.0, 0.0);

    const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth, length);

    REQUIRE_FALSE(island.empty());
    REQUIRE(radial_thickness_mm(contour, seam, island) == Approx(8.0).margin(line_width));
    REQUIRE(along_contour_mm(contour, seam, island) == Approx(4.0).margin(line_width));
}

TEST_CASE("Seam tower length 10 mm and depth 2 mm is along-wall not radial", "[SeamTower]")
{
    const coord_t gap        = scale_(0.1);
    const coord_t depth      = scale_(2.0);
    const coord_t length     = scale_(10.0);
    const double  line_width = 0.4;
    const Polygon contour    = square_contour_mm(20.0);
    const Point   seam       = mm(10.0, 0.0);

    const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth, length);

    REQUIRE_FALSE(island.empty());
    REQUIRE(radial_thickness_mm(contour, seam, island) == Approx(2.0).margin(line_width));
    REQUIRE(along_contour_mm(contour, seam, island) == Approx(10.0).margin(line_width));
}

static double dist_to_contour_mm(const Polygon &contour, const Point &p)
{
    const Point proj = contour.point_projection(p);
    return unscale<double>((p - proj).cast<double>().norm());
}

// Proper crossing of non-adjacent edges: a bow-tie's diagonals meet in their interiors.
static bool contour_self_intersects(const Polygon &poly)
{
    const Lines ls = poly.lines();
    const size_t n = ls.size();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j + 1 == n)
                continue;
            Point ip;
            if (!ls[i].intersection(ls[j], &ip))
                continue;
            const bool on_end_i = ip == ls[i].a || ip == ls[i].b;
            const bool on_end_j = ip == ls[j].a || ip == ls[j].b;
            if (!on_end_i && !on_end_j)
                return true;
        }
    }
    return false;
}

static int count_radial_caps(const Polygon &contour, const Polygon &island, double gap_mm, double depth_mm)
{
    int caps = 0;
    const double inner = gap_mm;
    const double outer = gap_mm + depth_mm;
    for (const Line &line : island.lines()) {
        const double da = dist_to_contour_mm(contour, line.a);
        const double db = dist_to_contour_mm(contour, line.b);
        const bool a_inner = std::abs(da - inner) < 0.08;
        const bool b_inner = std::abs(db - inner) < 0.08;
        const bool a_outer = std::abs(da - outer) < 0.15;
        const bool b_outer = std::abs(db - outer) < 0.15;
        if (!((a_inner && b_outer) || (a_outer && b_inner)))
            continue;
        const double len = unscale<double>(line.length());
        if (len > depth_mm * 0.5 && len < depth_mm * 1.5)
            ++caps;
    }
    return caps;
}

static void require_simple_or_empty(const ExPolygon &island)
{
    if (island.empty())
        return;
    REQUIRE(island.holes.empty());
    REQUIRE(island.contour.is_valid());
    REQUIRE(island.contour.area() > 0);
    REQUIRE_FALSE(contour_self_intersects(island.contour));
    const ExPolygons unified = union_ex(Polygons{island.contour});
    REQUIRE(unified.size() == 1);
    REQUIRE(unified.front().holes.empty());
}

TEST_CASE("Seam tower caps join inner and outer arcs into one closed loop", "[SeamTower]")
{
    const coord_t gap   = scale_(0.1);
    const coord_t depth = scale_(2.0);

    SECTION("square") {
        const Polygon contour = square_contour_mm(20.0);
        const Point   seam    = mm(10.0, 0.0);
        const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth);

        REQUIRE_FALSE(island.empty());
        REQUIRE(island.holes.empty());
        REQUIRE(island.contour.is_valid());
        REQUIRE(island.contour.area() > 0);
        REQUIRE(island.contour.lines().size() == island.contour.points.size());
        REQUIRE(count_radial_caps(contour, island.contour, 0.1, 2.0) == 2);
    }

    SECTION("triangle with non-grid vertices") {
        const Polygon contour{Points{mm(0, 0), mm(20.3, 0.7), mm(4.1, 18.9)}};
        const Point   seam = mm(10.15, 0.35);
        const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth);

        REQUIRE_FALSE(island.empty());
        REQUIRE(island.holes.empty());
        REQUIRE(island.contour.is_valid());
        REQUIRE(island.contour.area() > 0);
        REQUIRE(island.contour.lines().size() == island.contour.points.size());
        REQUIRE_FALSE(contour_self_intersects(island.contour));
        REQUIRE(count_radial_caps(contour, island.contour, 0.1, 2.0) == 2);
    }
}

static ExPolygon ring_mm(double outer, double inner)
{
    const coord_t s = scale_(outer);
    ExPolygon island{Polygon{Points{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)}}};
    const coord_t o = scale_((outer - inner) / 2.0);
    const coord_t i = scale_(inner);
    island.holes.emplace_back(Points{
        Point(o, o),
        Point(o, o + i),
        Point(o + i, o + i),
        Point(o + i, o)
    }); // CW hole
    return island;
}

static Point hole_bottom_seam(const Polygon &hole)
{
    return Point((hole.points.front().x() + hole.points.back().x()) / 2, hole.points.front().y());
}

TEST_CASE("Seam tower island sits inside a hole when min_size remains after gap and depth", "[SeamTower]")
{
    const coord_t gap      = scale_(0.1);
    const coord_t depth    = scale_(2.0);
    const coord_t min_size = scale_(3.0);
    const ExPolygon ring   = ring_mm(40.0, 20.0);
    const Polygon  &hole   = ring.holes.front();
    const Point     seam   = hole_bottom_seam(hole);

    REQUIRE(hole.is_clockwise());
    const ExPolygon island = make_seam_tower_island(hole, seam, gap, depth, scale_(4), 0, min_size);

    REQUIRE_FALSE(island.empty());
    REQUIRE(island.holes.empty());
    REQUIRE(island.contour.is_counter_clockwise());

    for (const Point &p : island.contour.points) {
        REQUIRE(hole.contains(p));
        REQUIRE_FALSE(ring.contains(p));
    }

    const Point  inner_pt  = island.contour.point_projection(seam);
    const double inner_gap = unscale<double>((inner_pt - seam).cast<double>().norm());
    REQUIRE(inner_gap == Approx(0.1).margin(0.05));
    REQUIRE(inner_pt.y() > seam.y());
}

TEST_CASE("Seam tower is skipped when a hole is smaller than min_size", "[SeamTower]")
{
    const coord_t gap      = scale_(0.1);
    const coord_t depth    = scale_(2.0);
    const coord_t min_size = scale_(10.0);
    const ExPolygon ring   = ring_mm(20.0, 8.0);
    const Polygon  &hole   = ring.holes.front();
    const Point     seam   = hole_bottom_seam(hole);

    REQUIRE(hole.is_clockwise());
    const ExPolygon island = make_seam_tower_island(hole, seam, gap, depth, scale_(4), 0, min_size);

    REQUIRE(island.empty());
}

TEST_CASE("Seam tower on a contour shorter than depth is simple or empty, never a bow-tie", "[SeamTower]")
{
    const coord_t gap   = scale_(0.1);
    const coord_t depth = scale_(2.0);

    SECTION("square whose perimeter is shorter than depth") {
        const Polygon contour = square_contour_mm(0.4);
        const Point   seam    = mm(0.2, 0.0);
        require_simple_or_empty(make_seam_tower_island(contour, seam, gap, depth));
    }

    SECTION("thin rectangle shorter than depth") {
        const coord_t w = scale_(0.8);
        const coord_t h = scale_(0.1);
        const Polygon contour{Points{Point(0, 0), Point(w, 0), Point(w, h), Point(0, h)}};
        const Point   seam = Point(w / 2, 0);
        require_simple_or_empty(make_seam_tower_island(contour, seam, gap, depth));
    }

    SECTION("tiny loop") {
        const Polygon contour = square_contour_mm(0.05);
        const Point   seam    = mm(0.025, 0.0);
        require_simple_or_empty(make_seam_tower_island(contour, seam, gap, depth));
    }

    SECTION("short triangle") {
        const Polygon contour{Points{mm(0, 0), mm(0.3, 0.05), mm(0.12, 0.28)}};
        const Point   seam = mm(0.15, 0.025);
        require_simple_or_empty(make_seam_tower_island(contour, seam, gap, depth));
    }
}

static Polygon square_from_to_mm(double x0, double y0, double x1, double y1)
{
    return Polygon{Points{mm(x0, y0), mm(x1, y0), mm(x1, y1), mm(x0, y1)}};
}

static double area_mm2(const ExPolygon &ex)
{
    return unscale<double>(unscale<double>(std::abs(ex.area())));
}

static double area_mm2(const ExPolygons &exs)
{
    double a = 0;
    for (const ExPolygon &ex : exs)
        a += area_mm2(ex);
    return a;
}

TEST_CASE("Seam tower on a receding contour stays inside the previous layer", "[SeamTower]")
{
    const coord_t gap        = scale_(0.1);
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const Polygon contour0   = square_contour_mm(20.0);
    const Point   seam0      = mm(10.0, 0.0);
    const ExPolygon t0       = make_seam_tower_island(contour0, seam0, gap, depth);

    REQUIRE_FALSE(t0.empty());

    const Polygon contour1 = square_from_to_mm(2.0, 2.0, 18.0, 18.0);
    const Point   seam1    = mm(10.0, 2.0);
    const ExPolygon island = make_seam_tower_island(contour1, seam1, gap, depth, scale_(4), 0, 0, &t0, line_width);

    REQUIRE_FALSE(island.empty());
    REQUIRE(diff_ex(island, t0).empty());
    for (const Point &p : island.contour.points)
        REQUIRE(t0.contains(p));
}

TEST_CASE("Seam tower replaces an overhanging strip with a rectangle clipped to the previous footprint", "[SeamTower]")
{
    const coord_t gap        = scale_(0.1);
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const Polygon contour0   = square_contour_mm(20.0);
    const Point   seam0      = mm(10.0, 0.0);
    const ExPolygon t0       = make_seam_tower_island(contour0, seam0, gap, depth);

    REQUIRE_FALSE(t0.empty());

    const Polygon contour1 = square_from_to_mm(2.0, 2.0, 18.0, 18.0);
    const Point   seam1    = mm(10.0, 2.0);
    const ExPolygon strip  = make_seam_tower_island(contour1, seam1, gap, depth);
    const ExPolygon island = make_seam_tower_island(contour1, seam1, gap, depth, scale_(4), 0, 0, &t0, line_width);

    REQUIRE_FALSE(strip.empty());
    REQUIRE_FALSE(diff_ex(strip, t0).empty());
    REQUIRE_FALSE(island.empty());
    REQUIRE(diff_ex(island, t0).empty());

    const ExPolygons clipped_only = intersection_ex(strip, t0);
    REQUIRE(area_mm2(island) > area_mm2(clipped_only) + 0.5);
    REQUIRE_FALSE(diff_ex(island, strip).empty());
}

TEST_CASE("Seam tower is skipped when the previous footprint is thinner than one line width", "[SeamTower]")
{
    const coord_t gap        = scale_(0.1);
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const Polygon contour    = square_contour_mm(20.0);
    const Point   seam       = mm(10.0, 0.0);

    SECTION("sliver previous") {
        const ExPolygon sliver{Polygon{Points{
            mm(9.0, -0.3), mm(11.0, -0.3), mm(11.0, 0.0), mm(9.0, 0.0)}}};
        REQUIRE(area_mm2(sliver) > 0);
        const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth, scale_(4), 0, 0, &sliver, line_width);
        REQUIRE(island.empty());
    }

    SECTION("previous area below one line width") {
        const ExPolygon tiny{Polygon{Points{
            mm(10.0, 0.0), mm(10.1, 0.0), mm(10.1, 0.1), mm(10.0, 0.1)}}};
        const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth, scale_(4), 0, 0, &tiny, line_width);
        REQUIRE(island.empty());
    }
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

TEST_CASE("Seam tower island is a closed offset strip outside a hexagon", "[SeamTower]")
{
    const coord_t gap   = scale_(0.1);
    const coord_t depth = scale_(2.0);

    SECTION("20 mm across, seam at a vertex") {
        const Polygon   contour = hexagon_contour_mm(10.0);
        const Point     seam    = contour.points.front();
        const ExPolygon island  = make_seam_tower_island(contour, seam, gap, depth);
        REQUIRE_FALSE(island.empty());
        REQUIRE(island.holes.empty());
        REQUIRE_FALSE(contour_self_intersects(island.contour));
    }

    SECTION("20 mm across, seam at mid-edge") {
        const Polygon   contour = hexagon_contour_mm(10.0);
        const Point     seam    = (contour.points[0] + contour.points[1]) / 2;
        const ExPolygon island  = make_seam_tower_island(contour, seam, gap, depth);
        REQUIRE_FALSE(island.empty());
        REQUIRE(island.holes.empty());
        REQUIRE_FALSE(contour_self_intersects(island.contour));
    }

    SECTION("small 8 mm across, seam at a vertex") {
        const Polygon   contour = hexagon_contour_mm(4.0);
        const Point     seam    = contour.points.front();
        const ExPolygon island  = make_seam_tower_island(contour, seam, gap, depth);
        REQUIRE_FALSE(island.empty());
        REQUIRE(island.holes.empty());
        REQUIRE_FALSE(contour_self_intersects(island.contour));
    }
}

// A 120° hole corner offset inward by gap+depth miters back further than
// length/2. The Hexagon Small Top hole seam sits on that corner (gap 1 mm,
// depth 4 mm, length 5 mm). The strip still has to be built into the hole.
TEST_CASE("Seam tower island is built inside a hexagon hole when the seam is on a vertex", "[SeamTower]")
{
    Polygon hole = hexagon_contour_mm(38.0);
    hole.make_clockwise();
    const Point seam = hole.points.front();
    const ExPolygon island = make_seam_tower_island(
        hole, seam, scale_(1.0), scale_(4.0), scale_(5.0), 0, scale_(3.0), nullptr, scale_(0.42));

    REQUIRE(hole.is_clockwise());
    REQUIRE_FALSE(island.empty());
    REQUIRE(island.holes.empty());
    REQUIRE(island.contour.is_counter_clockwise());
    REQUIRE_FALSE(contour_self_intersects(island.contour));
    for (const Point &p : island.contour.points)
        REQUIRE(hole.contains(p));
}

static double nearest_to_contour_mm(const ExPolygon &island, const Polygon &contour)
{
    double nearest = std::numeric_limits<double>::max();
    for (const Point &p : island.contour.points) {
        const double d = unscale<double>((contour.point_projection(p) - p).cast<double>().norm());
        nearest = std::min(nearest, d);
    }
    return nearest;
}

// The inner wall steps into the hole by 0.5 mm, the same 45° overhang as the
// first layers of the hexagon top. A 1 mm gap must not be closed by pinning
// the pad to the previous footprint.
TEST_CASE("Hole seam tower keeps a 1 mm gap when the inner wall overhangs", "[SeamTower]")
{
    const coord_t gap        = scale_(1.0);
    const coord_t depth      = scale_(4.0);
    const coord_t length     = scale_(5.0);
    const coord_t line_width = scale_(0.42);

    Polygon hole0 = square_contour_mm(20.0);
    hole0.make_clockwise();
    const ExPolygon t0 = make_seam_tower_island(
        hole0, mm(10.0, 0.0), gap, depth, length, 0, 0, nullptr, line_width);
    REQUIRE_FALSE(t0.empty());

    Polygon hole1{Points{mm(0.0, 0.5), mm(20.0, 0.5), mm(20.0, 20.0), mm(0.0, 20.0)}};
    hole1.make_clockwise();
    const ExPolygon island = make_seam_tower_island(
        hole1, mm(10.0, 0.5), gap, depth, length, 0, 0, &t0, line_width);

    REQUIRE_FALSE(island.empty());
    REQUIRE(nearest_to_contour_mm(island, hole1) == Approx(1.0).margin(0.15));
    REQUIRE_FALSE(intersection_ex(island, t0).empty());
}

TEST_CASE("Seam tower is skipped when first-layer depth is too thin for one line width", "[SeamTower]")
{
    const coord_t gap        = scale_(0.1);
    const coord_t line_width = scale_(0.4);
    const Polygon contour    = square_contour_mm(20.0);
    const Point   seam       = mm(10.0, 0.0);

    SECTION("depth zero") {
        const ExPolygon island = make_seam_tower_island(contour, seam, gap, 0);
        REQUIRE(island.empty());
    }

    SECTION("depth thinner than one line width") {
        const coord_t   depth  = scale_(0.3);
        const ExPolygon island = make_seam_tower_island(contour, seam, gap, depth, scale_(4), 0, 0, nullptr, line_width);
        REQUIRE(island.empty());
    }
}
