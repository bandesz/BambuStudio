#include "SeamTower.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Line.hpp"
#include "Polyline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

static size_t insert_on_contour(Polygon &poly, const Point &p)
{
    for (size_t i = 0; i < poly.points.size(); ++i)
        if (poly.points[i] == p)
            return i;

    double best  = std::numeric_limits<double>::max();
    size_t best_i = 0;
    Point  best_pt = p;
    const size_t n = poly.points.size();
    for (size_t i = 0; i < n; ++i) {
        Line line(poly.points[i], poly.points[(i + 1) % n]);
        Point closest;
        const double d2 = line.distance_to_squared(p, &closest);
        if (d2 >= best)
            continue;
        best    = d2;
        best_i  = i;
        best_pt = closest;
    }

    if (best_pt == poly.points[best_i])
        return best_i;
    const size_t next = (best_i + 1) % n;
    if (best_pt == poly.points[next])
        return next;
    poly.points.insert(poly.points.begin() + ptrdiff_t(best_i + 1), best_pt);
    return best_i + 1;
}

static Polyline contour_arc_around(const Polygon &contour, const Point &seam, double half_span)
{
    Polygon poly = contour;
    const Point on_contour = poly.point_projection(seam);
    const size_t idx = insert_on_contour(poly, on_contour);
    Polyline loop = poly.split_at_index(int(idx));

    // Keep a visible gap so a too-short contour stays a simple C-shape, not a ring/hole.
    const double keep = std::max(2.0, double(scale_(0.05)));
    const double loop_len = loop.length();
    double span = half_span;
    if (2.0 * span + keep > loop_len)
        span = 0.5 * (loop_len - keep);
    if (span <= 1.0)
        return {};

    Polyline fwd = loop;
    const double extra_fwd = fwd.length() - span;
    if (extra_fwd > 1.0)
        fwd.clip_end(extra_fwd - 1.0);

    Polyline bwd = loop;
    bwd.reverse();
    const double extra_bwd = bwd.length() - span;
    if (extra_bwd > 1.0)
        bwd.clip_end(extra_bwd - 1.0);
    bwd.reverse();

    Polyline arc = std::move(bwd);
    if (fwd.size() >= 2)
        arc.append(Points(fwd.points.begin() + 1, fwd.points.end()));
    return arc;
}

