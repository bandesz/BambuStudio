# Public surface

Additive `PrintRegionConfig` option. Default off, so existing profiles slice as they do today.

| | |
|---|---|
| Key | `reserve_outer_wall` |
| Type | `ConfigOptionBool` |
| Default | `false` |
| Label | Reserve outer wall |
| Category | Quality |
| Mode | `comAdvanced` |
| UI | Quality → Advanced, immediately after Order of walls (`wall_sequence`) |
| Tooltip | When the outer wall is printed before the inner walls, trim a later line only where it is thicker than a normal wall line and would push the outer bead outward. Ordinary wall lines stay where they are. Ignored unless the wall generator is Classic and the wall order is outer/inner. |

## When it changes toolpaths

All of these are true:

- `reserve_outer_wall` is true
- `wall_generator` is Classic
- `wall_sequence` is `WallSequence::OuterInner`
- spiral vase is off

Otherwise the generator and the infill clip do not run. Spiral vase produces no warning. The other mismatches warn.

## Warnings

`Print::validate` writes at most one warning. Set `warning->opt_key` to `reserve_outer_wall`. Set the string only when `warning->string` is empty.

| Condition | String |
|-----------|--------|
| Checkbox on, spiral vase off, wall order is not outer/inner | Reserve outer wall is ignored unless the order of walls is outer/inner |
| Checkbox on, spiral vase off, wall order is outer/inner, generator is not Classic | Reserve outer wall is ignored unless the wall generator is Classic |

Wall order is checked first. Arachne with inner/outer shows the wall-order string.

Valid call: `reserve_outer_wall = true`, Classic, outer/inner, spiral vase off. Wall lines match the checkbox off. Gap fill and infill stay out of the outer-bead core.

Invalid result: `reserve_outer_wall = true`, Classic, inner/outer. `validate` warning string is the wall-order sentence above, `opt_key` is `reserve_outer_wall`, and extrusion polylines match `reserve_outer_wall = false`.

## Registration

- `PrintRegionConfig` in `src/libslic3r/PrintConfig.hpp`, next to `precise_outer_wall`.
- Definition in `src/libslic3r/PrintConfig.cpp`.
- `s_Preset_print_options` in `src/libslic3r/Preset.cpp`, next to `precise_outer_wall`.
- Option line in `src/slic3r/GUI/Tab.cpp` immediately after `wall_sequence`.
- `PrintObject` step invalidation: `posPerimeters` and `posPrepareInfill`, same branch style as `precise_outer_wall` plus infill prepare. Not `posSlice`.

Changing the key does not change the meaning of `wall_sequence`, `precise_outer_wall`, `min_bead_width`, or `infill_wall_overlap`.
