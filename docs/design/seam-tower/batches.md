# Batches

This tree’s CMake option `SLIC3R_BUILD_TESTS` defaults to OFF. Enable it, then build the Catch2 target.

**Verify (all geometry/planner/gcode tasks):**

```
cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure
```

Catch tags are in brackets.

---

## Batch 1: Geometry library

**Status:** done  
**Excludes:** clustering across layers, collision with other objects, emit.

### Task 1.1: Closed offset strip
**Status:** done — `650096254`

- Files: Create `src/libslic3r/SeamTower.hpp`, `src/libslic3r/SeamTower.cpp`; Create `tests/seam_tower/test_seam_tower_geometry.cpp`, `tests/seam_tower/CMakeLists.txt`; Modify `src/libslic3r/CMakeLists.txt`, `tests/CMakeLists.txt`
- TDD: yes
- UI flow: Flow 2
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Domain skills: none
- Acceptance:
  - `make_seam_tower_island(contour, seam, gap=0.1mm, depth=2mm, …)` on a 20 mm square returns a closed `ExPolygon` whose inner edge is 0.1 mm outside the wall and whose thickness is 2 mm ± one line width.
  - Along-contour length is at least `depth`. Superseded: along-contour length is `max(seam_tower_length, stack XY window)`.
- Out of scope: holes, overhang clip, G-code

### Task 1.2: Caps and degenerates
**Status:** done — `95ffee5bb`

- Files: Modify `src/libslic3r/SeamTower.cpp`; Test `tests/seam_tower/test_seam_tower_geometry.cpp`
- TDD: yes
- UI flow: Flow 2
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - End caps close the inner and outer arcs into one polygon (`contour` is closed; area &gt; 0).
  - If the contour is shorter than `depth`, the function still returns a non-empty island or empty (skip), never a self-intersecting bow-tie.
- Out of scope: holes

### Task 1.3: Hole fit and min size
**Status:** done — `aac588de1`

- Files: Modify `src/libslic3r/SeamTower.cpp` / `.hpp`; Test `tests/seam_tower/test_seam_tower_geometry.cpp`
- TDD: yes
- UI flow: Flow 3
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - On a hole large enough for `min_size`, the island lies inside the hole and is `gap` from the hole wall.
  - On a hole smaller than `min_size`, the function returns empty.
  - `in_holes=false` path is a caller concern; this task only implements the hole construction + min-size skip.
- Out of scope: planner flag, G-code, collision with a second object

### Task 1.4: Overhang clip and rectangle fallback
**Status:** done — `83710f1a3`

- Files: Modify `src/libslic3r/SeamTower.cpp` / `.hpp`; Test `tests/seam_tower/test_seam_tower_geometry.cpp`
- TDD: yes
- UI flow: Flow 6
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - Given previous-layer tower T0 and a receding contour, layer 1’s island is contained in T0 (no overhang).
  - Where the contour-following strip would leave T0, the result uses a rectangle clipped to T0 instead of hanging.
  - If nothing of at least two line widths fits in T0, the result is empty (skip). Superseded: a strip that cannot hold one line width is empty; one loop is a valid single wall.
- Out of scope: clustering, G-code

---

## Batch 2: Clustering

**Status:** done  
**Excludes:** Print/GCode wiring.

### Task 2.1: Stable stacks
**Status:** done — `2907cb3cc` (+ review fixes: `905c24d01`)

- Files: Modify `src/libslic3r/SeamTower.hpp` / `.cpp`; Create `tests/seam_tower/test_seam_tower_cluster.cpp`
- TDD: yes
- UI flow: Flow 6
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - Seams within the jump threshold on successive layers form one stack; a jump opens a second stack.
  - Each seam is assigned to exactly one stack.
  - Stack height ends at the last seam layer in that stack.
- Out of scope: polygon build, collision

### Task 2.2: Column from the bed
**Status:** done — `94aeaee47`. Superseded: the planner no longer calls `project_seam_tower_stacks_to_bed`. A tower starts on its first seam layer.

- Files: Modify `src/libslic3r/SeamTower.cpp`; Test `tests/seam_tower/test_seam_tower_cluster.cpp`
- TDD: yes
- UI flow: Flow 6
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - A stack whose first seam is above layer 0 still has a footprint on layer 0 at that seam’s XY (rectangle or projected strip). Superseded: the tower starts on the first seam layer and is absent below it.
  - `make_seam_tower_island` for those lower layers does not require a seam point on that layer. Superseded: the tower starts on the first seam layer and is absent below it.
