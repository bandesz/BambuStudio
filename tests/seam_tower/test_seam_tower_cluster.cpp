#include <catch2/catch.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/SeamTower.hpp"

#include <algorithm>
#include <set>
#include <tuple>
#include <vector>

using namespace Slic3r;

static Point mm(double x, double y) { return Point::new_scale(x, y); }

static coord_t jump_threshold(coord_t depth, coord_t line_width)
{
    return std::max(depth, coord_t(4) * line_width);
}

static int count_seam_assignments(const std::vector<SeamTowerStack> &stacks)
{
    int n = 0;
    for (const SeamTowerStack &stack : stacks)
        n += int(stack.seams.size());
    return n;
}

static bool every_sample_in_exactly_one_stack(
    const std::vector<SeamTowerSample> &samples,
    const std::vector<SeamTowerStack>  &stacks)
{
    using Key = std::tuple<int, int, coord_t, coord_t>;
    std::set<Key> assigned;
    for (const SeamTowerStack &stack : stacks) {
        for (const SeamTowerSeam &seam : stack.seams) {
            const Key key{stack.contour_id, seam.layer_index, seam.point.x(), seam.point.y()};
            if (!assigned.insert(key).second)
                return false;
        }
    }
    if (assigned.size() != samples.size())
        return false;
    for (const SeamTowerSample &sample : samples) {
        const Key key{sample.contour_id, sample.layer_index, sample.point.x(), sample.point.y()};
        if (!assigned.count(key))
            return false;
    }
    return true;
}

TEST_CASE("Seams within the jump threshold on successive layers form one stack", "[SeamTower]")
{
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const std::vector<SeamTowerSample> samples{
        {0, 0, mm(10.0, 0.0)},
        {0, 1, mm(10.4, 0.0)},
        {0, 2, mm(10.8, 0.0)},
    };

    const std::vector<SeamTowerStack> stacks =
        cluster_seam_tower_stacks(samples, depth, line_width);

    REQUIRE(stacks.size() == 1);
    REQUIRE(stacks.front().contour_id == 0);
    REQUIRE(stacks.front().seams.size() == 3);
    REQUIRE(stacks.front().first_layer == 0);
    REQUIRE(stacks.front().last_layer == 2);
    REQUIRE(stacks.front().seams.back().layer_index == 2);
    REQUIRE(every_sample_in_exactly_one_stack(samples, stacks));
}

TEST_CASE("A jump beyond the threshold opens a second stack", "[SeamTower]")
{
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const double  thresh_mm  = unscale<double>(jump_threshold(depth, line_width));

    const std::vector<SeamTowerSample> samples{
        {0, 0, mm(10.0, 0.0)},
        {0, 1, mm(10.4, 0.0)},
        {0, 2, mm(10.0 + thresh_mm + 1.0, 0.0)},
    };

    const std::vector<SeamTowerStack> stacks =
        cluster_seam_tower_stacks(samples, depth, line_width);

    REQUIRE(stacks.size() == 2);
    REQUIRE(count_seam_assignments(stacks) == 3);
    REQUIRE(every_sample_in_exactly_one_stack(samples, stacks));

    const SeamTowerStack *with_jump = nullptr;
    const SeamTowerStack *stable    = nullptr;
    for (const SeamTowerStack &stack : stacks) {
        if (stack.seams.size() == 1 && stack.seams.front().layer_index == 2)
            with_jump = &stack;
        else if (stack.seams.size() == 2)
            stable = &stack;
    }
    REQUIRE(stable != nullptr);
    REQUIRE(with_jump != nullptr);
    REQUIRE(stable->last_layer == stable->seams.back().layer_index);
    REQUIRE(stable->last_layer == 1);
    REQUIRE(with_jump->first_layer == 2);
    REQUIRE(with_jump->last_layer == 2);
    REQUIRE(with_jump->last_layer == with_jump->seams.back().layer_index);
}

TEST_CASE("Each seam is assigned to exactly one stack", "[SeamTower]")
{
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const double  thresh_mm  = unscale<double>(jump_threshold(depth, line_width));

    const std::vector<SeamTowerSample> samples{
        {0, 0, mm(0.0, 0.0)},
        {0, 1, mm(0.2, 0.0)},
        {0, 2, mm(thresh_mm + 3.0, 0.0)},
        {1, 0, mm(50.0, 0.0)},
        {1, 1, mm(50.1, 0.0)},
        {1, 2, mm(50.2, 0.0)},
    };

    const std::vector<SeamTowerStack> stacks =
        cluster_seam_tower_stacks(samples, depth, line_width);

    REQUIRE_FALSE(stacks.empty());
    REQUIRE(count_seam_assignments(stacks) == int(samples.size()));
    REQUIRE(every_sample_in_exactly_one_stack(samples, stacks));
}

TEST_CASE("Stack last_layer equals the last assigned seam layer", "[SeamTower]")
{
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const double  thresh_mm  = unscale<double>(jump_threshold(depth, line_width));

    const std::vector<SeamTowerSample> samples{
        {0, 0, mm(0.0, 0.0)},
        {0, 1, mm(thresh_mm + 2.0, 0.0)},
        {0, 2, mm(thresh_mm + 2.3, 0.0)},
    };

    const std::vector<SeamTowerStack> stacks =
        cluster_seam_tower_stacks(samples, depth, line_width);

    REQUIRE(stacks.size() == 2);
    for (const SeamTowerStack &stack : stacks) {
        REQUIRE_FALSE(stack.seams.empty());
        REQUIRE(stack.last_layer == stack.seams.back().layer_index);
        REQUIRE(stack.first_layer == stack.seams.front().layer_index);
    }
}

TEST_CASE("A jump then return on the same contour opens a third stack", "[SeamTower]")
{
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);
    const double  thresh_mm  = unscale<double>(jump_threshold(depth, line_width));

    const std::vector<SeamTowerSample> samples{
        {0, 0, mm(0.0, 0.0)},
        {0, 1, mm(thresh_mm + 100.0, 0.0)},
        {0, 2, mm(0.1, 0.0)},
    };

    const std::vector<SeamTowerStack> stacks =
        cluster_seam_tower_stacks(samples, depth, line_width);

    REQUIRE(stacks.size() == 3);
    REQUIRE(count_seam_assignments(stacks) == 3);
    REQUIRE(every_sample_in_exactly_one_stack(samples, stacks));

    const SeamTowerStack *first = nullptr;
    for (const SeamTowerStack &stack : stacks) {
        if (stack.seams.size() == 1 && stack.seams.front().layer_index == 0)
            first = &stack;
    }
    REQUIRE(first != nullptr);
    REQUIRE(first->last_layer == 0);
    REQUIRE(first->last_layer == first->seams.back().layer_index);
}

TEST_CASE("A seam may join across a layer with no sample on that contour", "[SeamTower]")
{
    const coord_t depth      = scale_(2.0);
    const coord_t line_width = scale_(0.4);

    const std::vector<SeamTowerSample> samples{
        {0, 0, mm(0.0, 0.0)},
        {0, 2, mm(0.1, 0.0)},
    };

    const std::vector<SeamTowerStack> stacks =
        cluster_seam_tower_stacks(samples, depth, line_width);

    REQUIRE(stacks.size() == 1);
    REQUIRE(stacks.front().seams.size() == 2);
    REQUIRE(stacks.front().first_layer == 0);
    REQUIRE(stacks.front().last_layer == 2);
    REQUIRE(every_sample_in_exactly_one_stack(samples, stacks));
}
