#pragma once

#include "fs/file_error.h"
#include "fs/location.h"
#include "fs/posix_fd.h"
#include "operations/transfer_writeback.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace luciole::transfer {
  struct CopyWork;

  constexpr std::size_t kInitialPaceWindow = 1024 * 1024;
  constexpr std::size_t kMinPaceWindow = 256 * 1024;

  // Owned private staging folder transferred to an in-flight pacing helper on cancellation.
  struct DetachedStage {
    Fd parent;
    Fd directory;
    std::string name;
    struct stat identity{};
    std::string path;
  };

  // Handle for delivering the unpublished stage to an abandoned helper once copyTree() unwinds.
  class AbandonedCleanup {
  public:
    virtual ~AbandonedCleanup() = default;
    virtual void attachStage(DetachedStage stage) noexcept = 0;
  };

  // Closes a copied file descriptor once through the optional test hook, ensuring the OS descriptor is never leaked.
  int closeOutput(Fd& output, const WritebackOps* ops);

  // Adaptive writeback pacing for one regular file on removable media or mounted network shares.
  // Launches one helper thread lazily when the first full window fills, keeps up to two windows in flight,
  // reports monotonic progress with at most a one-window lead, and hands off ownership on stalled cancellation.
  class PacedWriter {
  public:
    PacedWriter(Fd& output, CopyWork& work);
    ~PacedWriter() noexcept;
    PacedWriter(const PacedWriter&) = delete;
    PacedWriter& operator=(const PacedWriter&) = delete;

    // Remaining bytes before the current window boundary. Always positive while the loop is active.
    std::size_t remaining() const;
    // Records bytes written to output, updates monotonic progress, and submits full windows.
    void wrote(std::uint64_t count);
    // Drains outstanding writeback and any trailing partial window, then joins the helper.
    void finish();

  private:
    struct State;
    void launchHelper();
    void submitPending();
    void harvestIfDone();
    void waitForHelper();
    void applyCompletion(std::uint64_t synced, std::uint64_t rangeLength, std::int64_t elapsedNs, bool usedDataSync);
    void advanceReported();
    void stopAndJoin() noexcept;
    void abandonLocked() noexcept;

    Fd& m_output;
    CopyWork& m_work;
    std::size_t m_maxWindow;
    std::size_t m_window;
    std::uint64_t m_written = 0;
    std::uint64_t m_submitted = 0;
    std::uint64_t m_synced = 0;
    std::uint64_t m_reported = 0;
    bool m_fallbackToDataSync = false;
    std::shared_ptr<State> m_state;
    std::thread m_thread;
  };

  // True while any cancelled paced transfer still has a detached helper closing or removing its staging folder.
  bool cleanupPending();
  // True while a detached helper holds descriptors on root's mount.
  bool cleanupPendingOn(const Location& root);
  // Drains close or staging-removal errors recorded by detached helpers.
  std::vector<FileError> takeCleanupErrors();
} // namespace luciole::transfer
