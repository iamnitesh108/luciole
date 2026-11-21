#pragma once

#include "fs/location.h"

namespace luciole {
  // Where the symbolic link at `link` points, one level deep: its target, resolved against the folder holding the
  // link when relative. The target may itself be a link. Throws a FileError when `link` is not a symbolic link
  // (InvalidArgument) or its target does not exist (Missing). Blocking: call on a worker.
  Location linkTarget(const Location& link);
} // namespace luciole
