# Testing strategy

This is libslic3r + wx settings, not a LiveView app. There is no Phoenix UI-flow skill. Observable behaviour is proven with Catch2 geometry tests and slice-to-G-code tests (`Slic3r::Test::slice` in `tests/fff_print/test_data.hpp`).

## Map flows to tests

| Flow | Test | Where |
|------|------|--------|
| 1 Enable and slice | Cube, `seam_tower=1`, aligned seams → G-code contains `seam tower` immediately before the matching outer wall; the last tower extrusion ends at the closest point to that wall start; retract then travel between them | `tests/seam_tower/test_seam_tower_gcode.cpp` |
| 1 Wall order | A 2 mm tower emits at most two loops (inner then outer); a one-line-width strip emits one loop | `tests/seam_tower/test_seam_tower_gcode.cpp` or geometry/planner tests |
| 1 Spiral vase | `spiral_mode=1`, `seam_tower=1` → no `seam tower` in G-code | same |
| 1 Validation | `seam_tower_gap` / `depth` / `length` / `min_size` option `min == 0` | `tests/seam_tower/test_seam_tower_config.cpp` |
| 2 Defaults | Strip polygon: inner offset 0.1 mm, depth 2 mm, min length 4 mm on a square | `tests/seam_tower/test_seam_tower_geometry.cpp` |
| 3 Holes on | Ring large enough → island inside the hole | geometry tests |
| 3 Holes skip | Ring below `min_size` → empty polygon | geometry tests |
| 3 Holes off | `in_holes=false` → no hole island even if it fits | planner or geometry |
| 4 Collision | Two squares closer than gap+depth → at most one of the facing towers; skip result empty | `tests/seam_tower/test_seam_tower_collision.cpp` |
| 4 Collision (Print path) | Two objects, two instances, and same-instance supports skip colliding towers | `tests/seam_tower/test_seam_tower_gcode.cpp` |
| 5 Inset start | Both flags on → first-layer G-code has `seam tower` and no `inset start` for that wall | gcode tests |
| 5 Inset without tower | Tower skipped (collision) or disabled → `inset start` still present on first layer | gcode tests |
| 6 Jump | Two seam points farther than threshold → two stacks | `tests/seam_tower/test_seam_tower_cluster.cpp` |
| 6 Overhang | Receding contour → rectangle clipped to previous footprint, not an overhanging strip | geometry tests |
| 6 Grounded column | A tower is present from the first seam layer through the last, and absent entirely when any layer in that range fails | `tests/seam_tower/test_seam_tower_collision.cpp` ("A stack emits a tower only on the layers its seam occupies", "A column whose first layer cannot be built emits nothing on the upper layers") |
| 7 Off | `seam_tower=0` → G-code identical in absence of `seam tower` comments | gcode tests |
| 8 Length vs depth | Island along-wall span follows `length`, not `depth`; a wide depth and short length stay short along the wall | `tests/seam_tower/test_seam_tower_geometry.cpp` |
| 9 Two-wall cap | Depth large enough for many concentric loops → upper-layer fill has ≤ 2 loops | planner fill tests and `tests/seam_tower/test_seam_tower_gcode.cpp` |
| 9 One wall | Strip of one line width → one loop, not skipped | same |
| 10 First-layer brim | First-layer fill has extra inward loops up to 5 mm; the next layer of the same tower has ≤ 2 loops | planner fill tests |
| 10 No leftover | Island filled by two walls → first layer has no extra loops | planner fill tests |

## Error paths in tasks

Flow 1 validation and spiral vase ship in Batch 5 (config/UI) and Batch 4 (G-code ignore). Flow 3 small-hole and Flow 4 collision ship in Batches 1 and 3. Flow 5 error-equivalent (collision skip or disabled → inset remains) ships in Batch 4. Flows 8–10 ship in Batch 7.

## Preview / GUI

No automated GUI test in v1. Preview is covered by asserting `erSeamTower` / `Seam tower` string mapping and that exported G-code roles parse. Settings presence is covered by option registration tests and by listing the keys in `Tab.cpp` (review). Marked in [scope.md](scope.md) as untested beyond that.

## Domain skills

None of the Phoenix/Ash skills apply. Geometry tests follow the same Catch2 style as `tests/inset_start/test_inset_start.cpp`.

## Coverage checklist (Phase 4 of implementing-design)

- [x] Flow 1 happy G-code order
- [x] Flow 1 wall order and outer-wall end point (Batch 6)
- [x] Flow 1 spiral vase skip
- [x] Flow 1 float keys `min == 0`
- [x] Flow 2 default strip dimensions
- [x] Flow 3 hole on / hole skip / holes off
- [x] Flow 4 collision skip
- [x] Flow 5 inset skipped vs kept
- [x] Flow 6 cluster jump + overhang rectangle
- [x] Flow 6 column matches the seam: present from the first seam layer through the last, absent entirely when any layer in that range fails
- [x] Flow 7 setting off
- [x] `erSeamTower` role string + colour slot
- [x] Flow 8 independent length vs depth (Batch 7)
- [x] Flow 9 two-wall cap on layers above the first (Batch 7)
- [x] Flow 10 first-layer brim ≤ 5 mm, absent on the next layer (Batch 7)

Defer: snap-off feel, preview colour polish (scope.md Optional v1).
