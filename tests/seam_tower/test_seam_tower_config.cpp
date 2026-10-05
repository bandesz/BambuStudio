#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

static const std::vector<std::string> kSeamTowerKeys = {
    "seam_tower",
    "seam_tower_gap",
    "seam_tower_depth",
    "seam_tower_length",
    "seam_tower_in_holes",
    "seam_tower_min_size",
};

static const ConfigOptionDef *require_def(const t_config_option_key &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    REQUIRE(def != nullptr);
    return def;
}

TEST_CASE("Seam tower keys default to false, 0.1, 2.0, 4.0, false, 3.0", "[SeamTower][Config]")
{
    const PrintObjectConfig cfg;
    REQUIRE(cfg.seam_tower.value == false);
    REQUIRE(cfg.seam_tower_gap.value == Approx(0.1));
    REQUIRE(cfg.seam_tower_depth.value == Approx(2.0));
    REQUIRE(cfg.seam_tower_length.value == Approx(4.0));
    REQUIRE(cfg.seam_tower_in_holes.value == false);
    REQUIRE(cfg.seam_tower_min_size.value == Approx(3.0));
}

TEST_CASE("Seam tower gap, depth, length, and min size have min 0", "[SeamTower][Config]")
{
    REQUIRE(require_def("seam_tower_gap")->min == 0);
    REQUIRE(require_def("seam_tower_depth")->min == 0);
    REQUIRE(require_def("seam_tower_length")->min == 0);
    REQUIRE(require_def("seam_tower_min_size")->min == 0);
}

TEST_CASE("Seam tower options sit in Quality / Advanced with public-api labels and tooltips", "[SeamTower][Config]")
{
    const ConfigOptionDef *enable = require_def("seam_tower");
    REQUIRE(enable->label == "Seam tower");
    REQUIRE(enable->category == "Quality");
    REQUIRE(enable->mode == comAdvanced);
    REQUIRE(enable->tooltip ==
            "Print a small sacrificial island next to each stable outer-wall seam. "
            "The island is printed first to dump pressure, then the nozzle retracts and travels to the wall.");

    const ConfigOptionDef *gap = require_def("seam_tower_gap");
    REQUIRE(gap->label == "Seam tower gap");
    REQUIRE(gap->category == "Quality");
    REQUIRE(gap->mode == comAdvanced);
    REQUIRE(gap->sidetext == "mm");
    REQUIRE(gap->tooltip ==
            "Distance between the object wall and the inner edge of the tower. "
            "Use enough gap that the tower can snap off; the nozzle retracts before crossing it.");

    const ConfigOptionDef *depth = require_def("seam_tower_depth");
    REQUIRE(depth->label == "Seam tower depth");
    REQUIRE(depth->category == "Quality");
    REQUIRE(depth->mode == comAdvanced);
    REQUIRE(depth->sidetext == "mm");
    REQUIRE(depth->tooltip ==
            "How far the tower extends away from the wall. "
            "The tower is a closed strip: a copy of the wall contour this far out, with the ends capped. "
            "Depth does not fill the island solid; at most two walls, plus a first-layer brim.");

    const ConfigOptionDef *length = require_def("seam_tower_length");
    REQUIRE(length->label == "Seam tower length");
    REQUIRE(length->category == "Quality");
    REQUIRE(length->mode == comAdvanced);
    REQUIRE(length->sidetext == "mm");
    REQUIRE(length->tooltip ==
            "How far the tower extends along the wall, centred on the seam. "
            "If the seam stack wanders farther than this, the island grows to cover that window.");

    const ConfigOptionDef *holes = require_def("seam_tower_in_holes");
    REQUIRE(holes->label == "Seam tower in holes");
    REQUIRE(holes->category == "Quality");
    REQUIRE(holes->mode == comAdvanced);
    REQUIRE(holes->tooltip == "Also build towers on hole contours when the hole is large enough.");

    const ConfigOptionDef *min_size = require_def("seam_tower_min_size");
    REQUIRE(min_size->label == "Seam tower min size");
    REQUIRE(min_size->category == "Quality");
    REQUIRE(min_size->mode == comAdvanced);
    REQUIRE(min_size->sidetext == "mm");
    REQUIRE(min_size->tooltip ==
            "Minimum hole size that may receive a tower. Smaller holes skip the tower.");
}

TEST_CASE("Seam tower keys follow first_layer_inset_start in process print options", "[SeamTower][Config]")
{
    const std::vector<std::string> &opts = Preset::print_options();
    const auto                      first = std::find(opts.begin(), opts.end(), "first_layer_inset_start");
    REQUIRE(first != opts.end());
    REQUIRE(size_t(std::distance(first, opts.end())) > kSeamTowerKeys.size());
    REQUIRE(std::vector<std::string>(first + 1, first + 1 + kSeamTowerKeys.size()) == kSeamTowerKeys);
}

TEST_CASE("Changing any seam tower key invalidates G-code without reslicing", "[SeamTower][Config]")
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

    Model        model;
    Print        print;
    ModelObject *object = model.add_object();
    object->name = "cube.stl";
    object->add_volume(make_cube(20, 20, 20));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const boost::filesystem::path temp =
        boost::filesystem::path("/Users/andras/src/github.com/BambuStudio/BambuStudio/build") /
        boost::filesystem::unique_path("seam-tower-config-%%%%.gcode");
    print.export_gcode(temp.string(), nullptr, nullptr);
    boost::nowide::remove(temp.string().c_str());
    REQUIRE(print.is_step_done(psGCodeExport));
    REQUIRE(print.objects().front()->is_step_done(posSlice));

    const std::vector<std::pair<std::string, std::string>> changes = {
        {"seam_tower", "1"},
        {"seam_tower_gap", "0.2"},
        {"seam_tower_depth", "3"},
        {"seam_tower_length", "6"},
        {"seam_tower_in_holes", "1"},
        {"seam_tower_min_size", "4"},
    };
    for (const auto &change : changes) {
        if (!print.is_step_done(psGCodeExport)) {
            print.export_gcode(temp.string(), nullptr, nullptr);
            boost::nowide::remove(temp.string().c_str());
            REQUIRE(print.is_step_done(psGCodeExport));
        }
        config.set_deserialize_strict(change.first, change.second);
        print.apply(model, config);
        REQUIRE_FALSE(print.is_step_done(psGCodeExport));
        REQUIRE(print.objects().front()->is_step_done(posSlice));
    }
}
