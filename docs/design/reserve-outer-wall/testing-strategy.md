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

The wedge is a sharp triangle. Classic decides which inner and gap lines fit. On this mesh every `erGapFill` is thinner than the 0.45 mm inner wall, so reserve outer wall leaves those lines where Classic drew them. A second mesh, a 1.35 mm bar beside a 2 mm bar, prints the thick gap. A gap line wider than the inner wall that intersects the outer bead is narrowed to that inner width in place, so its centerline stays and the coverage pulls back from both faces. A normal inner wall, and a gap line that does not touch the outer bead, stay where Classic drew them. Sparse infill may use the overlap band outside that core.

| `slice_z` | `print_z` | What is true there |
|-----------|-----------|--------------------|
| 1.9 | 2.0 | Plain 1 mm wall. No `erPerimeter`. A thin `erGapFill` (about 0.23 mm) runs the channel. It overlaps the outer bead, not the outer-bead core, and it is not wider than the inner wall |
| 4.1 | 4.2 | Wedge has just started. Protrusion is about 0.05 mm. No `erPerimeter`. The same thin channel gap fill is still printed |
| 5.3 | 5.4 | Flat-face plane. No `erPerimeter` on this mesh. The channel gap fill stays thinner than the inner wall |
| 5.9 | 6.0 | Near the top of the wedge. Infill must stay out of the outer-bead core |

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

Wall lines are `erExternalPerimeter`, `erOverhangPerimeter`, `erPerimeter`, and `erGapFill`. On the 1 mm wedge they match the checkbox off within 0.01 mm. That mesh does not print a gap line wider than the inner wall.

A normal inner wall, and a gap line that does not touch the outer bead, match the checkbox off within 0.01 mm. That includes the middle gap of the 2 mm bar. A gap line wider than the inner wall that intersects the outer bead, the center gap of the 1.35 mm bar, is narrowed to the inner wall width and its centerline stays within 0.01 mm of the checkbox off. Its overlap with the outer-bead core is ≤ 0.02 mm². The outer bead centerline and width stay as Classic drew them.

Outer-bead core overlap is the area of the intersection of a line's own round-join coverage with the outer-bead core. Sparse infill may use the overlap band. It is measured against that core at ≤ 0.02 mm².

Flat-face centerline: among `erExternalPerimeter` paths, the points with X in [0, 30] whose Y is within 0.3 mm of 0.21. Their Y values are the flat-face sample.

## Flow map

| Flow | Test |
|------|------|
| Flow 1 | `slice_z` 5.3 and 1.9 flat-face Y within 0.01 mm of each other and of 0.21. Outer width 0.42 ± 0.001. On the wedge, wall lines match the checkbox off within 0.01 mm. `slice_z` 1.9 and 4.1 have no `erPerimeter` and do have a thin `erGapFill` |
| Flow 2 | The 2 mm bar's middle gap does not touch the outer bead and matches the checkbox off within 0.01 mm, as does its normal inner wall. The 1.35 mm bar's gap is wider than 0.45 mm, intersects the outer bead, and is narrowed to 0.45 mm with outer-bead-core overlap ≤ 0.02 mm². The outer bead stays as Classic drew it |
| Hole bead | A 24 x 16 x 2 frame. The cavity's bottom wall is 1 mm, thickened to 1.35 mm for x in [6, 18] and z in [0.4, 1.4]. On `slice_z` 0.5, 0.7, 0.9, 1.1, and 1.3 the hole's external perimeter matches the checkbox off within 0.01 mm and 0.001 mm. The gap wider than 0.45 mm on that bead is narrowed to 0.45 mm with hole-bead-core overlap ≤ 0.02 mm². Checkbox off overlaps that hole bead by more than 0.02 mm² |
| Flow 3 | `reserve_outer_wall` 0. On `slice_z` 5.3 some later line has overlap area with the full outer bead > 0.02 mm². Fill overlap with the outer-bead core at `slice_z` 5.9 is > 0.05 mm² |
| Flow 4 | Warning string and `opt_key`. Polylines match the checkbox off within 0.01 mm |
| Infill clip | `slice_z` 5.9, checkbox on. Any fill-role extrusion has overlap area with the outer-bead core ≤ 0.02 mm². Sparse infill may use the overlap band |

Flow 4 cases: `wall_sequence` = `inner wall/outer wall` with Classic, and `wall_generator` = `arachne` with outer/inner. Each compared to the same config with `reserve_outer_wall` 0.

## Coverage

- [x] Flow 1 — flat face unchanged; `slice_z` 1.9 and 4.1 have no inner perimeter and a thin gap fill; the wedge has no thick gap line (`Reserve outer wall keeps the flat face and skips beads in the narrow channel`, `Reserve outer wall leaves classic wall lines in place`)
- [x] Flow 2 — middle gap that does not touch the outer bead, normal inner wall, and thick gap retract (`Reserve outer wall narrows a thick gap line and keeps the middle gap`)
- [x] Hole bead — inner-facing external perimeter unchanged, thick gap on that bead narrowed (`Reserve outer wall keeps a hole bead and narrows the thick line on it`)
- [x] Flow 3 — checkbox off overlaps the outer bead on `slice_z` 5.3 (`Reserve outer wall off overlaps the outer bead on the wedge`)
- [x] Flow 4 — warning strings, a pre-existing warning, spiral vase with no warning and no toolpath change including bottom shells, Classic inner/outer and Arachne polyline match
- [x] Merged regions — density modifier copies the interior; a modifier with the checkbox off does not share it; empty slices clear it; a re-slice with the checkbox off leaves every interior empty
