#pragma once

#include "fs/cancel_token.h"
#include "fs/posix_fd.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace luciole {
  // Worker-owned private staging. Only a pinned item may be removed, never recursively.
  class CreationStage {
  public:
    explicit CreationStage(int parent, bool discardOnClose = true);
    ~CreationStage();
    CreationStage(const CreationStage&) = delete;
    CreationStage& operator=(const CreationStage&) = delete;
    int fd() const { return m_directory.get(); }
    int parent() const { return m_parent.get(); }
    int item() const { return m_item.get(); }
    void validateItem() const { requireItem(); }
    void adopt(Fd item);
    void publish(const std::string& name, int parent = -1);
    // Exchange with a pinned visible item, possibly in a different same-filesystem folder.
    void exchange(const std::string& name, int parent, int visibleItem);
    void removeItem();
    bool clear();
    std::string recoveryPath() const;

  private:
    void requireCurrent() const;
    void requireItem() const;
    Fd m_parent;
    bool m_discardOnClose;
    std::string m_name, m_originalPath;
    Fd m_directory, m_item;
  };
  void copyCreationData(int source, int output, std::uint64_t size, const CancelToken& token);
  void writeCreationData(std::string_view data, int output, const CancelToken& token);
} // namespace luciole
