#include <catch2/catch.hpp>

#include <fstream>
#include <limits>
#include <string>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Fill/Fill.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Surface.hpp"

using namespace Slic3r;

namespace {

// Support interface fill: solid concentric, spacing left alone (dont_adjust).
Polylines fill_solid_concentric(const ExPolygon &shape, double spacing)
{
    std::unique_ptr<Fill> filler(Fill::new_from_type(ipConcentric));
    filler->spacing = spacing;
    filler->loop_clipping = 0;
    FillParams params;
    params.density = 1.f;
    params.dont_adjust = true;
    Surface surface(stInternal, shape);
    return filler->fill_surface(&surface, params);
}

double polyline_length_mm(const Polyline &path)
{
    double length = 0.;
    for (size_t i = 1; i < path.points.size(); ++i)
        length += unscale<double>(Line(path.points[i - 1], path.points[i]).length());
    return length;
}

double distance_to_paths(const Polylines &paths, const Point &point)
{
    double best = std::numeric_limits<double>::infinity();
    for (const Polyline &path : paths) {
        for (size_t i = 1; i < path.points.size(); ++i)
            best = std::min(best, Line(path.points[i - 1], path.points[i]).distance_to(point));
    }
    return best;
}

// Largest distance, in mm, from a point of `shape` inset by `inset_mm` to the
// nearest fill centerline. A solid fill with the given spacing covers such a
// point when this stays within half a spacing.
double widest_interior_gap_mm(const ExPolygon &shape, const Polylines &paths, double inset_mm, double step_mm)
{
    double worst = 0.;
    const coord_t step = std::max(coord_t(1), coord_t(scale_(step_mm)));
    for (const ExPolygon &island : offset_ex(shape, -float(scale_(inset_mm)))) {
        const BoundingBox bbox = get_extents(island);
        for (coord_t x = bbox.min.x(); x <= bbox.max.x(); x += step) {
            for (coord_t y = bbox.min.y(); y <= bbox.max.y(); y += step) {
                const Point point(x, y);
                if (!island.contains(point))
                    continue;
                worst = std::max(worst, distance_to_paths(paths, point));
            }
        }
    }
    return unscale<double>(worst);
}

ExPolygon square_mm(double width, double height)
{
    const double hx = width / 2.;
    const double hy = height / 2.;
    return ExPolygon({
        Point::new_scale(-hx, -hy),
        Point::new_scale( hx, -hy),
        Point::new_scale( hx,  hy),
        Point::new_scale(-hx,  hy),
    });
}

// Contour and hole of the support-interface frame from Problems.3mf, in mm,
// already centered. Written as the centerlines the slicer emitted, which are
// the polygon after Fill's half-spacing inset.
ExPolygon tray_rim_from_fixture()
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "/fill_concentric/tray_rim_rings.txt");
    Points outer, inner;
    Points *cur = nullptr;
    std::string tag;
    while (in >> tag) {
        if (tag == "OUTER") { cur = &outer; continue; }
        if (tag == "INNER") { cur = &inner; continue; }
        const double x = std::stod(tag);
        double y = 0.;
        in >> y;
        cur->push_back(Point::new_scale(x, y));
    }
    if (Polygon::area(outer) < 0)
        std::reverse(outer.begin(), outer.end());
    if (Polygon::area(inner) > 0)
        std::reverse(inner.begin(), inner.end());
    return ExPolygon(Polygon(std::move(outer)), Polygon(std::move(inner)));
}

} // namespace

