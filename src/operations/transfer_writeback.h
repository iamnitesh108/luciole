#pragma once

#include "fs/posix_fd.h"
#include "fs/transfer_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace luciole::transfer {
  constexpr std::size_t kDefaultPaceLimit = 16 * 1024 * 1024;
  constexpr std::size_t kSmbPaceLimit = 128 * 1024 * 1024;

  bool removableBlockDevice(const std::filesystem::path& device);
  struct MountInfo {
    dev_t filesystem;
    std::string type;
    std::string source;
  };
  // Resolves mountId from a mountinfo snapshot, regardless of filesystem type.
  std::optional<MountInfo> mountInfo(std::string_view mountinfo, std::uint64_t mountId);
  // Returns the adaptive writeback pacing cap for a mounted filesystem type, or 0 for unpaced local disks.
  std::size_t mountPaceLimit(std::string_view type);

  // Worker-side operations. Tests can hold a flush, pace or stall range syncs, return delayed I/O errors,
  // or supply mountinfo snapshots.
  struct WritebackOps {
    std::function<bool(int, dev_t)> removable = nullptr;
    std::function<std::size_t(int, dev_t)> paceLimit = nullptr;
    std::function<void(int)> sync = nullptr;
    std::function<int(int, std::uint64_t, std::uint64_t)> syncRange = nullptr;
    std::function<int(int)> dataSync = nullptr;
    std::function<int(Fd&)> closeFd = nullptr;
    std::function<std::int64_t()> clockNs = nullptr;
    std::function<ssize_t(int, int, std::size_t)> copyRange = nullptr;
    std::function<ssize_t(int, const void*, std::size_t)> writeChunk = nullptr;
    std::function<std::string()> mountinfo = nullptr;
    bool allowClone = true;
  };

  class Writeback {
  public:
    struct Volume {
      Fd directory;
      bool removable = false;
      std::size_t paceLimit = 0;
      bool rangeSyncUnsupported = false;
      std::vector<TransferResult*> published;
      std::optional<FileError> failure;
    };
    WritebackOps ops;
    std::function<void(TransferFlush)> phase;
    // Keep the earliest open file description so syncfs can report errors incurred during staging.
    Volume& observe(int directory, dev_t device, bool requireSync = false);
    void pin(Volume& volume, int directory);
    void published(Volume& volume, TransferResult& result, bool copiedMove = false);
    // Re-registers a result when post-flush cleanup mutates a healthy removable volume.
    void markDirty(Volume& volume, TransferResult& result);
    void markDirty(dev_t device, TransferResult& result);
    void flush(Volume& volume, bool final = false);
    void flushPublished(dev_t device);
    void flushAllPublished();
    // Flush all outstanding publications even after cancellation or an earlier filesystem failure.
    bool finish();

  private:
    std::map<dev_t, Volume> m_volumes;
    // Btrfs subvolumes can present different st_dev values for one superblock.
    std::map<dev_t, dev_t> m_aliases;
    std::optional<std::string> m_mountinfo;
    bool m_failed = false;
  };
} // namespace luciole::transfer
