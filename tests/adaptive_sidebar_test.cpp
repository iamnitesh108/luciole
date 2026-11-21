#include "model/adaptive_sidebar.h"
#include "test_check.h"

#include <print>

int main() {
  luciole::AdaptiveSidebar sidebar;
  TEST_CHECK(sidebar.visible());
  TEST_CHECK(sidebar.updateWidth(620) && sidebar.compact() && !sidebar.visible());
  TEST_CHECK(!sidebar.toggle().has_value() && sidebar.visible());
  TEST_CHECK(!sidebar.updateWidth(700) && sidebar.visible());
  sidebar.dismiss();
  TEST_CHECK(!sidebar.visible());
  TEST_CHECK(sidebar.updateWidth(1000) && sidebar.visible());
  TEST_CHECK(sidebar.toggle() == false && !sidebar.visible());
  TEST_CHECK(sidebar.updateWidth(620) && !sidebar.visible());
  TEST_CHECK(!sidebar.toggle().has_value() && sidebar.visible());
  TEST_CHECK(sidebar.updateWidth(1000) && !sidebar.visible());
  TEST_CHECK(sidebar.toggle() == true && sidebar.visible());
  TEST_CHECK(sidebar.updateWidth(luciole::AdaptiveSidebar::compactWidth - 1));
  TEST_CHECK(!sidebar.toggle().has_value() && sidebar.visible());
  sidebar.setPreferred(false);
  TEST_CHECK(!sidebar.visible());
  sidebar.setPreferred(true);
  TEST_CHECK(!sidebar.visible());
  TEST_CHECK(!sidebar.toggle().has_value() && sidebar.visible());
  TEST_CHECK(sidebar.updateWidth(luciole::AdaptiveSidebar::compactWidth) && sidebar.visible());
  TEST_CHECK(sidebar.updateWidth(620) && !sidebar.visible());
  std::println("Adaptive sidebar transitions and preference retention passed");
}
