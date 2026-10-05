# Scope

## Problem

The first extrusion of an outer wall dumps leftover pressure as a visible blob at the seam. Inset start (in-wall lead-in on the first layer) hides that blob *inside* the part. Seam tower hides it *outside*, on a disposable island, on every layer that has a tower, then the nozzle retracts before the travel to the wall.

## Constraints

- Gap must be an air gap so the tower snaps off (print tests: below ~1 mm welds completely).
- The tower must stand without overhangs (not by welding to the object as support).
- Each seam point on a layer belongs to at most one tower.
- Inner walls never get a tower. Holes are optional.
- Existing seam placement, scarf, wipe/prime tower, brim, and inset start stay in place except for the documented composition rule.
- `ExtrusionRole` visibility flags are `uint32_t`; adding `erSeamTower` must keep `erCount <= 32`.

## Assumptions

These were chosen without a further product fork; object in Phase 5 if they are wrong.

1. **Jump threshold** is not a setting. A seam continues the same stack when its XY distance to the previous layer’s seam is `<= max(depth, 4 × outer wall line width)` (same order as `SeamPlacer::seam_align_tolerable_dist_factor`).
2. **Along-contour length** is `max(seam_tower_length, stack XY window)` on the contour (half-span `length / 2` centred on the seam). Depth no longer sets this span. Caps stay non-degenerate because length defaults to 4 mm and `min = 0` still skips a strip that cannot hold a loop.
3. **A tower spans the layers of its seam**, from the first sample in the stack through the last, with no gaps. It is not extended below the first seam. If any layer in that range cannot be built, the whole tower is skipped. The same wall keeps one contour id across layers, matched by overlap, so a fin appearing later does not split the outer seam into a shorter tower. Raft layers themselves do not get a tower.
4. **Random / scattered seams** are best-effort. Aligned, rear, and painted seams are the intended mode. A forest of one-layer columns is acceptable to skip via collision; we do not add a separate min-stack setting in v1.
5. **Tower extrusion** uses the same flow, line width, and speed as the external perimeter it serves, so pressure at the hop matches the wall.
6. **Fill** of the strip is concentric wall loops (brim-style offsets), not infill patterns and not Arachne. Loops are printed innermost first and the outer wall last. **At most two walls** on every layer above the object's first layer (outer wall plus one inner, taken from the outside of the island). A strip that fits only one loop is a single wall. Remaining interior stays empty. On the object's first layer only, continue those offsets **inward from the inner wall** as an internal brim, stopping at **5 mm** of extra fill or when the island is full, whichever comes first. The 5 mm cap is hardcoded (not a setting). One loop is still a valid single wall.
7. **Approach to the tower**, and travels between the tower's own loops, may retract. After the last tower loop the nozzle retracts, then travels straight to the wall start. The wall unretracts. An unretracted hop is rejected: it welds at small gaps and strings at large gaps.
8. **Hop landing** is the object wall's loop start after `place_seam`. The outer wall is split so it starts and ends at the closest point on that wall's centerline to the wall start. The hop is the straight move between those two points. The tower is extruded immediately before that wall, so nothing is printed between the outer-wall end and the wall start.
9. **`seam_tower_min_size`** is the minimum hole inscribed size (mm) that must remain after placing a strip of `depth` plus `gap` on the hole wall. Default 3.0 mm. Outside the object there is no min-size check beyond collision.
10. **Arrange inflation** is `gap + depth` added to the object’s arrange brim/inflation when `seam_tower` is on (conservative; exact polygons are not known at arrange time).
11. **Warning copy:** `Seam tower skipped: not enough space.` One warning per skipped tower cause is enough; do not spam per layer.
12. **No weld to the object.** Print tests: gap below ~1 mm fully welds; the unretracted hop also strings. Retract before crossing. Snap-off is a requirement, not optional tacking.
13. **Sequential print by object:** towers belong to that object and print with it.
14. **Setting visibility:** gap, depth, length, in-holes, and min-size are shown only when `seam_tower` is on, perimeters exist, and spiral vase is off.
15. **Float mins** block the spin control (`min = 0`), they do not abort slicing. A region narrower than one outer-wall line width skips that tower. One loop is a valid single wall. (The earlier “two line widths” skip is superseded by this rule.)

## Non-goals (v1)

- User-painted or user-placed tower anchors
- Towers on inner walls (`erPerimeter`)
- Auto-shrinking depth to fit a gap
- Using the tower as a multi-material wipe / prime volume
- On by default in quality profiles
- Separate jump-threshold or speed settings
- Renaming `seam_tower_depth` to “size”
- A user-facing brim-width setting (5 mm cap is fixed)
- Auto-widening depth with height
- Fusing the tower to the wall on purpose as a tear-off tab along the full height
- Changing inset start’s own behaviour except skipping it when a tower is present

## Optional v1 (explicitly untested)

- Preview colour aesthetics beyond a distinct `erSeamTower` entry
- Exact snap-off ease (depends on filament and 0.1 mm gap)
- Interaction with fuzzy skin, scarf-on-circles, and wipe-on-loops beyond “scarf still runs after the hop”
