# User flows

Canonical behaviour. Leaf docs must match this file.

UI targets: Quality → Seam group in the process/object settings tab. Preview feature type **Seam tower**. G-code comment `seam tower`. Slicing warning `Seam tower skipped: not enough space.`

## Flow 1: Enable and slice

**Actor:** user  
**Precondition:** at least one object on the plate; `seam_tower` is false  
**Steps:**

1. Open process or object settings → Quality → Seam → enable **Seam tower** → checkbox on; **Seam tower gap**, **Seam tower depth**, **Seam tower length**, **Seam tower in holes**, and **Seam tower min size** are visible.
2. Slice → G-code preview shows closed strips beside aligned outer seams; feature type **Seam tower**.
3. Scrub a layer that has a tower → the tower is extruded immediately before that outer wall (`_extrude` comment `seam tower`, preview type **Seam tower**). Inside the tower, any inner loops come first and the outer wall is last. The outer wall ends at the point on the tower closest to that wall's start. Then a retract, a travel across the gap, then that outer wall unretracts at the seam.

**Error paths:**

- Spiral vase on → **Seam tower** control disabled; no towers in preview or G-code.
- `seam_tower_gap`, `seam_tower_depth`, `seam_tower_length`, and `seam_tower_min_size` have `min = 0` (same spin behaviour as **Brim-object gap**). Values too small to build a strip skip that tower instead of failing the slice.

## Flow 2: Defaults

**Actor:** user  
**Precondition:** Flow 1, settings left at defaults  
**Steps:**

1. Slice with gap 0.1 mm, depth 2.0 mm, and length 4.0 mm.
2. Preview: inner edge of each strip is 0.1 mm from the outer wall; strip thickness is ~2 mm; along-contour length is at least 4 mm, or the seam-stack window if that is larger.

**Error paths:** none beyond Flow 1 validation.

## Flow 3: Holes

**Actor:** user  
**Precondition:** object with at least one hole; `seam_tower` on  
**Steps:**

1. Enable **Seam tower in holes** → checkbox on.
2. Slice a hole whose inscribed size ≥ **Seam tower min size** → a tower sits inside the hole at `gap` from the hole wall.
3. Slice a hole smaller than min size → no tower in that hole; slicing warning `Seam tower skipped: not enough space.`; the hole wall uses a normal seam.

**Error paths:**

- **In holes** off (default) → no towers inside holes even when they would fit.

## Flow 4: Collision

**Actor:** user  
**Precondition:** `seam_tower` on; two objects closer than `gap + depth`, or a tower that would leave the plate / hit the prime tower / hit supports  
**Steps:**

1. Slice → slice succeeds.
2. The colliding tower is omitted. Warning `Seam tower skipped: not enough space.`
3. That wall uses a normal seam (and inset start on first layer if enabled).
4. Arrange with `seam_tower` on leaves `gap + depth` extra clearance around the object versus the setting off.

**Error paths:**

- Every tower on the plate collides → no towers, warnings, objects still print.

## Flow 5: Inset start composition

**Actor:** user  
**Precondition:** `first_layer_inset_start` true; `seam_tower` true  
**Steps:**

1. Slice.
2. First-layer outer wall that has a tower → G-code has `seam tower` then the wall; **no** `inset start` comment for that wall.
3. First-layer wall with no tower (skipped hole, inner wall, collision) → `inset start` still emitted as today.

**Error paths:** none. Scarf settings still apply to the wall after the hop.

## Flow 6: Unstable seams

**Actor:** user  
**Precondition:** `seam_tower` on; `seam_position` = random (or seams that jump more than the threshold)  
**Steps:**

1. Slice.
2. Seams that stay within the jump threshold share one tower from the first layer of that seam through the last. A seam that starts partway up does not grow a tower on the layers below it.
3. A stack that cannot be printed on any layer in that range (overhang, collision with the same object or another, or a missing contour) is skipped entirely. That seam is a normal seam.

**Error paths:**

- A layer in the seam's range cannot hold the strip → no tower, warning `Seam tower skipped: not enough space.` The wall prints with a normal seam.

## Flow 7: Off

**Actor:** user  
**Precondition:** `seam_tower` had been on  
**Steps:**

1. Disable **Seam tower** → dependent fields hide.
2. Slice → no `erSeamTower` paths, no `seam tower` comments, inset start and seams match current behaviour.

**Error paths:** none.

## Flow 8: Independent length and depth

**Actor:** user  
**Precondition:** `seam_tower` on; defaults otherwise  
**Steps:**

1. Set **Seam tower depth** to 8 mm and leave **Seam tower length** at 4 mm → slice.
2. Preview: the island extends ~8 mm away from the wall and ~4 mm along the wall (plus any larger stack window). Depth did not become the along-wall size.
3. Set **Seam tower length** to 10 mm and depth to 2 mm → slice. The island is ~2 mm thick and ~10 mm along the wall.

**Error paths:**

- Length `0` with a strip that cannot hold one loop → that tower is skipped; slice succeeds (Flow 1 validation).

## Flow 9: At most two walls above the first layer

**Actor:** user  
**Precondition:** Flow 8 step 1 (depth large enough that a solid fill would have many concentric loops)  
**Steps:**

1. Slice.
2. Scrub a layer **above** the object's first layer that has a tower → that tower has **at most two** closed loops (outer wall plus at most one inner). The interior of the island is empty. Inner loop, if present, prints before the outer wall. Hop contract unchanged.
3. Increase depth further and slice again → still at most two walls on that upper layer (no extra plastic from depth).

**Error paths:**

- Depth so thin that only one loop fits → one wall (the outer wall). Not skipped for lacking a second wall.

## Flow 10: First-layer internal brim

**Actor:** user  
**Precondition:** `seam_tower` on; depth large enough to leave a pocket inside two walls  
**Steps:**

1. Slice.
2. Scrub the object's **first layer** → inside the two walls, extra concentric loops fill inward up to 5 mm, or until the island is full, whichever comes first. Those extra loops print before the two walls; the outer wall is still last; hop contract unchanged.
3. Scrub the next layer → no brim loops; at most two walls.

**Error paths:**

- Island already filled by one or two walls → no extra brim loops (nothing left to fill).
- Remaining pocket thinner than one line width → no extra loop.
