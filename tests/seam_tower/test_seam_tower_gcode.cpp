#include <catch2/catch.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/GCode/SeamTowerPlanner.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SeamTower.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;

TEST_CASE("erSeamTower role_to_string round-trips and fits in 32-bit visibility flags", "[SeamTower][Role]")
{
    REQUIRE(ExtrusionEntity::role_to_string(erSeamTower) == "Seam tower");
    REQUIRE(ExtrusionEntity::string_to_role("Seam tower") == erSeamTower);
    REQUIRE(ExtrusionEntity::string_to_role(ExtrusionEntity::role_to_string(erSeamTower)) == erSeamTower);
    REQUIRE(static_cast<int>(erCount) <= 32);
    REQUIRE(static_cast<int>(erSeamTower) + 1 == static_cast<int>(erMixed));
}

static bool contains_ci(const std::string &haystack, std::string_view needle)
{
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                     std::tolower(static_cast<unsigned char>(b)); });
    return it != haystack.end();
}

static Point mm(double x, double y) { return Point::new_scale(x, y); }

static Polygon square_contour_mm(double size)
{
    const coord_t s = scale_(size);
    return Polygon{Points{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)}};
}

static SeamTowerParams enabled_params()
{
    SeamTowerParams p;
    p.enabled     = true;
    p.gap         = scale_(0.1);
    p.depth       = scale_(2.0);
    p.in_holes    = false;
    p.min_size    = scale_(3.0);
    p.line_width  = scale_(0.4);
    return p;
}

struct CubeSpec
{
    Vec3d               offset{0., 0., 0.};
    double              size_xy    = 20.;
    double              height     = 20.;
    std::optional<bool> seam_tower;
    bool                with_hole  = false;
};

static int count_substr(const std::string &haystack, std::string_view needle)
{
    int    n   = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

static DynamicPrintConfig slice_print_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"printable_area", "0x0,200x0,200x200,0x200"},
        {"printable_height", 200},
        {"layer_height", 0.2},
        {"first_layer_height", 0.2},
        {"first_layer_extrusion_width", 0},
        {"initial_layer_print_height", 0.2},
        {"wall_loops", 2},
        {"sparse_infill_density", 0},
        {"gcode_comments", true},
        {"start_gcode", ""},
        {"seam_position", "aligned"},
        {"first_layer_inset_start", 0},
        {"skirt_loops", 0},
        {"brim_width", 0},
        {"line_width", 0.4},
        {"enable_support", 0},
    });
    config.set_deserialize_strict(extra);
    return config;
}

static std::string export_processed_gcode(Print &print)
{
    print.validate();
    print.set_status_silent();
    const boost::filesystem::path temp =
        boost::filesystem::path("/Users/andras/src/github.com/BambuStudio/BambuStudio/build") /
        boost::filesystem::unique_path("seam-tower-%%%%.gcode");
    print.process();
    REQUIRE(print.objects().front()->layer_count() > 0);
    print.export_gcode(temp.string(), nullptr, nullptr);
    std::ifstream in(temp.string());
    REQUIRE(in.good());
    std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    boost::nowide::remove(temp.string().c_str());
    return gcode;
}

static std::string gcode_for_object_id(const std::string &gcode, size_t label_id)
{
    const std::string tag = "; OBJECT_ID: " + std::to_string(label_id) + "\n";
    const size_t      start = gcode.find(tag);
    if (start == std::string::npos)
        return {};
    const size_t next = gcode.find("; OBJECT_ID: ", start + tag.size());
    if (next == std::string::npos)
        return gcode.substr(start);
    return gcode.substr(start, next - start);
}

static std::string slice_cubes(const std::vector<CubeSpec>                      &cubes,
                               std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = slice_print_config(extra);

    Model model;
    Print print;
    for (size_t i = 0; i < cubes.size(); ++i) {
        const CubeSpec &spec = cubes[i];
        ModelObject    *object = model.add_object();
        object->name = "cube" + std::to_string(i) + ".stl";
        object->add_volume(make_cube(spec.size_xy, spec.size_xy, spec.height));
        if (spec.with_hole) {
            object->add_volume(make_cube(spec.size_xy * 0.5, spec.size_xy * 0.5, spec.height + 2.),
                               ModelVolumeType::NEGATIVE_VOLUME);
        }
        object->add_instance();
        object->instances.front()->set_offset(spec.offset);
        object->ensure_on_bed();
        if (spec.seam_tower.has_value())
            object->config.set("seam_tower", *spec.seam_tower);
        print.auto_assign_extruders(object);
    }
    print.apply(model, config);
    return export_processed_gcode(print);
}

static std::string slice_cube(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    return slice_cubes({CubeSpec{}}, extra);
}

static std::string hop_window(const std::string &gcode)
{
    const std::string role_tag = " FEATURE: Seam tower";
    const size_t      role_pos = gcode.find(role_tag);
    REQUIRE(role_pos != std::string::npos);

    size_t last_tower_e = std::string::npos;
    size_t cursor       = role_pos;
    while (true) {
        const size_t line_end = gcode.find('\n', cursor + 1);
        if (line_end == std::string::npos)
            break;
        const std::string line = gcode.substr(cursor + 1, line_end - cursor - 1);
        if (line.find(" FEATURE: ") != std::string::npos && line.find("Seam tower") == std::string::npos)
            break;
        const bool is_move = line.rfind("G1", 0) == 0 || line.rfind("G0", 0) == 0;
        if (is_move && line.find('E') != std::string::npos &&
            (line.find('X') != std::string::npos || line.find('Y') != std::string::npos))
            last_tower_e = line_end;
        cursor = line_end;
    }
    REQUIRE(last_tower_e != std::string::npos);

    size_t first_wall_e = std::string::npos;
    cursor              = last_tower_e;
    while (true) {
        const size_t line_end = gcode.find('\n', cursor + 1);
        if (line_end == std::string::npos)
            break;
        const std::string line   = gcode.substr(cursor + 1, line_end - cursor - 1);
        const bool       is_move = line.rfind("G1", 0) == 0 || line.rfind("G0", 0) == 0;
        if (is_move && line.find('E') != std::string::npos &&
            (line.find('X') != std::string::npos || line.find('Y') != std::string::npos)) {
            first_wall_e = cursor + 1;
            break;
        }
        cursor = line_end;
    }
    REQUIRE(first_wall_e != std::string::npos);
    return gcode.substr(last_tower_e, first_wall_e - last_tower_e);
}

static bool line_is_xy_travel(const std::string &line)
{
    const size_t start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos)
        return false;
    const std::string_view s(line.data() + start, line.size() - start);
    if (s.rfind("G1", 0) != 0 && s.rfind("G0", 0) != 0)
        return false;
    if (s.find('E') != std::string_view::npos)
        return false;
    return s.find('X') != std::string_view::npos || s.find('Y') != std::string_view::npos;
}

static int count_xy_travels(const std::string &text)
{
    int    n   = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t line_end = text.find('\n', pos);
        if (line_end == std::string::npos)
            line_end = text.size();
        if (line_is_xy_travel(text.substr(pos, line_end - pos)))
            ++n;
        pos = line_end + 1;
    }
    return n;
}

static bool hop_has_extra_feature(const std::string &hop)
{
    const std::string tag = " FEATURE: ";
    size_t            pos = 0;
    while ((pos = hop.find(tag, pos)) != std::string::npos) {
        const size_t      start = pos + tag.size();
        const size_t      eol   = hop.find('\n', start);
        const std::string name  = hop.substr(start, (eol == std::string::npos ? hop.size() : eol) - start);
        if (name.find("Outer wall") == std::string::npos && name.find("Seam tower") == std::string::npos)
            return true;
        pos = start;
    }
    return false;
}

struct XyPoint
{
    double x = 0.;
    double y = 0.;
};

static double xy_distance(XyPoint a, XyPoint b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

static std::string gcode_without_comment(const std::string &line)
{
    const size_t comment = line.find(';');
    return comment == std::string::npos ? line : line.substr(0, comment);
}

static std::optional<double> gcode_axis_value(const std::string &code, char axis)
{
    const char want = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));
    for (size_t i = 0; i < code.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(code[i])) != want)
            continue;
        if (i > 0 && std::isalnum(static_cast<unsigned char>(code[i - 1])))
            continue;
        const char *begin = code.c_str() + i + 1;
        char       *end   = nullptr;
        const double value = std::strtod(begin, &end);
        if (end == begin)
            continue;
        return value;
    }
    return std::nullopt;
}

static bool gcode_is_motion(const std::string &code)
{
    const size_t start = code.find_first_not_of(" \t\r");
    if (start == std::string::npos)
        return false;
    const std::string_view word(code.data() + start, code.size() - start);
    return word.rfind("G0", 0) == 0 || word.rfind("G1", 0) == 0 || word.rfind("G2", 0) == 0 || word.rfind("G3", 0) == 0;
}

static bool gcode_has_e_retract(const std::string &text)
{
    if (text.find("G10") != std::string::npos)
        return true;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t line_end = text.find('\n', pos);
        if (line_end == std::string::npos)
            line_end = text.size();
        const std::string motion = gcode_without_comment(text.substr(pos, line_end - pos));
        if (gcode_is_motion(motion)) {
            const auto e      = gcode_axis_value(motion, 'E');
            const bool has_xy = gcode_axis_value(motion, 'X').has_value() || gcode_axis_value(motion, 'Y').has_value();
            if (e.has_value() && !has_xy && *e < -1e-6)
                return true;
        }
        pos = line_end + 1;
    }
    return false;
}

static double distance_to_segment(XyPoint point, XyPoint a, XyPoint b)
{
    const double dx   = b.x - a.x;
    const double dy   = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 1e-18)
        return xy_distance(point, a);
    double t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / len2;
    if (t < 0.)
        t = 0.;
    else if (t > 1.)
        t = 1.;
    return xy_distance(point, XyPoint{a.x + t * dx, a.y + t * dy});
}

static std::vector<std::string> split_gcode_lines(const std::string &gcode)
{
    std::vector<std::string> lines;
    size_t                   pos = 0;
    while (pos < gcode.size()) {
        size_t end = gcode.find('\n', pos);
        if (end == std::string::npos)
            end = gcode.size();
        std::string line = gcode.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == gcode.size())
            break;
        pos = end + 1;
    }
    return lines;
}

// Last seam-tower extrusion before each outer wall is the closest point on that
// tower outer-wall path to the wall start. Crossing the gap retracts, then a
// straight XY travel; the wall unretracts (E-only) before extruding.
static void require_hop_leaves_outer_wall_end(const std::string &gcode)
{
    constexpr double kEndpointMm  = 0.002;
    constexpr double kClosestMm   = 0.02;
    constexpr double kGapMm       = 0.1;
    constexpr double kLineWidthMm = 0.4;
    constexpr double kMaxHopMm    = kGapMm + kLineWidthMm + 0.05;

    const std::vector<std::string> lines = split_gcode_lines(gcode);
    XyPoint                        pos;
    bool                           have_pos        = false;
    bool                           in_tower        = false;
    bool                           awaiting_wall   = false;
    int                            last_extrude    = -1;
    XyPoint                        hop_depart;
    bool                           have_hop_depart = false;
    std::vector<XyPoint>           run;
    std::vector<XyPoint>           outer;
    int                            pairs = 0;

    auto close_run = [&]() {
        if (run.size() >= 2)
            outer = std::move(run);
        run.clear();
    };

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const std::string &line = lines[i];
        if (line.find(" FEATURE: ") != std::string::npos) {
            if (line.find("Seam tower") != std::string::npos) {
                in_tower        = true;
                awaiting_wall   = false;
                have_hop_depart = false;
                last_extrude    = -1;
                run.clear();
                outer.clear();
            } else if (in_tower && line.find("Outer wall") != std::string::npos) {
                close_run();
                in_tower      = false;
                awaiting_wall = true;
            } else {
                in_tower      = false;
                awaiting_wall = false;
                run.clear();
            }
            continue;
        }

        const std::string code = gcode_without_comment(line);
        if (!gcode_is_motion(code))
            continue;

        const auto x          = gcode_axis_value(code, 'X');
        const auto y          = gcode_axis_value(code, 'Y');
        const auto z          = gcode_axis_value(code, 'Z');
        const auto e          = gcode_axis_value(code, 'E');
        const bool has_xy     = x.has_value() || y.has_value();
        const bool extrude_xy = e.has_value() && has_xy;

        if (awaiting_wall && extrude_xy) {
            const XyPoint wall_start = pos;
            REQUIRE(have_pos);
            REQUIRE(outer.size() >= 2);
            REQUIRE(last_extrude >= 0);
            REQUIRE(have_hop_depart);
            const XyPoint tower_end = outer.back();
            std::string   between;
            for (int j = last_extrude + 1; j < i; ++j)
                between += lines[j] + '\n';

            double min_d = xy_distance(outer.front(), wall_start);
            for (size_t k = 1; k < outer.size(); ++k)
                min_d = std::min(min_d, distance_to_segment(wall_start, outer[k - 1], outer[k]));
            const double hop = xy_distance(tower_end, wall_start);
            INFO("tower end " << tower_end.x << "," << tower_end.y << " wall start " << wall_start.x << "," << wall_start.y
                              << " hop " << hop << " min path dist " << min_d);
            REQUIRE(xy_distance(hop_depart, tower_end) <= kEndpointMm);
            REQUIRE(hop <= min_d + kClosestMm);
            REQUIRE(hop > 0.);
            REQUIRE(hop <= kMaxHopMm);
            const bool retracted = gcode_has_e_retract(between);
            REQUIRE(retracted);
            REQUIRE(count_xy_travels(between) >= 1);
            REQUIRE(contains_ci(between, "travel to wall"));
            {
                size_t cursor = 0;
                while (cursor < between.size()) {
                    size_t eol = between.find('\n', cursor);
                    if (eol == std::string::npos)
                        eol = between.size();
                    const std::string motion = gcode_without_comment(between.substr(cursor, eol - cursor));
                    if (gcode_is_motion(motion)) {
                        const bool has_e  = gcode_axis_value(motion, 'E').has_value();
                        const bool has_xy = gcode_axis_value(motion, 'X').has_value() ||
                                            gcode_axis_value(motion, 'Y').has_value();
                        REQUIRE_FALSE((has_e && has_xy));
                    }
                    if (eol == between.size())
                        break;
                    cursor = eol + 1;
                }
            }
            ++pairs;
            awaiting_wall = false;
        }

        if (extrude_xy && in_tower) {
            if (run.empty() && have_pos)
                run.push_back(pos);
            if (x)
                pos.x = *x;
            if (y)
                pos.y = *y;
            have_pos = true;
            run.push_back(pos);
            last_extrude = i;
            continue;
        }

        if (extrude_xy) {
            if (x)
                pos.x = *x;
            if (y)
                pos.y = *y;
            have_pos = true;
            continue;
        }

        if (has_xy || z.has_value() || e.has_value()) {
            if (in_tower)
                close_run();
            if (has_xy && (in_tower || awaiting_wall)) {
                hop_depart      = pos;
                have_hop_depart = have_pos;
            }
            if (x)
                pos.x = *x;
            if (y)
                pos.y = *y;
            if (has_xy)
                have_pos = true;
        }
    }
    REQUIRE(pairs > 0);
}

