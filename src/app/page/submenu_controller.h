#pragma once

#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct KeyboardEvent;
struct wl_output;
struct wl_surface;
class AnimationManager;
class Flex;
class PageContext;
class PopupSurface;
class RenderContext;
class ScrollView;
class WaylandConnection;

// Submenus of the context menu: a child popup beside a row whose entry has `hasSubmenu`. Hovering or clicking the
// row, or Enter or Right while it is highlighted, opens it; Left or Escape closes it back to the context menu.
class SubmenuController {
public:
  SubmenuController(
      PageContext& page, WaylandConnection& connection, RenderContext& renderer, AnimationManager& animations
  );

  // Makes row `row` of the shown context menu, whose entry id is `id`, open a submenu: `fill` fills it with show(),
  // now or later; `closed` runs when that submenu closes.
  void attach(std::size_t row, int id, std::function<void()> fill, std::function<void()> closed = {});
  // Fills the submenu that is opening or open; activating an entry calls `action` with its id, then closes every
  // menu.
  void show(std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, float width);
  // Opens the submenu of the attached row `id`, closing another open one.
  void open(int id, bool keyboard);
  // Opens the submenu of the context menu's highlighted row from the keyboard; false when that row has none.
  bool openHighlighted();
  // Changes whenever a submenu opens or closes, so content that arrives later can tell it is stale.
  std::uint64_t generation() const { return m_generation; }

  // BrowserPage::closePopup(): closes the submenu and forgets the rows.
  void menuClosed();
  bool owns(const wl_surface* surface) const;
  InputDispatcher& dispatcher() { return m_dispatcher; }
  void outputChanged(wl_output* output);
  // Handles keys while a submenu is open; false when none is.
  bool key(const KeyboardEvent& event);
  // Pointer activity on the submenu keeps it open.
  void pointer() { m_closePending = false; }
  // Pointer entered or moved over the context menu.
  void menuHover();
  // The context menu scrolled.
  void menuScrolled();

private:
  struct Row {
    std::size_t index = 0;
    int id = 0;
    std::function<void()> fill;
    std::function<void()> closed;
  };

  const Row* findRow(int id) const;
  bool create(float width, float height);
  void close();
  void dismiss();

  PageContext& m_page;
  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  AnimationManager& m_animations;
  std::vector<Row> m_rows;
  // The row whose submenu is open or opening.
  std::optional<int> m_open;
  std::unique_ptr<PopupSurface> m_surface;
  std::unique_ptr<Flex> m_root;
  ContextMenuControl* m_menu = nullptr;
  ScrollView* m_scroll = nullptr;
  InputDispatcher m_dispatcher;
  std::uint64_t m_generation = 0;
  bool m_closePending = false;
  bool m_keyboard = false;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