- Out of scope: skip when the column collides (Batch 3)

---

## Batch 3: Planner, collision, arrange

**Status:** done

**Scope:** Wire geometry to `SeamPlacer` results; skip-and-warn; arrange inflation.  
**Excludes:** `extrude_loop` pairing (Batch 4), settings widgets (Batch 5). Planner may read config keys once they exist; if Batch 5 has not landed, use explicit arguments in the planner API and keep a thin config adapter.

### Task 3.1: Planner from SeamPlacer
**Status:** done — `7754bb7f2`

- Files: Create `src/libslic3r/GCode/SeamTowerPlanner.hpp` / `.cpp`; Modify `src/libslic3r/CMakeLists.txt`, `src/libslic3r/GCode.cpp` (call `build` after `m_seam_placer.init`); Test `tests/seam_tower/test_seam_tower_planner.cpp`
- TDD: yes
- UI flow: Flow 1
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - `SeamTowerPlanner::build` produces at least one per-layer `erSeamTower` collection for a cube with aligned seams when enabled.
  - Disabled / spiral vase → empty planner.
  - Hole stacks only when `in_holes` is true.
- Out of scope: actually writing G-code; inset start

### Task 3.2: Collision skip and warning
**Status:** done — `918fab739`

- Files: Modify `src/libslic3r/GCode/SeamTowerPlanner.cpp`; Create `tests/seam_tower/test_seam_tower_collision.cpp`; Modify Print warning site as needed
- TDD: yes
- UI flow: Flow 4
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - Island that intersects another object, the plate boundary, or the prime-tower box is dropped.
  - Geometry skip (hole below `min_size`, empty overhang clip) is dropped with the same message.
  - Skip records the message `Seam tower skipped: not enough space.` (assertable on the planner result / warning list).
  - Other towers on the same object still build.
- Out of scope: arrange

### Task 3.3: Arrange inflation
**Status:** done — `3a0545ca9`

- Files: Modify `src/libslic3r/Arrange.cpp` (or the call site that sets `ap.brim_width`); Test `tests/seam_tower/test_seam_tower_collision.cpp` or existing arrange tests
- TDD: yes
- UI flow: Flow 4 step 4
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - With `seam_tower` on, arrange inflation is at least `gap + depth` larger than with it off (same brim).
- Out of scope: exact polygon arrange

---

## Batch 4: G-code pairing

**Status:** done

**Scope:** Paired emit, hop contract, inset-start skip. Co-ships Flow 1/5/7 G-code behaviour.

### Task 4.1: Emit tower then hop then wall
**Status:** done — `3a6cae56d` (+ review fixes: `fd3d2bd34`)

- Files: Modify `src/libslic3r/GCode.cpp`, `src/libslic3r/GCode.hpp`; Create `tests/seam_tower/test_seam_tower_gcode.cpp`
- TDD: yes
- UI flow: Flow 1
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - For a sliced 20 mm cube with `seam_tower=1` and aligned seams, G-code contains `_extrude` comment `seam tower` immediately before that outer wall.
  - Between the last seam-tower extrusion and the first extrusion of that wall the nozzle retracts (`G10` / retract comment), then travels (`travel to wall`); nothing is extruded on that travel.
  - `seam_tower=0` → no `seam tower` comments (Flow 7).
  - `spiral_mode=1` → no `seam tower` comments (Flow 1 error path).
- Out of scope: inset start, UI

### Task 4.2: Skip inset start when a tower printed
**Status:** done — `cd6630616`

- Files: Modify `src/libslic3r/GCode.cpp` (`extrude_inset_lead_in`); Test `tests/seam_tower/test_seam_tower_gcode.cpp`
- TDD: yes
- UI flow: Flow 5
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - First layer, both flags on, wall with a tower → G-code has `seam tower` and does **not** contain `inset start` for that island.
  - First layer, tower skipped / disabled → `inset start` still present when `first_layer_inset_start` is on.
- Out of scope: scarf changes

---

## Batch 5: Settings, role, preview, UI

**Status:** done

**Scope:** Public keys, `erSeamTower` plumbing, Seam tab, toggles.  
**Excludes:** new geometry behaviour.

### Task 5.1: Config keys and invalidation
**Status:** done — `6c27df389`

