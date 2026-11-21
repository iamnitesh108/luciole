#include "platform/terminal_command.h"

#include "fs/gio_interop.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <gio/gdesktopappinfo.h>
#include <utility>

namespace {
  using namespace luciole;
  std::vector<std::string> parse(const std::string& command) {
    if (command.size() > 4096 || command.find_first_of("\r\n") != std::string::npos || command.contains('\0')) {
      throwFileError(FileErrorKind::InvalidArgument, "The terminal command must be one line of at most 4096 bytes.");
    }
    int count = 0;
    char** values = nullptr;
    GError* raw = nullptr;
    if (!g_shell_parse_argv(command.c_str(), &count, &values, &raw)) {
      const GErrorPtr error(raw);
      throwFileError(FileErrorKind::InvalidArgument, "Invalid terminal command: " + std::string(error->message));
    }
    const std::unique_ptr<char*, decltype(&g_strfreev)> args(values, g_strfreev);
    std::vector<std::string> result(values, values + count);
    if (result.front().starts_with("~/")) {
      result.front().replace(0, 1, g_get_home_dir());
    }
    const luciole::GString executable(g_find_program_in_path(result.front().c_str()));
    if (!executable) {
      throwFileError(
          FileErrorKind::Unavailable,
          "Terminal '" + result.front() + "' was not found. Check Settings > General > Terminal."
      );
    }
    result.front() = executable.get();
    return result;
  }
  std::vector<std::string> select(const std::string& configured) {
    if (!configured.empty()) {
      return parse(configured);
    }
    if (const char* value = g_getenv("TERMINAL"); value && *value) {
      return parse(value);
    }
    constexpr auto candidates = std::to_array<const char*>(
        {"xdg-terminal-exec", "x-terminal-emulator", "foot", "kitty", "alacritty", "ghostty", "wezterm", "konsole",
         "gnome-terminal", "kgx", "ptyxis", "xfce4-terminal", "xterm"}
    );
    for (const auto* candidate : candidates) {
      if (const luciole::GString found(g_find_program_in_path(candidate)); found) {
        return {found.get()};
      }
    }
    throwFileError(
        FileErrorKind::Unavailable, "No terminal was found. Choose a terminal in Settings > General > Terminal."
    );
  }
  std::string execArgument(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
      if (c == '"' || c == '\\' || c == '$' || c == '`') {
        result += '\\';
      }
      result += c;
      if (c == '%') {
        result += '%';
      }
    }
    return result + '"';
  }
  // WezTerm's config options belong before its subcommand. An explicit remote
  // or management subcommand must not be silently turned into a local start.
  bool weztermStart(std::vector<std::string>& args) {
    std::size_t index = 1;
    while (index < args.size()) {
      const auto& arg = args[index];
      if (arg == "--config-file" || arg == "--config") {
        index += 2;
      } else if (
          arg == "-n" || arg == "--skip-config" || arg.starts_with("--config-file=") || arg.starts_with("--config=")
      ) {
        ++index;
      } else {
        return arg == "start" || arg == "-e" || (arg == "cli" && index + 1 < args.size() && args[index + 1] == "spawn");
      }
    }
    args.emplace_back("start");
    return true;
  }
  void directoryArgument(std::vector<std::string>& args, const std::string& option, const std::string& directory) {
    // A requested folder overrides a configured default. Avoid duplicate
    // options, which some terminals reject instead of accepting the last one.
    for (std::size_t index = 1; index < args.size();) {
      if (args[index] == option) {
        args.erase(
            args.begin() + static_cast<std::ptrdiff_t>(index),
            args.begin() + static_cast<std::ptrdiff_t>(std::min(index + 2, args.size()))
        );
      } else if (args[index].starts_with(option + '=')) {
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
      } else {
        ++index;
      }
    }
    args.push_back(option + '=' + directory);
  }
} // namespace

std::vector<std::string>
luciole::terminalArguments(const std::string& configured, bool execute, const std::string& directory) {
  auto result = select(configured);
  const auto name = std::filesystem::path(result.front()).filename().string();
  std::string separator;
  if (result.back() == "--" || result.back() == "-x" || (result.back() == "-e" && name != "wezterm")) {
    separator = std::move(result.back());
    result.pop_back();
  }
  const bool wezterm = name == "wezterm" && weztermStart(result);
  if (!directory.empty()) {
    if (name == "xdg-terminal-exec") {
      directoryArgument(result, "--dir", directory);
    } else if (wezterm || name == "kitty") {
      directoryArgument(result, wezterm ? "--cwd" : "--directory", directory);
    } else if (name == "konsole") {
      directoryArgument(result, "--workdir", directory);
    } else if (
        name == "alacritty"
        || name == "foot"
        || name == "footclient"
        || name == "ghostty"
        || name == "gnome-terminal"
        || name == "kgx"
        || name == "ptyxis"
        || name == "mate-terminal"
        || name == "xfce4-terminal"
    ) {
      directoryArgument(result, "--working-directory", directory);
    }
    if (name == "ghostty" && std::ranges::find(result, "+new-window") == result.end()) {
      // A normal GTK remote activation does not forward these per-launch
      // settings to an existing Ghostty process. Keep this request independent.
      directoryArgument(result, "--gtk-single-instance", "false");
    }
  }
  if (!separator.empty()) {
    result.push_back(std::move(separator));
    return result;
  }
  if (!execute) {
    return result;
  }
  if (name == "wezterm") {
    result.emplace_back("--");
  } else if (name == "ptyxis") {
    result.emplace_back("--standalone");
    result.emplace_back("--");
  } else if (
      name == "xdg-terminal-exec"
      || name == "foot"
      || name == "footclient"
      || name == "kitty"
      || name == "gnome-terminal"
      || name == "kgx"
  ) {
    result.emplace_back("--");
  } else if (name == "mate-terminal" || name == "xfce4-terminal") {
    result.emplace_back("-x");
  } else {
    result.emplace_back("-e");
  }
  return result;
}

