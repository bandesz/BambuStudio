#include "InsetStart.hpp"

#include "libslic3r.h"
#include "ClipperUtils.hpp"
#include "Line.hpp"

#include <cmath>
#include <limits>

namespace Slic3r {

static Vec2d inward_unit_normal(const ExPolygon &island, const Point &p)
{
    double best_dist2 = std::numeric_limits<double>::max();
    Vec2d  normal     = Vec2d::Zero();

    auto consider = [&](const Polygon &poly) {
        for (const Line &line : poly.lines()) {
            Point closest;
            const double d2 = line.distance_to_squared(p, &closest);
            if (d2 > best_dist2)
                continue;
            Vec2d dxy = (line.b - line.a).cast<double>();
            const double len2 = dxy.squaredNorm();
            if (len2 < 1.0)
                continue;
            dxy /= std::sqrt(len2);
            // Left of a directed edge is the ExPolygon interior (CCW contour, CW holes).
            best_dist2 = d2;
            normal     = Vec2d(-dxy.y(), dxy.x());
        }
    };

    consider(island.contour);
    for (const Polygon &hole : island.holes)
        consider(hole);

    return normal;
}

Polyline make_inset_lead_in(
    const ExPolygon &island,
    const Point     &wall_start,
    coord_t          line_width,
    coord_t          preferred_inset)
{
    if (line_width <= 0 || preferred_inset <= 0 || island.empty())
        return {};

    // One-line-thick parts collapse when shrunk by half a line from every boundary.
    if (offset_ex(island, -float(line_width) * 0.5f).empty())
        return {};

    const Vec2d n = inward_unit_normal(island, wall_start);
    if (n.squaredNorm() < 0.25)
        return {};

    const double step       = std::max(double(line_width) / 8.0, scale_(0.05));
    const double max_search = std::max(double(preferred_inset) * 2.0, double(line_width) * 4.0);
    double       last_inside = 0.0;
    for (double t = step; t <= max_search + step; t += step) {
        const Point q = wall_start + Point(coord_t(std::round(n.x() * t)), coord_t(std::round(n.y() * t)));
        if (island.contains(q))
            last_inside = t;
        else
            break;
    }

    const double inset = std::min(double(preferred_inset), last_inside - double(line_width) * 0.5);
    if (inset < double(line_width) * 0.5)
        return {};

    const Point inner = wall_start + Point(coord_t(std::round(n.x() * inset)), coord_t(std::round(n.y() * inset)));
    if (!island.contains(inner) || inner == wall_start)
        return {};

    Polyline lead;
    lead.append(inner);
    lead.append(wall_start);
    return lead;
}

Polygons inset_start_exclusion(const Polyline &lead, coord_t line_width)
{
    if (lead.size() < 2 || line_width <= 0)
        return {};
    return offset(lead, float(line_width) * 0.5f + float(SCALED_EPSILON));
}

Polylines clip_against_inset_start(const Polyline &subject, const Polygons &exclusion)
{
    if (subject.size() < 2)
        return {};
    if (exclusion.empty())
        return {subject};
    if (intersection_pl(subject, exclusion).empty())
        return {subject};
    return diff_pl(subject, exclusion);
}

} // namespace Slic3r