TEST_CASE("Planner take_matching_tower matches hop_target once per layer", "[SeamTower][GCode]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    std::vector<SeamTowerBuildInput> inputs{{0, 0, seam, contour, false}};

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());
    REQUIRE_FALSE(planner.empty());

    const coord_t match = planner.match_distance();
    REQUIRE(planner.take_matching_tower(0, seam, match) != nullptr);
    REQUIRE(planner.take_matching_tower(0, seam, match) == nullptr);
    REQUIRE(planner.take_matching_tower(0, mm(0, 10), match) == nullptr);
}

TEST_CASE("Planner take_matching_tower pairs by PrintObject", "[SeamTower][GCode]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    const ExPolygon island{contour};

    const PrintObject *obj_a = reinterpret_cast<const PrintObject *>(1);
    const PrintObject *obj_b = reinterpret_cast<const PrintObject *>(2);

    std::vector<SeamTowerBuildInput> inputs{
        {0, 0, seam, contour, false, 0.2f, obj_a},
    };

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());
    REQUIRE_FALSE(planner.empty());

    const coord_t match = planner.match_distance();
    REQUIRE(planner.has_tower_for(0, seam, match, obj_a));
    REQUIRE_FALSE(planner.has_tower_for(0, seam, match, obj_b));
    REQUIRE(planner.has_tower_for(0, island, obj_a));
    REQUIRE_FALSE(planner.has_tower_for(0, island, obj_b));
    REQUIRE(planner.take_matching_tower(0, seam, match, obj_b) == nullptr);
    REQUIRE(planner.take_matching_tower(0, seam, match, obj_a) != nullptr);
}

TEST_CASE("Planner take_matching_tower prints once per instance without reset_printed", "[SeamTower][GCode]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    const PrintObject *obj = reinterpret_cast<const PrintObject *>(1);
    std::vector<SeamTowerBuildInput> inputs{{0, 0, seam, contour, false, 0.2f, obj}};

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());
    REQUIRE_FALSE(planner.empty());

    const coord_t match   = planner.match_distance();
    const Point   shift_a = mm(0, 0);
    const Point   shift_b = mm(50, 0);
    REQUIRE(planner.take_matching_tower(0, seam, match, obj, shift_a) != nullptr);
    REQUIRE(planner.take_matching_tower(0, seam, match, obj, shift_a) == nullptr);
    REQUIRE(planner.take_matching_tower(0, seam, match, obj, shift_b) != nullptr);
}

TEST_CASE("Planner take_matching_tower A then B then A is null without reset_printed", "[SeamTower][GCode]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    const PrintObject *obj = reinterpret_cast<const PrintObject *>(1);
    std::vector<SeamTowerBuildInput> inputs{{0, 0, seam, contour, false, 0.2f, obj}};

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());
    REQUIRE_FALSE(planner.empty());

    const coord_t match   = planner.match_distance();
    const Point   shift_a = mm(0, 0);
    const Point   shift_b = mm(50, 0);
    REQUIRE(planner.take_matching_tower(0, seam, match, obj, shift_a) != nullptr);
    REQUIRE(planner.take_matching_tower(0, seam, match, obj, shift_b) != nullptr);
    REQUIRE(planner.take_matching_tower(0, seam, match, obj, shift_a) == nullptr);
}

TEST_CASE("Planner has_tower_for matches hop_target without consuming take", "[SeamTower][GCode]")
{
    const Polygon contour = square_contour_mm(20.0);
    const Point   seam    = mm(10.0, 0.0);
    std::vector<SeamTowerBuildInput> inputs{{0, 0, seam, contour, false}};

    SeamTowerPlanner planner;
    planner.build(inputs, enabled_params());
    REQUIRE_FALSE(planner.empty());

    const coord_t match = planner.match_distance();
    const ExPolygon disjoint_island{Polygon{Points{mm(100, 100), mm(120, 100), mm(120, 120), mm(100, 120)}}};
    REQUIRE(planner.has_tower_for(0, seam, match));
    REQUIRE(planner.has_tower_for(0, ExPolygon{contour}));
    REQUIRE_FALSE(planner.has_tower_for(0, disjoint_island));
    REQUIRE(planner.take_matching_tower(0, seam, match) != nullptr);
    REQUIRE(planner.has_tower_for(0, seam, match));
    REQUIRE_FALSE(planner.has_tower_for(0, mm(0, 10), match));
}

TEST_CASE("Planner has_tower_for is false when a hole is below min_size", "[SeamTower][GCode]")
{
    const coord_t o = scale_((20.0 - 8.0) / 2.0);
    const coord_t i = scale_(8.0);
    const Polygon hole{Points{Point(o, o), Point(o, o + i), Point(o + i, o + i), Point(o + i, o)}};
    REQUIRE(hole.is_clockwise());
    const Point seam = Point((hole.points.front().x() + hole.points.back().x()) / 2, hole.points.front().y());

    SeamTowerParams params = enabled_params();
    params.in_holes        = true;
    params.min_size        = scale_(10.0);

    SeamTowerPlanner planner;
    planner.build({{1, 0, seam, hole, true}}, params);
    REQUIRE(planner.empty());
    REQUIRE_FALSE(planner.has_tower_for(0, seam, planner.match_distance()));
}

// export_gcode serializes DynamicPrintConfig, including coEnums options that
// can be constructed with keys_map == nullptr. Without a numeric fallback,
// serialize/deserialize SIGSEGV (Task 4.1 hop tests).
TEST_CASE("ConfigOptionEnumsGeneric round-trips integers when keys_map is null", "[SeamTower][GCode]")
{
    ConfigOptionEnumsGeneric opt;
    REQUIRE(opt.keys_map == nullptr);
    opt.values = {2, 5};
    REQUIRE(opt.serialize() == "2,5");
    REQUIRE(opt.deserialize("3,7"));
    REQUIRE(opt.values == std::vector<int>{3, 7});
}

TEST_CASE("Sliced cube with seam_tower emits tower immediately before the outer wall", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({{"seam_tower", 1}});

    REQUIRE(contains_ci(gcode, "seam tower"));

    const size_t tower_role = gcode.find(" FEATURE: Seam tower");
    REQUIRE(tower_role != std::string::npos);
    const size_t outer_role = gcode.find(" FEATURE: Outer wall", tower_role);
    REQUIRE(outer_role != std::string::npos);
    const size_t other_role = gcode.find(" FEATURE: ", tower_role + 1);
    REQUIRE(other_role == outer_role);
}

TEST_CASE("Travel from seam tower to the wall retracts then crosses the gap", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({{"seam_tower", 1}});
    const std::string hop   = hop_window(gcode);

    REQUIRE(gcode_has_e_retract(hop));
    REQUIRE(contains_ci(hop, "travel to wall"));
    REQUIRE(count_xy_travels(hop) >= 1);
    REQUIRE_FALSE(hop_has_extra_feature(hop));
}

static bool loop_has_role(const ExtrusionLoop &loop, ExtrusionRole role)
{
    for (const ExtrusionPath &path : loop.paths)
        if (path.role() == role)
            return true;
    return false;
}

template <typename Fn>
static void for_each_loop(ExtrusionEntity *entity, const Fn &fn)
{
    if (entity == nullptr)
        return;
    if (auto *coll = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
        for (ExtrusionEntity *child : coll->entities)
            for_each_loop(child, fn);
        return;
    }
    if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity))
        fn(*loop);
}

// Split an external loop so the seam vertex lies on an erOverhangPerimeter path
// and one other path keeps companion. place_seam starts the loop at a corner;
// every original corner stays on an overhang path, listed before the companion
// edge, so the first path after the split is overhang.
static bool retag_overhang_seam_loop(ExtrusionLoop &loop, ExtrusionRole companion)
{
    if (!loop_has_role(loop, erExternalPerimeter) || loop.paths.empty())
        return false;
    const Points ring = loop.polygon().points;
    if (ring.size() < 3 || ring.front() == ring.back())
        return false;

    const Point a = ring.front();
    const Point d = ring.back();
    const Point m1(d.x() + (a.x() - d.x()) / 3, d.y() + (a.y() - d.y()) / 3);
    const Point m2(d.x() + 2 * (a.x() - d.x()) / 3, d.y() + 2 * (a.y() - d.y()) / 3);
    if (m1 == d || m1 == m2 || m2 == a)
        return false;

    const ExtrusionPath tmpl = loop.paths.front();
    auto make_path = [&](Points pts, ExtrusionRole role) {
        ExtrusionPath path = tmpl;
        path.polyline.points = std::move(pts);
        path.polyline.fitting_result.clear();
        path.set_extrusion_role(role);
        return path;
    };

    Points overhang_pts = ring;
    overhang_pts.push_back(m1);
    loop.paths.clear();
    loop.paths.push_back(make_path(std::move(overhang_pts), erOverhangPerimeter));
    loop.paths.push_back(make_path(Points{m1, m2}, companion));
    loop.paths.push_back(make_path(Points{m2, a}, erOverhangPerimeter));

    bool saw_overhang  = false;
    bool saw_companion = false;
    bool saw_external  = false;
    for (const ExtrusionPath &path : loop.paths) {
        if (path.role() == erOverhangPerimeter)
            saw_overhang = true;
        if (path.role() == companion)
            saw_companion = true;
        if (path.role() == erExternalPerimeter)
            saw_external = true;
    }
    REQUIRE(saw_overhang);
    REQUIRE(saw_companion);
    REQUIRE(loop.paths.front().role() == erOverhangPerimeter);
    if (companion == erPerimeter)
        REQUIRE_FALSE(saw_external);
    if (companion == erExternalPerimeter)
        REQUIRE(saw_external);
    return true;
}

static int retag_external_loops(Print &print, ExtrusionRole companion)
{
    int tagged = 0;
    for (PrintObject *po : print.objects_mutable()) {
        for (Layer *layer : po->layers()) {
            if (layer == nullptr)
                continue;
            for (size_t r = 0; r < layer->region_count(); ++r) {
                LayerRegion *region = layer->get_region(int(r));
                if (region == nullptr)
                    continue;
                for (ExtrusionEntity *entity : region->perimeters.entities)
                    for_each_loop(entity, [&](ExtrusionLoop &loop) {
                        if (retag_overhang_seam_loop(loop, companion))
                            ++tagged;
                    });
            }
        }
    }
    return tagged;
}

// role() is paths.front(). An outer wall whose every path is overhang still
// pairs with the tower, but a visit scan that only reads role() misses it.
static int retag_external_loops_all_overhang(Print &print)
{
    int tagged = 0;
    for (PrintObject *po : print.objects_mutable()) {
        for (Layer *layer : po->layers()) {
            if (layer == nullptr)
                continue;
            for (size_t r = 0; r < layer->region_count(); ++r) {
                LayerRegion *region = layer->get_region(int(r));
                if (region == nullptr)
                    continue;
                for (ExtrusionEntity *entity : region->perimeters.entities)
                    for_each_loop(entity, [&](ExtrusionLoop &loop) {
                        if (!loop_has_role(loop, erExternalPerimeter))
                            return;
                        // Outermost contour stays elrDefault. depth == 1 is the
                        // wall just inside it and carries elrSecondPerimeter.
                        REQUIRE((loop.loop_role() & elrSecondPerimeter) == 0);
                        for (ExtrusionPath &path : loop.paths)
                            path.set_extrusion_role(erOverhangPerimeter);
                        REQUIRE(loop.paths.front().role() == erOverhangPerimeter);
                        REQUIRE_FALSE(loop_has_role(loop, erExternalPerimeter));
                        REQUIRE_FALSE(loop_has_role(loop, erPerimeter));
                        ++tagged;
                    });
            }
        }
    }
    return tagged;
}

// The wall just inside the outer contour (loop.depth == 1) is elrSecondPerimeter.
// Retag only that loop so an all-overhang inner wall can be told from the outer contour.
static int retag_second_perimeter_loops_all_overhang(Print &print)
{
    int tagged = 0;
    for (PrintObject *po : print.objects_mutable()) {
        for (Layer *layer : po->layers()) {
            if (layer == nullptr)
                continue;
            for (size_t r = 0; r < layer->region_count(); ++r) {
                LayerRegion *region = layer->get_region(int(r));
                if (region == nullptr)
                    continue;
                for (ExtrusionEntity *entity : region->perimeters.entities)
                    for_each_loop(entity, [&](ExtrusionLoop &loop) {
                        if ((loop.loop_role() & elrSecondPerimeter) == 0)
                            return;
                        if (loop_has_role(loop, erExternalPerimeter) || loop.paths.empty())
                            return;
                        for (ExtrusionPath &path : loop.paths)
                            path.set_extrusion_role(erOverhangPerimeter);
                        REQUIRE((loop.loop_role() & elrSecondPerimeter) != 0);
                        REQUIRE(loop.paths.front().role() == erOverhangPerimeter);
                        REQUIRE_FALSE(loop_has_role(loop, erExternalPerimeter));
                        REQUIRE_FALSE(loop_has_role(loop, erPerimeter));
                        ++tagged;
                    });
            }
        }
    }
    return tagged;
}

// Arachne's inset_idx != 0 walls are erPerimeter, not erExternalPerimeter.
// Force those paths to overhang without writing a loop role. The role that
// reaches the tower predicate is the one traverse_extrusions assigned.
static int retag_non_external_perimeter_loops_all_overhang(Print &print)
{
    int tagged = 0;
    for (PrintObject *po : print.objects_mutable()) {
        for (Layer *layer : po->layers()) {
            if (layer == nullptr)
                continue;
            for (size_t r = 0; r < layer->region_count(); ++r) {
                LayerRegion *region = layer->get_region(int(r));
                if (region == nullptr)
                    continue;
                for (ExtrusionEntity *entity : region->perimeters.entities)
                    for_each_loop(entity, [&](ExtrusionLoop &loop) {
                        if (loop_has_role(loop, erExternalPerimeter) || loop.paths.empty())
                            return;
                        if (!loop_has_role(loop, erPerimeter))
                            return;
                        const ExtrusionLoopRole role = loop.loop_role();
                        for (ExtrusionPath &path : loop.paths)
                            path.set_extrusion_role(erOverhangPerimeter);
                        REQUIRE(loop.loop_role() == role);
                        REQUIRE(loop.paths.front().role() == erOverhangPerimeter);
                        REQUIRE_FALSE(loop_has_role(loop, erExternalPerimeter));
                        REQUIRE_FALSE(loop_has_role(loop, erPerimeter));
                        ++tagged;
                    });
            }
        }
    }
    return tagged;
}

static bool extrusion_tree_has_role(const ExtrusionEntity *entity, ExtrusionRole role)
{
    if (entity == nullptr)
        return false;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            if (extrusion_tree_has_role(child, role))
                return true;
        return false;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
        return loop_has_role(*loop, role);
    if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        for (const ExtrusionPath &path : multi->paths)
            if (path.role() == role)
                return true;
        return false;
    }
    return entity->role() == role;
}

static bool perimeters_have_role(const Print &print, ExtrusionRole role)
{
    for (const PrintObject *po : print.objects()) {
        for (const Layer *layer : po->layers()) {
            if (layer == nullptr)
                continue;
            for (size_t r = 0; r < layer->region_count(); ++r) {
                const LayerRegion *region = layer->get_region(int(r));
                if (region == nullptr)
                    continue;
                for (const ExtrusionEntity *entity : region->perimeters.entities)
                    if (extrusion_tree_has_role(entity, role))
                        return true;
            }
        }
    }
    return false;
}

