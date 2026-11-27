#include "operations/transfer_pacing.h"

#include "fs/filename.h"
#include "operations/transfer_copy.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <format>
#include <glib.h>
#include <limits>
#include <mutex>
#include <optional>
#include <print>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>

namespace luciole::transfer {
  namespace {
    constexpr std::int64_t kTargetWindowNs = 200'000'000;
    constexpr auto kCancelPollInterval = std::chrono::milliseconds(50);

    struct MountTag {
      dev_t device = 0;
      std::optional<std::uint64_t> mountId;
      bool valid = false;
    };

    // Asks only for the mount id: requesting mtime makes NFS flush dirty pages, stalling exactly when cancelling.
    MountTag mountTag(int directory, const char* path, int flags) noexcept {
      struct statx stx{};
      if (::statx(directory, path, flags | AT_STATX_DONT_SYNC, STATX_MNT_ID, &stx) != 0) {
        return {};
      }
      MountTag tag{.device = makedev(stx.stx_dev_major, stx.stx_dev_minor), .mountId = std::nullopt, .valid = true};
      if (stx.stx_mask & STATX_MNT_ID) {
        tag.mountId = stx.stx_mnt_id;
      }
      return tag;
    }

    bool sameMountTag(const MountTag& a, const MountTag& b) noexcept {
      if (!a.valid || !b.valid) {
        return true;
      }
      if (a.mountId && b.mountId) {
        return *a.mountId == *b.mountId;
      }
      return a.device == b.device;
    }

    struct ActiveCleanup {
      std::uint64_t id = 0;
      MountTag mount;
    };

    struct CleanupRegistry {
      std::atomic<unsigned> pending{0};
      std::mutex mutex;
      std::uint64_t nextId = 0;
      std::vector<ActiveCleanup> active;
      std::vector<FileError> errors;
    };

    CleanupRegistry& cleanupRegistry() {
      // Detached cleanup helpers may finish during or after process shutdown.
      static auto* value = new CleanupRegistry;
      return *value;
    }

    std::uint64_t registerCleanup(MountTag mount) noexcept {
      auto& registry = cleanupRegistry();
      const std::scoped_lock lock(registry.mutex);
      const auto id = ++registry.nextId;
      registry.active.push_back({.id = id, .mount = mount});
      registry.pending.fetch_add(1, std::memory_order_release);
      return id;
    }

    void unregisterCleanup(std::uint64_t id) noexcept {
      auto& registry = cleanupRegistry();
      {
        const std::scoped_lock lock(registry.mutex);
        std::erase_if(registry.active, [id](const ActiveCleanup& item) { return item.id == id; });
        registry.pending.fetch_sub(1, std::memory_order_release);
      }
      g_main_context_wakeup(nullptr);
    }

    void recordCleanupError(FileError error) {
      std::println(stderr, "[transfer-cleanup] {}", error.message);
      auto& registry = cleanupRegistry();
      {
        const std::scoped_lock lock(registry.mutex);
        if (registry.errors.size() == 12) {
          registry.errors.erase(registry.errors.begin());
        }
        registry.errors.push_back(std::move(error));
      }
      g_main_context_wakeup(nullptr);
    }

    std::int64_t nowNs(const std::function<std::int64_t()>& clockHook) {
      if (clockHook) {
        return clockHook();
      }
      return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
    }

    int closeFdWithHook(Fd& fd, const std::function<int(Fd&)>& hook) {
      if (!hook) {
        return fd.close();
      }
      const int result = hook(fd);
      const int saved = errno;
      if (fd.valid()) {
        (void)fd.close();
      }
      errno = saved;
      return result;
    }

    int runSyncRange(
        int fd, std::uint64_t offset, std::uint64_t length,
        const std::function<int(int, std::uint64_t, std::uint64_t)>& hook
    ) {
      constexpr unsigned kFlags = SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER;
      for (;;) {
        const int result = hook ? hook(fd, offset, length)
                                : ::sync_file_range(fd, static_cast<off_t>(offset), static_cast<off_t>(length), kFlags);
        if (result < 0 && errno == EINTR) {
          continue;
        }
        return result;
      }
    }

    int runDataSync(int fd, const std::function<int(int)>& hook) {
      for (;;) {
        const int result = hook ? hook(fd) : ::fdatasync(fd);
        if (result < 0 && errno == EINTR) {
          continue;
        }
        return result;
      }
    }

