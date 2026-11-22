#pragma once

#include <filesystem>

namespace formaview::imaging {
inline std::filesystem::path nativeImagePath(const std::filesystem::path &path) {
#ifdef _WIN32
  if (path.is_absolute() && path.native().size() < 260)
    return path;
  auto native = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
  if (native.size() >= 260 && !native.starts_with(L"\\\\?\\") &&
      !native.starts_with(L"\\\\.\\")) {
    if (native.starts_with(L"\\\\"))
      return L"\\\\?\\UNC\\" + native.substr(2);
    return L"\\\\?\\" + native;
  }
#endif
  return path;
}
} // namespace formaview::imaging