// Support calls Fill::fill_surface with density 1 and dont_adjust, so concentric
// takes the fixed-spacing loop path. The loop step refuses a ring once the
// remainder is narrower than one spacing plus half a bead, which leaves the
// middle of the island unfilled. Rectilinear does not do this.
TEST_CASE("Solid concentric fill covers the center of an island", "[FillConcentric]")
{
    const double spacing = 0.45;
    // Half the centerline spacing, plus a small margin for Clipper rounding.
    // A real hole left by the old loop step is several times larger.
    const double max_gap = spacing / 2. + 0.05;

    SECTION("circle whose radius stops the last loop short of the center") {
        // After the half-spacing inset Fill applies, twenty steps of `spacing`
        // land the innermost loop just inside the distance that would grow
        // another ring, about 0.67mm from the center.
        const double radius = 9.895;
        const ExPolygon disk(make_circle(scale_(radius), scale_(0.01)));
        const Polylines paths = fill_solid_concentric(disk, spacing);
        REQUIRE_FALSE(paths.empty());
        const double center_gap = unscale<double>(distance_to_paths(paths, Point::new_scale(0., 0.)));
        INFO("center gap " << center_gap << " mm");
        REQUIRE(center_gap <= max_gap);
    }

    SECTION("square") {
        const ExPolygon shape = square_mm(20., 20.);
        const Polylines paths = fill_solid_concentric(shape, spacing);
        const double gap = widest_interior_gap_mm(shape, paths, spacing, 0.35);
        INFO("widest interior gap " << gap << " mm");
        REQUIRE(gap <= max_gap);
    }

    SECTION("rectangle") {
        const ExPolygon shape = square_mm(18., 7.3);
        const Polylines paths = fill_solid_concentric(shape, spacing);
        const double gap = widest_interior_gap_mm(shape, paths, spacing, 0.35);
        INFO("widest interior gap " << gap << " mm");
        REQUIRE(gap <= max_gap);
    }

    SECTION("tray rim support keeps the middle of each flat covered") {
        // Problems.3mf support interface is this rounded hexagonal frame.
        // The flats are narrower than the corners, so a morphological open
        // deletes the flats and leaves a gap wider than the bead. The recorded
        // loops are the centerlines after Fill's half-spacing inset; grow
        // them back so fill_surface feeds the filler the same frame.
        const double sp = 0.377;
        const ExPolygon recorded = tray_rim_from_fixture();
        REQUIRE(recorded.contour.size() > 3);
        REQUIRE_FALSE(recorded.holes.empty());
        const ExPolygons grown = offset_ex(recorded, float(scale_(sp / 2.)));
        REQUIRE_FALSE(grown.empty());
        const Polylines paths = fill_solid_concentric(grown.front(), sp);
        const double gap = widest_interior_gap_mm(grown.front(), paths, sp / 2., 0.2);
        INFO("widest ring gap " << gap << " mm");
        REQUIRE(gap <= sp / 2. + 0.05);
        // The middle of the frame has to be one more closed ring, the same
        // kind of loop as the rings around it. A separate outline on each
        // flat is shorter than the frame perimeter.
        Polylines rings;
        for (const Polyline &path : paths)
            if (polyline_length_mm(path) > 150.)
                rings.push_back(path);
        const double ring_gap = widest_interior_gap_mm(grown.front(), rings, sp / 2., 0.2);
        INFO("gap left by the long rings " << ring_gap << " mm, long rings " << rings.size());
        REQUIRE(ring_gap <= sp / 2. + 0.05);
        // One wall, one spacing. Stepping in from both edges and then packing
        // the remainder in the middle makes a tight group between two normal groups.
        const BoundingBox box = get_extents(rings.front());
        const Point center((box.min.x() + box.max.x()) / 2, (box.min.y() + box.max.y()) / 2);
        std::vector<double> radii;
        radii.reserve(rings.size());
        for (const Polyline &path : rings) {
            double sum = 0.;
            for (const Point &p : path.points)
                sum += (p - center).cast<double>().norm();
            radii.push_back(unscale<double>(sum / double(path.size())));
        }
        std::sort(radii.begin(), radii.end());
        double min_gap = std::numeric_limits<double>::infinity();
        double max_gap = 0.;
        for (size_t i = 1; i < radii.size(); ++i) {
            const double d = radii[i] - radii[i - 1];
            min_gap = std::min(min_gap, d);
            max_gap = std::max(max_gap, d);
        }
        INFO("ring spacing " << min_gap << " .. " << max_gap << " mm");
        REQUIRE(max_gap <= min_gap * 1.25);
    }

    SECTION("hole in the island stays empty") {
        Points hole {
            Point::new_scale(-3., -3.),
            Point::new_scale(-3.,  3.),
            Point::new_scale( 3.,  3.),
            Point::new_scale( 3., -3.),
        };
        std::reverse(hole.begin(), hole.end());
        const ExPolygon shape(square_mm(20., 20.).contour, Polygon(std::move(hole)));
        const Polylines paths = fill_solid_concentric(shape, spacing);
        const double band_gap = widest_interior_gap_mm(shape, paths, spacing, 0.4);
        INFO("widest band gap " << band_gap << " mm");
        REQUIRE(band_gap <= max_gap);
        const double hole_gap = unscale<double>(distance_to_paths(paths, Point::new_scale(0., 0.)));
        INFO("hole center gap " << hole_gap << " mm");
        REQUIRE(hole_gap > 1.);
    }
}
