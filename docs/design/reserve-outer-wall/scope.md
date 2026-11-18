# Scope

## Problem

A 1 mm wall printed with the Classic generator, 2 wall loops, a 0.42 mm outer line, and a 0.45 mm inner line has one outer loop. The two faces are the two sides of that loop. The inner loop does not fit in 1 mm. Where a wedge on one face adds about 0.45 mm or more, an inner loop appears and runs along the flat face. With Order of walls set to outer/inner, that loop is extruded after the outer bead and overlaps it by about 0.04 mm, more at the ends because of the inset overlap tolerance. The flat face bulges on those layers.

## Constraints

- Order of walls stays outer/inner. The fix must not require inner/outer.
- Configured outer and inner line widths stay the configured values. The outer bead keeps today's centerline and today's width.
- The feature is off unless the user turns it on.
- Minimum wall width (`min_bead_width` on `PrintObjectConfig`) is the skip threshold. It is a percent of the smallest `nozzle_diameter`, the same quantity Arachne already uses: `min_bead_width / 100 * min(nozzle_diameter)`.
- Infill may still overlap an inner wall, and it may use the normal spacing overlap of the outer bead. It may not enter the outer bead past that overlap.

## Assumptions

- Ordinary wall lines, including their designed overlap, stay. A later line may not enter the outer bead past `layer_height * (1 - π/4)`.
- On a plain 1 mm wall with a 0.42 mm outer line, the center gap is below the 0.34 mm minimum, so that center line is not printed.
- The wedge is a sharp triangle. A 0.45 mm inner bead first fits at `slice_z` 5.3, not at a plane chosen by subtracting two outer widths from the total thickness.
- `Print::validate` stores one warning. If `warning->string` is already non-empty, Reserve outer wall does not replace it.
- Spiral vase ignores the checkbox and does not warn. Spiral vase already forces a single wall.
- The reserved region is the coverage of the emitted outer loop, after elephant-foot compensation and the narrow-island width. It is not a second theoretical inset of the slice.
- A thinned bead uses role `erGapFill`. A bead at the nominal inner width uses role `erPerimeter`.
- `wall_loops` is unchanged. The checkbox does not add a wall. Space inside the nominal inner loops stays infill.

## Non-goals

- Arachne toolpaths. Wall distribution count is unchanged.
- Order of walls inner/outer, and inner/outer/inner. Those slices match the checkbox being off.
- Precise wall. Its gate and its inset stay as they are.
- A configurable count of protected walls. v1 protects the outermost loop only.
- Changing `min_bead_width`, `infill_wall_overlap`, or line-width defaults.
- Spiral vase toolpaths.