luciole::GioPtr<GAppInfo> luciole::terminalApplication(
    GAppInfo* application, const std::string& configured, const std::string& workingDirectory
) {
  if (!G_IS_DESKTOP_APP_INFO(application)
      || !g_desktop_app_info_get_boolean(G_DESKTOP_APP_INFO(application), "Terminal")) {
    return GioPtr<GAppInfo>(G_APP_INFO(g_object_ref(application)));
  }
  auto* desktop = G_DESKTOP_APP_INFO(application);
  const luciole::GString original(g_desktop_app_info_get_string(desktop, "Exec"));
  if (!original || !*original) {
    throwFileError(FileErrorKind::Unsupported, "The terminal application has no executable command.");
  }
  const char* filename = g_desktop_app_info_get_filename(desktop);
  const luciole::GString uri(filename ? g_filename_to_uri(filename, nullptr, nullptr) : nullptr);
  const auto command = uri ? desktopExecCommand(original.get(), uri.get()) : std::string(original.get());
  const luciole::GString directory(g_desktop_app_info_get_string(desktop, "Path"));
  const std::string cwd = directory && *directory ? directory.get() : workingDirectory;
  const auto prefix = terminalArguments(configured, true, cwd);
  std::string exec;
  for (const auto& part : prefix) {
    exec += execArgument(part) + ' ';
  }
  exec += command;
  const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> key(g_key_file_new(), g_key_file_unref);
  g_key_file_set_string(key.get(), "Desktop Entry", "Type", "Application");
  g_key_file_set_string(key.get(), "Desktop Entry", "Name", g_app_info_get_name(application));
  g_key_file_set_string(key.get(), "Desktop Entry", "Exec", exec.c_str());
  for (const auto* item : {"Icon", "Path", "StartupWMClass"}) {
    const luciole::GString text(g_desktop_app_info_get_string(desktop, item));
    if (text) {
      g_key_file_set_string(key.get(), "Desktop Entry", item, text.get());
    }
  }
  if (!cwd.empty()) {
    g_key_file_set_string(key.get(), "Desktop Entry", "Path", cwd.c_str());
  }
  g_key_file_set_boolean(
      key.get(), "Desktop Entry", "StartupNotify", g_desktop_app_info_get_boolean(desktop, "StartupNotify")
  );
  // The terminal is already explicit. GIO still expands the original Exec fields.
  g_key_file_set_boolean(key.get(), "Desktop Entry", "Terminal", false);
  g_key_file_set_boolean(key.get(), "Desktop Entry", "DBusActivatable", false);
  GioPtr<GAppInfo> result(G_APP_INFO(g_desktop_app_info_new_from_keyfile(key.get())));
  if (!result) {
    throwFileError(FileErrorKind::Unavailable, "The terminal application could not be prepared.");
  }
  return result;
}

std::string luciole::desktopExecCommand(const std::string& command, const std::string& locationUri) {
  std::string result;
  bool quoted = false;
  for (std::size_t index = 0; index < command.size(); ++index) {
    const char value = command[index];
    if (value == '\\' && index + 1 < command.size()) {
      result += value;
      result += command[++index];
    } else if (value == '"') {
      quoted = !quoted;
      result += value;
    } else if (value == '%' && index + 1 < command.size()) {
      const char code = command[++index];
      if (code != 'k') {
        result += '%';
        result += code;
        continue;
      }
      if (quoted
          || (index > 1 && command[index - 2] != ' ' && command[index - 2] != '\t')
          || (index + 1 < command.size() && command[index + 1] != ' ' && command[index + 1] != '\t')) {
        throwFileError(FileErrorKind::Unsupported, "The launcher location field must be a separate argument.");
      }
      // GDesktopAppInfo created from captured contents has no filename. Supply
      // the original URI for %k, escaping its percent bytes for GIO's later pass.
      std::string uri;
      for (const char ch : locationUri) {
        uri += ch;
        if (ch == '%') {
          uri += '%';
        }
      }
      const luciole::GString escaped(g_shell_quote(uri.c_str()));
      result += escaped.get();
    } else {
      result += value;
    }
  }
  return result;
}
