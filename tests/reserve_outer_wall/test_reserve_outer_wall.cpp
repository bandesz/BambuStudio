#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>

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
