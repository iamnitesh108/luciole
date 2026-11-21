#pragma once

#include "fs/gio_ptr.h"

#include <string>
#include <vector>

struct _GAppInfo;
using GAppInfo = _GAppInfo;

namespace luciole {
  // No shell expansion. Empty settings use TERMINAL, then automatic discovery.
  std::vector<std::string>
  terminalArguments(const std::string& configured, bool execute, const std::string& directory = {});
  GioPtr<GAppInfo>
  terminalApplication(GAppInfo* application, const std::string& configured, const std::string& workingDirectory = {});
  std::string desktopExecCommand(const std::string& command, const std::string& uri);
} // namespace luciole
