#pragma once

#include "fs/file_error.h"
#include "fs/location.h"

#include <optional>
#include <utility>
#include <vector>

namespace luciole {
  // Verified transfer backends; this check never performs remote I/O.
  bool transferLocationSupported(const Location& location);

  // Includes conservative same-server SMB case aliases, without remote I/O.
  bool transferLocationsEqual(const Location& source, const Location& target);
  bool transferLocationInside(const Location& child, const Location& parent);

  // Namespace eligibility only. The worker's staged symlink operation checks actual provider support.
  std::optional<std::string> transferLinkTarget(const Location& source, const Location& destination);

  enum class TransferFlush { None, Device, Final };
  enum class TransferAction { Move, Copy, Link };
  enum class TransferOutcome { Moved, Copied, Skipped, Failed, Cancelled, Partial, Linked, Deleted };
  struct TransferResult {
    TransferResult(
        Location location, TransferOutcome value = TransferOutcome::Failed, std::optional<FileError> failure = {}
    )
        : source(std::move(location)), outcome(value), error(std::move(failure)) {}
    Location source;
    TransferOutcome outcome = TransferOutcome::Failed;
    std::optional<FileError> error;
    std::vector<TransferResult> children;
    bool retryable = false;
    std::optional<Location> destination;
  };
  bool
  transferAffectsLocation(const std::vector<TransferResult>& results, const Location& location, bool recursive = false);
} // namespace luciole
