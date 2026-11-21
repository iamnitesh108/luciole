#include "fs/file_error.h"
#include "fs/gio_ptr.h"
#include "fs/link_target.h"
#include "test_check.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;

  luciole::Location at(const fs::path& path) { return luciole::Location::fromPath(path.string()); }

  // The kind of FileError linkTarget() throws for `link`, or nullopt when it returns.
  std::optional<luciole::FileErrorKind> failure(const fs::path& link) {
    try {
      (void)luciole::linkTarget(at(link));
      return std::nullopt;
    } catch (const luciole::FileErrorException& error) {
      return error.error().kind;
    }
  }

  void run(const fs::path& root) {
    const auto docs = root / "docs";
    const auto links = root / "links";
    fs::create_directories(docs / "nested");
    fs::create_directories(links);
    std::ofstream(docs / "report.txt") << "text";

    // A relative target resolves against the link's own folder, including "..".
    fs::create_symlink("../docs/report.txt", links / "relative");
    TEST_CHECK(luciole::linkTarget(at(links / "relative")) == at(docs / "report.txt"));
    // An absolute target is used as it is.
    fs::create_symlink(docs / "nested", links / "absolute");
    TEST_CHECK(luciole::linkTarget(at(links / "absolute")) == at(docs / "nested"));
    // One level only: a link to a link gives the second link.
    fs::create_symlink("relative", links / "chain");
    TEST_CHECK(luciole::linkTarget(at(links / "chain")) == at(links / "relative"));
    // A target that is itself a broken link still exists as an item.
    fs::create_symlink("missing", links / "broken");
    fs::create_symlink("broken", links / "to-broken");
    TEST_CHECK(luciole::linkTarget(at(links / "to-broken")) == at(links / "broken"));
    // A broken link and an item that isn't a link are refused.
    TEST_CHECK(failure(links / "broken") == luciole::FileErrorKind::Missing);
    TEST_CHECK(failure(docs / "report.txt") == luciole::FileErrorKind::InvalidArgument);
  }
} // namespace

int main() {
  try {
    const luciole::GString name(g_dir_make_tmp("luciole-link-target-XXXXXX", nullptr));
    TEST_CHECK(name != nullptr);
    const fs::path root = name.get();
    run(root);
    std::error_code ignored;
    fs::remove_all(root, ignored);
    std::println("link target checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "link target test: {}", error.what());
    return 1;
  }
}
