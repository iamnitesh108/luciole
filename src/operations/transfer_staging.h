#pragma once

#include "fs/location.h"
#include "fs/posix_fd.h"
#include "fs/transfer_types.h"
#include "operations/operation_history.h"
#include "operations/transfer_copy.h"
#include "operations/transfer_pacing.h"
#include "operations/transfer_service.h"
#include "operations/transfer_writeback.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

// Staged publication of transfer items: each item is copied into a private folder
// beside its destination, verified, and only then renamed into place. Merges recurse
// into per-child commits.
namespace luciole {
  class CopyEffect;
  class CopiedMoveEffect;
  class MoveEffect;
} // namespace luciole
namespace luciole::transfer {
  struct Source {
    std::size_t index;
    Location location;
    Fd parent;
    std::string name;
    struct stat metadata;
    std::string parentPath;
    struct stat parentMetadata;
  };

  struct Destination {
    Location location;
    struct stat metadata;
    std::shared_ptr<const Destination> ancestor;
  };

  struct Node;

  // A moved item whose copy is published but whose source waits for the next durability flush.
  struct PendingMove {
    Node* node;
    Tree tree;
    Writeback::Volume* destination;
    // The node's outcome once its source is removed.
    TransferOutcome outcome = TransferOutcome::Moved;
    std::shared_ptr<const CopiedMoveEffect> proof;
  };
  // Cross-filesystem moves sync the destination filesystem once per this many items, not once per item.
  constexpr std::size_t kMoveSyncBatch = 64;

  struct Work : CopyWork {
    // Asks how to resolve a name conflict; forcePrompt bypasses remembered policies.
    using Choose = std::function<TransferConflictChoice(
        const Source& source, const Location& destination, const std::string& target, const struct stat& existing,
        bool forcePrompt
    )>;
    Work(CancelToken cancel, std::atomic<std::uint64_t>& copied, Choose ask)
        : CopyWork(std::move(cancel), copied), choose(std::move(ask)) {}
    Choose choose;
    Writeback writeback;
    // Optional successful same-filesystem rename capture. False stops further optional capture.
    std::function<bool(const Source&, const Location&, int, int)> recordMove;
    std::function<std::shared_ptr<const MoveEffect>(const Source&, const Location&, int)> prepareMove;
    std::function<void(const std::shared_ptr<const MoveEffect>&)> recordPreparedMove;
    std::function<void(const std::shared_ptr<const MoveEffect>&)> discardPreparedMove;
    std::function<bool(const Source&, const Location&, int)> recordMerge;
    std::function<void(const Location&)> prepareMerge;
    std::function<bool(const Location&)> copiedMergeParent;
    std::function<void(const Location&, bool)> checkpointCopy;
    std::function<std::shared_ptr<const CopyEffect>(std::size_t, const Location&, int, int, bool)> prepareCopy;
    std::function<void(const std::shared_ptr<const CopyEffect>&, int)> recordCopy;
    std::function<std::shared_ptr<const CopiedMoveEffect>(const Source&, const Location&, int, int, bool)>
        prepareCopiedMove;
    bool allowMerge = true;
    OperationHistory* operationHistory = nullptr;
    std::optional<Location> exactTarget;
    std::size_t mergedItems = 0;
    // Owned by flushMoves(); nodes outlive the job's final flush.
    std::vector<PendingMove> pendingMoves;
  };

  struct Node {
    Node(Source captured, std::shared_ptr<const Destination> target, TransferResult outcome, unsigned level = 0)
        : source(std::move(captured)), destination(std::move(target)), result(std::move(outcome)), depth(level) {}
    Source source;
    std::shared_ptr<const Destination> destination;
    TransferResult result;
    std::vector<std::shared_ptr<Node>> children;
    bool merging = false;
    unsigned depth = 0;
    bool attempted = false;
    std::shared_ptr<const Destination> mergeTarget;
  };

  // Private staging folder inside a destination; removed on destruction unless retained.
  class Stage {
  public:
    Stage(int parent, CopyWork& work, std::string parentPath = {});
    ~Stage();
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    bool clear() noexcept;
    // A failed post-exchange flush leaves the displaced file in this stage.
    void flushReplacement(Writeback& writeback, Writeback::Volume& volume);
    // Pins the parent descriptor and stage identity so cancellation can hand off removal without syscalls.
    void prepareAsyncCleanup();
    DetachedStage detachForCleanup() noexcept;
    int fd() const { return m_fd.get(); }
    const std::string& name() const { return m_name; }
    void retain() { m_cleaned = true; }
    void keepContents() { m_keepContents = true; }

  private:
    int m_parent;
    CopyWork& m_work;
    std::string m_parentPath;
    std::string m_name;
    Fd m_fd;
    Fd m_ownedParent;
    struct stat m_identity{};
    std::string m_path;
    bool m_cleaned = false;
    bool m_keepContents = false;
  };

  // Scopes destination pacing and async cancelled-stage handoff around one copyTree() call.
  class PacingScope {
  public:
    PacingScope(Work& work, Stage& stage, Writeback::Volume& volume);
    ~PacingScope();
    PacingScope(const PacingScope&) = delete;
    PacingScope& operator=(const PacingScope&) = delete;

  private:
    CopyWork& m_work;
    Stage& m_stage;
    std::size_t m_previousPaceLimit;
    bool* m_previousRangeSyncUnsupported;
    const WritebackOps* m_previousOps;
    std::function<void()> m_previousPrepare;
  };

  Fd openSourceParent(const Source& source);
  // Runs node once, or on retry only its retryable parts.
  void execute(Node& node, TransferAction action, Work& work, bool retry);
  // Makes pending moved copies durable with one syncfs per destination filesystem, then removes their sources.
  // Every job flushes before it reports outcomes.
  void flushMoves(Work& work);
  std::size_t retryCount(const Node& node);
  TransferResult snapshot(Node& node);
} // namespace luciole::transfer