    bool rangeSyncUnsupportedError(int code) {
      return code == ENOSYS || code == EOPNOTSUPP || code == ENOTSUP || code == EINVAL;
    }

    std::size_t adaptWindow(std::size_t current, std::uint64_t syncedBytes, std::int64_t elapsedNs, std::size_t cap) {
      const std::size_t maxStep = std::min(cap, current > cap / 2 ? cap : current * 2);
      const std::size_t floor = std::min(kMinPaceWindow, cap);
      if (elapsedNs <= 0 || syncedBytes == 0) {
        return std::clamp(maxStep, floor, cap);
      }
      const double scaled =
          static_cast<double>(syncedBytes) * (static_cast<double>(kTargetWindowNs) / static_cast<double>(elapsedNs));
      const std::size_t desired = scaled >= static_cast<double>(maxStep)
          ? maxStep
          : static_cast<std::size_t>(std::max(scaled, static_cast<double>(floor)));
      const std::size_t step = std::min<std::size_t>(64 * 1024, floor);
      const std::size_t aligned = step > 1 ? ((desired + step / 2) / step) * step : desired;
      return std::clamp(aligned, floor, maxStep);
    }

    // Removes entries inside a verified private stage without pinning unlinked files (avoiding NFS `.nfsXXXX`)
    // and without crossing into mounted subtrees or failing on FAT32/exFAT/SMB3 directory permissions.
    void removeDetachedEntry(int parent, const std::string& name, dev_t stageDevice, unsigned depth) {
      if (depth > 256) {
        throwFileError(FileErrorKind::Unsupported, "The private transfer folder is too deep.");
      }
      const auto entry = info(parent, name);
      if (entry.st_dev != stageDevice) {
        throwFileError(FileErrorKind::Changed, "The private transfer folder crossed a filesystem boundary.");
      }
      const bool folder = S_ISDIR(entry.st_mode);
      if (folder) {
        auto dir = directory(parent, name);
        const auto openedStat = statFd(dir.get());
        if (!sameIdentity(entry, openedStat) || openedStat.st_dev != stageDevice) {
          throwFileError(FileErrorKind::Changed, "A private transfer subfolder changed during cleanup.");
        }
        if ((openedStat.st_mode & 0700) != 0700 && ::fchmod(dir.get(), 0700) != 0) {
          if (errno != EPERM && errno != EOPNOTSUPP && errno != ENOTSUP) {
            fail("Could not clean temporary folder");
          }
        }
        for (const auto& child : children(dir.get())) {
          removeDetachedEntry(dir.get(), child, stageDevice, depth + 1);
        }
        dir = Fd{};
        if (!sameIdentity(entry, info(parent, name))) {
          throwFileError(FileErrorKind::Changed, "A private transfer subfolder changed during cleanup.");
        }
      }
      if (::unlinkat(parent, name.c_str(), folder ? AT_REMOVEDIR : 0) != 0) {
        fail("Could not clean temporary item");
      }
    }