static std::string export_gcode_file(Print &print)
{
    const boost::filesystem::path temp =
        boost::filesystem::path("/Users/andras/src/github.com/BambuStudio/BambuStudio/build") /
        boost::filesystem::unique_path("seam-tower-%%%%.gcode");
    print.export_gcode(temp.string(), nullptr, nullptr);
    std::ifstream in(temp.string());
    REQUIRE(in.good());
    std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    boost::nowide::remove(temp.string().c_str());
    return gcode;
}

// Slice a cube, then rebuild each outer-wall loop so its first path is
// erOverhangPerimeter while companion stays on another path. Export does not
// reslice, so extrude_loop sees that loop.
static std::string slice_cube_with_overhang_seam(ExtrusionRole companion)
{
    DynamicPrintConfig config = slice_print_config({{"seam_tower", 1}});
    Model               model;
    Print               print;
    ModelObject        *object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(make_cube(20., 20., 1.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(0., 0., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().front()->layer_count() > 0);
    REQUIRE(retag_external_loops(print, companion) > 0);
    return export_gcode_file(print);
}

TEST_CASE("Overhang-first outer wall with an external path still retracts and hops", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube_with_overhang_seam(erExternalPerimeter);
    const std::string hop   = hop_window(gcode);

    REQUIRE(contains_ci(hop, "travel to wall"));
    REQUIRE(gcode_has_e_retract(hop));
    REQUIRE(count_xy_travels(hop) >= 1);

    const size_t travel   = gcode.find("travel to wall");
    const size_t overhang = gcode.find(" FEATURE: Overhang wall", travel);
    const size_t outer    = gcode.find(" FEATURE: Outer wall", travel);
    REQUIRE(travel != std::string::npos);
    REQUIRE(overhang != std::string::npos);
    REQUIRE(outer != std::string::npos);
    REQUIRE(overhang < outer);
}

TEST_CASE("Overhang loop that still has an inner wall does not hop", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube_with_overhang_seam(erPerimeter);
    REQUIRE(contains_ci(gcode, "Seam tower"));
    REQUIRE_FALSE(contains_ci(gcode, "travel to wall"));
}

TEST_CASE("Seam tower hop leaves the outer-wall end nearest the wall start", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({{"seam_tower", 1}});
    require_hop_leaves_outer_wall_end(gcode);
}

struct SeamTowerGcodeFeature
{
    double z     = 0.;
    int    loops = 0;
};

// Each concentric loop is one extrusion run inside a FEATURE: Seam tower block.
// Travels between loops (retract allowed) split runs; retract + travel to the
// wall is after the last tower extrusion and is not counted.
static std::vector<SeamTowerGcodeFeature> seam_tower_gcode_features(const std::string &gcode)
{
    const std::vector<std::string>     lines = split_gcode_lines(gcode);
    std::vector<SeamTowerGcodeFeature> features;
    double                             z         = 0.;
    bool                               in_tower  = false;
    bool                               in_run    = false;
    int                                loops     = 0;
    double                             feature_z = 0.;

    auto finish_tower = [&]() {
        if (!in_tower)
            return;
        if (in_run)
            ++loops;
        features.push_back({feature_z, loops});
        in_tower = false;
        in_run   = false;
        loops    = 0;
    };

    for (const std::string &line : lines) {
        if (line.find(" FEATURE: ") != std::string::npos) {
            if (line.find("Seam tower") != std::string::npos) {
                finish_tower();
                in_tower  = true;
                loops     = 0;
                in_run    = false;
                feature_z = z;
            } else {
                finish_tower();
            }
            continue;
        }

        const std::string code = gcode_without_comment(line);
        if (!gcode_is_motion(code))
            continue;

        const auto x          = gcode_axis_value(code, 'X');
        const auto y          = gcode_axis_value(code, 'Y');
        const auto zz         = gcode_axis_value(code, 'Z');
        const auto e          = gcode_axis_value(code, 'E');
        const bool has_xy     = x.has_value() || y.has_value();
        const bool extrude_xy = e.has_value() && has_xy;

        if (zz)
            z = *zz;

        if (!in_tower)
            continue;

        if (extrude_xy) {
            in_run = true;
            continue;
        }

        if (has_xy || zz.has_value() || e.has_value()) {
            if (in_run) {
                ++loops;
                in_run = false;
            }
        }
    }
    finish_tower();
    return features;
}

TEST_CASE("Sliced cube with large seam_tower_depth is not solid-filled above the first layer", "[SeamTower][GCode]")
{
    CubeSpec cube;
    cube.offset = Vec3d(50., 50., 0.);
    cube.height = 6.;
    const std::string gcode = slice_cubes({cube}, {
        {"seam_tower", 1},
        {"seam_tower_depth", 8},
        {"seam_tower_length", 4},
    });

    REQUIRE(contains_ci(gcode, "seam tower"));
    REQUIRE(gcode.find(" FEATURE: Seam tower") != std::string::npos);

    const std::vector<SeamTowerGcodeFeature> features = seam_tower_gcode_features(gcode);
    REQUIRE_FALSE(features.empty());

    int  first_loops = -1;
    int  upper_max   = -1;
    bool saw_upper   = false;
    for (const SeamTowerGcodeFeature &feature : features) {
        if (feature.z <= 0.25) {
            first_loops = std::max(first_loops, feature.loops);
            continue;
        }
        saw_upper = true;
        REQUIRE(feature.loops <= 2);
        REQUIRE(feature.loops >= 1);
        upper_max = std::max(upper_max, feature.loops);
    }
    REQUIRE(first_loops > 0);
    REQUIRE(saw_upper);
    REQUIRE(first_loops > upper_max);

    const std::string hop = hop_window(gcode);
    REQUIRE(gcode_has_e_retract(hop));
    REQUIRE(contains_ci(hop, "travel to wall"));
    require_hop_leaves_outer_wall_end(gcode);
}

TEST_CASE("Sliced hexagon with seam_tower emits tower on a centered plate object", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({{"seam_tower", 1}});
    Model              model;
    Print              print;
    ModelObject       *object = model.add_object();
    object->name              = "hexagon.stl";
    object->add_volume(make_cylinder(10., 10., 2. * PI / 6.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const std::string gcode = export_processed_gcode(print);
    REQUIRE(contains_ci(gcode, "seam tower"));
}

TEST_CASE("Sliced hexagon at the plate origin still slices", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({{"seam_tower", 1}, {"seam_position", "aligned"}});
    Model              model;
    Print              print;
    ModelObject       *object = model.add_object();
    object->name              = "hexagon.stl";
    object->add_volume(make_cylinder(10., 10., 2. * PI / 6.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(0., 0., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const std::string gcode = export_processed_gcode(print);
    // May skip if the aligned seam faces off the 0..200 plate; slice must still succeed.
    REQUIRE(gcode.find("G1") != std::string::npos);
}

TEST_CASE("seam_tower off produces no seam tower comments", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({{"seam_tower", 0}});
    REQUIRE_FALSE(contains_ci(gcode, "seam tower"));
}

static int count_loops_with_role(Print &print, ExtrusionLoopRole role)
{
    int n = 0;
    for (PrintObject *po : print.objects_mutable()) {
        for (Layer *layer : po->layers()) {
            if (layer == nullptr)
                continue;
            for (size_t r = 0; r < layer->region_count(); ++r) {
                LayerRegion *region = layer->get_region(int(r));
                if (region == nullptr)
                    continue;
                for (ExtrusionEntity *entity : region->perimeters.entities)
                    for_each_loop(entity, [&](ExtrusionLoop &loop) {
                        if ((loop.loop_role() & role) != 0)
                            ++n;
                    });
            }
        }
    }
    return n;
}

static void process_arachne_cube(Model &model, Print &print, int seam_tower)
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", seam_tower},
        {"wall_loops", 2},
        {"wall_generator", "arachne"},
    });
    ModelObject *object = model.add_object();
    object->name        = "cube.stl";
    object->add_volume(make_cube(20., 20., 2.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

// traverse_extrusions tags inset_idx != 0 only so an enabled tower can ignore
// an all-overhang inner wall. With the option off, those loops stay untagged.
TEST_CASE("Arachne inner walls are not second perimeters when seam tower is off", "[SeamTower][GCode]")
{
    Model model;
    Print print;
    process_arachne_cube(model, print, 0);
    REQUIRE(print.objects().front()->layer_count() > 0);
    REQUIRE(count_loops_with_role(print, elrSecondPerimeter) == 0);
}

TEST_CASE("Arachne inner walls are second perimeters when seam tower is on", "[SeamTower][GCode]")
{
    Model model;
    Print print;
    process_arachne_cube(model, print, 1);
    REQUIRE(print.objects().front()->layer_count() > 0);
    REQUIRE(count_loops_with_role(print, elrSecondPerimeter) > 0);
}

TEST_CASE("spiral_mode produces no seam tower comments", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({{"seam_tower", 1}, {"spiral_mode", 1}});
    REQUIRE_FALSE(contains_ci(gcode, "seam tower"));
}

TEST_CASE("Cube with too-thin seam_tower_depth skips the tower and still slices", "[SeamTower][GCode]")
{
    SECTION("depth zero") {
        const std::string gcode = slice_cube({{"seam_tower", 1}, {"seam_tower_depth", 0.0}});
        REQUIRE_FALSE(contains_ci(gcode, "seam tower"));
        REQUIRE(gcode.find("G1") != std::string::npos);
    }

    SECTION("depth thinner than one line width") {
        const std::string gcode = slice_cube({{"seam_tower", 1}, {"seam_tower_depth", 0.3}, {"line_width", 0.4}});
        REQUIRE_FALSE(contains_ci(gcode, "seam tower"));
        REQUIRE(gcode.find("G1") != std::string::npos);
    }
}

TEST_CASE("First-layer cube with a seam tower skips inset start", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({{"seam_tower", 1}, {"first_layer_inset_start", 1}});

    REQUIRE(contains_ci(gcode, "seam tower"));
    REQUIRE(gcode.find(" FEATURE: Seam tower") != std::string::npos);
    REQUIRE_FALSE(contains_ci(gcode, "inset start"));
}

TEST_CASE("First-layer cube without a seam tower still emits inset start", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({{"seam_tower", 0}, {"first_layer_inset_start", 1}});

    REQUIRE_FALSE(contains_ci(gcode, "seam tower"));
    REQUIRE(contains_ci(gcode, "inset start"));
}

TEST_CASE("First-layer colliding cubes keep inset start when a tower is skipped", "[SeamTower][GCode]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> extra{
        {"seam_tower", 1},
        {"first_layer_inset_start", 1},
        {"seam_position", "back"},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
    };
    CubeSpec a;
    a.offset         = Vec3d(50., 50., 0.);
    a.height         = 6.;
    CubeSpec b_close = a;
    b_close.offset   = Vec3d(50., 71., 0.);
    CubeSpec b_far   = a;
    b_far.offset     = Vec3d(50., 130., 0.);

    const std::string close_gcode = slice_cubes({a, b_close}, extra);
    const std::string far_gcode   = slice_cubes({a, b_far}, extra);

    const int close_n = count_substr(close_gcode, " FEATURE: Seam tower");
    const int far_n   = count_substr(far_gcode, " FEATURE: Seam tower");
    REQUIRE(far_n > 0);
    REQUIRE(close_n < far_n);
    REQUIRE(contains_ci(close_gcode, "inset start"));
}

static void require_each_seam_tower_precedes_outer_wall(const std::string &gcode)
{
    size_t pos = 0;
    int    n   = 0;
    while ((pos = gcode.find(" FEATURE: Seam tower", pos)) != std::string::npos) {
        ++n;
        const size_t next = gcode.find(" FEATURE: ", pos + 1);
        REQUIRE(next != std::string::npos);
        REQUIRE(gcode.compare(next, 20, " FEATURE: Outer wall") == 0);
        pos = next;
    }
    REQUIRE(n > 0);
}

static std::string slice_cube_instances(const std::vector<Vec3d>                    &offsets,
                                        std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = slice_print_config(extra);

    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "cube-instances.stl";
    object->add_volume(make_cube(20., 20., 6.));
    for (const Vec3d &offset : offsets) {
        ModelInstance *instance = object->add_instance();
        instance->set_offset(offset);
    }
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    REQUIRE(print.objects().front()->instances().size() == offsets.size());
    return export_processed_gcode(print);
}

TEST_CASE("Two cubes closer than gap plus depth skip the colliding tower", "[SeamTower][GCode][Collision]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> extra{
        {"seam_tower", 1},
        {"seam_position", "back"},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
    };
    CubeSpec a;
    a.offset = Vec3d(50., 50., 0.);
    a.height = 6.;
    CubeSpec b_close = a;
    b_close.offset   = Vec3d(50., 71., 0.);
    CubeSpec b_far   = a;
    b_far.offset     = Vec3d(50., 130., 0.);

    const std::string close_gcode = slice_cubes({a, b_close}, extra);
    const std::string far_gcode   = slice_cubes({a, b_far}, extra);

    const int close_n = count_substr(close_gcode, " FEATURE: Seam tower");
    const int far_n   = count_substr(far_gcode, " FEATURE: Seam tower");
    REQUIRE(far_n > 0);
    REQUIRE(close_n < far_n);
}

TEST_CASE("Two instances of one object closer than gap plus depth skip the colliding tower", "[SeamTower][GCode][Collision]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> extra{
        {"seam_tower", 1},
        {"seam_position", "back"},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
    };
    const Vec3d a(50., 50., 0.);
    const Vec3d b_close(50., 71., 0.);
    const Vec3d b_far(50., 130., 0.);

    const std::string single_gcode = slice_cube_instances({a}, extra);
    const std::string close_gcode  = slice_cube_instances({a, b_close}, extra);
    const std::string far_gcode    = slice_cube_instances({a, b_far}, extra);

    const int single_n = count_substr(single_gcode, " FEATURE: Seam tower");
    const int close_n  = count_substr(close_gcode, " FEATURE: Seam tower");
    const int far_n    = count_substr(far_gcode, " FEATURE: Seam tower");
    REQUIRE(single_n > 0);
    REQUIRE(far_n == 2 * single_n);
    REQUIRE(close_n < far_n);
}

TEST_CASE("Two-object slice emits towers only for the object with seam_tower on", "[SeamTower][GCode]")
{
    CubeSpec a;
    a.offset     = Vec3d(50., 50., 0.);
    a.height     = 6.;
    a.seam_tower = true;
    CubeSpec b   = a;
    b.offset     = Vec3d(50., 130., 0.);
    b.seam_tower = false;

    const std::string a_only = slice_cubes({a, b}, {{"seam_tower", 1}, {"seam_position", "back"}});
    b.seam_tower             = true;
    const std::string both   = slice_cubes({a, b}, {{"seam_tower", 1}, {"seam_position", "back"}});

    const int a_only_n = count_substr(a_only, " FEATURE: Seam tower");
    const int both_n   = count_substr(both, " FEATURE: Seam tower");
    REQUIRE(a_only_n > 0);
    REQUIRE(a_only_n < both_n);
}

TEST_CASE("Cube at layer_height 0.3 still emits a seam tower", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube({
        {"seam_tower", 1},
        {"layer_height", 0.3},
        {"first_layer_height", 0.3},
        {"initial_layer_print_height", 0.3},
    });
    REQUIRE(contains_ci(gcode, "seam tower"));
    REQUIRE(gcode.find(" FEATURE: Seam tower") != std::string::npos);
}

static void collect_path_heights(const ExtrusionEntity *entity, std::vector<float> &heights)
{
    if (entity == nullptr)
        return;
    if (entity->is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            collect_path_heights(child, heights);
        return;
    }
    if (entity->is_loop()) {
        for (const ExtrusionPath &path : static_cast<const ExtrusionLoop *>(entity)->paths)
            heights.push_back(path.height);
        return;
    }
    heights.push_back(static_cast<const ExtrusionPath *>(entity)->height);
}

TEST_CASE("Print-path projected layers use PrintObject layer height", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({
        {"layer_height", 0.3},
        {"first_layer_height", 0.3},
        {"initial_layer_print_height", 0.3},
        {"seam_tower", 1},
    });

    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(make_cube(20., 20., 6.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject *po = print.objects().front();
    REQUIRE(po->layer_count() > 3);
    REQUIRE(po->get_layer(0)->height == Approx(0.3));

    SeamPlacer placer;
    placer.init(print, []() {});
    auto it = placer.m_seam_per_object.find(po);
    REQUIRE(it != placer.m_seam_per_object.end());
    REQUIRE(it->second.layers.size() > 3);
    it->second.layers[0].perimeters.clear();
    it->second.layers[0].points.clear();
    it->second.layers[1].perimeters.clear();
    it->second.layers[1].points.clear();

    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);
    REQUIRE_FALSE(planner.empty());
    // The seam starts on layer 2. The column still stands from the first layer,
    // at that layer's own height.
    const ExtrusionEntityCollection *ground = planner.collection_at_layer(0);
    REQUIRE(ground != nullptr);
    std::vector<float> ground_heights;
    for (const ExtrusionEntity *entity : ground->entities)
        collect_path_heights(entity, ground_heights);
    REQUIRE_FALSE(ground_heights.empty());
    for (float h : ground_heights)
        REQUIRE(h == Approx(0.3f));

    const ExtrusionEntityCollection *coll = planner.collection_at_layer(2);
    REQUIRE(coll != nullptr);
    std::vector<float> heights;
    for (const ExtrusionEntity *entity : coll->entities)
        collect_path_heights(entity, heights);
    REQUIRE_FALSE(heights.empty());
    for (float h : heights)
        REQUIRE(h == Approx(0.3f));
}

TEST_CASE("Inner hole walls do not consume a seam tower", "[SeamTower][GCode]")
{
    CubeSpec cube;
    cube.offset     = Vec3d(50., 50., 0.);
    cube.size_xy    = 30.;
    cube.height     = 6.;
    cube.with_hole  = true;
    const std::string gcode = slice_cubes({cube}, {
        {"seam_tower", 1},
        {"seam_tower_in_holes", 1},
        {"wall_loops", 2},
        {"wall_sequence", "inner wall/outer wall"},
        {"seam_position", "back"},
    });
    require_each_seam_tower_precedes_outer_wall(gcode);
}

static bool has_skip_warning(const SeamTowerPlanner &planner)
{
    const auto &w = planner.warnings();
    return std::find(w.begin(), w.end(), "Seam tower skipped: not enough space.") != w.end();
}

static void inject_support_over_first_layer_strip(PrintObject *po)
{
    REQUIRE(po != nullptr);
    REQUIRE(po->layer_count() > 0);
    const Layer *layer0 = po->get_layer(0);
    const coord_t expand =
        scale_(po->config().seam_tower_gap) + scale_(po->config().seam_tower_depth);
    ExPolygons support = offset_ex(layer0->lslices, expand);
    REQUIRE_FALSE(support.empty());
    SupportLayer *sl = po->add_support_layer(0, 0, layer0->height, layer0->print_z);
    sl->support_islands = std::move(support);
}

TEST_CASE("Print-path same-instance support islands skip the colliding tower", "[SeamTower][GCode][Collision]")
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_position", "aligned"},
    });

    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(make_cube(20., 20., 6.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    PrintObject *po = print.get_object(0);
    REQUIRE(po != nullptr);
    REQUIRE(po->instances().size() == 1);

    SeamPlacer placer;
    placer.init(print, []() {});

    {
        SeamTowerParams params;
        params.enabled = true;
        SeamTowerPlanner planner;
        planner.build(print, placer, params);
        REQUIRE_FALSE(planner.empty());
        REQUIRE(planner.warnings().empty());
    }

    inject_support_over_first_layer_strip(po);

    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);
    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

// make_cube is not centered, so center_offset is half the size. A thinner blocker
// has a different center_offset: shift and shift - center_offset then disagree.
static bool first_layer_outer_seam(const PrintObject *po, const PrintObjectSeamData &seams, Point &seam, Polygon &contour)
{
    if (po == nullptr || po->layer_count() == 0 || seams.layers.empty() || po->get_layer(0)->lslices.empty())
        return false;
    const PrintObjectSeamData::LayerSeams &layer_seams = seams.layers.front();
    bool found = false;
    for (const SeamPlacerImpl::Perimeter &perim : layer_seams.perimeters) {
        if (perim.finalized)
            seam = Point::new_scale(double(perim.final_seam_position.x()), double(perim.final_seam_position.y()));
        else if (perim.seam_index < layer_seams.points.size())
            seam = Point::new_scale(double(layer_seams.points[perim.seam_index].position.x()),
                                    double(layer_seams.points[perim.seam_index].position.y()));
        else
            continue;
        found = true;
        break;
    }
    if (!found)
        return false;

    double         best      = 0.;
    const Polygon *best_poly = nullptr;
    for (const ExPolygon &ex : po->get_layer(0)->lslices) {
        const double d2 = (ex.contour.point_projection(seam) - seam).cast<double>().squaredNorm();
        if (best_poly == nullptr || d2 < best) {
            best      = d2;
            best_poly = &ex.contour;
        }
    }
    if (best_poly == nullptr)
        return false;
    contour = *best_poly;
    return true;
}

static ExPolygon first_layer_tower_island(const PrintObject *po, const Point &seam, const Polygon &contour)
{
    return make_seam_tower_island(contour, seam, scale_(po->config().seam_tower_gap), scale_(po->config().seam_tower_depth),
                                  scale_(po->config().seam_tower_length), 0, scale_(po->config().seam_tower_min_size), nullptr,
                                  scale_(0.4));
}

static ExPolygons slices_translated(const PrintObject *po, const Point &shift)
{
    ExPolygons local;
    for (const Layer *layer : po->layers())
        local.insert(local.end(), layer->lslices.begin(), layer->lslices.end());
    ExPolygons moved;
    for (ExPolygon ex : union_ex(local)) {
        ex.translate(shift);
        moved.push_back(std::move(ex));
    }
    return moved;
}

static void apply_and_slice(Model &model, Print &print, const DynamicPrintConfig &config)
{
    for (ModelObject *object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    const StringObjectException valid = print.validate();
    INFO(valid.string);
    REQUIRE(valid.string.empty());
    print.set_status_silent();
    print.process();
}

TEST_CASE("Print-path skips a tower that hits another object only in the instance shift frame", "[SeamTower][GCode][Collision]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> extra{
        {"seam_tower", 1},
        {"seam_position", "aligned"},
        {"wall_loops", 1},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
        {"seam_tower_length", 4.0},
    };
    const DynamicPrintConfig config = slice_print_config(extra);
    const Vec3d               cube_offset(50., 50., 0.);

    Vec3d  blocker_offset(0., 0., 0.);
    double blocker_sx = 20.;
    double blocker_sy = 4.;
    {
        Model model;
        Print print;
        ModelObject *object = model.add_object();
        object->name = "cube.stl";
        object->add_volume(make_cube(20., 20., 6.));
        object->add_instance();
        object->instances.front()->set_offset(cube_offset);
        object->ensure_on_bed();
        apply_and_slice(model, print, config);

        const PrintObject *po = print.objects().front();
        REQUIRE(po->center_offset() == Point::new_scale(10., 10.));
        SeamPlacer placer;
        placer.init(print, []() {});
        auto seams = placer.m_seam_per_object.find(po);
        REQUIRE(seams != placer.m_seam_per_object.end());
        Point   seam;
        Polygon contour;
        REQUIRE(first_layer_outer_seam(po, seams->second, seam, contour));
        const ExPolygon island = first_layer_tower_island(po, seam, contour);
        REQUIRE_FALSE(island.empty());

        const Point delta = get_extents(island).center() - get_extents(contour).center();
        const bool  along_x  = std::llabs(static_cast<long long>(delta.x())) >= std::llabs(static_cast<long long>(delta.y()));
        const bool  positive = along_x ? delta.x() > 0 : delta.y() > 0;
        ExPolygon   world    = island;
        world.translate(po->instances().front().shift);
        const BoundingBox bb = get_extents(world);
        // 4 mm is thicker than the 2 mm tower and thinner than the 8 mm center_offset
        // disagreement (cube center 10 mm, thin-axis center 2 mm), so only instance.shift overlaps.
        constexpr double thick = 4.;
        constexpr double span  = 20.;
        if (along_x) {
            blocker_sx     = thick;
            blocker_sy     = span;
            blocker_offset = Vec3d(positive ? unscale<double>(bb.min.x()) : unscale<double>(bb.max.x()) - thick,
                                   unscale<double>(bb.center().y()) - span / 2., 0.);
        } else {
            blocker_sx     = span;
            blocker_sy     = thick;
            blocker_offset = Vec3d(unscale<double>(bb.center().x()) - span / 2.,
                                   positive ? unscale<double>(bb.min.y()) : unscale<double>(bb.max.y()) - thick, 0.);
        }
    }

    Model model;
    Print print;
    ModelObject *cube = model.add_object();
    cube->name = "cube.stl";
    cube->add_volume(make_cube(20., 20., 6.));
    cube->add_instance();
    cube->instances.front()->set_offset(cube_offset);
    cube->ensure_on_bed();

    ModelObject *blocker = model.add_object();
    blocker->name = "blocker.stl";
    blocker->add_volume(make_cube(blocker_sx, blocker_sy, 6.));
    blocker->add_instance();
    blocker->instances.front()->set_offset(blocker_offset);
    blocker->ensure_on_bed();
    blocker->config.set("seam_tower", false);
    apply_and_slice(model, print, config);

    const PrintObject *cube_po     = nullptr;
    const PrintObject *blocker_po  = nullptr;
    for (const PrintObject *po : print.objects()) {
        if (po->model_object()->name == "cube.stl")
            cube_po = po;
        else
            blocker_po = po;
    }
    REQUIRE(cube_po != nullptr);
    REQUIRE(blocker_po != nullptr);
    REQUIRE(cube_po->center_offset() == Point::new_scale(10., 10.));

    SeamPlacer placer;
    placer.init(print, []() {});
    auto seams = placer.m_seam_per_object.find(cube_po);
    REQUIRE(seams != placer.m_seam_per_object.end());
    Point   seam;
    Polygon contour;
    REQUIRE(first_layer_outer_seam(cube_po, seams->second, seam, contour));
    const ExPolygon island = first_layer_tower_island(cube_po, seam, contour);
    REQUIRE_FALSE(island.empty());

    const PrintInstance &cube_inst = cube_po->instances().front();
    ExPolygon            by_shift  = island;
    by_shift.translate(cube_inst.shift);
    ExPolygon by_shift_minus_center = island;
    by_shift_minus_center.translate(cube_inst.shift - cube_po->center_offset());
    const ExPolygons blocker_by_shift = slices_translated(blocker_po, blocker_po->instances().front().shift);
    const ExPolygons blocker_by_shift_minus_center =
        slices_translated(blocker_po, blocker_po->instances().front().shift - blocker_po->center_offset());
    REQUIRE_FALSE(intersection_ex(by_shift, blocker_by_shift).empty());
    REQUIRE(intersection_ex(by_shift_minus_center, blocker_by_shift_minus_center).empty());

    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);
    REQUIRE(planner.empty());
    REQUIRE(has_skip_warning(planner));
}

TEST_CASE("Print-path keeps a clear instance tower when the other copy hits an object", "[SeamTower][GCode][Collision]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> extra{
        {"seam_tower", 1},
        {"seam_position", "aligned"},
        {"wall_loops", 1},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
        {"seam_tower_length", 4.0},
    };
    const DynamicPrintConfig config = slice_print_config(extra);
    const Vec3d               blocked_offset(50., 50., 0.);
    const Vec3d               clear_offset(140., 140., 0.);

    Vec3d  blocker_offset(0., 0., 0.);
    double blocker_sx = 20.;
    double blocker_sy = 4.;
    {
        Model model;
        Print print;
        ModelObject *object = model.add_object();
        object->name = "cube.stl";
        object->add_volume(make_cube(20., 20., 6.));
        object->add_instance();
        object->instances.front()->set_offset(blocked_offset);
        object->ensure_on_bed();
        apply_and_slice(model, print, config);

        const PrintObject *po = print.objects().front();
        SeamPlacer placer;
        placer.init(print, []() {});
        auto seams = placer.m_seam_per_object.find(po);
        REQUIRE(seams != placer.m_seam_per_object.end());
        Point   seam;
        Polygon contour;
        REQUIRE(first_layer_outer_seam(po, seams->second, seam, contour));
        const ExPolygon island = first_layer_tower_island(po, seam, contour);
        REQUIRE_FALSE(island.empty());

        const Point delta    = get_extents(island).center() - get_extents(contour).center();
        const bool  along_x  = std::llabs(static_cast<long long>(delta.x())) >= std::llabs(static_cast<long long>(delta.y()));
        const bool  positive = along_x ? delta.x() > 0 : delta.y() > 0;
        ExPolygon   world    = island;
        world.translate(po->instances().front().shift);
        const BoundingBox bb = get_extents(world);
        constexpr double thick = 4.;
        constexpr double span  = 20.;
        if (along_x) {
            blocker_sx     = thick;
            blocker_sy     = span;
            blocker_offset = Vec3d(positive ? unscale<double>(bb.min.x()) : unscale<double>(bb.max.x()) - thick,
                                   unscale<double>(bb.center().y()) - span / 2., 0.);
        } else {
            blocker_sx     = span;
            blocker_sy     = thick;
            blocker_offset = Vec3d(unscale<double>(bb.center().x()) - span / 2.,
                                   positive ? unscale<double>(bb.min.y()) : unscale<double>(bb.max.y()) - thick, 0.);
        }
    }

    Model model;
    Print print;
    ModelObject *cube = model.add_object();
    cube->name = "cube.stl";
    cube->add_volume(make_cube(20., 20., 6.));
    cube->add_instance()->set_offset(blocked_offset);
    cube->add_instance()->set_offset(clear_offset);
    cube->ensure_on_bed();

    ModelObject *blocker = model.add_object();
    blocker->name = "blocker.stl";
    blocker->add_volume(make_cube(blocker_sx, blocker_sy, 6.));
    blocker->add_instance();
    blocker->instances.front()->set_offset(blocker_offset);
    blocker->ensure_on_bed();
    blocker->config.set("seam_tower", false);
    apply_and_slice(model, print, config);

    const PrintObject *cube_po    = nullptr;
    const PrintObject *blocker_po = nullptr;
    for (const PrintObject *po : print.objects()) {
        if (po->model_object()->name == "cube.stl")
            cube_po = po;
        else
            blocker_po = po;
    }
    REQUIRE(cube_po != nullptr);
    REQUIRE(blocker_po != nullptr);
    REQUIRE(cube_po->instances().size() == 2);

    const PrintInstance *blocked_inst = nullptr;
    const PrintInstance *clear_inst   = nullptr;
    for (const PrintInstance &inst : cube_po->instances()) {
        const Vec2d xy = inst.model_instance->get_offset().head<2>();
        if ((xy - blocked_offset.head<2>()).norm() < 1e-4)
            blocked_inst = &inst;
        if ((xy - clear_offset.head<2>()).norm() < 1e-4)
            clear_inst = &inst;
    }
    REQUIRE(blocked_inst != nullptr);
    REQUIRE(clear_inst != nullptr);

    SeamPlacer placer;
    placer.init(print, []() {});
    auto seams = placer.m_seam_per_object.find(cube_po);
    REQUIRE(seams != placer.m_seam_per_object.end());
    Point   seam;
    Polygon contour;
    REQUIRE(first_layer_outer_seam(cube_po, seams->second, seam, contour));
    const ExPolygon island = first_layer_tower_island(cube_po, seam, contour);
    REQUIRE_FALSE(island.empty());

    ExPolygon blocked_world = island;
    blocked_world.translate(blocked_inst->shift);
    ExPolygon clear_world = island;
    clear_world.translate(clear_inst->shift);
    const ExPolygons blocker_world = slices_translated(blocker_po, blocker_po->instances().front().shift);
    const ExPolygons blocked_body  = slices_translated(cube_po, blocked_inst->shift);
    const ExPolygons clear_body    = slices_translated(cube_po, clear_inst->shift);
    REQUIRE_FALSE(intersection_ex(blocked_world, blocker_world).empty());
    REQUIRE(intersection_ex(clear_world, blocker_world).empty());
    REQUIRE(intersection_ex(blocked_world, clear_body).empty());
    REQUIRE(intersection_ex(clear_world, blocked_body).empty());

    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);
    REQUIRE(has_skip_warning(planner));
    REQUIRE(planner.warnings().size() == 1);

    const coord_t match = planner.match_distance();
    bool          clear_has_tower = false;
    for (int layer = 0; layer < int(cube_po->layer_count()); ++layer) {
        REQUIRE(planner.take_matching_tower(layer, seam, match, cube_po, blocked_inst->shift) == nullptr);
        REQUIRE(planner.take_unprinted_towers(layer, cube_po, blocked_inst->shift).empty());
        if (planner.take_matching_tower(layer, seam, match, cube_po, clear_inst->shift) != nullptr)
            clear_has_tower = true;
    }
    REQUIRE(clear_has_tower);
}

TEST_CASE("Blocked copy keeps first-layer inset start while the clear copy prints the seam tower", "[SeamTower][GCode]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> extra{
        {"seam_tower", 1},
        {"seam_position", "aligned"},
        {"wall_loops", 1},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
        {"seam_tower_length", 4.0},
        {"first_layer_inset_start", 1},
    };
    const DynamicPrintConfig config = slice_print_config(extra);
    const Vec3d               blocked_offset(50., 50., 0.);
    const Vec3d               clear_offset(140., 140., 0.);

    Vec3d  blocker_offset(0., 0., 0.);
    double blocker_sx = 20.;
    double blocker_sy = 4.;
    {
        Model model;
        Print print;
        ModelObject *object = model.add_object();
        object->name = "cube.stl";
        object->add_volume(make_cube(20., 20., 6.));
        object->add_instance();
        object->instances.front()->set_offset(blocked_offset);
        object->ensure_on_bed();
        apply_and_slice(model, print, config);

        const PrintObject *po = print.objects().front();
        SeamPlacer placer;
        placer.init(print, []() {});
        auto seams = placer.m_seam_per_object.find(po);
        REQUIRE(seams != placer.m_seam_per_object.end());
        Point   seam;
        Polygon contour;
        REQUIRE(first_layer_outer_seam(po, seams->second, seam, contour));
        const ExPolygon island = first_layer_tower_island(po, seam, contour);
        REQUIRE_FALSE(island.empty());

        const Point delta    = get_extents(island).center() - get_extents(contour).center();
        const bool  along_x  = std::llabs(static_cast<long long>(delta.x())) >= std::llabs(static_cast<long long>(delta.y()));
        const bool  positive = along_x ? delta.x() > 0 : delta.y() > 0;
        ExPolygon   world    = island;
        world.translate(po->instances().front().shift);
        const BoundingBox bb = get_extents(world);
        constexpr double thick = 4.;
        constexpr double span  = 20.;
        if (along_x) {
            blocker_sx     = thick;
            blocker_sy     = span;
            blocker_offset = Vec3d(positive ? unscale<double>(bb.min.x()) : unscale<double>(bb.max.x()) - thick,
                                   unscale<double>(bb.center().y()) - span / 2., 0.);
        } else {
            blocker_sx     = span;
            blocker_sy     = thick;
            blocker_offset = Vec3d(unscale<double>(bb.center().x()) - span / 2.,
                                   positive ? unscale<double>(bb.min.y()) : unscale<double>(bb.max.y()) - thick, 0.);
        }
    }

    Model model;
    Print print;
    ModelObject *cube = model.add_object();
    cube->name = "cube.stl";
    cube->add_volume(make_cube(20., 20., 6.));
    cube->add_instance()->set_offset(blocked_offset);
    cube->add_instance()->set_offset(clear_offset);
    cube->ensure_on_bed();

    ModelObject *blocker = model.add_object();
    blocker->name = "blocker.stl";
    blocker->add_volume(make_cube(blocker_sx, blocker_sy, 6.));
    blocker->add_instance();
    blocker->instances.front()->set_offset(blocker_offset);
    blocker->ensure_on_bed();
    blocker->config.set("seam_tower", false);
    apply_and_slice(model, print, config);

    const PrintObject *cube_po    = nullptr;
    const PrintObject *blocker_po = nullptr;
    for (const PrintObject *po : print.objects()) {
        if (po->model_object()->name == "cube.stl")
            cube_po = po;
        else
            blocker_po = po;
    }
    REQUIRE(cube_po != nullptr);
    REQUIRE(blocker_po != nullptr);

    const PrintInstance *blocked_inst = nullptr;
    const PrintInstance *clear_inst   = nullptr;
    for (const PrintInstance &inst : cube_po->instances()) {
        const Vec2d xy = inst.model_instance->get_offset().head<2>();
        if ((xy - blocked_offset.head<2>()).norm() < 1e-4)
            blocked_inst = &inst;
        if ((xy - clear_offset.head<2>()).norm() < 1e-4)
            clear_inst = &inst;
    }
    REQUIRE(blocked_inst != nullptr);
    REQUIRE(clear_inst != nullptr);
    const size_t blocked_id = blocked_inst->model_instance->get_labeled_id();
    const size_t clear_id   = clear_inst->model_instance->get_labeled_id();
    REQUIRE(blocked_id != clear_id);

    const std::string gcode = export_processed_gcode(print);
    // By-layer order prints every copy's first layer before the next layer, so the
    // first OBJECT_ID block for each copy is that copy's first layer.
    const std::string blocked_layer = gcode_for_object_id(gcode, blocked_id);
    const std::string clear_layer   = gcode_for_object_id(gcode, clear_id);
    REQUIRE_FALSE(blocked_layer.empty());
    REQUIRE_FALSE(clear_layer.empty());
    REQUIRE(contains_ci(blocked_layer, "inset start"));
    REQUIRE(contains_ci(clear_layer, "seam tower"));
    REQUIRE_FALSE(contains_ci(clear_layer, "inset start"));
}

