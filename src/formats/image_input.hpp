#pragma once
#include "formaview/pro_image.hpp"
#include <fstream>
#include <span>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace formaview::imaging {
// Read-only file mapping is virtual address space, not an owned full-file copy.
// Codec reads fault in only the requested compressed chunks. The caller keeps
// its read lease and validates the source stamp before publishing a result.
class ImageInput {
public:
  ImageInput() = default;
  ImageInput(const ImageInput &) = delete;
  ImageInput &operator=(const ImageInput &) = delete;
  ~ImageInput() {
#ifdef _WIN32
    if (mapped_)
      UnmapViewOfFile(mapped_);
    if (mapping_)
      CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE)
      CloseHandle(file_);
#else
    if (mapped_)
      munmap(const_cast<std::byte *>(mapped_), length_);
    if (file_ >= 0)
      close(file_);
#endif
  }
  void map(const std::filesystem::path &path) {
#ifdef _WIN32
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER n{};
    if (file_ == INVALID_HANDLE_VALUE || !GetFileSizeEx(file_, &n) ||
        n.QuadPart <= 0 || static_cast<std::uint64_t>(n.QuadPart) > SIZE_MAX)
      fail();
    length_ = static_cast<std::size_t>(n.QuadPart);
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_)
      fail();
    mapped_ = static_cast<const std::byte *>(
        MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
#else
    file_ = ::open(path.c_str(), O_RDONLY);
    struct stat s{};
    if (file_ < 0 || fstat(file_, &s) || s.st_size <= 0 ||
        static_cast<std::uint64_t>(s.st_size) > SIZE_MAX)
      fail();
    length_ = static_cast<std::size_t>(s.st_size);
    auto *p = mmap(nullptr, length_, PROT_READ, MAP_PRIVATE, file_, 0);
    if (p != MAP_FAILED)
      mapped_ = static_cast<const std::byte *>(p);
#endif
    if (!mapped_)
      fail();
  }
  void resize(std::size_t n) { owned_.resize(n); }
  const std::byte *data() const { return mapped_ ? mapped_ : owned_.data(); }
  std::byte *data() {
    return mapped_ ? const_cast<std::byte *>(mapped_) : owned_.data();
  }
  std::size_t size() const { return mapped_ ? length_ : owned_.size(); }
  std::size_t resident_bytes() const { return owned_.size(); }
  const std::byte &at(std::size_t n) const {
    if (n >= size())
      throw skeletal::Failure(ErrorCode::InvalidArgument,
                              "Truncated image header");
    return data()[n];
  }
  operator std::span<const std::byte>() const { return {data(), size()}; }

private:
  [[noreturn]] static void fail() {
    throw skeletal::Failure(ErrorCode::Io, "Cannot map image input");
  }
  std::vector<std::byte> owned_;
  const std::byte *mapped_{};
  std::size_t length_{};
#ifdef _WIN32
  HANDLE file_{INVALID_HANDLE_VALUE}, mapping_{};
#else
  int file_{-1};
#endif
};
} // namespace formaview::imaging
