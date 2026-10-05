# Architecture

Chosen in Phase 2: a **SeamTower planner** after `SeamPlacer::init`, first-class `erSeamTower` paths, paired emit in `GCode::extrude_loop`.

Seams are not final until G-code generation (`SeamPlacer::init` in `GCode.cpp`). Slice-time helper islands would drift from the real seam. Pure injection inside `extrude_loop` is how inset start works, but collision, preview, and tests need polygons.

## Components

| Piece | Location | Responsibility |
|-------|----------|----------------|
| Geometry | `src/libslic3r/SeamTower.hpp` / `.cpp` | Pure functions: offset strip, caps, overhang clip, rectangle fallback, hole fit |
| Clustering | same | Group seam XY samples into stacks from the first seam through the last |
| Planner | `src/libslic3r/GCode/SeamTowerPlanner.hpp` / `.cpp` | After seams init: build per-layer paths, collision, skip list |
| Emit | `GCode.cpp` `extrude_loop` | Print matching tower (inner loops, then outer wall), retract, travel from the outer-wall end, skip inset start |
| Role | `ExtrusionEntity.hpp` | `erSeamTower` |
| Settings | `PrintConfig.hpp` / `.cpp`, `Tab.cpp`, `ConfigManipulation.cpp` | Keys and UI |

Keep geometry free of `GCode` / `Print` so Catch2 tests can call it like `InsetStart`.

## Geometry

Inputs: wall contour (`Polygon`), seam point on that contour, along-contour half-span, `gap`, `depth`, previous layer’s tower polygon (empty on the stack’s first seam layer).

1. Take a contour arc centred on the seam, length `max(seam_tower_length, stack window)`.
2. Offset that arc **away from the solid** (outward on the outer contour, inward into a hole) by `gap` → inner edge.
3. Offset again by `depth` → outer edge.
4. Cap the ends (line between corresponding inner/outer endpoints). Result is a closed `ExPolygon`.
5. If previous tower is non-empty: `this = intersection(this, offset(previous, 0))`. Where the desired strip falls outside that support, replace the missing part with a **rectangle** of size `depth × max(length, remaining length)` clipped to the previous footprint. If the result is empty or narrower than one line width, this layer cannot be built → skip the whole tower. Do not emit the layers above it.
6. The first layer of a stack (its first seam, not a projected bed pad): no intersection clip. Later layers clip to the previous island.

Hole: same construction on the hole polygon. If the hole’s available width &lt; `seam_tower_min_size`, return empty (skip).

## Clustering

Inputs: per-layer seam points for external perimeters (and hole perimeters iff `seam_tower_in_holes`), each with a contour id.

Walk layers bottom-up. A point joins the open stack on the same contour whose last XY is within the jump threshold. Otherwise it opens a new stack.

Each stack:

- `layers[first_seam_layer … last_seam_layer]`, with no gaps
- The column is not projected below the first seam
- One tower id; contour ids follow the wall across layers by polygon overlap, not by island order
- If any layer in the range fails, drop the whole stack

Clustering is `cluster_seam_tower_stacks` in `SeamTower.cpp` with a jump threshold of `max(depth, 4 * line_width)`. Do not require `seam_align_minimum_string_seams`; a first-layer-only stack is a valid pad.

## Collision

After polygons exist, in instance world coordinates, skip a tower if it intersects:

- the same object's slices on **that layer**. A fin aimed at another island (the outer wall) is skipped. A later layer of the same wall sticking into the column is trimmed off that layer, so a textured wall does not cancel the tower.
- another object’s slices (including their planned towers already accepted)
- support islands
- wipe/prime tower bounding box
- outside the plate

Skipped towers: `active_step_add_warning(NON_CRITICAL, "Seam tower skipped: not enough space.")`. The wall prints normally.

## Data flow

Fill of one island (`fill_seam_tower_island`):

1. Concentric closed loops, inset by half a line width then by one line width, same as a brim. No infill, no Arachne. Generate from the **outside** of the island.
2. Keep at most **two** loops (outer wall, then one inner). Drop every further concentric loop.
3. On the object's **first layer only**, keep additional inward loops after those two walls until extra fill width is **5 mm** or the remaining pocket cannot hold another loop. Hardcoded cap; not a setting. Upper layers never keep these extra loops.
4. Emit the kept loops innermost first and the outermost loop last. One loop is a single wall and is the outer wall.
5. Split the outer wall so its first point and its last point are the closest point on that centerline to the object wall start (`hop_target` on that layer). Inner loops may start anywhere.

```
Print processed
  → GCode generation starts
  → SeamPlacer::init (existing)
  → SeamTowerPlanner::build(print, seam_placer)
       cluster seams → polygons per layer from the first seam through the last
       → collision against this layer's slices, other objects, and supports → extrusion collections
       → drop a stack unless every layer in that seam range exists
  → for each layer / island / loop:
       place_seam
       if external (or hole with in_holes) and planner has a tower for this seam
            travel_to(tower start)          // retract allowed
            _extrude inner loops, then outer wall ("seam tower")
            retract, then travel from outer-wall end to loop.start
            skip extrude_inset_lead_in
       extrude loop (scarf unchanged)
       after that object's perimeters on the layer:
            extrude towers on this seam layer that take_matching_tower did not pair
            // travel may retract; no hop
```

Matching key: hop_target + PrintObject. After `place_seam`, match the loop’s first point to stored `Point hop_target` if distance ≤ `gap + line_width`. `printed` is once per layer **per instance** (`PrintObject*` + instance shift).

## Error handling

| Case | Behaviour |
|------|-----------|
| Setting off, spiral vase, no external wall | Planner no-ops |
| Degenerate strip, hole too small, overhang, collision | Skip the whole tower; warning; wall unchanged |
| Invalid config values | UI min blocks negatives; a strip that cannot hold one loop skips that tower; the slice continues |
| Inset start + tower | Skip inset start for that wall only |
| Planner has a tower but no wall start is close enough to hop | After that object's external-wall extruder visit (not an earlier infill or support visit), extrude towers on seam layers that `take_matching_tower` did not pair. Normal travel (retract allowed), no hop. |

Do not fail the slice for skip cases.