    void removeDetachedStage(DetachedStage& stage) {
      if (!stage.parent.valid() || !stage.directory.valid()) {
        return;
      }
      struct stat current{};
      if (::fstatat(stage.parent.get(), stage.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
          || !sameIdentity(current, stage.identity)
          || !sameIdentity(current, statFd(stage.directory.get()))) {
        throwFileError(FileErrorKind::Changed, "The private transfer folder changed.");
      }
      for (const auto& entry : children(stage.directory.get())) {
        removeDetachedEntry(stage.directory.get(), entry, stage.identity.st_dev, 0);
      }
      // Close the stage directory descriptor before rmdir so SMB/FUSE backends do not report busy.
      stage.directory = Fd{};
      if (::fstatat(stage.parent.get(), stage.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
          || !sameIdentity(current, stage.identity)) {
        throwFileError(FileErrorKind::Changed, "The private transfer folder changed.");
      }
      if (::unlinkat(stage.parent.get(), stage.name.c_str(), AT_REMOVEDIR) != 0) {
        fail("Could not remove staging folder");
      }
    }

    void cleanupAbandoned(
        std::uint64_t cleanupId, std::optional<FileError> closeError, std::optional<DetachedStage> stage
    ) noexcept {
      const std::string stagePath = stage && !stage->path.empty() ? stage->path
          : stage && !stage->name.empty()                         ? stage->name
                                                                  : std::string("temporary transfer folder");
      std::optional<FileError> removeError;
      if (stage) {
        try {
          removeDetachedStage(*stage);
        } catch (...) {
          removeError = currentFileError();
        }
      }
      // Release every owned directory descriptor before marking the mount clear for removal.
      stage.reset();
      if (closeError || removeError) {
        std::string detail;
        if (closeError && removeError) {
          detail = std::format("Close failed ({}); cleanup failed ({})", closeError->message, removeError->message);
        } else if (closeError) {
          detail = "Close failed: " + closeError->message;
        } else {
          detail = removeError->message;
        }
        recordCleanupError({
            .kind = removeError ? removeError->kind : closeError->kind,
            .message = "Cancelled transfer staging cleanup failed at " + filenameDisplayText(stagePath) + ": " + detail,
        });
      }
      unregisterCleanup(cleanupId);
    }
  } // namespace

  struct PacedWriter::State final : AbandonedCleanup {
    int fd = -1;
    std::function<int(int, std::uint64_t, std::uint64_t)> syncRange;
    std::function<int(int)> dataSync;
    std::function<int(Fd&)> closeFd;
    std::function<std::int64_t()> clockNs;

    std::mutex mutex;
    std::condition_variable cv;
    bool busy = false;
    bool stop = false;
    bool abandoned = false;
    bool stageReady = false;
    bool useDataSync = false;
    std::uint64_t cleanupId = 0;
    std::uint64_t jobOffset = 0;
    std::uint64_t jobLength = 0;

    std::atomic<bool> completed{false};
    std::uint64_t synced = 0;
    std::uint64_t lastLength = 0;
    std::int64_t lastElapsedNs = 0;
    bool fallbackToDataSync = false;
    std::optional<FileError> error;

    Fd abandonedOutput;
    std::optional<DetachedStage> abandonedStage;

    void attachStage(DetachedStage stage) noexcept override {
      {
        const std::scoped_lock lock(mutex);
        abandonedStage = std::move(stage);
        stageReady = true;
      }
      cv.notify_one();
    }

    static void finishAbandoned(const std::shared_ptr<State>& state, std::uint64_t cleanupId, Fd ownedOutput) noexcept {
      // Close the file descriptor first so NFS does not turn unlink into `.nfsXXXX`, then await the stage.
      std::optional<FileError> closeError;
      try {
        if (ownedOutput.valid() && closeFdWithHook(ownedOutput, state->closeFd) != 0) {
          closeError = errnoError(errno);
        }
      } catch (...) {
        closeError = currentFileError();
      }
      ownedOutput = Fd{};
      std::optional<DetachedStage> ownedStage;
      {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->stageReady; });
        ownedStage = std::move(state->abandonedStage);
      }
      cleanupAbandoned(cleanupId, std::move(closeError), std::move(ownedStage));
    }

    static void run(std::shared_ptr<State> state) noexcept {
      for (;;) {
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        bool useDataSync = false;
        {
          std::unique_lock lock(state->mutex);
          state->cv.wait(lock, [&] { return state->busy || state->stop; });
          if (state->abandoned) {
            state->busy = false;
            const auto cleanupId = state->cleanupId;
            auto ownedOutput = std::move(state->abandonedOutput);
            lock.unlock();
            state->cv.notify_one();
            finishAbandoned(state, cleanupId, std::move(ownedOutput));
            break;
          }
          if (!state->busy) {
            break;
          }
          offset = state->jobOffset;
          length = state->jobLength;
          useDataSync = state->useDataSync;
        }

        const auto started = nowNs(state->clockNs);
        std::optional<FileError> failure;
        bool fellBack = useDataSync;
        if (!useDataSync) {
          constexpr auto kMaxOff = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
          if (offset > kMaxOff || length > kMaxOff - offset) {
            failure = FileError{.kind = FileErrorKind::Unsupported, .message = "File offset exceeds range limit."};
          } else if (runSyncRange(state->fd, offset, length, state->syncRange) != 0) {
            const int code = errno;
            if (rangeSyncUnsupportedError(code)) {
              fellBack = true;
            } else {
              auto err = errnoError(code);
              err.message = "Could not write data to destination: " + err.message;
              failure = std::move(err);
            }
          }
        }
        if (!failure && fellBack) {
          if (runDataSync(state->fd, state->dataSync) != 0) {
            auto err = errnoError(errno);
            err.message = "Could not synchronize copied file: " + err.message;
            failure = std::move(err);
          }
        }
        const auto elapsed = nowNs(state->clockNs) - started;

        bool wasAbandoned = false;
        std::uint64_t cleanupId = 0;
        Fd ownedOutput;
        {
          std::unique_lock lock(state->mutex);
          state->busy = false;
          if (state->abandoned) {
            wasAbandoned = true;
            cleanupId = state->cleanupId;
            ownedOutput = std::move(state->abandonedOutput);
          } else {
            state->lastLength = length;
            state->lastElapsedNs = elapsed;
            state->fallbackToDataSync = fellBack;
            if (failure) {
              state->error = std::move(failure);
            } else {
              state->synced = offset + length;
            }
            state->completed.store(true, std::memory_order_release);
          }
        }
        state->cv.notify_one();

        if (wasAbandoned) {
          finishAbandoned(state, cleanupId, std::move(ownedOutput));
          break;
        }
      }
    }
  };

