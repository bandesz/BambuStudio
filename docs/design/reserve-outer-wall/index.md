# Reserve outer wall

When the outer wall is printed first, a later line that overlaps the outer bead pushes that bead outward. On a thin wall this shows up as a short ridge on the flat face, on the layers where a bump on the other side is thick enough to spawn that later line.

**Status:** v1 design.

## Implementation status
**Status:** complete
**Review fixes:** `eeba08335`

## Where to go

| Doc | Contents |
|-----|----------|
| [scope.md](scope.md) | Problem, constraints, assumptions, non-goals |
| [user-flows.md](user-flows.md) | Canonical user-visible behaviour |
| [public-api.md](public-api.md) | Setting, warning strings, compatibility |
| [architecture.md](architecture.md) | Classic generator steps, infill clip, data flow |
| [testing-strategy.md](testing-strategy.md) | Mesh, slice planes, how each flow is proven |
| [batches.md](batches.md) | One batch, TDD-sized tasks |

## Decisions

| Topic | Choice | Why |
|-------|--------|-----|
| Guarantee | Trim a later line only where it enters the outer bead past the normal spacing overlap | A normal inner wall uses that overlap. Removing it opens a gap in the wall |
| Line that does not fit | Leave Classic wall lines alone. Clip gap fill and infill to the outside of the outer-bead core | The bulge comes from a thick gap or infill line, not from the ordinary wall spacing |
| When it runs | New checkbox, default off. Classic generator and wall order outer/inner only | Inner/outer slices stay identical. Arachne and other wall orders are unchanged |
| How | Keep Classic wall lines. Clip gap regions and fill surfaces to the outside of the outer-bead core | Rebuilding the inner loops deletes the middle line of a normal wall |
| Protected bead | The outermost loop only | That is the bead already printed when the next line lands |
| Ignored cases | Slice as if the checkbox were off, and warn | One warning slot already exists on `Print::validate` |

## Public surface

Additive `PrintRegionConfig` bool `reserve_outer_wall`, default false, Quality → Advanced, next to Order of walls. Ignored cases warn and do not change toolpaths. Contract: [public-api.md](public-api.md).

## Batches (summary)

1. **Setting, Classic interior, infill clip** — checkbox and warnings, then a reserved outer-bead core, then gap fill and infill clipped to the outside of that core. Classic wall lines stay put.

Task-level Acceptance, Verify, and files: [batches.md](batches.md).
