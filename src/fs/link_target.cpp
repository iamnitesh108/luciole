#include "fs/link_target.h"

#include "fs/file_error.h"
#include "fs/gio_interop.h"
#include "fs/gio_ptr.h"

luciole::Location luciole::linkTarget(const Location& link) {
  GError* raw = nullptr;
  const GioPtr<GFileInfo> info(g_file_query_info(
      gioFile(link).get(), G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &raw
  ));
  const GErrorPtr error(raw);
  if (!info) {
    throw FileErrorException(error ? fileError(*error) : FileError{.kind = FileErrorKind::Other, .message = {}});
  }
  // The raw bytes, not the display form: a target name need not be valid UTF-8.
  const char* target = g_file_info_get_attribute_byte_string(info.get(), G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET);
  if (g_file_info_get_is_symlink(info.get()) == FALSE || target == nullptr || target[0] == '\0') {
    throwFileError(FileErrorKind::InvalidArgument, "This item is not a link.");
  }
  const auto folder = link.parent();
  if (!folder) {
    throwFileError(FileErrorKind::InvalidArgument, "This item is not a link.");
  }
  auto resolved = folder->resolve(target);
  // Only the target itself must exist; it may be another link, even a broken one.
  const GioPtr<GFileInfo> found(g_file_query_info(
      gioFile(resolved).get(), G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
  ));
  if (!found) {
    throwFileError(FileErrorKind::Missing, "The link's target no longer exists.");
  }
  return resolved;
}
