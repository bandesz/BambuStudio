# User flows

Canonical behaviour. Other docs match this file.

## Flow 1: Wedge, outer wall printed first
**Actor:** User slicing a part that uses outer/inner.
**Precondition:** The test wall in [testing-strategy.md](testing-strategy.md) is on the plate. Wall generator is Classic. Order of walls is outer/inner. Outer line width is 0.42 mm, inner is 0.45 mm, layer height is 0.2 mm, Minimum wall width is 85%, nozzle diameter is 0.4 mm. Reserve outer wall is on.

**Steps:**
1. Slice.
2. Open the layer whose `slice_z` is 5.3 and color the preview by line type.
3. Open the layer whose `slice_z` is 5.9.

**Expected:** Outer, inner, and gap-fill lines match the same slice with Reserve outer wall off. The flat-face outer bead stays at Y = 0.21 and 0.42 mm wide. A line is shortened only where it would enter the outer bead past the normal spacing overlap, about `layer_height * (1 - π/4)`. On `slice_z` 5.9, infill may use that overlap band and may still overlap an inner wall. It does not enter the outer-bead core.

## Flow 2: Plain 1 mm wall
**Actor:** User.
**Precondition:** Flow 1.

**Steps:**
1. Inspect the layer whose `slice_z` is 1.9.
2. Inspect the layer whose `slice_z` is 4.1. The wedge has just started. The remaining channel is still below 0.34 mm.

**Expected:** The same lines as with Reserve outer wall off, including a middle gap line when Classic prints one. The checkbox does not open a gap in a wall that was already filled.

## Flow 3: Setting off
**Actor:** User.
**Precondition:** Reserve outer wall is off. Everything else matches Flow 1.

**Steps:**
1. Slice the same plate.

**Expected:** Toolpaths match a build from before this feature. On `slice_z` 5.3 a later line overlaps the outer bead. The flat face on that layer is the case this feature removes.

## Flow 4: Setting on, but ignored
**Actor:** User.
**Precondition:** Reserve outer wall is on, and either Order of walls is not outer/inner, or the wall generator is Arachne.

**Steps:**
1. Slice.

**Expected:** The slicer warns with the string in [public-api.md](public-api.md). Toolpaths match Reserve outer wall off. The slice succeeds.

**Error paths:**
- Both the wall order and the generator disqualify the setting, and no other warning is already stored: the wall-order warning is the one shown.
- Another warning is already stored: that warning stays, and the toolpaths still match the checkbox off.
