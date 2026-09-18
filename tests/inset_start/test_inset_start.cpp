#include <catch2/catch.hpp>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/InsetStart.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;

static Point mm(double x, double y) { return Point::new_scale(x, y); }

static ExPolygon square_mm(double size)
{
    const coord_t s = scale_(size);
    return ExPolygon{Polygon{Points{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)}}};
}

static ExPolygon ring_mm(double outer, double inner)
{
    ExPolygon island = square_mm(outer);
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

TEST_CASE("Inset lead-in starts inside a thick square and ends at the wall", "[InsetStart]")
{
    const coord_t line_width = scale_(0.4);
    const coord_t preferred  = scale_(1.0);
    const ExPolygon island   = square_mm(20.0);
    const Point wall_start   = mm(10.0, 0.0); // midpoint of bottom edge

    const Polyline lead = make_inset_lead_in(island, wall_start, line_width, preferred);

    REQUIRE(lead.size() >= 2);
    REQUIRE(lead.last_point() == wall_start);
    REQUIRE(island.contains(lead.first_point()));
    const double inset = unscale<double>((lead.first_point() - wall_start).cast<double>().norm());
    REQUIRE(inset == Approx(1.0).margin(0.15));
    REQUIRE(lead.first_point().y() > wall_start.y());
}

TEST_CASE("Inset lead-in is skipped when the part is one line thick", "[InsetStart]")
{
    const coord_t line_width = scale_(0.4);
    ExPolygon island         = square_mm(20.0);
    // 0.4mm-tall strip: one line of 0.4mm
    island.contour = Polygon{Points{mm(0, 0), mm(20, 0), mm(20, 0.4), mm(0, 0.4)}};
    const Point wall_start = mm(10.0, 0.2);

    const Polyline lead = make_inset_lead_in(island, wall_start, line_width, scale_(1.0));

    REQUIRE(lead.empty());
}

TEST_CASE("Inset lead-in is used on a two-line-thick ring", "[InsetStart]")
{
    const coord_t line_width = scale_(0.4);
    // 0.8mm ring around a 20mm square → two 0.4mm lines
    const ExPolygon island = ring_mm(20.0, 18.4);
    const Point wall_start = mm(10.0, 0.0);

    const Polyline lead = make_inset_lead_in(island, wall_start, line_width, scale_(1.0));

    REQUIRE_FALSE(lead.empty());
    REQUIRE(lead.last_point() == wall_start);
    REQUIRE(island.contains(lead.first_point()));
}

TEST_CASE("Inset lead-in is skipped on a one-line-thick ring", "[InsetStart]")
{
    const coord_t line_width = scale_(0.4);
    const ExPolygon island   = ring_mm(20.0, 19.2); // 0.4mm ring
    const Point wall_start   = mm(10.0, 0.0);

    const Polyline lead = make_inset_lead_in(island, wall_start, line_width, scale_(1.0));

    REQUIRE(lead.empty());
}

TEST_CASE("Infill that crosses the lead-in is clipped so it does not overprint", "[InsetStart]")
{
    const coord_t line_width = scale_(0.4);
    Polyline lead;
    lead.append(mm(10.0, 1.0));
    lead.append(mm(10.0, 0.0));

    const Polygons exclusion = inset_start_exclusion(lead, line_width);
    REQUIRE_FALSE(exclusion.empty());

    Polyline infill;
    infill.append(mm(0.0, 0.5));
    infill.append(mm(20.0, 0.5));

    const Polylines clipped = clip_against_inset_start(infill, exclusion);
    REQUIRE_FALSE(clipped.empty());

    double remaining = 0;
    for (const Polyline &pl : clipped)
        remaining += unscale<double>(pl.length());
    REQUIRE(remaining < 20.0 - 0.2);

    const Point stub_mid = mm(10.0, 0.5);
    for (const Polyline &pl : clipped) {
        for (const Line &line : pl.lines())
            REQUIRE(line.distance_to(stub_mid) > double(line_width) * 0.25);
    }
}

TEST_CASE("Infill that misses the lead-in is left intact", "[InsetStart]")
{
    const coord_t line_width = scale_(0.4);
    Polyline lead;
    lead.append(mm(10.0, 1.0));
    lead.append(mm(10.0, 0.0));
    const Polygons exclusion = inset_start_exclusion(lead, line_width);

    Polyline infill;
    infill.append(mm(0.0, 5.0));
    infill.append(mm(20.0, 5.0));

    const Polylines clipped = clip_against_inset_start(infill, exclusion);
    REQUIRE(clipped.size() == 1);
    REQUIRE(clipped.front().first_point() == infill.first_point());
    REQUIRE(clipped.front().last_point() == infill.last_point());
    REQUIRE(clipped.front().length() == Approx(double(infill.length())));
}
