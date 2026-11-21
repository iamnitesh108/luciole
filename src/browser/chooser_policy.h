#pragma once

#include "browser/directory_model.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace luciole {
  struct ChooserMode {
    bool directory = false;
    bool multiple = false;
  };
  struct ChooserDecision {
    enum class Kind : std::uint8_t { Unavailable, Enter, Accept };
    Kind kind = Kind::Unavailable;
    std::vector<Location> locations;
    std::string_view reason; // empty when nothing needs explaining
  };
  ChooserDecision
  chooserDecision(ChooserMode mode, const std::optional<Location>& folder, std::span<const DirectoryEntry> entries);
  // Whether selecting `entry` while saving offers its name to save over; folders and links to folders are entered.
  bool offersSaveName(const DirectoryEntry& entry);
  // The selected entries of `model`, in row order.
  std::vector<DirectoryEntry> chooserSelection(const DirectoryModel& model);
  // Why `name` (raw bytes) cannot name a new item in a folder; empty when it can.
  std::string_view fileNameProblem(std::string_view name);
} // namespace luciole
