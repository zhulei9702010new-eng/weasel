#pragma once

#include <limits>

// Geometry only. The caller supplies rectangles in one consistent coordinate
// space; this class never reads text or moves a window.
namespace weasel {
namespace candidate_motion {

struct OwnerGeometry {
  HWND root = nullptr;
  POINT origin = {};
  LONG width = 0;
  LONG height = 0;
  UINT dpi = 0;
};

enum class Projection { kUnavailable, kTranslated, kNeedsLayout };

class OwnerAnchor {
 public:
  void Reset() { valid_ = false; }

  // A fresh TSF rectangle is authoritative. Repeated old screen coordinates
  // during a pure owner translation are the one bounded fallback case.
  bool Capture(const RECT& source,
               const OwnerGeometry& owner,
               RECT& effective,
               bool& repeatedOldSource) {
    repeatedOldSource = false;
    if (!ValidOwner(owner) || !ValidRect(source)) {
      Reset();
      return false;
    }
    if (valid_ && SameShape(base_, owner) && SameRect(source, source_) &&
        (owner.origin.x != base_.origin.x ||
         owner.origin.y != base_.origin.y)) {
      if (Project(owner, effective) == Projection::kTranslated) {
        repeatedOldSource = true;
        return true;
      }
      Reset();
      return false;
    }
    source_ = source;
    base_ = owner;
    valid_ = true;
    effective = source;
    return true;
  }

  Projection Project(const OwnerGeometry& owner, RECT& result) const {
    if (!valid_ || !ValidOwner(owner))
      return Projection::kUnavailable;
    if (!SameShape(base_, owner))
      return Projection::kNeedsLayout;
    const long long dx =
        static_cast<long long>(owner.origin.x) - base_.origin.x;
    const long long dy =
        static_cast<long long>(owner.origin.y) - base_.origin.y;
    const long long left = static_cast<long long>(source_.left) + dx;
    const long long top = static_cast<long long>(source_.top) + dy;
    const long long right = static_cast<long long>(source_.right) + dx;
    const long long bottom = static_cast<long long>(source_.bottom) + dy;
    if (!Fits(left) || !Fits(top) || !Fits(right) || !Fits(bottom))
      return Projection::kUnavailable;
    result = {static_cast<LONG>(left), static_cast<LONG>(top),
              static_cast<LONG>(right), static_cast<LONG>(bottom)};
    return Projection::kTranslated;
  }

  static bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
  }

 private:
  static bool ValidOwner(const OwnerGeometry& g) {
    return g.root && g.width > 0 && g.height > 0 && g.dpi > 0;
  }
  static bool ValidRect(const RECT& r) {
    // A caret can be a point or have zero width. Reject only an inverted or
    // all-zero unavailable rectangle; never invent a minimum caret size.
    return r.right >= r.left && r.bottom >= r.top &&
           (r.left != 0 || r.top != 0 || r.right != 0 || r.bottom != 0);
  }
  static bool SameShape(const OwnerGeometry& a, const OwnerGeometry& b) {
    return a.root == b.root && a.width == b.width && a.height == b.height &&
           a.dpi == b.dpi;
  }
  static bool Fits(long long v) {
    return v >= (std::numeric_limits<LONG>::min)() &&
           v <= (std::numeric_limits<LONG>::max)();
  }
  RECT source_ = {};
  OwnerGeometry base_;
  bool valid_ = false;
};

}  // namespace candidate_motion
}  // namespace weasel
