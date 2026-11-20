# Architecture

Classic `PerimeterGenerator::process_classic` builds every wall loop as it does today. When the checkbox applies, those loops are left in place, including the normal bead-to-bead overlap. Only the core of the outer bead, past that overlap, is reserved. Gap fill and infill are clipped to the outside of that core, so a thicker line is trimmed where it would push the already printed outer bead. A normal inner wall is not moved or deleted.

## Apply flag

```
applies = region.reserve_outer_wall
    && region.wall_generator == Classic
    && region.wall_sequence == OuterInner
    && !print.spiral_mode
```

`process_arachne` does not read the flag.

## Outer loop

Unchanged. Centerline inset, width, narrow-island width, elephant foot, and overhang splitting stay on the existing path. The reserved core is built from the depth-0 loops already stored at `contours[0]` and `holes[0]`:

- Offset each depth-0 centerline by half that loop's width, round joins. That polygon is the outer bead.
- Shrink it by `layer_height * (1 - π/4)`. That distance is the designed spacing overlap of two beads (`Flow::rounded_rectangle_extrusion_spacing`). What remains is the core a later line must not enter.
- `interior = diff(slice, core)`. Empty core means `interior` is the whole slice.

The normal overlap band between the outer bead and the next line stays inside `interior`.

## Later walls and gap fill

Inner loops (`erPerimeter`) are the Classic insets. They are not rebuilt where Classic kept them, and they are not deleted when they use the designed overlap.

Where a chamfer on the opposite face makes Classic drop the second wall and fill that band with a thicker gap line, the missing inner wall is drawn again. It is the same inset the layer below used, and only on the face that did not move. The restored bead is taken out of the gap region and out of the infill interior. A constant thin wall, where the layer below made the same choice, is left as Classic drew it.

Gap regions are intersected with `interior` before the existing medial-axis gap fill. A gap line that does not touch the outer bead is left as Classic drew it, including the middle line of a three-line wall. A gap line wider than the inner wall that does touch the outer bead is narrowed to the inner wall width where it sits. The centerline is not moved, so the surplus is not pushed into the bead on the opposite face. Classic still drops a gap below the minimum bead width.

Solid infill surfaces are then cut back to the inner face of the outer bead. Sparse infill keeps the normal overlap band.

Detect thin wall stays on the Classic path.

## Infill

Fill surfaces are written by the Classic path. Store `interior` on the `LayerRegion` as `reserved_outer_wall_interior` (`ExPolygons`). Empty means the flag did not apply. Do not clip when it is empty.

`PrintObject` vertical-shell and horizontal-shell passes may expand solid infill by their existing tiny overlap. After both passes, when `reserved_outer_wall_interior` is non-empty, intersect every fill surface on that region with it. A surface that becomes empty is dropped.

## Data flow

1. `process_classic` emits Classic wall loops, builds `interior` from the outer-bead core, clips gap regions to `interior`, writes fill surfaces, stores `reserved_outer_wall_interior`.
2. Surface classification and shell discovery run as today.
3. The post-shell intersect drops any fill that crossed `interior`.

## Errors

No new hard failure. A disqualified checkbox warns, as [public-api.md](public-api.md) specifies, and the generator takes the existing path. An empty `interior` (the outer-bead core covers the slice) leaves the Classic wall lines and drops gap fill and infill in that region. The slice still succeeds.