- Files: Modify `src/libslic3r/PrintConfig.hpp`, `src/libslic3r/PrintConfig.cpp`, `src/libslic3r/Print.cpp`, `src/libslic3r/PrintObject.cpp`, `src/libslic3r/Preset.cpp`; Create `tests/seam_tower/test_seam_tower_config.cpp`
- TDD: yes
- UI flow: Flow 1 error paths
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - Five keys exist with defaults `false`, `0.1`, `2.0`, `false`, `3.0`.
  - `seam_tower_gap`, `seam_tower_depth`, and `seam_tower_min_size` have `min == 0`.
  - Changing any key is in the G-code invalidation list next to `first_layer_inset_start`.
- Out of scope: wx widgets

### Task 5.2: Extrusion role and preview tables
**Status:** done — `8e234cc48`

- Files: Modify `src/libslic3r/ExtrusionEntity.hpp`, `src/libslic3r/ExtrusionEntity.cpp`, `src/slic3r/GUI/GCodeRenderer/BaseRenderer.cpp`, `src/libslic3r/GCode/GCodeProcessor.cpp` (role parse), and any `switch (role)` that must be exhaustive
- TDD: yes
- UI flow: Flow 1 step 2
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - `role_to_string(erSeamTower) == "Seam tower"` (localized via `L()`).
  - `string_to_role` round-trips.
  - Role colour array has one slot for `erSeamTower`; `erCount` still fits in 32 bits.
- Out of scope: picking a final colour with design review

### Task 5.3: Seam tab and field toggles
**Status:** done — `f8229bfd3`

- Files: Modify `src/slic3r/GUI/Tab.cpp`, `src/slic3r/GUI/ConfigManipulation.cpp`, `src/slic3r/GUI/GUI_Factories.cpp`
- TDD: no (static settings markup)
- UI flow: Flow 1 steps 1, Flow 7 step 1, Flow 1 spiral vase disable
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure` (config tests still pass; this task is wiring)
- Domain skills: none
- Acceptance:
  - Quality → Seam shows **Seam tower** after `first_layer_inset_start`.
  - Gap, depth, in holes, min size are visible only when `seam_tower` is on, `wall_loops > 0`, and spiral vase is off.
  - Spiral vase on disables the **Seam tower** checkbox.
- Out of scope: translations beyond `L("…")` wrappers

---

## Batch 6: Wall order and grounded column

**Status:** done

**Scope:** The tower's last extrusion is its outer wall and ends next to the object wall start. Every tower is a continuous column from its first seam layer through its last. Supersedes the Batch 1 "two line widths" skip: one loop is a valid single wall. The earlier "column from the object's first layer" rule is superseded.
**Excludes:** new settings, changing seam placement, scarf.

### Task 6.1: Inner loops first, outer wall ends at the wall start
**Status:** done — `1fd100eaf`

- Files: Modify `src/libslic3r/GCode/SeamTowerPlanner.cpp`; Test `tests/seam_tower/test_seam_tower_gcode.cpp` or `tests/seam_tower/test_seam_tower_geometry.cpp`
- TDD: yes
- UI flow: Flow 1 step 3
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - A strip deep enough for several loops emits the innermost loop first and the outermost loop last.
  - The outer loop's first and last point is the closest point on that centerline to the object wall start for that layer.
  - A strip that fits one loop emits that single wall and does not skip it. A strip narrower than one line width is still empty.
- Out of scope: G-code hop text, bed projection

### Task 6.2: Hop leaves the outer-wall end
**Status:** done — `df016a553`

- Files: Modify `src/libslic3r/GCode.cpp`; Test `tests/seam_tower/test_seam_tower_gcode.cpp`
- TDD: yes
- UI flow: Flow 1 step 3
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - For a sliced 20 mm cube with `seam_tower=1` and aligned seams, the last `seam tower` extrusion before the outer wall ends at the point closest to that wall's start.
  - The travel between that point and the wall start retracts, then a straight XY move (`travel to wall`); nothing is extruded on that travel.
  - No other extrusion is emitted between them.
- Out of scope: inset start, which layers exist

### Task 6.3: Column from the first layer, or nothing
**Status:** done — `1839c5f93` (+ review fixes: `7fc6097bc`, `3c4268ffe`, `359c312b6`)

- Files: Modify `src/libslic3r/GCode/SeamTowerPlanner.cpp`, `src/libslic3r/SeamTower.cpp` as needed; Test `tests/seam_tower/test_seam_tower_cluster.cpp`, `tests/seam_tower/test_seam_tower_gcode.cpp`
- TDD: yes
- UI flow: Flow 6
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - A stack emits a tower only on the layers its seam occupies. Layers below the first seam stay empty.
  - If any layer in that range cannot be built, the planner emits no tower for the stack, and records `Seam tower skipped: not enough space.`
- Out of scope: wall order, hop geometry

---

## Batch 7: Two-wall fill, length, first-layer brim

**Status:** done

**Scope:** Independent along-wall length; concentric fill capped at two walls; first-layer internal brim ≤ 5 mm. Supersedes “fill the whole island” in `fill_seam_tower_island` and “along-contour length from depth” in geometry.  
**Excludes:** renaming depth; a brim-width setting; auto-widening depth with height.

### Task 7.1: `seam_tower_length` setting

**Status:** done — `0042034ba`

- Files: Modify `src/libslic3r/PrintConfig.hpp`, `src/libslic3r/PrintConfig.cpp`, `src/libslic3r/Print.cpp`, `src/libslic3r/PrintObject.cpp`, `src/slic3r/GUI/Tab.cpp`, `src/slic3r/GUI/ConfigManipulation.cpp`; Test `tests/seam_tower/test_seam_tower_config.cpp`
- TDD: yes
- UI flow: Flow 1 step 1, Flow 8
- Verify: `cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Domain skills: none
- Acceptance:
  - Key `seam_tower_length` exists with default `4.0` and `min == 0`.
  - Changing it invalidates G-code. It does **not** add to arrange inflation (still `gap + depth`).
  - Seam tab shows **Seam tower length** with gap/depth when `seam_tower` is on.
