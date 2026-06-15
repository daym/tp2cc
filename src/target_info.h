#pragma once

#include <cstdint>

namespace tp2cc {

// Properties of the machine the emitted C++ is compiled for. Pascal's
// pointer-sized integer primitives (PtrInt/PtrUInt/SizeInt/SizeUInt) follow
// this width; every other primitive has a fixed width in the primitive table.
// Standalone on purpose: includes nothing under src/.
struct TargetInfo {
  uint8_t pointer_bits;  // 32 or 64. No default: the caller must choose the
                         // target, or pointer-sized types miscompile silently.
};

// Return a TargetInfo matching the host machine's pointer width.
inline TargetInfo default_target_info() {
  return {.pointer_bits = static_cast<uint8_t>(sizeof(void*) * 8)};
}

}  // namespace tp2cc
