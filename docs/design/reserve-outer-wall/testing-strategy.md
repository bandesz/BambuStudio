# Testing strategy

Catch2 target `reserve_outer_wall_tests`, same shape as `tests/narrow_bottom_infill`: a `*_tests.cpp` that includes `catch_main.hpp`, plus `test_reserve_outer_wall.cpp`. No browser UI. Each flow is a slice of a mesh built in the test.

Desktop slicer. The user-visible checks are the warning string and the extrusion geometry. There is no separate UI-flow harness.

## Mesh

Millimetres. Axis-aligned wall plus a wedge on +Y. `ModelObject::ensure_on_bed` leaves the mesh on Z = 0 because its minimum Z is 0.

Wall box, corners:

- (0, 0, 0), (30, 0, 0), (30, 1, 0), (0, 1, 0)
- (0, 0, 10), (30, 0, 10), (30, 1, 10), (0, 1, 10)

Wedge. At height `z` in [4, 6], `t = (z - 4) / 2`. The added triangle is:

- (15 - t, 1, z), (15 + t, 1, z), (15, 1 + t, z)

At z = 4 the triangle is the point (15, 1, 4). At z = 6 it is (14, 1, 6), (16, 1, 6), (15, 2, 6). Build it as a closed triangle mesh of that taper (the point, the tip edge, and the two base points on the wall face at z = 6). Union it with the wall box.

The flat face is Y = 0.

## Slice planes

`layer_height` and `initial_layer_print_height` are 0.2. `Layer::slice_z` is the midplane (`Slicing.cpp` sets it to halfway between the layer's bottom and `print_z`). Tests select layers by `slice_z`, not by `print_z`.

The wedge is a sharp triangle. Classic decides which inner and gap lines fit. Reserve outer wall does not add or remove those lines. It only keeps gap fill and infill out of the outer-bead core, the bead inset by `layer_height * (1 - π/4)`.

| `slice_z` | `print_z` | What is true there |
|-----------|-----------|--------------------|
| 1.9 | 2.0 | Plain 1 mm wall. No inner bead |
| 4.1 | 4.2 | Wedge has just started. Protrusion is about 0.05 mm. No inner bead |
| 5.3 | 5.4 | First plane where a 0.45 mm `erPerimeter` fits inside the interior |
| 5.9 | 6.0 | Near the top of the wedge. An inner bead exists, and infill must stay out of the outer bead |

`min_bead_mm` is 0.34.

## Config fixed by every slice test

Overrides on `DynamicPrintConfig::full_print_config()`:

| Key | Value |
|-----|--------|
| `wall_generator` | `classic` |
| `wall_sequence` | `outer wall/inner wall` unless the test says otherwise |
| `wall_loops` | 2 |
| `outer_wall_line_width` | 0.42 |
| `inner_wall_line_width` | 0.45 |
| `line_width` | 0.42 |
| `layer_height` | 0.2 |
| `initial_layer_print_height` | 0.2 |
| `min_bead_width` | 85% |
| `nozzle_diameter` | 0.4 |
| `sparse_infill_density` | 15% |
| `infill_wall_overlap` | 15% |
| `ensure_vertical_shell_thickness` | `enabled` |
| `detect_thin_wall` | 0 |
| `top_one_wall_type` | `all top` |
| `enable_support` | 0 |
| `brim_type` | `no_brim` |
| `reserve_outer_wall` | 1 or 0 as the flow says |

## Measurements

Outer bead coverage: round-join offset of each `erExternalPerimeter` and `erOverhangPerimeter` path by half that path's width, then union.

Outer-bead core: that coverage, inset by `layer_height * (1 - π/4)`.

Wall lines are `erExternalPerimeter`, `erOverhangPerimeter`, `erPerimeter`, and `erGapFill`. With the checkbox on they match the checkbox off within 0.01 mm.

Overlap area is the area of the intersection of a fill line's own round-join coverage with the outer-bead core.

Flat-face centerline: among `erExternalPerimeter` paths, the points with X in [0, 30] whose Y is within 0.3 mm of 0.21. Their Y values are the flat-face sample.

## Flow map

| Flow | Test |
|------|------|
| Flow 1 | `slice_z` 5.3 and 1.9 flat-face Y within 0.01 mm of each other and of 0.21. Outer width 0.42 ± 0.001. Wall lines match the checkbox off within 0.01 mm |
| Flow 2 | Wall lines, including gap fill, match the checkbox off. The checkbox does not delete a middle line |
| Flow 3 | `reserve_outer_wall` 0. On `slice_z` 5.3 some later line has overlap area with the full outer bead > 0.02 mm². Fill overlap with the outer-bead core at `slice_z` 5.9 is > 0.05 mm² |
| Flow 4 | Warning string and `opt_key`. Polylines match the checkbox off within 0.01 mm |
| Infill clip | `slice_z` 5.9, checkbox on. Any fill-role extrusion has overlap area with the outer-bead core ≤ 0.02 mm² |

Flow 4 cases: `wall_sequence` = `inner wall/outer wall` with Classic, and `wall_generator` = `arachne` with outer/inner. Each compared to the same config with `reserve_outer_wall` 0.

## Coverage

- [x] Flow 1 — flat face and wall lines unchanged (`Reserve outer wall keeps the flat face and skips beads in the narrow channel`, `Reserve outer wall leaves classic wall lines in place`)
- [x] Flow 2 — gap fill and inner walls match the checkbox off (same wall-line test)
- [x] Flow 3 — checkbox off overlaps the outer bead on `slice_z` 5.3 (`Reserve outer wall off overlaps the outer bead on the wedge`)
- [x] Flow 4 — warning strings, a pre-existing warning, spiral vase with no warning and no toolpath change including bottom shells, Classic inner/outer and Arachne polyline match
- [x] Merged regions — density modifier copies the interior; a modifier with the checkbox off does not share it; empty slices clear it; a re-slice with the checkbox off leaves every interior empty
