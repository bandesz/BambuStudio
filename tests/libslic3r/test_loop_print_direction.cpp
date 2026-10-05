#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

static const ConfigOptionDef *require_def(const t_config_option_key &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    REQUIRE(def != nullptr);
    return def;
}

TEST_CASE("Loop print direction follows the printer unless the project overrides it", "[LoopPrintDirection]")
{
    FullPrintConfig cfg;
    REQUIRE(cfg.loop_print_direction.value == LoopPrintDirection::Printer);

    cfg.print_in_clockwise.value = true;
    REQUIRE(print_loop_clockwise(cfg));

    cfg.print_in_clockwise.value = false;
    REQUIRE_FALSE(print_loop_clockwise(cfg));

    cfg.print_in_clockwise.value = false;
    cfg.loop_print_direction.value = LoopPrintDirection::Clockwise;
    REQUIRE(print_loop_clockwise(cfg));

    cfg.print_in_clockwise.value = true;
    cfg.loop_print_direction.value = LoopPrintDirection::CounterClockwise;
    REQUIRE_FALSE(print_loop_clockwise(cfg));
}

TEST_CASE("Loop print direction is a Quality advanced process option", "[LoopPrintDirection][Config]")
{
    const ConfigOptionDef *def = require_def("loop_print_direction");
    REQUIRE(def->type == coEnum);
    REQUIRE(def->label == "Loop direction");
    REQUIRE(def->category == "Quality");
    REQUIRE(def->mode == comAdvanced);
    REQUIRE(def->tooltip ==
            "Project override for closed-loop print direction. Follow printer uses the printer profile. "
            "Clockwise and counter-clockwise keep the same seam start and walk the wall the other way.");
    REQUIRE(def->enum_values == std::vector<std::string>{"printer", "clockwise", "counterclockwise"});
    REQUIRE(def->enum_labels == std::vector<std::string>{"Follow printer", "Clockwise", "Counter-clockwise"});
    REQUIRE(def->default_value->getInt() == int(LoopPrintDirection::Printer));
}
