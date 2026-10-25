#pragma once
#include "formaview/contracts.hpp"
#include <functional>
#include <memory>
#include <span>
#include <stop_token>

namespace formaview {
// Syntax validation only: never opens a file or grants access to a scope.
[[nodiscard]] std::filesystem::path
validated_local_path(const std::filesystem::path &);
// Explicit dedicated service-state directory. Rejects reparse traversal and
// foreign ownership, then restricts inherited access to the current user.
class PrivateDirectory {
public:
  static PrivateDirectory open(const std::filesystem::path &);
  PrivateDirectory(PrivateDirectory &&) noexcept;
  PrivateDirectory &operator=(PrivateDirectory &&) noexcept;
  ~PrivateDirectory();

private:
  struct Impl;
  explicit PrivateDirectory(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
// Windows worker APIs. Holds checked ancestor and file handles for the whole
// conversion; denies reparse traversal and concurrent input write/delete.
class InputLease {
public:
  static InputLease open(const AccessScope &, const std::filesystem::path &,
                         std::uint64_t maximum_bytes, std::stop_token = {});
  InputLease(InputLease &&) noexcept;
  InputLease &operator=(InputLease &&) noexcept;
  ~InputLease();
  [[nodiscard]] const InputSnapshot &snapshot() const;
  void require_unchanged(const InputSnapshot &) const;

private:
  struct Impl;
  explicit InputLease(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
struct FileDiscovery {
  struct RejectedPath { std::filesystem::path path; Error error; };
  std::vector<std::filesystem::path> files, rejected_reparse_points;
  std::vector<RejectedPath> rejected_paths;
  std::uint64_t visited_entries{}, ignored_files{};
};
// Membership snapshot only; content is revalidated by InputLease in preflight.
[[nodiscard]] FileDiscovery
discover_files(const AccessScope &, const std::filesystem::path &,
               bool recursive, std::span<const std::wstring_view> extensions,
               std::stop_token = {});
struct FileCommit {
  bool skipped{};
  std::filesystem::path path;
};
struct OutputInspection {
  std::filesystem::path path;
  bool exists{};
  std::string file_identity;
};
// Compare one checked destination against cached input identities without
// reopening that destination for each input. Transactions recheck at commit.
void require_output_not_input(const OutputInspection &,
                              std::span<const InputSnapshot>);
struct OutputIntent {
  std::filesystem::path temporary, destination;
  std::string file_identity;
};
// Recovery removes only the recorded temporary identity under current scope.
// A replaced file, reparse point, or non-FormaView temporary name is rejected.
void remove_owned_temporary(const AccessScope &, const OutputIntent &);
[[nodiscard]] OutputInspection inspect_output(const AccessScope &,
                                              const std::filesystem::path &,
                                              const InputSnapshot &,
                                              ConflictPolicy);
// Read-only name allocation. Reserved paths include every other batch request
// and names already allocated by this plan. Rename commit never reallocates.
[[nodiscard]] OutputInspection
allocate_output(const AccessScope &, const std::filesystem::path &,
                const InputSnapshot &, ConflictPolicy,
                std::span<const std::filesystem::path> reserved = {},
                std::stop_token = {});
class OutputTransaction {
public:
  OutputTransaction(const AccessScope &, const std::filesystem::path &,
                    const InputSnapshot &, ConflictPolicy,
                    std::span<const InputSnapshot> protected_inputs = {});
  OutputTransaction(OutputTransaction &&) noexcept;
  OutputTransaction &operator=(OutputTransaction &&) noexcept;
  ~OutputTransaction();
  void write(std::span<const std::byte>, std::stop_token = {});
  [[nodiscard]] OutputIntent temporary() const;
  [[nodiscard]] FileCommit commit(
      std::stop_token = {},
      const std::function<void(const OutputIntent &)> &before_rename = {});

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace formaview