TEST_CASE("Print-path emits a tower when instance shift includes a non-zero plate origin", "[SeamTower][GCode][Collision]")
{
    const Vec3d plate_origin(250., 0., 0.);
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_position", "aligned"},
        {"wall_loops", 1},
    });

    Model model;
    Print print;
    print.set_plate_origin(plate_origin);
    ModelObject *object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(make_cube(20., 20., 6.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.) + plate_origin);
    object->ensure_on_bed();
    apply_and_slice(model, print, config);

    const PrintObject   *po   = print.objects().front();
    const PrintInstance &inst = po->instances().front();
    const Point          plate = Point::new_scale(plate_origin.x(), plate_origin.y());
    REQUIRE(inst.shift - inst.shift_without_plate_offset() == plate);
    const BoundingBox bed(get_bed_shape(print.config()));
    const BoundingBox plate_local(Point::new_scale(50., 50.), Point::new_scale(70., 70.));
    REQUIRE(bed.defined);
    REQUIRE(bed.contains(plate_local));

    SeamPlacer placer;
    placer.init(print, []() {});
    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);
    REQUIRE_FALSE(planner.empty());
}

static void add_cube_with_back_shelf(Model &model, Print &print)
{
    ModelObject *object = model.add_object();
    object->name = "cube-shelf.stl";
    object->add_volume(make_cube(20., 20., 8.));
    ModelVolume *shelf = object->add_volume(make_cube(20., 8., 2.));
    shelf->set_offset(Vec3d(0., 20., 3.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
}

static bool print_has_support_islands(const Print &print)
{
    for (const PrintObject *po : print.objects()) {
        for (const SupportLayer *sl : po->support_layers()) {
            if (!sl->support_islands.empty())
                return true;
        }
    }
    return false;
}

TEST_CASE("Overhang supports skip the colliding seam tower", "[SeamTower][GCode][Collision]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> seam_opts{
        {"seam_tower", 1},
        {"seam_position", "back"},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
        {"support_type", "normal(auto)"},
        {"support_on_build_plate_only", 1},
        {"support_object_xy_distance", 0.2},
    };

    Model model_off;
    Print print_off;
    add_cube_with_back_shelf(model_off, print_off);
    print_off.apply(model_off, slice_print_config(seam_opts));
    const std::string without = export_processed_gcode(print_off);

    Model model_on;
    Print print_on;
    add_cube_with_back_shelf(model_on, print_on);
    DynamicPrintConfig config_on = slice_print_config(seam_opts);
    config_on.set_deserialize_strict({{"enable_support", 1}});
    print_on.apply(model_on, config_on);
    print_on.validate();
    print_on.set_status_silent();
    print_on.process();
    REQUIRE(print_has_support_islands(print_on));
    const std::string with_supports = export_processed_gcode(print_on);

    const int without_n = count_substr(without, " FEATURE: Seam tower");
    const int with_n    = count_substr(with_supports, " FEATURE: Seam tower");
    REQUIRE(without_n > 0);
    REQUIRE(with_n < without_n);
}

TEST_CASE("Stepped wall keeps a seam tower when the gap clears support expansion", "[SeamTower][GCode][Collision]")
{
    // Foot 20 mm, wall 28 mm from z = 2, so the wall overhangs by 4 mm. Supports
    // expand 1 mm past that wall. The tower gap is 2 mm, so the pad beside the
    // wall is clear of the supports under the step.
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "step.stl";
    // add_volume centers the mesh, so the offset is the center. The foot sits on
    // the bed; the wall starts 2 mm up and overhangs it by 4 mm on every side.
    ModelVolume *foot = object->add_volume(make_cube(20., 20., 2.));
    foot->set_offset(Vec3d(0., 0., 1.));
    ModelVolume *wall = object->add_volume(make_cube(28., 28., 8.));
    wall->set_offset(Vec3d(0., 0., 6.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);

    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_position", "aligned"},
        {"seam_tower_gap", 2.0},
        {"seam_tower_depth", 5.0},
        {"seam_tower_length", 5.0},
        {"support_type", "normal(auto)"},
        {"support_style", "snug"},
        {"support_expansion", 1.0},
        {"support_object_xy_distance", 0.35},
        {"raft_first_layer_expansion", 0},
        {"enable_support", 1},
    });
    print.apply(model, config);
    const std::string gcode = export_processed_gcode(print);
    REQUIRE(print_has_support_islands(print));
    REQUIRE(count_substr(gcode, " FEATURE: Seam tower") > 0);
}