  int closeOutput(Fd& output, const WritebackOps* ops) {
    return closeFdWithHook(output, ops ? ops->closeFd : std::function<int(Fd&)>{});
  }

  PacedWriter::PacedWriter(Fd& output, CopyWork& work)
      : m_output(output), m_work(work), m_maxWindow(work.paceLimit),
        m_window(std::min(kInitialPaceWindow, work.paceLimit)),
        m_fallbackToDataSync(work.rangeSyncUnsupported && *work.rangeSyncUnsupported) {}

  PacedWriter::~PacedWriter() noexcept {
    if (!m_thread.joinable()) {
      return;
    }
    std::unique_lock lock(m_state->mutex);
    while (m_state->busy) {
      if (m_work.token.cancelled()) {
        abandonLocked();
        lock.unlock();
        m_state->cv.notify_one();
        m_thread.detach();
        return;
      }
      m_state->cv.wait_for(lock, kCancelPollInterval, [&] { return !m_state->busy; });
    }
    m_state->stop = true;
    lock.unlock();
    m_state->cv.notify_one();
    m_thread.join();
  }

  std::size_t PacedWriter::remaining() const {
    const std::uint64_t pending = m_written - m_submitted;
    return pending < m_window ? static_cast<std::size_t>(m_window - pending) : 0;
  }

  void PacedWriter::wrote(std::uint64_t count) {
    m_written += count;
    harvestIfDone();
    advanceReported();
    if (m_written - m_submitted >= m_window) {
      submitPending();
    }
  }

  void PacedWriter::finish() {
    if (!m_thread.joinable()) {
      m_synced = m_written;
      advanceReported();
      return;
    }
    waitForHelper();
    if (m_written > m_submitted) {
      submitPending();
      waitForHelper();
    }
    m_synced = m_written;
    advanceReported();
    stopAndJoin();
  }

  void PacedWriter::launchHelper() {
    if (m_thread.joinable()) {
      return;
    }
    if (m_work.prepareAsyncStage) {
      m_work.prepareAsyncStage();
    }
    m_state = std::make_shared<State>();
    m_state->fd = m_output.get();
    if (m_work.writebackOps) {
      m_state->syncRange = m_work.writebackOps->syncRange;
      m_state->dataSync = m_work.writebackOps->dataSync;
      m_state->closeFd = m_work.writebackOps->closeFd;
      m_state->clockNs = m_work.writebackOps->clockNs;
    }
    m_thread = std::thread(&State::run, m_state);
  }

  void PacedWriter::submitPending() {
    if (m_thread.joinable()) {
      waitForHelper();
    }
    m_work.check();
    if (m_written <= m_submitted) {
      return;
    }
    launchHelper();
    const std::uint64_t offset = m_submitted;
    const std::uint64_t length = m_written - m_submitted;
    {
      const std::scoped_lock lock(m_state->mutex);
      m_state->jobOffset = offset;
      m_state->jobLength = length;
      m_state->useDataSync = m_fallbackToDataSync;
      m_state->completed.store(false, std::memory_order_relaxed);
      m_state->busy = true;
    }
    m_submitted = m_written;
    m_state->cv.notify_one();
    if (m_fallbackToDataSync) {
      waitForHelper();
    }
  }

