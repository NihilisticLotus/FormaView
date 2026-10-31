#include "formaview/file_access.hpp"
#include "formaview/skeletal.hpp"
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <objbase.h>
#include <sstream>
#include <utility>
#include <windows.h>
#include <bcrypt.h>

namespace formaview {
namespace {
[[noreturn]] void fail(ErrorCode code, const char *message, DWORD error = 0) {
  skeletal::Failure f(code, message);
  if (error)
    f.error.details["win32"] = std::to_string(error);
  throw f;
}
void cancel(std::stop_token stop) {
  if (stop.stop_requested())
    fail(ErrorCode::Cancelled, "File operation cancelled");
}
struct Handle {
  HANDLE h{INVALID_HANDLE_VALUE};
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : h(value) {}
  Handle(Handle &&o) noexcept : h(std::exchange(o.h, INVALID_HANDLE_VALUE)) {}
  Handle &operator=(Handle &&o) noexcept {
    if (this != &o) {
      if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
      h = std::exchange(o.h, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};
bool equal(const std::wstring &a, const std::wstring &b) {
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::filesystem::path normal(const std::filesystem::path &path) {
  auto text = path.wstring();
  std::replace(text.begin(), text.end(), L'/', L'\\');
  if (text.size() < 3 ||
      !((text[0] >= L'A' && text[0] <= L'Z') ||
        (text[0] >= L'a' && text[0] <= L'z')) ||
      text[1] != L':' || text[2] != L'\\')
    fail(ErrorCode::OutOfScope,
         "Only absolute local drive paths are authorized");
  if (text.size() > 32700)
    fail(ErrorCode::InvalidArgument, "Path too long");
  if (GetDriveTypeW(text.substr(0, 3).c_str()) == DRIVE_REMOTE)
    fail(ErrorCode::OutOfScope, "Mapped network drives are not authorized");
  std::filesystem::path result(text.substr(0, 3));
  std::size_t begin = 3;
  while (begin < text.size()) {
    const auto end = text.find(L'\\', begin);
    const auto part =
        text.substr(begin, end == std::wstring::npos ? end : end - begin);
    if (part.empty() || part == L"." || part == L".." || part.back() == L'.' ||
        part.back() == L' ' || part.size() > 255)
      fail(ErrorCode::OutOfScope, "Ambiguous path component rejected");
    for (auto c : part)
      if (c < 32 ||
          std::wstring_view(L":*?\"<>|").find(c) != std::wstring_view::npos)
        fail(ErrorCode::OutOfScope, "Device/stream/wildcard path rejected");
    const auto stem = part.substr(0, part.find(L'.'));
    if (equal(stem, L"CON") || equal(stem, L"PRN") || equal(stem, L"AUX") ||
        equal(stem, L"NUL") ||
        (stem.size() == 4 &&
         (equal(stem.substr(0, 3), L"COM") ||
          equal(stem.substr(0, 3), L"LPT")) &&
         stem[3] >= L'0' && stem[3] <= L'9'))
      fail(ErrorCode::OutOfScope, "Reserved device name rejected");
    result /= part;
    if (end == std::wstring::npos)
      break;
    begin = end + 1;
  }
  return result;
}
bool beneath(const std::filesystem::path &path,
             const std::filesystem::path &root) {
  auto p = path.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++p)
    if (p == path.end() || !equal(p->wstring(), r->wstring()))
      return false;
  return true;
}
void authorize(const std::filesystem::path &path,
               const std::vector<std::filesystem::path> &roots) {
  for (const auto &root : roots)
    if (beneath(path, normal(root)))
      return;
  fail(ErrorCode::OutOfScope, "Path is outside the configured scope");
}
std::wstring extended(const std::filesystem::path &path) {
  return L"\\\\?\\" + path.wstring();
}
BY_HANDLE_FILE_INFORMATION info(const Handle &h, bool directory) {
  BY_HANDLE_FILE_INFORMATION i{};
  if (!GetFileInformationByHandle(h.h, &i))
    fail(ErrorCode::Io, "Cannot inspect file handle", GetLastError());
  if (i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
    fail(ErrorCode::OutOfScope, "Reparse points are not allowed");
  if (static_cast<bool>(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) !=
      directory)
    fail(ErrorCode::InvalidArgument, "Unexpected file/directory type");
  if (GetFileType(h.h) != FILE_TYPE_DISK)
    fail(ErrorCode::OutOfScope, "Only disk files are allowed");
  return i;
}
std::string identity(const BY_HANDLE_FILE_INFORMATION &i) {
  std::ostringstream out;
  out << std::hex << i.dwVolumeSerialNumber << ':' << i.nFileIndexHigh << ':'
      << i.nFileIndexLow;
  return out.str();
}
std::vector<Handle> parents(const std::filesystem::path &parent) {
  std::vector<Handle> handles;
  auto current = parent.root_path();
  const auto hold = [&] {
    Handle h(CreateFileW(
        extended(current).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h)
      fail(ErrorCode::Io, "Cannot hold parent directory", GetLastError());
    (void)info(h, true);
    handles.push_back(std::move(h));
  };
  hold();
  for (const auto &part : parent.relative_path()) {
    current /= part;
    hold();
  }
  return handles;
}
std::string hash_file(const Handle &handle, std::uint64_t size,
                      std::uint64_t maximum, std::stop_token stop) {
  if (size > maximum || size > SIZE_MAX)
    fail(ErrorCode::ResourceLimit, "Input exceeds snapshot budget");
  cancel(stop);
  struct Hash {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE value{};
    ~Hash() {
      if (value) BCryptDestroyHash(value);
      if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
  } hash;
  if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM,
                                  nullptr, 0) < 0 ||
      BCryptCreateHash(hash.algorithm, &hash.value, nullptr, 0, nullptr, 0, 0) < 0)
    fail(ErrorCode::Internal, "Cannot initialize input SHA256");
  // Bounded scratch space independent of file length. Hash each read before
  // reusing the buffer, keeping cancellation checks between 1 MiB blocks.
  std::vector<unsigned char> bytes(1024 * 1024);
  for (std::uint64_t offset = 0; offset < size;) {
    cancel(stop);
    const auto n = static_cast<DWORD>(
        std::min<std::uint64_t>(bytes.size(), size - offset));
    DWORD got = 0;
    if (!ReadFile(handle.h, bytes.data(), n, &got, nullptr) ||
        got != n)
      fail(ErrorCode::Io, "Input snapshot read failed", GetLastError());
    if (BCryptHashData(hash.value, bytes.data(), got, 0) < 0)
      fail(ErrorCode::Internal, "Input SHA256 update failed");
    offset += got;
  }
  cancel(stop);
  std::array<unsigned char, 32> digest{};
  if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
    fail(ErrorCode::Internal, "Input SHA256 finish failed");
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto value : digest) {
    result.push_back(digits[value >> 4]);
    result.push_back(digits[value & 15]);
  }
  return result;
}
std::wstring unique_name() {
  GUID id{};
  if (FAILED(CoCreateGuid(&id)))
    fail(ErrorCode::Internal, "Cannot allocate output identity");
  wchar_t value[40]{};
  StringFromGUID2(id, value, 40);
  return std::wstring(L".formaview-") + value + L".tmp";
}
void check_target(const std::filesystem::path &path,
                  const InputSnapshot &input,
                  std::span<const InputSnapshot> others = {}) {
  Handle h(CreateFileW(
      extended(path).c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr));
  if (!h) {
    const auto e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) {
      const OutputInspection output{path, false, {}};
      require_output_not_input(output, std::span(&input, 1));
      require_output_not_input(output, others);
      return;
    }
    fail(ErrorCode::Io, "Cannot inspect output destination", e);
  }
  const OutputInspection output{path, true, identity(info(h, false))};
  require_output_not_input(output, std::span(&input, 1));
  require_output_not_input(output, others);
}
} // namespace
void require_output_not_input(const OutputInspection &output,
                              std::span<const InputSnapshot> inputs) {
  for (const auto &input : inputs) {
    // InputSnapshot paths come from validated InputLease snapshots (or the
    // validated fallback used for unreadable group members).
    if (equal(output.path.native(),
              input.resolved_path.lexically_normal().make_preferred().native()))
      fail(ErrorCode::Conflict, "Output cannot replace an input");
    if (output.exists && !input.file_identity.empty() &&
        output.file_identity == input.file_identity)
      fail(ErrorCode::Conflict, "Output is a hard link to an input");
  }
}
std::filesystem::path validated_local_path(const std::filesystem::path &path) {
  return normal(path);
}
struct PrivateDirectory::Impl {
  std::vector<Handle> parents;
  Handle directory;
};
PrivateDirectory::PrivateDirectory(std::unique_ptr<Impl> p)
    : impl_(std::move(p)) {}
PrivateDirectory::PrivateDirectory(PrivateDirectory &&) noexcept = default;
PrivateDirectory &
PrivateDirectory::operator=(PrivateDirectory &&) noexcept = default;
PrivateDirectory::~PrivateDirectory() = default;
PrivateDirectory PrivateDirectory::open(const std::filesystem::path &path) {
  auto result = std::make_unique<Impl>();
  const auto full = normal(path);
  result->parents = parents(full.parent_path());
  result->directory = Handle(CreateFileW(
      extended(full).c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!result->directory)
    fail(ErrorCode::Io, "Cannot hold private journal directory",
         GetLastError());
  (void)info(result->directory, true);
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h))
    fail(ErrorCode::Io, "Cannot inspect process identity", GetLastError());
  DWORD size = 0;
  (void)GetTokenInformation(token.h, TokenUser, nullptr, 0, &size);
  std::vector<std::byte> user(size);
  if (!GetTokenInformation(token.h, TokenUser, user.data(), size, &size))
    fail(ErrorCode::Io, "Cannot read process identity", GetLastError());
  const auto sid = reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid;
  DWORD owner_size = 0;
  (void)GetTokenInformation(token.h, TokenOwner, nullptr, 0, &owner_size);
  std::vector<std::byte> default_owner(owner_size);
  if (!GetTokenInformation(token.h, TokenOwner, default_owner.data(),
                           owner_size, &owner_size))
    fail(ErrorCode::Io, "Cannot read token default owner", GetLastError());
  const auto default_owner_sid =
      reinterpret_cast<TOKEN_OWNER *>(default_owner.data())->Owner;
  struct LocalMemory {
    HLOCAL p{};
    ~LocalMemory() {
      if (p)
        LocalFree(p);
    }
  } security, acl_memory;
  PSID owner{};
  PSECURITY_DESCRIPTOR descriptor{};
  const auto inspected = GetSecurityInfo(
      result->directory.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
      nullptr, nullptr, nullptr, &descriptor);
  security.p = descriptor;
  // Elevated Windows tokens commonly create directories owned by the token's
  // default Administrators group. Accept that actual token owner as well;
  // the installed DACL still grants access only to TokenUser.
  if (inspected != ERROR_SUCCESS || !owner ||
      (!EqualSid(owner, sid) && !EqualSid(owner, default_owner_sid)))
    fail(ErrorCode::OutOfScope,
         "Journal directory must belong to the current user", inspected);
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = FILE_ALL_ACCESS;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
  PACL acl{};
  const auto built = SetEntriesInAclW(1, &access, nullptr, &acl);
  acl_memory.p = acl;
  if (built != ERROR_SUCCESS)
    fail(ErrorCode::Io, "Cannot create private journal ACL", built);
  const auto secured = SetSecurityInfo(result->directory.h, SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION |
                                           PROTECTED_DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, acl, nullptr);
  if (secured != ERROR_SUCCESS)
    fail(ErrorCode::Io, "Cannot restrict journal access", secured);
  return PrivateDirectory(std::move(result));
}
struct InputIdentityLease::Impl {
  std::vector<Handle> parents;
  Handle file;
  InputSnapshot snapshot;
};
InputIdentityLease::InputIdentityLease(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
InputIdentityLease::InputIdentityLease(InputIdentityLease &&) noexcept = default;
InputIdentityLease &InputIdentityLease::operator=(InputIdentityLease &&) noexcept = default;
InputIdentityLease::~InputIdentityLease() = default;
InputIdentityLease InputIdentityLease::open(const AccessScope &scope,
                            const std::filesystem::path &path,
                            std::stop_token stop) {
  cancel(stop);
  auto p = std::make_unique<Impl>();
  const auto full = normal(path);
  authorize(full, scope.input_roots);
  p->parents = parents(full.parent_path());
  p->file = Handle(CreateFileW(
      extended(full).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!p->file)
    fail(ErrorCode::Io, "Cannot hold input file", GetLastError());
  const auto i = info(p->file, false);
  auto &s = p->snapshot;
  s.requested_path = path;
  s.resolved_path = full;
  s.size_bytes =
      (static_cast<std::uint64_t>(i.nFileSizeHigh) << 32) | i.nFileSizeLow;
  s.modified_ticks = static_cast<std::int64_t>(
      (static_cast<std::uint64_t>(i.ftLastWriteTime.dwHighDateTime) << 32) |
      i.ftLastWriteTime.dwLowDateTime);
  s.file_identity = identity(i);
  cancel(stop);
  return InputIdentityLease(std::move(p));
}
const InputSnapshot &InputIdentityLease::snapshot() const { return impl_->snapshot; }
struct InputLease::Impl {
  InputIdentityLease identity;
  InputSnapshot snapshot;
  explicit Impl(InputIdentityLease pin)
      : identity(std::move(pin)), snapshot(identity.snapshot()) {}
};
InputLease::InputLease(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
InputLease::InputLease(InputLease &&) noexcept = default;
InputLease &InputLease::operator=(InputLease &&) noexcept = default;
InputLease::~InputLease() = default;
InputLease InputLease::open(const AccessScope &scope,
                            const std::filesystem::path &path,
                            std::uint64_t maximum, std::stop_token stop) {
  auto p = std::make_unique<Impl>(InputIdentityLease::open(scope, path, stop));
  auto &s = p->snapshot;
  s.content_sha256 = hash_file(p->identity.impl_->file, s.size_bytes, maximum, stop);
  s.asset = asset_id_from_sha256(s.content_sha256);
  return InputLease(std::move(p));
}
const InputSnapshot &InputLease::snapshot() const { return impl_->snapshot; }
FileDiscovery discover_files(const AccessScope &scope,
                             const std::filesystem::path &root, bool recursive,
                             std::span<const std::wstring_view> extensions,
                             std::stop_token stop, bool bounded_membership) {
  const auto full = normal(root);
  authorize(full, scope.input_roots);
  std::vector<std::filesystem::path> pending{full};
  FileDiscovery result;
  std::size_t directories = 0, path_bytes = 0;
  while (!pending.empty()) {
    cancel(stop);
    const auto current = pending.back();
    pending.pop_back();
    if (++directories > 512)
      fail(ErrorCode::ResourceLimit, "Discovery exceeds 512 directories");
    const auto held = parents(current);
    WIN32_FIND_DATAW data{};
    struct Search {
      HANDLE value{INVALID_HANDLE_VALUE};
      ~Search() {
        if (value != INVALID_HANDLE_VALUE)
          FindClose(value);
      }
    } search;
    search.value =
        FindFirstFileExW(extended(current / L"*").c_str(), FindExInfoBasic,
                         &data, FindExSearchNameMatch, nullptr, 0);
    if (search.value == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND)
        continue;
      fail(ErrorCode::Io, "Cannot enumerate authorized directory",
           GetLastError());
    }
    do {
      cancel(stop);
      const std::wstring name(data.cFileName);
      if (name == L"." || name == L"..")
        continue;
      if (++result.visited_entries > 100000 && bounded_membership)
        fail(ErrorCode::ResourceLimit, "Discovery exceeds 100000 entries");
      std::filesystem::path path;
      try {
        path = normal(current / name);
      } catch (const skeletal::Failure &error) {
        if (error.error.code != ErrorCode::OutOfScope &&
            error.error.code != ErrorCode::InvalidArgument) throw;
        if (result.rejected_paths.size() >= 100)
          fail(ErrorCode::ResourceLimit, "Discovery exceeds 100 rejected path entries");
        const auto rejected = current / name;
        path_bytes += rejected.wstring().size() * sizeof(wchar_t);
        if (path_bytes > (bounded_membership ? 1024ULL * 1024 : 128ULL * 1024 * 1024))
          fail(ErrorCode::ResourceLimit, "Discovery membership exceeds bounded storage");
        result.rejected_paths.push_back({rejected, error.error});
        continue;
      }
      if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (result.rejected_reparse_points.size() >= 100)
          fail(ErrorCode::ResourceLimit,
               "Discovery exceeds 100 rejected reparse entries");
        result.rejected_reparse_points.push_back(path);
        continue;
      }
      if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (recursive) {
          if (pending.size() + directories >= 512 && bounded_membership)
            fail(ErrorCode::ResourceLimit, "Discovery exceeds 512 directories");
          pending.push_back(path);
        }
        continue;
      }
      const auto extension = path.extension().wstring();
      if (std::none_of(extensions.begin(), extensions.end(), [&](auto value) {
            return equal(extension, std::wstring(value));
          })) {
        ++result.ignored_files;
        continue;
      }
      path_bytes += path.wstring().size() * sizeof(wchar_t);
      if ((bounded_membership && result.files.size() >= 4096) ||
          path_bytes > (bounded_membership ? 1024ULL * 1024 : 128ULL * 1024 * 1024))
        fail(ErrorCode::ResourceLimit,
             "Discovery membership exceeds bounded storage");
      result.files.push_back(path);
    } while (FindNextFileW(search.value, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES)
      fail(ErrorCode::Io, "Directory enumeration interrupted", GetLastError());
  }
  std::sort(
      result.files.begin(), result.files.end(),
      [](const auto &a, const auto &b) { return a.native() < b.native(); });
  return result;
}
OutputInspection inspect_output(const AccessScope &scope,
                                const std::filesystem::path &path,
                                const InputSnapshot &input,
                                ConflictPolicy conflict) {
  const auto full = normal(path);
  authorize(full, scope.output_roots);
  if (conflict == ConflictPolicy::Overwrite && !scope.allow_overwrite)
    fail(ErrorCode::OutOfScope, "Overwrite permission is not enabled");
  if (conflict != ConflictPolicy::Skip && conflict != ConflictPolicy::Rename &&
      conflict != ConflictPolicy::Overwrite)
    fail(ErrorCode::InvalidArgument, "Unknown conflict policy");
  const auto held = parents(full.parent_path());
  check_target(full, input);
  Handle h(CreateFileW(
      extended(full).c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr));
  if (!h) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
      return {full, false, {}};
    fail(ErrorCode::Io, "Cannot inspect destination", GetLastError());
  }
  return {full, true, identity(info(h, false))};
}
OutputInspection allocate_output(
    const AccessScope &scope, const std::filesystem::path &requested,
    const InputSnapshot &input, ConflictPolicy conflict,
    std::span<const std::filesystem::path> reserved, std::stop_token stop) {
  const auto full = normal(requested);
  for (std::uint32_t attempt = 0; attempt < 10000; ++attempt) {
    cancel(stop);
    auto target = full;
    if (attempt)
      target = full.parent_path() /
               (full.stem().wstring() + L" (" + std::to_wstring(attempt) +
                L")" + full.extension().wstring());
    const bool claimed =
        std::any_of(reserved.begin(), reserved.end(), [&](const auto &other) {
          return equal(target.wstring(), other.lexically_normal().wstring());
        });
    if (claimed) {
      if (conflict == ConflictPolicy::Rename)
        continue;
      fail(ErrorCode::Conflict, "Output is reserved by another batch item");
    }
    const auto candidate = inspect_output(scope, target, input, conflict);
    if (conflict != ConflictPolicy::Rename || !candidate.exists)
      return candidate;
  }
  fail(ErrorCode::Conflict, "Output rename candidates exhausted");
}
void InputLease::require_unchanged(const InputSnapshot &expected) const {
  const auto &s = snapshot();
  if (!equal(s.resolved_path.wstring(),
             normal(expected.resolved_path).wstring()) ||
      s.size_bytes != expected.size_bytes ||
      s.modified_ticks != expected.modified_ticks ||
      s.file_identity != expected.file_identity ||
      s.content_sha256 != expected.content_sha256)
    fail(ErrorCode::InputChanged, "Input changed after preflight");
}
struct OutputTransaction::Impl {
  std::vector<Handle> parents;
  Handle file;
  std::filesystem::path requested, temp;
  InputSnapshot input;
  std::vector<InputSnapshot> protected_inputs;
  ConflictPolicy conflict{};
  bool committed{}, written{}, started{};
  ~Impl() {
    if (file && !committed) {
      FILE_DISPOSITION_INFO remove{TRUE};
      (void)SetFileInformationByHandle(file.h, FileDispositionInfo, &remove,
                                       sizeof(remove));
    }
  }
};
OutputTransaction::OutputTransaction(
    const AccessScope &scope, const std::filesystem::path &path,
    const InputSnapshot &input, ConflictPolicy conflict,
    std::span<const InputSnapshot> protected_inputs)
    : impl_(std::make_unique<Impl>()) {
  auto &p = *impl_;
  p.requested = normal(path);
  authorize(p.requested, scope.output_roots);
  if (conflict == ConflictPolicy::Overwrite && !scope.allow_overwrite)
    fail(ErrorCode::OutOfScope, "Overwrite permission is not enabled");
  if (conflict != ConflictPolicy::Skip && conflict != ConflictPolicy::Rename &&
      conflict != ConflictPolicy::Overwrite)
    fail(ErrorCode::InvalidArgument, "Unknown conflict policy");
  p.parents = parents(p.requested.parent_path());
  check_target(p.requested, input, protected_inputs);
  p.protected_inputs.assign(protected_inputs.begin(), protected_inputs.end());
  p.input = input;
  p.conflict = conflict;
  for (int attempt = 0; attempt < 8; ++attempt) {
    p.temp = p.requested.parent_path() / unique_name();
    p.file = Handle(CreateFileW(
        extended(p.temp).c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr));
    if (p.file)
      return;
    if (GetLastError() != ERROR_FILE_EXISTS)
      fail(ErrorCode::Io, "Cannot create temporary output", GetLastError());
  }
  fail(ErrorCode::Conflict, "Temporary output identity collision");
}
OutputTransaction::OutputTransaction(OutputTransaction &&) noexcept = default;
OutputTransaction &
OutputTransaction::operator=(OutputTransaction &&) noexcept = default;
OutputTransaction::~OutputTransaction() = default;
void OutputTransaction::write(std::span<const std::byte> bytes,
                              std::stop_token stop) {
  auto &p = *impl_;
  if (p.committed || p.started)
    fail(ErrorCode::InvalidArgument, "Output transaction already written");
  p.started = true;
  for (std::size_t offset = 0; offset < bytes.size();) {
    cancel(stop);
    const auto n = static_cast<DWORD>(
        std::min<std::size_t>(1024 * 1024, bytes.size() - offset));
    DWORD written = 0;
    if (!WriteFile(p.file.h, bytes.data() + offset, n, &written, nullptr) ||
        written != n)
      fail(ErrorCode::Io, "Temporary output write failed", GetLastError());
    offset += written;
  }
  cancel(stop);
  if (!FlushFileBuffers(p.file.h))
    fail(ErrorCode::Io, "Temporary output flush failed", GetLastError());
  p.written = true;
}
OutputIntent OutputTransaction::temporary() const {
  return {impl_->temp, {}, identity(info(impl_->file, false))};
}
void remove_owned_temporary(const AccessScope &scope,
                            const OutputIntent &intent) {
  const auto full = normal(intent.temporary);
  authorize(full, scope.output_roots);
  const auto name = full.filename().wstring();
  GUID guid{};
  if (name.size() != 11 + 38 + 4 || name.substr(0, 11) != L".formaview-" ||
      name.substr(name.size() - 4) != L".tmp" ||
      FAILED(CLSIDFromString(name.substr(11, 38).c_str(), &guid)) ||
      intent.file_identity.empty())
    fail(ErrorCode::InvalidArgument, "Invalid owned temporary identity");
  const auto held = parents(full.parent_path());
  Handle file(CreateFileW(
      extended(full).c_str(), FILE_READ_ATTRIBUTES | DELETE, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!file) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
      return;
    fail(ErrorCode::Io, "Cannot hold recovery temporary", GetLastError());
  }
  if (identity(info(file, false)) != intent.file_identity)
    fail(ErrorCode::Conflict, "Recovery temporary was replaced");
  FILE_DISPOSITION_INFO remove{TRUE};
  if (!SetFileInformationByHandle(file.h, FileDispositionInfo, &remove,
                                  sizeof(remove)))
    fail(ErrorCode::Io, "Cannot remove owned temporary", GetLastError());
}
FileCommit OutputTransaction::commit(
    std::stop_token stop,
    const std::function<void(const OutputIntent &)> &before_rename) {
  auto &p = *impl_;
  if (p.committed || !p.written)
    fail(ErrorCode::InvalidArgument, "Output transaction is not ready");
  {
    cancel(stop);
    const auto &target = p.requested;
    (void)normal(target);
    check_target(target, p.input, p.protected_inputs);
    const auto name = extended(target);
    const auto size = offsetof(FILE_RENAME_INFO, FileName) +
                      (name.size() + 1) * sizeof(wchar_t);
    std::vector<std::byte> buffer(size);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
    rename->ReplaceIfExists = p.conflict == ConflictPolicy::Overwrite;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
    FILE_BASIC_INFO basic{};
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileInformationByHandle(p.file.h, FileBasicInfo, &basic,
                                    sizeof(basic)))
      fail(ErrorCode::Io, "Cannot finalize output attributes", GetLastError());
    if (before_rename)
      before_rename({p.temp, target, identity(info(p.file, false))});
    cancel(stop);
    // Automatic cleanup starts at CreateFile, before any journal exists.
    // Clear it only after the durable intent callback succeeds. Unsupported
    // filesystems fail closed; the handle still reclaims the temporary.
    FILE_DISPOSITION_INFO_EX keep{};
    keep.Flags = FILE_DISPOSITION_FLAG_ON_CLOSE;
    if (!SetFileInformationByHandle(p.file.h, FileDispositionInfoEx, &keep,
                                    sizeof(keep)))
      fail(ErrorCode::Unsupported,
           "Filesystem cannot prepare a recoverable output commit",
           GetLastError());
    if (SetFileInformationByHandle(p.file.h, FileRenameInfo, rename,
                                   static_cast<DWORD>(size))) {
      p.committed = true;
      p.file = Handle();
      return {false, target};
    }
    const auto error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
      fail(ErrorCode::Io, "Atomic output commit failed", error);
    if (p.conflict == ConflictPolicy::Skip)
      return {true, {}};
    fail(ErrorCode::Conflict,
         "Approved output name is occupied; run a new preflight", error);
  }
}
} // namespace formaview