static std::vector<const ExtrusionLoop *> loops_of(const ExtrusionEntityCollection &coll)
{
    std::vector<const ExtrusionLoop *> loops;
    for (const ExtrusionEntity *entity : coll.entities) {
        if (entity != nullptr && entity->is_loop())
            loops.push_back(static_cast<const ExtrusionLoop *>(entity));
    }
    return loops;
}

static double loop_abs_area(const ExtrusionLoop &loop)
{
    return std::abs(loop.polygon().area());
}

static void require_outer_loop_split_at_wall_start(const ExtrusionLoop &loop, const Point &wall_start)
{
    const Polygon centerline = loop.polygon();
    const Point   closest    = centerline.point_projection(wall_start);
    REQUIRE(loop.paths.front().polyline.points.front() == closest);
    REQUIRE(loop.paths.back().polyline.points.back() == closest);
}

TEST_CASE("Seam tower fill emits the innermost loop first and the outermost loop last", "[SeamTower]")
{
    const Point seam = mm(10.0, 0.0);
    const Polygon contour = square_contour_mm(40.0);
    SeamTowerParams params = enabled_params();
    params.depth  = scale_(8.0);
    params.length = scale_(20.0);

    SeamTowerPlanner planner;
    planner.build({
        SeamTowerBuildInput{0, 0, seam, contour, false},
        SeamTowerBuildInput{0, 1, seam, contour, false},
    }, params);

    const ExtrusionEntityCollection *coll = planner.collection_at_layer(1);
    REQUIRE(coll != nullptr);
    const std::vector<const ExtrusionLoop *> loops = loops_of(*coll);
    REQUIRE(loops.size() == 2);
    REQUIRE(loop_abs_area(*loops[1]) > loop_abs_area(*loops[0]));
}

TEST_CASE("Seam tower fill does not add a third wall when depth increases", "[SeamTower]")
{
    const Point seam = mm(10.0, 0.0);
    const Polygon contour = square_contour_mm(40.0);

    auto loop_count_on_upper_layer = [&](double depth_mm) {
        SeamTowerParams params = enabled_params();
        params.depth  = scale_(depth_mm);
        params.length = scale_(20.0);
        SeamTowerPlanner planner;
        planner.build({
            SeamTowerBuildInput{0, 0, seam, contour, false},
            SeamTowerBuildInput{0, 1, seam, contour, false},
        }, params);
        const ExtrusionEntityCollection *coll = planner.collection_at_layer(1);
        REQUIRE(coll != nullptr);
        return loops_of(*coll).size();
    };

    REQUIRE(loop_count_on_upper_layer(8.0) == 2);
    REQUIRE(loop_count_on_upper_layer(16.0) == 2);
}

static double extra_fill_width_from_inner_wall_mm(const std::vector<const ExtrusionLoop *> &loops)
{
    if (loops.size() <= 2)
        return 0.0;
    const BoundingBox inner_wall = get_extents(loops[loops.size() - 2]->polygon());
    const BoundingBox innermost  = get_extents(loops.front()->polygon());
    const auto inner_sz = inner_wall.size();
    const auto extra_sz = innermost.size();
    const double inner_min = unscale<double>(std::min(inner_sz.x(), inner_sz.y()));
    const double extra_min = unscale<double>(std::min(extra_sz.x(), extra_sz.y()));
    return 0.5 * (inner_min - extra_min);
}

