# Seam tower

Sacrificial start pad for outer-wall seams. On each layer that has a tower, the slicer prints that tower immediately before the wall, finishes the tower's outer wall at the point closest to the wall start, retracts, travels across the gap, then unretracts on the wall. The tower is a continuous column from that seam's first layer through its last. Pressure is dumped on the tower; the gap must stay an air gap so the tower can snap off.

**Status:** v1 design, revised for a retracted gap crossing (print tests: unretracted hop welds).

## Implementation status

**Status:** complete
**Review fixes:** `cc72f99a3`, `039ef130b`, `5740dde48`, `3244f5cbb`, `1c5a4bfcf`, `5188dcc49`, `a11765f00`, `47f501180`, `359c312b6`, `2fa63992d`

## Where to go

| Doc | Contents |
|-----|----------|
| [scope.md](scope.md) | Problem, constraints, assumptions, non-goals |
| [user-flows.md](user-flows.md) | Canonical user-visible behaviour (source of truth) |
| [public-api.md](public-api.md) | Settings, extrusion role, compatibility |
| [architecture.md](architecture.md) | Planner, geometry, G-code pairing, data flow, errors |
| [testing-strategy.md](testing-strategy.md) | How each flow is proven |
| [batches.md](batches.md) | Implementation batches and TDD-sized tasks |

## Decisions

| Topic | Choice | Why |
|-------|--------|-----|
| Purpose | Sacrificial start pad | Distinct from prime/wipe tower and from inset start |
| Print order | Paired: tower → retract → travel → that wall | Gap must snap off; unretracted hop welded in print tests |
| Tower walls | Inner loops first; outer wall last (one loop is a single wall) | The nozzle must finish on the outer wall, nearest the seam |
| Outer-wall end | Closest point on that wall to the object wall start | Travel still crosses the gap, not a detour |
| Column | Continuous from the stack's first seam through its last seam | A tower is not extended below that seam, and the tower is skipped entirely if any layer in that range cannot be built |
| Clustering | Stable stacks with a jump threshold | Multiple towers; height = last seam in the stack |
| Shape | Closed offset strip (gap then depth × length), rectangle when the contour recedes | Self-supporting; your 2 mm parallel copy with capped ends |
| Fill | At most two concentric walls; first-layer internal brim ≤ 5 mm | Changing depth must not solid-fill the island |
| Length vs depth | Keep `seam_tower_depth`; add `seam_tower_length` (default 4 mm) | Depth is radial; length is along the wall; do not rename depth to size |
| Enablement | Opt-in per object, default off | Extra plastic and plate structures |
| No-fit | Skip that tower and warn; slice succeeds | Quality aid must not fail the plate |
| Inset start | Skipped on a wall that has a tower this layer; scarf still runs | One start pad, not two |
| Architecture | Planner after `SeamPlacer::init`; emit in `extrude_loop` | Seams are only final then; geometry stays unit-testable |

## Public surface

Additive `PrintObjectConfig` keys under Quality / Seam (`seam_tower`, `seam_tower_gap`, `seam_tower_depth`, `seam_tower_length`, `seam_tower_in_holes`, `seam_tower_min_size`) plus extrusion role `erSeamTower`. Depth is not renamed. Defaults and invalid inputs: [public-api.md](public-api.md).

## Batches (summary)

1. **Geometry library** — offset strip, caps, overhang clip, rectangle fallback, hole/min-size skip
2. **Clustering** — stable stacks from seam points; columns from the first seam through the last, with no pad underneath
3. **Planner + collision + arrange** — wire to `SeamPlacer`, skip-and-warn, inflate arrange by gap+depth
4. **G-code pairing** — `erSeamTower` emit, retract then travel, skip inset start
5. **Settings, preview, UI** — config keys, Seam tab, role colour/legend, spiral-vase disable
6. **Wall order and first-seam column** — outer wall last, end next to the wall start, column from the first seam through the last
7. **Two-wall fill, length, first-layer brim** — cap walls at two; `seam_tower_length`; layer-0 internal brim ≤ 5 mm

Task-level Acceptance, Verify, and files: [batches.md](batches.md).