static const Polygon &nearest_polygon(const Polygons &polys, const Point &p)
{
    size_t best = 0;
    double best_d = std::numeric_limits<double>::max();
    for (size_t i = 0; i < polys.size(); ++i) {
        const Point proj = polys[i].point_projection(p);
        const double d   = (proj - p).cast<double>().squaredNorm();
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    return polys[best];
}

static Polyline walk_until(const Polygon &poly, const Point &from, const Point &to)
{
    Polygon work = poly;
    size_t ia = insert_on_contour(work, work.point_projection(from));
    const size_t n1 = work.points.size();
    size_t ib = insert_on_contour(work, work.point_projection(to));
    if (work.points.size() != n1 && ib <= ia)
        ++ia;
    if (ia == ib)
        return {};

    const Point end_pt = work.points[ib];
    Polyline loop = work.split_at_index(int(ia));
    Polyline out;
    out.append(loop.points.front());
    for (size_t i = 1; i < loop.points.size(); ++i) {
        out.append(loop.points[i]);
        if (loop.points[i] == end_pt)
            return out;
    }
    return {};
}

static bool narrower_than_one_line_width(const ExPolygon &ex, coord_t line_width)
{
    if (ex.empty())
        return true;
    if (line_width <= 0)
        return false;
    return offset(ex, -0.5f * float(line_width)).empty();
}

static ExPolygon largest_thick_enough(const ExPolygons &parts, coord_t line_width)
{
    ExPolygon best;
    double    best_area = 0;
    for (const ExPolygon &ex : parts) {
        if (narrower_than_one_line_width(ex, line_width))
            continue;
        const double a = ex.area();
        if (a > best_area) {
            best_area = a;
            best      = ex;
        }
    }
    return best;
}

static ExPolygon fallback_rectangle(const ExPolygon &support, coord_t depth, coord_t along)
{
    const BoundingBox bb     = get_extents(support);
    const Point       center = bb.center();
    const Point       sz     = bb.size();
    const coord_t     hx     = (sz.x() >= sz.y() ? along : depth) / 2;
    const coord_t     hy     = (sz.x() >= sz.y() ? depth : along) / 2;
    return ExPolygon{BoundingBox(center - Point(hx, hy), center + Point(hx, hy)).polygon()};
}

static double remaining_length(const ExPolygons &hung)
{
    double remaining = 0;
    for (const ExPolygon &h : hung) {
        const Point sz = get_extents(h).size();
        remaining = std::max(remaining, double(std::max(sz.x(), sz.y())));
    }
    return remaining;
}

static bool has_area(const ExPolygons &exs, double min_area)
{
    for (const ExPolygon &ex : exs)
        if (std::abs(ex.area()) > min_area)
            return true;
    return false;
}

static ExPolygon clip_to_previous(const ExPolygon &island, const ExPolygon &previous, coord_t depth, coord_t length, coord_t line_width)
{
    const ExPolygons support = offset_ex(previous, 0.f);
    if (support.empty())
        return {};

    const ExPolygon &footprint = (support.size() == 1) ? support.front() : previous;
    ExPolygons       kept      = intersection_ex(island, footprint);
    const ExPolygons hung      = diff_ex(island, footprint);
    const double     sliver    = double(scale_(scale_(0.01)));

    if (has_area(hung, sliver)) {
        const coord_t   along         = std::max(length, coord_t(std::llround(remaining_length(hung))));
        const ExPolygon rect          = fallback_rectangle(footprint, depth, along);
        const ExPolygons clipped_rect = intersection_ex(rect, footprint);
        kept.insert(kept.end(), clipped_rect.begin(), clipped_rect.end());
    }

    return largest_thick_enough(union_ex(kept), line_width);
}

ExPolygon make_seam_tower_island(
    const Polygon    &contour,
    const Point      &seam,
    coord_t           gap,
    coord_t           depth,
    coord_t           length,
    coord_t           stack_window,
    coord_t           min_size,
    const ExPolygon  *previous,
    coord_t           line_width)
{
    if (contour.points.size() < 3 || depth <= 0)
        return {};

    if (min_size > 0 && contour.is_clockwise()) {
        Polygon hole = contour;
        hole.make_counter_clockwise();
        const float half = 0.5f * float(min_size + gap + depth);
        if (offset(hole, -half).empty())
            return {};
    }

    const double half_span = 0.5 * double(std::max(length, stack_window));
    const Polyline arc = contour_arc_around(contour, seam, half_span);
    if (arc.size() < 2)
        return {};

    const Polygons inners = offset(contour, float(gap));
    const Polygons outers = offset(contour, float(gap + depth));
    if (inners.empty() || outers.empty())
        return {};

    // Walk offsets in contour vertex order so a near-full wrap stays the
    // intended strip, not the leftover gap on the far side.
    const Polyline inner_arc = walk_until(nearest_polygon(inners, seam), arc.first_point(), arc.last_point());
    const Polyline outer_arc = walk_until(nearest_polygon(outers, seam), arc.first_point(), arc.last_point());
    if (inner_arc.size() < 2 || outer_arc.size() < 2)
        return {};

    Polygon island;
    island.points.reserve(inner_arc.size() + outer_arc.size());
    island.points.insert(island.points.end(), inner_arc.points.begin(), inner_arc.points.end());
    island.points.insert(island.points.end(), outer_arc.points.rbegin(), outer_arc.points.rend());
    remove_same_neighbor(island);
    island.make_counter_clockwise();
    if (!island.is_valid() || island.area() <= 0)
        return {};

    const ExPolygons unified = union_ex(Polygons{island});
    if (unified.size() != 1 || !unified.front().holes.empty())
        return {};

    ExPolygon result{std::move(island)};
    if (previous == nullptr || previous->empty()) {
        if (line_width > 0 && narrower_than_one_line_width(result, line_width))
            return {};
        return result;
    }

    return clip_to_previous(result, *previous, depth, length, line_width);
}

std::vector<SeamTowerStack> cluster_seam_tower_stacks(
    const std::vector<SeamTowerSample> &samples,
    coord_t                             depth,
    coord_t                             line_width)
{
    std::vector<SeamTowerSample> ordered = samples;
    std::sort(ordered.begin(), ordered.end(), [](const SeamTowerSample &a, const SeamTowerSample &b) {
        if (a.layer_index != b.layer_index)
            return a.layer_index < b.layer_index;
        return a.contour_id < b.contour_id;
    });

    const double thresh  = double(std::max(depth, coord_t(4) * line_width));
    const double thresh2 = thresh * thresh;

    std::vector<SeamTowerStack> stacks;
    for (const SeamTowerSample &sample : ordered) {
        int open = -1;
        for (int i = int(stacks.size()) - 1; i >= 0; --i) {
            if (stacks[size_t(i)].contour_id == sample.contour_id) {
                open = i;
                break;
            }
        }
        if (open >= 0) {
            SeamTowerStack &stack = stacks[size_t(open)];
            const double d2 = (sample.point - stack.seams.back().point).cast<double>().squaredNorm();
            if (d2 <= thresh2) {
                stack.seams.push_back(SeamTowerSeam{sample.layer_index, sample.point});
                stack.last_layer = sample.layer_index;
                continue;
            }
        }
        stacks.push_back(SeamTowerStack{
            sample.contour_id,
            sample.layer_index,
            sample.layer_index,
            {SeamTowerSeam{sample.layer_index, sample.point}}});
    }
    return stacks;
}

coordf_t seam_tower_arrange_inflation_mm(bool enabled, double gap_mm, double depth_mm)
{
    return enabled ? gap_mm + depth_mm : 0;
}

} // namespace Slic3r