TEST_CASE("First-layer seam tower keeps an internal brim up to 5 mm", "[SeamTower]")
{
    const Point   seam    = mm(10.0, 0.0);
    const Polygon contour = square_contour_mm(40.0);
    SeamTowerParams params = enabled_params();
    params.depth  = scale_(16.0);
    params.length = scale_(20.0);
    const double lw_mm = unscale<double>(params.line_width);

    SeamTowerPlanner planner;
    planner.build({
        SeamTowerBuildInput{0, 0, seam, contour, false},
        SeamTowerBuildInput{0, 1, seam, contour, false},
    }, params);

    const ExtrusionEntityCollection *first = planner.collection_at_layer(0);
    const ExtrusionEntityCollection *next  = planner.collection_at_layer(1);
    REQUIRE(first != nullptr);
    REQUIRE(next != nullptr);

    const std::vector<const ExtrusionLoop *> first_loops = loops_of(*first);
    const std::vector<const ExtrusionLoop *> next_loops  = loops_of(*next);
    REQUIRE(next_loops.size() == 2);
    REQUIRE(first_loops.size() > next_loops.size());

    const double extra_fill = extra_fill_width_from_inner_wall_mm(first_loops);
    REQUIRE(extra_fill > 5.0 - lw_mm);
    REQUIRE(extra_fill <= 5.0);

    const BoundingBox innermost = get_extents(first_loops.front()->polygon());
    const auto innermost_sz = innermost.size();
    const double innermost_min_mm = unscale<double>(std::min(innermost_sz.x(), innermost_sz.y()));
    REQUIRE(innermost_min_mm > 2.0 * lw_mm);

    for (size_t i = 1; i < first_loops.size(); ++i)
        REQUIRE(loop_abs_area(*first_loops[i]) > loop_abs_area(*first_loops[i - 1]));
    require_outer_loop_split_at_wall_start(*first_loops.back(), seam);
}

TEST_CASE("First-layer seam tower adds no extra loops when two walls fill the island", "[SeamTower]")
{
    const Point   seam    = mm(10.0, 0.0);
    const Polygon contour = square_contour_mm(40.0);
    SeamTowerParams params = enabled_params();
    params.depth  = scale_(2.0);
    params.length = scale_(20.0);

    SeamTowerPlanner planner;
    planner.build({
        SeamTowerBuildInput{0, 0, seam, contour, false},
        SeamTowerBuildInput{0, 1, seam, contour, false},
    }, params);

    const ExtrusionEntityCollection *first = planner.collection_at_layer(0);
    const ExtrusionEntityCollection *next  = planner.collection_at_layer(1);
    REQUIRE(first != nullptr);
    REQUIRE(next != nullptr);
    REQUIRE(loops_of(*first).size() == 2);
    REQUIRE(loops_of(*next).size() == 2);
}

TEST_CASE("Seam tower outer loop starts and ends at the closest point to the wall start", "[SeamTower]")
{
    const Point seam = mm(10.0, 0.0);
    SeamTowerParams params = enabled_params();
    params.depth = scale_(3.0);

    SeamTowerPlanner planner;
    planner.build({SeamTowerBuildInput{0, 0, seam, square_contour_mm(20.0), false}}, params);

    const ExtrusionEntityCollection *coll = planner.collection_at_layer(0);
    REQUIRE(coll != nullptr);
    const std::vector<const ExtrusionLoop *> loops = loops_of(*coll);
    REQUIRE(loops.size() >= 2);
    require_outer_loop_split_at_wall_start(*loops.back(), seam);
}

TEST_CASE("Seam tower one line width deep emits one wall", "[SeamTower]")
{
    const Point seam = mm(10.0, 0.0);
    SeamTowerParams params = enabled_params();
    params.depth      = scale_(0.6);
    params.line_width = scale_(0.4);

    SeamTowerPlanner planner;
    planner.build({SeamTowerBuildInput{0, 0, seam, square_contour_mm(20.0), false}}, params);

    const ExtrusionEntityCollection *coll = planner.collection_at_layer(0);
    REQUIRE(coll != nullptr);
    const std::vector<const ExtrusionLoop *> loops = loops_of(*coll);
    REQUIRE(loops.size() == 1);
    require_outer_loop_split_at_wall_start(*loops.front(), seam);
}

TEST_CASE("Seam tower narrower than one line width stays empty", "[SeamTower]")
{
    SeamTowerParams params = enabled_params();
    params.depth      = scale_(0.3);
    params.line_width = scale_(0.4);

    SeamTowerPlanner planner;
    planner.build({SeamTowerBuildInput{0, 0, mm(10.0, 0.0), square_contour_mm(20.0), false}}, params);

    REQUIRE(planner.empty());
    REQUIRE(planner.collection_at_layer(0) == nullptr);
}

static double first_layer_seam_tower_max_y(const std::string &gcode)
{
    double z        = 0.;
    double y        = 0.;
    double max_y    = -1e9;
    bool   have_z   = false;
    bool   have_y   = false;
    bool   in_tower = false;
    for (const std::string &line : split_gcode_lines(gcode)) {
        if (line.find("FEATURE:") != std::string::npos)
            in_tower = line.find("Seam tower") != std::string::npos;
        const std::string code = gcode_without_comment(line);
        if (!gcode_is_motion(code))
            continue;
        if (const std::optional<double> zz = gcode_axis_value(code, 'Z')) {
            z      = *zz;
            have_z = true;
        }
        if (const std::optional<double> yy = gcode_axis_value(code, 'Y')) {
            y      = *yy;
            have_y = true;
        }
        if (in_tower && have_z && have_y && z < 1.0 && gcode_axis_value(code, 'E'))
            max_y = std::max(max_y, y);
    }
    return max_y;
}

TEST_CASE("An overhang seam still prints its tower pad on the first layer", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({{"seam_tower", 1}, {"seam_position", "back"}});
    Model              model;
    Print              print;
    ModelObject       *object = model.add_object();
    object->name              = "overhang-seam.stl";
    object->add_volume(make_cube(20., 20., 4.));
    ModelVolume *top = object->add_volume(make_cube(10., 10., 8.));
    top->set_offset(Vec3d(0., 18., 4.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const std::string gcode = export_processed_gcode(print);
    // Instance is at y=50. The base ends at y=70, so its own tower stays near y=72.
    // The overhang's back is past that, and its pad on the first layer reaches ~75.
    const double max_y = first_layer_seam_tower_max_y(gcode);
    REQUIRE(max_y > 73.5);
}

static bool tower_layer_has_y(const SeamTowerPlanner &planner, int layer, double ymin_mm, double ymax_mm)
{
    const auto collections = planner.layer_collections();
    const auto it          = collections.find(layer);
    if (it == collections.end())
        return false;
    Points pts;
    it->second.collect_points(pts);
    for (const Point &p : pts) {
        const double y = unscale<double>(p.y());
        if (y >= ymin_mm && y <= ymax_mm)
            return true;
    }
    return false;
}

TEST_CASE("Internal fins do not steal the outer tower or grow into the wall", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_tower_in_holes", 1},
        {"seam_position", "back"},
        {"wall_loops", 2},
    });
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "hex-hole.stl";
    object->add_volume(make_cylinder(25., 10., 2. * PI / 6.));
    object->add_volume(make_cylinder(16., 12., 2. * PI / 6.), ModelVolumeType::NEGATIVE_VOLUME);
    // Rear face at y=14.5, 1.5 mm short of the hole vertex at y=16, so a 2 mm tower hits the wall.
    ModelVolume *outward = object->add_volume(make_cube(4., 4., 10.));
    outward->set_offset(Vec3d(0., 12.5, 5.));
    // Only the top 4 mm. Rear seam points toward the hole center, with room for a tower.
    ModelVolume *center = object->add_volume(make_cube(4., 4., 4.));
    center->set_offset(Vec3d(0., -8., 8.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(80., 80., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject *po = print.objects().front();
    SeamPlacer placer;
    placer.init(print, []() {});
    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);

    const int top = int(po->layer_count()) - 1;
    REQUIRE(top > 40);
    // Outer vertex seam keeps its tower from the bed through the last layer.
    REQUIRE(tower_layer_has_y(planner, 0, 24.0, 30.0));
    REQUIRE(tower_layer_has_y(planner, top, 24.0, 30.0));
    // Outward fin and the facing hole wall must not print a tower into the shell.
    REQUIRE_FALSE(tower_layer_has_y(planner, 0, 13.0, 20.0));
    REQUIRE_FALSE(tower_layer_has_y(planner, top, 13.0, 20.0));
    // A seam that starts partway up is grounded on the first layer, or skipped
    // entirely when that column cannot be built.
    REQUIRE(tower_layer_has_y(planner, 0, -20.0, -1.0) == tower_layer_has_y(planner, top, -20.0, -1.0));
}

TEST_CASE("A thin shell keeps the outer seam tower when walls are outer then inner", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_position", "back"},
        {"wall_loops", 2},
        {"wall_sequence", "outer wall/inner wall"},
    });
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "thin-hex.stl";
    object->add_volume(make_cylinder(12., 4., 2. * PI / 6.));
    object->add_volume(make_cylinder(11.2, 6., 2. * PI / 6.), ModelVolumeType::NEGATIVE_VOLUME);
    // Faces the shell. Its tower must not be printed into that wall.
    ModelVolume *fin = object->add_volume(make_cube(2., 2., 4.));
    fin->set_offset(Vec3d(0., 8., 2.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(80., 80., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    SeamPlacer placer;
    placer.init(print, []() {});
    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);

    const int top = int(print.objects().front()->layer_count()) - 1;
    REQUIRE(tower_layer_has_y(planner, 0, 11.0, 16.0));
    REQUIRE(tower_layer_has_y(planner, top, 11.0, 16.0));
    REQUIRE_FALSE(tower_layer_has_y(planner, 0, 8.5, 11.2));
}

TEST_CASE("A textured back face keeps its tower when upper layers stick out", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_position", "back"},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
    });
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "textured-back.stl";
    object->add_volume(make_cube(20., 20., 6.));
    // make_cube starts at the origin and centers in place, so the cube occupies
    // [0,20]×[0,20]×[0,6]. The rib is only on the top 2 mm and sticks 1 mm past the back.
    ModelVolume *rib = object->add_volume(make_cube(24., 1., 2.));
    rib->set_offset(Vec3d(10., 20.5, 5.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    SeamPlacer placer;
    placer.init(print, []() {});
    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);
    // The lower wall is clear. The rib only exists above it, so the tower stays.
    REQUIRE(tower_layer_has_y(planner, 0, 10.2, 14.0));
}

// Same order as get_wipe_tower_extrusions_extents: rotate the local footprint
// about its origin, then translate by the tower position.
static Polygon prime_tower_footprint_world(const Polygon &local, double angle_deg, const Point &tower_shift, bool rotate_local)
{
    Polygon poly = local;
    if (rotate_local)
        poly.rotate(Geometry::deg2rad(angle_deg));
    poly.translate(tower_shift);
    return poly;
}

static bool polygons_overlap(const ExPolygon &island, const Polygon &poly)
{
    return poly.size() >= 3 && !island.empty() && !intersection_ex(island, ExPolygons{ExPolygon{poly}}).empty();
}

static const PrintObject *find_print_object(const Print &print, const std::string &name)
{
    for (const PrintObject *po : print.objects())
        if (po->model_object() != nullptr && po->model_object()->name == name)
            return po;
    return nullptr;
}

static bool object_tower_world(const Print &print, const SeamPlacer &placer, const std::string &name, ExPolygon &world, Point &shift)
{
    const PrintObject *po = find_print_object(print, name);
    if (po == nullptr || po->instances().empty())
        return false;
    const auto seams = placer.m_seam_per_object.find(po);
    if (seams == placer.m_seam_per_object.end())
        return false;
    Point   seam;
    Polygon contour;
    if (!first_layer_outer_seam(po, seams->second, seam, contour))
        return false;
    world = first_layer_tower_island(po, seam, contour);
    if (world.empty())
        return false;
    shift = po->instances().front().shift;
    world.translate(shift);
    return true;
}

static DynamicPrintConfig rotated_prime_tower_config(double tower_x, double tower_y)
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_position", "back"},
        {"wall_loops", 1},
        {"seam_tower_gap", 0.1},
        {"seam_tower_depth", 2.0},
        {"seam_tower_length", 4.0},
        {"enable_prime_tower", 1},
        {"prime_tower_rib_wall", 0},
        {"prime_tower_brim_width", 0},
        {"prime_tower_width", 20},
        {"wipe_tower_rotation_angle", 90},
    });
    config.set_num_filaments(2);
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FF0000", "#00FF00"});
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PLA"});
    config.set_key_value("filament_prime_volume", new ConfigOptionFloats{80., 80.});
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(TimelapseType::tlSmooth));
    config.option<ConfigOptionFloats>("wipe_tower_x")->values = {tower_x};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values = {tower_y};
    return config;
}

struct PlacedBox
{
    const char *name;
    Vec3d       offset;
    int         extruder;
    bool        seam_tower;
};

static void slice_prime_scene(Print &print, Model &model, const DynamicPrintConfig &config, const std::vector<PlacedBox> &boxes)
{
    for (const PlacedBox &box : boxes) {
        ModelObject *object = model.add_object();
        object->name        = box.name;
        object->add_volume(make_cube(10., 10., 6.));
        object->add_instance();
        object->instances.front()->set_offset(box.offset);
        // A second filament has to be printed or the prime tower is turned off. That
        // filament's own first layer comes out empty, which aborts slicing, so the
        // tool-change cube rides on an extruder-1 part that does fill layer 0.
        object->volumes.front()->config.set("extruder", 1);
        object->config.set_key_value("extruder", new ConfigOptionInt(1));
        if (box.extruder != 1) {
            ModelVolume *toolchange = object->add_volume(make_cube(8., 8., 6.));
            toolchange->set_offset(Vec3d(18., 0., 0.));
            toolchange->config.set("extruder", box.extruder);
        }
        object->ensure_on_bed();
        object->config.set("seam_tower", box.seam_tower);
        print.auto_assign_extruders(object);
    }
    print.apply(model, config);
    const StringObjectException valid = print.validate();
    INFO(valid.string);
    REQUIRE(valid.string.empty());
    print.set_status_silent();
    print.process();
}

static Vec3d offset_placing_island_at(const Point &rel_min, const Point &rel_max, const Point &shift_bias, const Point &local_center,
                                      const Point &tower_shift)
{
    const Point rel_center    = Point((rel_min.x() + rel_max.x()) / 2, (rel_min.y() + rel_max.y()) / 2);
    const Point desired_shift = local_center + tower_shift - rel_center;
    const Point offset_pt     = desired_shift - shift_bias;
    return Vec3d(unscale<double>(offset_pt.x()), unscale<double>(offset_pt.y()), 0.);
}