  void PacedWriter::harvestIfDone() {
    if (!m_state || !m_state->completed.load(std::memory_order_acquire)) {
      return;
    }
    std::optional<FileError> failure;
    std::uint64_t synced = 0;
    std::uint64_t length = 0;
    std::int64_t elapsed = 0;
    bool usedDataSync = false;
    {
      const std::scoped_lock lock(m_state->mutex);
      if (!m_state->completed.load(std::memory_order_relaxed)) {
        return;
      }
      m_state->completed.store(false, std::memory_order_relaxed);
      failure = std::move(m_state->error);
      synced = m_state->synced;
      length = m_state->lastLength;
      elapsed = m_state->lastElapsedNs;
      usedDataSync = m_state->fallbackToDataSync;
    }
    if (failure) {
      stopAndJoin();
      throw FileErrorException(std::move(*failure));
    }
    applyCompletion(synced, length, elapsed, usedDataSync);
  }

  void PacedWriter::waitForHelper() {
    if (!m_thread.joinable()) {
      return;
    }
    std::unique_lock lock(m_state->mutex);
    while (m_state->busy) {
      if (m_work.token.cancelled()) {
        abandonLocked();
        lock.unlock();
        m_state->cv.notify_one();
        m_thread.detach();
        throw TransferCancelled();
      }
      m_state->cv.wait_for(lock, kCancelPollInterval, [&] { return !m_state->busy; });
    }
    if (m_work.token.cancelled()) {
      m_state->stop = true;
      lock.unlock();
      m_state->cv.notify_one();
      m_thread.join();
      throw TransferCancelled();
    }
    if (!m_state->completed.load(std::memory_order_relaxed)) {
      return;
    }
    m_state->completed.store(false, std::memory_order_relaxed);
    auto failure = std::move(m_state->error);
    const auto synced = m_state->synced;
    const auto length = m_state->lastLength;
    const auto elapsed = m_state->lastElapsedNs;
    const bool usedDataSync = m_state->fallbackToDataSync;
    lock.unlock();
    if (failure) {
      stopAndJoin();
      throw FileErrorException(std::move(*failure));
    }
    applyCompletion(synced, length, elapsed, usedDataSync);
  }

  void PacedWriter::applyCompletion(
      std::uint64_t synced, std::uint64_t rangeLength, std::int64_t elapsedNs, bool usedDataSync
  ) {
    m_synced = std::max(m_synced, synced);
    if (usedDataSync) {
      m_fallbackToDataSync = true;
      if (m_work.rangeSyncUnsupported) {
        *m_work.rangeSyncUnsupported = true;
      }
    }
    m_window = adaptWindow(m_window, rangeLength, elapsedNs, m_maxWindow);
    advanceReported();
  }

  void PacedWriter::advanceReported() {
    const std::uint64_t allowed =
        m_fallbackToDataSync ? std::min(m_written, m_synced) : std::min(m_written, m_synced + m_window);
    if (allowed > m_reported) {
      m_work.bytes.fetch_add(allowed - m_reported);
      m_reported = allowed;
    }
  }

  void PacedWriter::stopAndJoin() noexcept {
    if (!m_thread.joinable()) {
      return;
    }
    {
      const std::scoped_lock lock(m_state->mutex);
      m_state->stop = true;
    }
    m_state->cv.notify_one();
    m_thread.join();
  }

  void PacedWriter::abandonLocked() noexcept {
    const auto tag = mountTag(m_output.get(), "", AT_EMPTY_PATH);
    m_state->cleanupId = registerCleanup(tag);
    m_state->abandonedOutput = std::move(m_output);
    m_state->abandoned = true;
    m_state->stop = true;
    if (m_work.prepareAsyncStage) {
      m_work.abandonedCleanup = m_state;
    } else {
      m_state->stageReady = true;
    }
  }

  bool cleanupPending() { return cleanupRegistry().pending.load(std::memory_order_acquire) != 0; }

  bool cleanupPendingOn(const Location& root) {
    auto& registry = cleanupRegistry();
    if (registry.pending.load(std::memory_order_acquire) == 0) {
      return false;
    }
    const auto path = root.nativePath();
    if (!path) {
      return false;
    }
    const auto rootTag = mountTag(AT_FDCWD, path->c_str(), 0);
    const std::scoped_lock lock(registry.mutex);
    return std::ranges::any_of(registry.active, [&](const ActiveCleanup& item) {
      return sameMountTag(item.mount, rootTag);
    });
  }

  std::vector<FileError> takeCleanupErrors() {
    auto& registry = cleanupRegistry();
    const std::scoped_lock lock(registry.mutex);
    return std::exchange(registry.errors, {});
  }
} // namespace luciole::transfer
