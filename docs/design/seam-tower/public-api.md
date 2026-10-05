# Public surface

Compatibility: **additive**. No existing key is renamed or removed.

Who can call: any user via process or per-object settings (same as `seam_position`). No auth/tenant.

## Settings (`PrintObjectConfig`)

Category: Quality. UI group: Seam. Mode: Advanced.

| Key | Type | Default | Constraints | Label |
|-----|------|---------|-------------|-------|
| `seam_tower` | `ConfigOptionBool` | `false` | — | Seam tower |
| `seam_tower_gap` | `ConfigOptionFloat` | `0.1` | `min = 0`, sidetext mm | Seam tower gap |
| `seam_tower_depth` | `ConfigOptionFloat` | `2.0` | `min = 0`, sidetext mm | Seam tower depth |
| `seam_tower_length` | `ConfigOptionFloat` | `4.0` | `min = 0`, sidetext mm | Seam tower length |
| `seam_tower_in_holes` | `ConfigOptionBool` | `false` | — | Seam tower in holes |
| `seam_tower_min_size` | `ConfigOptionFloat` | `3.0` | `min = 0`, sidetext mm | Seam tower min size |

Tooltips:

- **Seam tower:** Print a small sacrificial island next to each stable outer-wall seam. The island is printed first to dump pressure, then the nozzle retracts and travels to the wall.
- **Seam tower gap:** Distance between the object wall and the inner edge of the tower. Large enough to snap off (print tests: below ~1 mm welds). The nozzle retracts before crossing.
- **Seam tower depth:** How far the tower extends away from the wall. The tower is a closed strip: a copy of the wall contour this far out, with the ends capped. Depth does not fill the island solid; at most two walls, plus a first-layer brim (see [architecture.md](architecture.md)).
- **Seam tower length:** How far the tower extends along the wall, centred on the seam. If the seam stack wanders farther than this, the island grows to cover that window.
- **Seam tower in holes:** Also build towers on hole contours when the hole is large enough.
- **Seam tower min size:** Minimum hole size that may receive a tower. Smaller holes skip the tower.

### Valid example

```
seam_tower = 1
seam_tower_gap = 0.1
seam_tower_depth = 2
seam_tower_length = 4
seam_tower_in_holes = 0
seam_tower_min_size = 3
```

Slice of a 20 mm cube with aligned seams produces at least one `seam tower` feature immediately before the corresponding outer wall. On that feature the last extrusion is the tower's outer wall, and it ends at the point closest to the wall start. The tower is also present on the object's first layer.

### Invalid result

`seam_tower_gap = -0.1` → option `min = 0`; the spin control does not accept a negative value (same as `brim_object_gap`).

A depth too thin to hold one line width does not fail the slice: that tower is skipped (see [architecture.md](architecture.md)). One line width is a single wall and is kept.

## Extrusion role

Add `erSeamTower` immediately before `erMixed` in `ExtrusionRole`.

- `role_to_string` / preview legend: `Seam tower`
- G-code feature tag and `_extrude` comment: `seam tower`
- Colour: a distinct entry in `BaseRenderer` role colours (any unused colour; not the same as Outer wall)

Callers: G-code export, G-code preview, time estimate by role. Additive; old G-code files without the tag still load.

## G-code hop contract

The last extrusion of the tower is its outer wall. That loop's end point is the closest point on the outer-wall centerline to the object wall's loop start. Inner loops, when the strip fits more than one, are extruded before that outer wall. Above the object's first layer there are at most two loops. On the first layer, internal brim loops (capped at 5 mm) may appear before those walls.

Travel from that end point to the wall loop start:

- retract after the last tower extrusion
- straight XY travel (no avoid-crossing detour); Z-hop follows the filament lift setting
- nothing extruded between the outer-wall end and the wall start (retract E-only, then travel)
- the wall unretracts at its loop start

Travel *to* the tower before printing it, and travels between the tower's own loops, use the normal travel path (retract allowed).

## Arrange

When `seam_tower` is true for an object, arrange inflation includes an extra `seam_tower_gap + seam_tower_depth` (mm) on top of the existing brim inflation. Not a new public function.

## Invalidation

Changing any of the six keys invalidates G-code generation the same way `first_layer_inset_start` does (`Print.cpp` / `PrintObject.cpp` seam-related opt keys). Geometry keys that change plate footprint (`gap`, `depth`, `in_holes`, `min_size`, enable) also invalidate arrange clearance. **Length does not change arrange inflation** (still `gap + depth`).