// psWipeTower has finished: the mesh footprint is local, and a 90 degree tower
// must be tested after that rotation. A seam tower that only covers the
// unrotated translation of the same footprint has to stay.
TEST_CASE("Rotated prime-tower mesh footprint skips only the tower on the rotated box", "[SeamTower][GCode][Collision]")
{
    constexpr double     tower_x = 120.;
    constexpr double     tower_y = 70.;
    const DynamicPrintConfig config = rotated_prime_tower_config(tower_x, tower_y);
    const Point          tower_shift = Point(scale_(tower_x), scale_(tower_y));

    Point   rel_min;
    Point   rel_max;
    Point   shift_bias;
    Polygon probed_bottom;
    {
        constexpr double probe_x = 170.;
        constexpr double probe_y = 40.;
        Model            model;
        Print            print;
        slice_prime_scene(print, model, config, {
            {"probe", Vec3d(probe_x, probe_y, 0.), 1, true},
            {"donor", Vec3d(40., 150., 0.), 2, false},
        });
        REQUIRE(print.has_wipe_tower());
        REQUIRE(print.is_step_done(psWipeTower));
        REQUIRE(print.wipe_tower_data().wipe_tower_mesh_data.has_value());
        probed_bottom = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
        REQUIRE(probed_bottom.size() >= 3);

        SeamPlacer placer;
        placer.init(print, []() {});
        ExPolygon world;
        Point     shift;
        REQUIRE(object_tower_world(print, placer, "probe", world, shift));
        shift_bias = shift - Point::new_scale(probe_x, probe_y);
        const BoundingBox bb = get_extents(world);
        rel_min = bb.min - shift;
        rel_max = bb.max - shift;
    }

    Polygon rotated_local = probed_bottom;
    rotated_local.rotate(Geometry::deg2rad(90.));
    const Vec3d hit_offset  = offset_placing_island_at(rel_min, rel_max, shift_bias, get_extents(rotated_local).center(), tower_shift);
    const Vec3d keep_offset = offset_placing_island_at(rel_min, rel_max, shift_bias, get_extents(probed_bottom).center(), tower_shift);

    auto run_subject = [&](const Vec3d &subject_offset, bool expect_skip) {
        Model model;
        Print print;
        slice_prime_scene(print, model, config, {
            {"subject", subject_offset, 1, true},
            {"donor", Vec3d(40., 150., 0.), 2, false},
        });
        REQUIRE(print.is_step_done(psWipeTower));
        REQUIRE(print.wipe_tower_data().wipe_tower_mesh_data.has_value());
        const Polygon &bottom = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
        const Polygon  correct = prime_tower_footprint_world(bottom, print.config().wipe_tower_rotation_angle.value, tower_shift, true);
        const Polygon  unrotated = prime_tower_footprint_world(bottom, print.config().wipe_tower_rotation_angle.value, tower_shift, false);
        const BoundingBox correct_bb   = get_extents(correct);
        const BoundingBox unrotated_bb = get_extents(unrotated);
        REQUIRE((correct_bb.min != unrotated_bb.min || correct_bb.max != unrotated_bb.max));

        SeamPlacer placer;
        placer.init(print, []() {});
        ExPolygon world;
        Point     shift;
        REQUIRE(object_tower_world(print, placer, "subject", world, shift));
        const BoundingBox island_bb = get_extents(world);
        INFO("subject " << subject_offset.x() << "," << subject_offset.y()
             << " island " << unscale<double>(island_bb.min.x()) << "," << unscale<double>(island_bb.min.y())
             << " .. " << unscale<double>(island_bb.max.x()) << "," << unscale<double>(island_bb.max.y()));
        if (expect_skip) {
            REQUIRE(polygons_overlap(world, correct));
            REQUIRE_FALSE(polygons_overlap(world, unrotated));
        } else {
            REQUIRE(polygons_overlap(world, unrotated));
            REQUIRE_FALSE(polygons_overlap(world, correct));
        }

        SeamTowerParams params;
        params.enabled = true;
        SeamTowerPlanner planner;
        planner.build(print, placer, params);
        if (expect_skip) {
            REQUIRE(planner.empty());
            REQUIRE(has_skip_warning(planner));
        } else {
            REQUIRE_FALSE(planner.empty());
            REQUIRE(planner.warnings().empty());
        }
    };

    run_subject(hit_offset, true);
    run_subject(keep_offset, false);
}

static std::string feature_trace(const std::string &layer)
{
    std::string       out;
    const std::string tag = "FEATURE: ";
    size_t            pos = 0;
    int               n   = 0;
    while ((pos = layer.find(tag, pos)) != std::string::npos && n < 16) {
        const size_t eol = layer.find('\n', pos);
        out += layer.substr(pos, (eol == std::string::npos ? layer.size() : eol) - pos);
        out += " | ";
        pos = (eol == std::string::npos) ? layer.size() : eol + 1;
        ++n;
    }
    return out;
}

static std::vector<std::string> split_on_layer_z(const std::string &gcode)
{
    std::vector<size_t> z_lines;
    size_t              pos = 0;
    while (pos < gcode.size()) {
        const size_t nl  = gcode.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? gcode.size() : nl;
        const std::string line = gcode.substr(pos, end - pos);
        if (line.rfind("G1 Z", 0) == 0 && line.find('X') == std::string::npos && line.find('Y') == std::string::npos)
            z_lines.push_back(pos);
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }

    // A lift and the real layer height are consecutive Z moves. Keep the last one.
    std::vector<size_t> layers;
    for (size_t i = 0; i < z_lines.size(); ++i) {
        const bool last_of_run = (i + 1 == z_lines.size())
            || gcode.substr(z_lines[i], z_lines[i + 1] - z_lines[i]).find("FEATURE:") != std::string::npos;
        if (last_of_run)
            layers.push_back(z_lines[i]);
    }

    std::vector<std::string> out;
    for (size_t i = 0; i < layers.size(); ++i) {
        const size_t end = (i + 1 < layers.size()) ? layers[i + 1] : gcode.size();
        out.push_back(gcode.substr(layers[i], end - layers[i]));
    }
    return out;
}

static size_t first_feature(const std::string &layer, std::string_view name)
{
    return layer.find(std::string("FEATURE: ") + std::string(name));
}

// Infill is filament 1, which is not a component of the mixed wall slot. Walls stay
// on that slot, so an infill visit must not print the seam tower.
static void apply_infill_then_wall(Model &model, Print &print)
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_position", "aligned"},
        {"wall_loops", 2},
        {"sparse_infill_density", 20},
        {"bottom_shell_layers", 1},
        {"top_shell_layers", 1},
        {"enable_prime_tower", 0},
        {"single_extruder_multi_material", 1},
        {"enable_mixed_color_sublayer", 1},
    });
    config.set_num_filaments(4);
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75, 1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FF0000", "#00FF00", "#0000FF", "#FFFF00"});
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA", "PLA", "PLA", "PLA"});
    config.set_key_value("filament_vendor", new ConfigOptionStrings{"Generic", "Generic", "Generic", "Generic"});
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings{"", "", "", ""});
    config.set_key_value("filament_end_gcode", new ConfigOptionStrings{"", "", "", ""});
    config.set_key_value("filament_shrink", new ConfigOptionPercents{100, 100, 100, 100});
    config.set_key_value("filament_map", new ConfigOptionInts{1, 1, 1, 1});
    config.set_key_value("filament_is_mixed", new ConfigOptionBools{false, false, false, true});
    config.set_key_value("filament_mixed_components", new ConfigOptionStrings{"", "", "", "2,3"});
    config.set_key_value("filament_mixed_sublayer_ratios", new ConfigOptionStrings{"", "", "", "0.5,0.5"});
    config.set_key_value("filament_mixed_gradient", new ConfigOptionBools{false, false, false, true});
    config.set_key_value("filament_mixed_gradient_range", new ConfigOptionStrings{"", "", "", "0.20,0.80"});
    std::vector<double> flush(16, 140.);
    for (int i = 0; i < 4; ++i)
        flush[i * 4 + i] = 0.;
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush));
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(TimelapseType::tlSmooth));

    ModelObject *object = model.add_object();
    object->name        = "mixed-wall-cube";
    object->add_volume(make_cube(20., 20., 4.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(20., 20., 0.));
    object->ensure_on_bed();
    object->config.set("seam_tower", true);
    ModelVolume *volume = object->volumes.front();
    volume->config.set_key_value("wall_filament", new ConfigOptionInt(4));
    volume->config.set_key_value("sparse_infill_filament", new ConfigOptionInt(1));
    volume->config.set_key_value("solid_infill_filament", new ConfigOptionInt(1));
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const StringObjectException valid = print.validate();
    INFO(valid.string);
    REQUIRE(valid.string.empty());
}

static std::string slice_infill_then_wall()
{
    Model model;
    Print print;
    apply_infill_then_wall(model, print);
    return export_processed_gcode(print);
}

// Same visits as slice_infill_then_wall, then every outer-wall path is
// erOverhangPerimeter. The later visit still extrudes that wall; the earlier
// infill visit must not emit the pad.
static std::string slice_infill_then_all_overhang_wall()
{
    Model model;
    Print print;
    apply_infill_then_wall(model, print);
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().front()->layer_count() > 0);
    REQUIRE(retag_external_loops_all_overhang(print) > 0);
    REQUIRE_FALSE(perimeters_have_role(print, erExternalPerimeter));
    REQUIRE(perimeters_have_role(print, erOverhangPerimeter));
    return export_gcode_file(print);
}

TEST_CASE("Earlier infill visit does not emit the seam tower before the mixed outer wall", "[SeamTower][GCode]")
{
    const std::string gcode = slice_infill_then_wall();
    REQUIRE(contains_ci(gcode, "seam tower"));

    int  infill_before_wall = 0;
    int  checked            = 0;
    for (const std::string &layer : split_on_layer_z(gcode)) {
        const size_t wall = first_feature(layer, "Outer wall");
        if (wall == std::string::npos)
            continue;
        const size_t sparse = first_feature(layer, "Sparse infill");
        const size_t solid  = first_feature(layer, "Internal solid infill");
        size_t       infill = std::string::npos;
        if (sparse != std::string::npos)
            infill = sparse;
        if (solid != std::string::npos && (infill == std::string::npos || solid < infill))
            infill = solid;
        if (infill == std::string::npos || infill > wall)
            continue;
        ++infill_before_wall;

        const size_t tower = first_feature(layer, "Seam tower");
        INFO(feature_trace(layer));
        REQUIRE(tower != std::string::npos);
        REQUIRE(tower > infill);
        REQUIRE(tower < wall);
        const std::string between = layer.substr(tower, wall - tower);
        // Unmatched pads are extruded on the infill visit and never hop.
        REQUIRE(contains_ci(between, "travel to wall"));
        REQUIRE(gcode_has_e_retract(between));
        ++checked;
    }
    REQUIRE(infill_before_wall > 0);
    REQUIRE(checked > 0);
}

// The later visit's outer wall has no erExternalPerimeter path. Pairing still
// hops; an earlier infill visit must not take the tower as an unmatched pad.
TEST_CASE("Earlier infill visit pairs the seam tower on a later all-overhang outer wall", "[SeamTower][GCode]")
{
    const std::string gcode = slice_infill_then_all_overhang_wall();
    REQUIRE(contains_ci(gcode, "seam tower"));
    REQUIRE(gcode.find("FEATURE: Outer wall") == std::string::npos);

    int checked = 0;
    for (const std::string &layer : split_on_layer_z(gcode)) {
        const size_t wall = first_feature(layer, "Overhang wall");
        if (wall == std::string::npos)
            continue;
        const size_t sparse = first_feature(layer, "Sparse infill");
        const size_t solid  = first_feature(layer, "Internal solid infill");
        size_t       infill = std::string::npos;
        if (sparse != std::string::npos)
            infill = sparse;
        if (solid != std::string::npos && (infill == std::string::npos || solid < infill))
            infill = solid;
        if (infill == std::string::npos || infill > wall)
            continue;

        const size_t tower = first_feature(layer, "Seam tower");
        INFO(feature_trace(layer));
        REQUIRE(tower != std::string::npos);
        REQUIRE(tower > infill);
        REQUIRE(tower < wall);
        const std::string between = layer.substr(tower, wall - tower);
        REQUIRE(contains_ci(between, "travel to wall"));
        REQUIRE(gcode_has_e_retract(between));
        ++checked;
    }
    REQUIRE(checked > 0);
}

