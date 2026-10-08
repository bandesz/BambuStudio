#ifndef slic3r_BridgeOverInfill_hpp_
#define slic3r_BridgeOverInfill_hpp_

#include "Line.hpp"

namespace Slic3r {

// Order vertical scan sections from bridge-over-infill polygon reconstruction.
// Both keys must come from the same line. Comparing a.a.y() with b.b.y() is not
// a strict weak ordering: a section with a.y() < b.y() compares less than itself,
// and overlapping sections compare less in both directions. libc++ then aborts
// inside std::sort.
inline bool bridge_over_infill_section_less(const Line &lhs, const Line &rhs)
{
    if (lhs.a.y() != rhs.a.y())
        return lhs.a.y() < rhs.a.y();
    return lhs.b.y() < rhs.b.y();
}

} // namespace Slic3r

#endif // slic3r_BridgeOverInfill_hpp_
