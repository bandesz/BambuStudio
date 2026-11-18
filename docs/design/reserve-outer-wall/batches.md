# Batches

`SLIC3R_BUILD_TESTS` defaults to OFF. Enable it, then build the Catch2 target.

**Verify (every task):**

```
cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target reserve_outer_wall_tests && ctest --test-dir build -R reserve_outer_wall_tests --output-on-failure
```

## Batch 1: Setting, Classic interior, infill clip

**Status:** done
**Scope:** The checkbox, the Classic outer-bead reservation, and the infill clip. Excludes Arachne generation, inner/outer toolpath changes, and spiral vase.

### Task 1.1: Checkbox and warnings
**Status:** done — `3b640bfd4`
- Files: Modify `src/libslic3r/PrintConfig.hpp`, `src/libslic3r/PrintConfig.cpp`, `src/libslic3r/Preset.cpp`, `src/libslic3r/Print.cpp`, `src/libslic3r/PrintObject.cpp`, `src/slic3r/GUI/Tab.cpp`; Create `tests/reserve_outer_wall/CMakeLists.txt`, `tests/reserve_outer_wall/reserve_outer_wall_tests.cpp`, `tests/reserve_outer_wall/test_reserve_outer_wall.cpp`; Modify `tests/CMakeLists.txt`
- TDD: yes
- UI flow: Flow 4
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target reserve_outer_wall_tests && ctest --test-dir build -R reserve_outer_wall_tests --output-on-failure`
- Domain skills: none
- Acceptance:
  - `reserve_outer_wall` defaults to false on `PrintRegionConfig`.
  - Classic, outer/inner, checkbox on, spiral vase off: `Print::validate` does not set a `reserve_outer_wall` warning.
  - Classic, inner/outer, checkbox on, and no earlier warning: `warning->string` is `Reserve outer wall is ignored unless the order of walls is outer/inner` and `warning->opt_key` is `reserve_outer_wall`.
  - Arachne, outer/inner, checkbox on, and no earlier warning: `warning->string` is `Reserve outer wall is ignored unless the wall generator is Classic`.
  - Arachne and inner/outer together: the wall-order string is the one stored.
  - A non-empty `warning->string` already present is left unchanged.
  - Quality → Advanced shows the checkbox labeled Reserve outer wall immediately after Order of walls.
- Out of scope: toolpath changes, slice comparisons

### Task 1.2: Interior beads
**Status:** done — `fb7293bff`
- Files: Modify `src/libslic3r/PerimeterGenerator.cpp`, `src/libslic3r/Layer.hpp` (field `reserved_outer_wall_interior`); Test `tests/reserve_outer_wall/test_reserve_outer_wall.cpp`
- TDD: yes
- UI flow: Flow 1, Flow 2, Flow 3
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target reserve_outer_wall_tests && ctest --test-dir build -R reserve_outer_wall_tests --output-on-failure`
- Domain skills: none
- Acceptance:
  - Mesh, planes, and measurements are those in [testing-strategy.md](testing-strategy.md).
  - Checkbox on, `slice_z` 5.3: flat-face `erExternalPerimeter` Y is within 0.01 mm of the `slice_z` 1.9 flat-face Y, and within 0.01 mm of 0.21. Path width is 0.42 ± 0.001. Later-line overlap area ≤ 0.01 mm². An `erPerimeter` path overlaps X [14.5, 15.5] at width 0.45 ± 0.001.
  - Checkbox on, `slice_z` 1.9 and 4.1: no `erPerimeter` and no `erGapFill`.
  - Checkbox off, `slice_z` 5.3: some later line has overlap area > 0.02 mm².
  - Classic + inner/outer, and Arachne + outer/inner, checkbox on: extrusion polylines match checkbox off within 0.01 mm.
  - `reserved_outer_wall_interior` is non-empty on those Classic outer/inner layers and empty when the flag does not apply.
- Out of scope: infill-role overlap on `slice_z` 5.8, warning strings

### Task 1.3: Infill stays inside the interior
**Status:** done — `53275a5d8`
- Files: Modify `src/libslic3r/PrintObject.cpp` (intersect fill surfaces with `reserved_outer_wall_interior` after vertical and horizontal shell discovery); Modify `src/libslic3r/PerimeterGenerator.cpp` if the fill region written in Task 1.2 still crosses `interior`; Test `tests/reserve_outer_wall/test_reserve_outer_wall.cpp`
- TDD: yes
- UI flow: Flow 1
- Verify: `cmake -S . -B build -DSLIC3R_BUILD_TESTS=ON && cmake --build build --target reserve_outer_wall_tests && ctest --test-dir build -R reserve_outer_wall_tests --output-on-failure`
- Domain skills: none
- Acceptance:
  - Checkbox on, `slice_z` 5.9: every fill-role extrusion has overlap area with the outer bead ≤ 0.01 mm².
  - Where an `erPerimeter` bead exists on that layer, infill coverage may intersect that inner bead (the 15% overlap). It does not intersect the outer bead beyond the 0.01 mm² allowance.
  - Checkbox off is not clipped. `reserved_outer_wall_interior` empty skips the intersect.
- Out of scope: changing shell thickness, Arachne infill, new overlap percent
