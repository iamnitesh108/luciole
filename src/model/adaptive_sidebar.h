#pragma once

#include <optional>

namespace luciole {
  // The docked preference survives temporary drawer presentation and window resizing.
  class AdaptiveSidebar {
  public:
    static constexpr float compactWidth = 760.0F;
    bool compact() const { return m_compact; }
    bool visible() const { return m_compact ? m_open : m_preferred; }
    bool updateWidth(float width) {
      const bool compact = width < compactWidth;
      if (compact == m_compact) {
        return false;
      }
      m_compact = compact;
      m_open = false;
      return true;
    }
    void setPreferred(bool visible) {
      m_preferred = visible;
      if (!visible) {
        m_open = false;
      }
    }
    // Only deliberate changes to the docked preference should be persisted.
    std::optional<bool> toggle() {
      if (m_compact) {
        m_open = !m_open;
        return std::nullopt;
      }
      m_preferred = !m_preferred;
      return m_preferred;
    }
    void dismiss() { m_open = false; }

  private:
    bool m_preferred = true;
    bool m_compact = false;
    bool m_open = false;
  };
} // namespace luciole