// Default order is inner then outer. Both seams land on the same corner, so the
// inner start is one wall inset on both axes. gap + line width covers that diagonal
// and would let the inner loop take the tower.
static std::string slice_cube_with_all_overhang_second_perimeter()
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_tower_gap", 0.2},
        {"wall_loops", 2},
        {"wall_sequence", "inner wall/outer wall"},
        // Classic stamps elrSecondPerimeter on depth == 1. Arachne is a separate test.
        {"wall_generator", "classic"},
    });
    Model        model;
    Print        print;
    ModelObject *object = model.add_object();
    object->name        = "cube.stl";
    object->add_volume(make_cube(20., 20., 2.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(0., 0., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().front()->layer_count() > 0);
    REQUIRE(retag_second_perimeter_loops_all_overhang(print) > 0);
    REQUIRE(perimeters_have_role(print, erExternalPerimeter));
    REQUIRE(perimeters_have_role(print, erOverhangPerimeter));
    return export_gcode_file(print);
}

TEST_CASE("All-overhang second perimeter does not take the outer wall seam tower", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube_with_all_overhang_second_perimeter();
    REQUIRE(contains_ci(gcode, "seam tower"));
    const size_t first_overhang = gcode.find(" FEATURE: Overhang wall");
    const size_t first_tower    = gcode.find(" FEATURE: Seam tower");
    REQUIRE(first_overhang != std::string::npos);
    REQUIRE(first_tower != std::string::npos);
    // The inner wall is extruded first and must not consume the tower.
    REQUIRE(first_overhang < first_tower);

    int checked = 0;
    size_t pos  = 0;
    while ((pos = gcode.find(" FEATURE: Seam tower", pos)) != std::string::npos) {
        const size_t next = gcode.find(" FEATURE: ", pos + 1);
        REQUIRE(next != std::string::npos);
        REQUIRE(gcode.compare(next, 20, " FEATURE: Outer wall") == 0);
        const std::string between = gcode.substr(pos, next - pos);
        REQUIRE(contains_ci(between, "travel to wall"));
        REQUIRE(gcode_has_e_retract(between));
        ++checked;
        pos = next;
    }
    REQUIRE(checked > 0);
}

// Same inset as the classic case, but the loop role comes from Arachne.
// An all-overhang inner wall must not consume the tower the outer wall hops from.
static std::string slice_cube_with_all_overhang_arachne_inner_wall()
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_tower_gap", 0.2},
        {"wall_loops", 2},
        {"wall_sequence", "inner wall/outer wall"},
        {"wall_generator", "arachne"},
    });
    Model        model;
    Print        print;
    ModelObject *object = model.add_object();
    object->name        = "cube.stl";
    object->add_volume(make_cube(20., 20., 2.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(0., 0., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().front()->layer_count() > 0);
    REQUIRE(retag_non_external_perimeter_loops_all_overhang(print) > 0);
    REQUIRE(perimeters_have_role(print, erExternalPerimeter));
    REQUIRE(perimeters_have_role(print, erOverhangPerimeter));
    return export_gcode_file(print);
}

TEST_CASE("All-overhang inner Arachne wall does not take the outer wall seam tower", "[SeamTower][GCode]")
{
    const std::string gcode = slice_cube_with_all_overhang_arachne_inner_wall();
    REQUIRE(contains_ci(gcode, "seam tower"));
    const size_t first_overhang = gcode.find(" FEATURE: Overhang wall");
    const size_t first_tower    = gcode.find(" FEATURE: Seam tower");
    REQUIRE(first_overhang != std::string::npos);
    REQUIRE(first_tower != std::string::npos);
    // The inner wall is extruded first and must not consume the tower.
    REQUIRE(first_overhang < first_tower);

    int    checked = 0;
    size_t pos     = 0;
    while ((pos = gcode.find(" FEATURE: Seam tower", pos)) != std::string::npos) {
        const size_t next = gcode.find(" FEATURE: ", pos + 1);
        REQUIRE(next != std::string::npos);
        REQUIRE(gcode.compare(next, 20, " FEATURE: Outer wall") == 0);
        const std::string between = gcode.substr(pos, next - pos);
        REQUIRE(contains_ci(between, "travel to wall"));
        REQUIRE(gcode_has_e_retract(between));
        ++checked;
        pos = next;
    }
    REQUIRE(checked > 0);
}

struct GcodeFeatureMark
{
    std::string name;
    double      z      = 0.;
    size_t      offset = 0;
};

static std::string trim_gcode_token(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

// Z is committed on the travel that precedes a FEATURE comment, so the height
// recorded at the comment is the layer being printed.
static std::vector<GcodeFeatureMark> gcode_feature_marks(const std::string &gcode)
{
    std::vector<GcodeFeatureMark> marks;
    double                        z   = 0.;
    size_t                        pos = 0;
    while (pos < gcode.size()) {
        const size_t nl   = gcode.find('\n', pos);
        const size_t end  = (nl == std::string::npos) ? gcode.size() : nl;
        const std::string line = gcode.substr(pos, end - pos);
        const size_t tag = line.find(" FEATURE: ");
        if (tag != std::string::npos) {
            marks.push_back({trim_gcode_token(line.substr(tag + 10)), z, pos});
        } else {
            const std::string code = gcode_without_comment(line);
            if (gcode_is_motion(code)) {
                if (const std::optional<double> zz = gcode_axis_value(code, 'Z'))
                    z = *zz;
            }
        }
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return marks;
}

static bool same_print_z(double a, double b)
{
    return std::abs(a - b) <= 0.02;
}

// Object layer ids start at raft_layers(), while the planner indexes object
// layers from 0. raft_layers is larger than this cube's object-layer count, so
// forgetting to subtract raft_layers looks up an index that has no tower.
// The gap clears the raft outline; a smaller gap is skipped as a collision.
TEST_CASE("Seam tower with a raft stays off raft layers and pairs on the first object layer", "[SeamTower][GCode]")
{
    CubeSpec cube;
    cube.offset = Vec3d(50., 50., 0.);
    cube.height = 2.0;
    const std::string gcode = slice_cubes({cube}, {
        {"seam_tower", 1},
        {"seam_tower_gap", 8},
        {"raft_layers", 15},
        {"raft_expansion", 0},
        {"raft_first_layer_expansion", 0},
    });

    const std::vector<GcodeFeatureMark> marks = gcode_feature_marks(gcode);
    double raft_top       = -1.;
    double first_object_z = 1e9;
    int    support_n      = 0;
    std::vector<double> object_wall_zs;
    for (const GcodeFeatureMark &mark : marks) {
        if (mark.name.find("Support") != std::string::npos) {
            ++support_n;
            raft_top = std::max(raft_top, mark.z);
        }
        if (mark.name == "Outer wall") {
            first_object_z = std::min(first_object_z, mark.z);
            bool seen = false;
            for (double z : object_wall_zs) {
                if (same_print_z(z, mark.z)) {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                object_wall_zs.push_back(mark.z);
        }
    }
    REQUIRE(support_n > 0);
    REQUIRE_FALSE(object_wall_zs.empty());
    REQUIRE(object_wall_zs.size() < 15);
    REQUIRE(first_object_z > raft_top + 0.05);

    int towers_on_raft = 0;
    int towers         = 0;
    for (const GcodeFeatureMark &mark : marks) {
        if (mark.name != "Seam tower")
            continue;
        ++towers;
        if (mark.z <= raft_top + 0.05)
            ++towers_on_raft;
    }
    REQUIRE(towers > 0);
    REQUIRE(towers_on_raft == 0);

    bool paired = false;
    for (size_t i = 0; i + 1 < marks.size(); ++i) {
        if (marks[i].name != "Seam tower" || !same_print_z(marks[i].z, first_object_z))
            continue;
        REQUIRE(marks[i + 1].name == "Outer wall");
        REQUIRE(same_print_z(marks[i + 1].z, first_object_z));
        const std::string between = gcode.substr(marks[i].offset, marks[i + 1].offset - marks[i].offset);
        REQUIRE(gcode_has_e_retract(between));
        REQUIRE(contains_ci(between, "travel to wall"));
        paired = true;
        break;
    }
    REQUIRE(paired);
}

struct SeamTowerWallFeed
{
    double z       = 0.;
    double tower_f = 0.;
    double wall_f  = 0.;
};

// First extrusion F inside a seam-tower block, then the first extrusion F of
// the outer wall that follows it. Travels update F but are not extrusions.
static std::vector<SeamTowerWallFeed> seam_tower_wall_feeds(const std::string &gcode)
{
    std::vector<SeamTowerWallFeed> feeds;
    double z = 0.;
    double f = 0.;
    bool   in_tower     = false;
    bool   in_wall      = false;
    bool   have_tower_f = false;
    bool   have_wall_f  = false;
    double feature_z    = 0.;
    double tower_f      = 0.;
    double wall_f       = 0.;

    auto finish = [&]() {
        if (in_wall && have_tower_f && have_wall_f)
            feeds.push_back({feature_z, tower_f, wall_f});
        in_tower     = false;
        in_wall      = false;
        have_tower_f = false;
        have_wall_f  = false;
    };

    size_t pos = 0;
    while (pos < gcode.size()) {
        const size_t nl   = gcode.find('\n', pos);
        const size_t end  = (nl == std::string::npos) ? gcode.size() : nl;
        const std::string line = gcode.substr(pos, end - pos);
        const size_t tag = line.find(" FEATURE: ");
        if (tag != std::string::npos) {
            const std::string name = trim_gcode_token(line.substr(tag + 10));
            if (name == "Seam tower") {
                finish();
                in_tower  = true;
                feature_z = z;
            } else if (name == "Outer wall" && in_tower) {
                in_tower = false;
                in_wall  = true;
            } else {
                finish();
            }
        } else {
            const std::string code = gcode_without_comment(line);
            if (gcode_is_motion(code)) {
                if (const std::optional<double> zz = gcode_axis_value(code, 'Z'))
                    z = *zz;
                if (const std::optional<double> ff = gcode_axis_value(code, 'F'))
                    f = *ff;
                const bool has_xy = gcode_axis_value(code, 'X').has_value() || gcode_axis_value(code, 'Y').has_value();
                const auto e      = gcode_axis_value(code, 'E');
                if (has_xy && e.has_value() && *e > 0.) {
                    if (in_tower && !have_tower_f) {
                        tower_f      = f;
                        have_tower_f = true;
                    }
                    if (in_wall && !have_wall_f) {
                        wall_f      = f;
                        have_wall_f = true;
                    }
                }
            }
        }
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    finish();
    return feeds;
}

// initial_layer_speed replaces role speed on the bed layer (layer id 0). Layers
// above that keep outer_wall_speed, which erSeamTower shares with the outer wall.
TEST_CASE("Seam tower extrusion feedrate matches the following outer wall above the first layer", "[SeamTower][GCode]")
{
    CubeSpec cube;
    cube.height = 1.2;
    const double outer_mm_s = 40.;
    const std::string gcode = slice_cubes({cube}, {
        {"seam_tower", 1},
        {"outer_wall_speed", outer_mm_s},
        {"inner_wall_speed", 80},
        {"initial_layer_speed", 15},
        {"slow_down_layer_time", 0},
        {"filament_max_volumetric_speed", 30},
        {"enable_overhang_speed", 0},
    });

    const std::vector<SeamTowerWallFeed> feeds = seam_tower_wall_feeds(gcode);
    int checked = 0;
    for (const SeamTowerWallFeed &feed : feeds) {
        if (feed.z <= 0.3)
            continue;
        REQUIRE(feed.tower_f == Approx(feed.wall_f));
        REQUIRE(feed.tower_f == Approx(outer_mm_s * 60.));
        ++checked;
    }
    REQUIRE(checked > 0);
}

// A frame and a large island inside its hole overlap by more than half the
// frame's outer area, so they share a contour id. The island is stored last.
// The frame's back seam must still be offset from the frame, outside the part.
TEST_CASE("Seam tower on a frame stays outside when an inner island shares the contour id", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_tower_gap", 2},
        {"seam_tower_depth", 2},
        {"seam_tower_length", 4},
        {"seam_position", "back"},
    });

    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "frame-island.stl";
    object->add_volume(make_cube(40., 40., 1.));
    // add_volume centers each mesh and stores that center as the volume offset.
    // The cube therefore occupies 0..40. The moat leaves a 2 mm frame and a
    // 30 mm island. 30*30 / 40*40 > 0.5, so the two outer contours share an id.
    const struct { double w, d, x, y; } bars[] = {
        {36., 3., 20., 36.5},
        {36., 3., 20., 3.5},
        {3., 30., 3.5, 20.},
        {3., 30., 36.5, 20.},
    };
    for (const auto &bar : bars) {
        ModelVolume *cut = object->add_volume(make_cube(bar.w, bar.d, 2.), ModelVolumeType::NEGATIVE_VOLUME);
        cut->set_offset(Vec3d(bar.x, bar.y, 0.5));
    }
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(50., 50., 0.));
    object->ensure_on_bed();
    object->config.set("seam_tower", true);
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const std::string gcode = export_processed_gcode(print);

    // The inner island used to overwrite the frame's contour. The frame tower
    // was then built in the moat. Every tower extrusion has to sit outside the
    // outer-wall box.
    double wall_min_x = 1e9, wall_max_x = -1e9, wall_min_y = 1e9, wall_max_y = -1e9;
    std::vector<Vec2d> tower_pts;
    std::string role;
    double x = 0, y = 0;
    bool have_x = false, have_y = false;
    size_t pos = 0;
    while (pos < gcode.size()) {
        const size_t end = gcode.find('\n', pos);
        const std::string line = gcode.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? gcode.size() : end + 1;
        if (line.find(" FEATURE: ") != std::string::npos) {
            const size_t at = line.find(" FEATURE: ");
            role = line.substr(at + std::strlen(" FEATURE: "));
            if (!role.empty() && role.back() == '\r')
                role.pop_back();
        }
        if (line.rfind("G1", 0) != 0 && line.rfind("G0", 0) != 0)
            continue;
        const size_t x_at = line.find(" X");
        const size_t y_at = line.find(" Y");
        if (x_at != std::string::npos) {
            x = std::strtod(line.c_str() + x_at + 2, nullptr);
            have_x = true;
        }
        if (y_at != std::string::npos) {
            y = std::strtod(line.c_str() + y_at + 2, nullptr);
            have_y = true;
        }
        if (!have_x || !have_y || line.find('E') == std::string::npos)
            continue;
        if (role == "Outer wall") {
            wall_min_x = std::min(wall_min_x, x);
            wall_max_x = std::max(wall_max_x, x);
            wall_min_y = std::min(wall_min_y, y);
            wall_max_y = std::max(wall_max_y, y);
        } else if (role == "Seam tower")
            tower_pts.emplace_back(x, y);
    }
    REQUIRE(wall_max_x > wall_min_x);
    REQUIRE_FALSE(tower_pts.empty());
    const double inset = 0.05;
    for (const Vec2d &pt : tower_pts) {
        const bool inside = pt.x() > wall_min_x + inset && pt.x() < wall_max_x - inset &&
                            pt.y() > wall_min_y + inset && pt.y() < wall_max_y - inset;
        INFO(pt.x() << "," << pt.y());
        REQUIRE_FALSE(inside);
    }
}

// A tower point that sits in a slice hole, not in the solid wall.
static bool point_in_slice_hole(const ExPolygons &slices, const Point &p)
{
    for (const ExPolygon &island : slices) {
        if (island.contour.size() < 3 || !island.contour.contains(p))
            continue;
        if (!island.contains(p))
            return true;
    }
    return false;
}

static int hole_towers_on_layer(const SeamTowerPlanner &planner, const ExPolygons &slices, int layer)
{
    const auto collections = planner.layer_collections();
    const auto it          = collections.find(layer);
    if (it == collections.end())
        return 0;
    Points pts;
    it->second.collect_points(pts);
    int inside = 0;
    for (const Point &p : pts) {
        if (point_in_slice_hole(slices, p))
            ++inside;
    }
    return inside;
}

TEST_CASE("A hexagonal frame with the flat to the back gets a seam tower in the hole", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_tower_gap", 1},
        {"seam_tower_depth", 4},
        {"seam_tower_length", 5},
        {"seam_tower_in_holes", 1},
        {"seam_tower_min_size", 3},
        {"seam_position", "back"},
        {"wall_loops", 2},
        {"line_width", 0.42},
        {"printable_area", "0x0,256x0,256x256,0x256"},
    });
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "hex-frame.stl";
    // Vertex at +Y before rotation. After 30 degrees the rear edge is horizontal,
    // so the back seam sits on a hole vertex. gap 1 mm + depth 4 mm miters that
    // 120° corner past a 5 mm tower.
    object->add_volume(make_cylinder(42., 2., 2. * PI / 6.));
    object->add_volume(make_cylinder(38., 4., 2. * PI / 6.), ModelVolumeType::NEGATIVE_VOLUME);
    object->add_instance();
    object->instances.front()->set_rotation(Vec3d(0., 0., PI / 6.));
    object->instances.front()->set_offset(Vec3d(128., 128., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    SeamPlacer placer;
    placer.init(print, []() {});
    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);

    const PrintObject *po = print.objects().front();
    const int inside = hole_towers_on_layer(planner, po->get_layer(0)->lslices, 0);
    const int top    = int(po->layer_count()) - 1;
    const int inside_top = hole_towers_on_layer(planner, po->get_layer(top)->lslices, top);
    REQUIRE(planner.warnings().empty());
    REQUIRE(inside > 0);
    REQUIRE(inside_top > 0);
}

// The notch pulls the hole's back seam 8 mm outward for the middle 2 mm. That
// sample starts its own stack, then the vertex seam returns and rejoins, so
// the hole column has layers with no seam. Those layers are still the hole.
TEST_CASE("A hole seam tower survives layers where the seam sample jumps away", "[SeamTower][GCode]")
{
    DynamicPrintConfig config = slice_print_config({
        {"seam_tower", 1},
        {"seam_tower_gap", 1},
        {"seam_tower_depth", 4},
        {"seam_tower_length", 5},
        {"seam_tower_in_holes", 1},
        {"seam_tower_min_size", 3},
        {"seam_position", "back"},
        {"wall_loops", 2},
        {"line_width", 0.42},
        {"layer_height", 0.2},
        {"printable_area", "0x0,256x0,256x256,0x256"},
    });
    Model model;
    Print print;
    ModelObject *object = model.add_object();
    object->name = "notched-hex-frame.stl";
    object->add_volume(make_cylinder(50., 6., 2. * PI / 6.));
    object->add_volume(make_cylinder(38., 8., 2. * PI / 6.), ModelVolumeType::NEGATIVE_VOLUME);
    // Bite the wall beside the back vertex, not through it. The hole's back
    // seam jumps to the bite for the middle layers, then returns. The vertex
    // stays on the hole, and the outer wall (y≈50) stays closed.
    ModelVolume *notch = object->add_volume(make_cube(8., 14., 2.), ModelVolumeType::NEGATIVE_VOLUME);
    notch->set_offset(Vec3d(8., 37., 3.));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(80., 80., 0.));
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    SeamPlacer placer;
    placer.init(print, []() {});
    SeamTowerParams params;
    params.enabled = true;
    SeamTowerPlanner planner;
    planner.build(print, placer, params);

    const PrintObject *po = print.objects().front();
    const int top = int(po->layer_count()) - 1;
    REQUIRE(top > 20);
    REQUIRE(hole_towers_on_layer(planner, po->get_layer(0)->lslices, 0) > 0);
    REQUIRE(hole_towers_on_layer(planner, po->get_layer(top)->lslices, top) > 0);
}