- Out of scope: island geometry, fill

### Task 7.2: Island span uses length

**Status:** done — `f02a05b85`

- Files: Modify `src/libslic3r/SeamTower.cpp` / `.hpp`, `src/libslic3r/GCode/SeamTowerPlanner.cpp`; Test `tests/seam_tower/test_seam_tower_geometry.cpp`
- TDD: yes
- UI flow: Flow 2, Flow 8
- Verify: `cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - `make_seam_tower_island` along-contour length is `max(length, stack window)`, not `max(depth, stack window + depth)`.
  - Depth 8 mm and length 4 mm → island ~8 mm radial and ~4 mm along the wall on a square (no stack window).
  - Length 10 mm and depth 2 mm → ~2 mm radial and ~10 mm along the wall.
- Out of scope: fill loop count

### Task 7.3: Cap fill at two walls

**Status:** done — `4c915c783`

- Files: Modify `src/libslic3r/GCode/SeamTowerPlanner.cpp`; Test `tests/seam_tower/test_seam_tower_gcode.cpp` and/or planner tests in `tests/seam_tower/`
- TDD: yes
- UI flow: Flow 9
- Verify: `cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - For a first-layer flag **false** (upper layer) and an island deep enough for many concentric loops, `fill_seam_tower_island` (or equivalent) emits **at most two** loops, inner then outer last.
  - A one-line-width island still emits one loop.
  - Increasing depth does not add a third loop on that layer.
- Out of scope: first-layer extra loops (Task 7.4)

### Task 7.4: First-layer internal brim ≤ 5 mm

**Status:** done — `d70f585f3` (+ review fixes: `46bcff204`)

- Files: Modify `src/libslic3r/GCode/SeamTowerPlanner.cpp`; Test `tests/seam_tower/test_seam_tower_gcode.cpp` and/or planner tests
- TDD: yes
- UI flow: Flow 10
- Verify: `cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - Object first layer, island with a pocket inside two walls: extra concentric loops inward from the inner wall, extra fill width ≤ 5 mm, or until the island is full.
  - Those extra loops emit before the two walls; outer wall last; hop target still on the outer wall.
  - The next layer of the same tower has no extra loops (Task 7.3 cap).
  - If two walls already fill the island, no extra loops.
- Out of scope: a brim-width config key

### Task 7.5: G-code — large depth is not solid-filled

**Status:** done — `36dccd144`

- Files: Test `tests/seam_tower/test_seam_tower_gcode.cpp`; modify planner/G-code only if Task 7.3–7.4 left a pairing bug
- TDD: yes
- UI flow: Flow 9, Flow 10
- Verify: `cmake --build build --target seam_tower_tests && ctest --test-dir build -R seam_tower_tests --output-on-failure`
- Acceptance:
  - Slice a cube with `seam_tower=1`, `seam_tower_depth=8`, `seam_tower_length=4`: a `seam tower` feature on a layer above the first has at most two extrusion loops; the first-layer tower of that stack may have more loops than the layer above; after the outer-wall end the nozzle retracts and travels to the wall.
- Out of scope: UI screenshots

---

## Batch verification

After each batch, run the Verify command at the top of this file.
