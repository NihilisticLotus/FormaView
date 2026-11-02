#include "formaview/file_access.hpp"
#include "formaview/skeletal.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <windows.h>
#include <winioctl.h>

using namespace formaview;
namespace {
void require(bool ok, const char *s) {
  if (!ok)
    throw std::runtime_error(std::string(s) +
                             " win32=" + std::to_string(GetLastError()));
}
void write(const std::filesystem::path &path, std::string_view value) {
  std::ofstream f(path, std::ios::binary);
  f << value;
  require(static_cast<bool>(f), "Fixture write failed");
}
std::string read(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
template <class F> void rejects(F f, ErrorCode code) {
  try {
    f();
  } catch (const skeletal::Failure &e) {
    require(e.error.code == code, "Wrong structured error");
    return;
  }
  throw std::runtime_error("Expected rejection");
}
std::span<const std::byte> bytes(std::string_view value) {
  return std::as_bytes(std::span(value.data(), value.size()));
}
void junction(const std::filesystem::path &link,
              const std::filesystem::path &target) {
  std::filesystem::create_directory(link);
  const auto substitute = L"\\??\\" + target.wstring(),
             print = target.wstring();
  struct Header {
    DWORD tag;
    WORD length, reserved, sub_offset, sub_length, print_offset, print_length;
  };
  std::vector<std::byte> data(sizeof(Header) +
                              (substitute.size() + print.size() + 2) * 2);
  auto *header = reinterpret_cast<Header *>(data.data());
  header->tag = IO_REPARSE_TAG_MOUNT_POINT;
  header->length = static_cast<WORD>(data.size() - 8);
  header->sub_length = static_cast<WORD>(substitute.size() * 2);
  header->print_offset = static_cast<WORD>((substitute.size() + 1) * 2);
  header->print_length = static_cast<WORD>(print.size() * 2);
  std::memcpy(data.data() + sizeof(Header), substitute.c_str(),
              (substitute.size() + 1) * 2);
  std::memcpy(data.data() + sizeof(Header) + header->print_offset,
              print.c_str(), (print.size() + 1) * 2);
  const auto handle = CreateFileW(
      link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  require(handle != INVALID_HANDLE_VALUE, "Create junction handle");
  DWORD returned = 0;
  const auto ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data.data(),
                                  static_cast<DWORD>(data.size()), nullptr, 0,
                                  &returned, nullptr);
  const auto error = GetLastError();
  CloseHandle(handle);
  SetLastError(error);
  require(ok != FALSE, "Create junction");
}
DWORD child(const std::wstring &mode, const std::filesystem::path &root) {
  std::wstring executable(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  require(length != 0 && length < executable.size(), "Get test executable");
  executable.resize(length);
  auto command = L"\"" + executable + L"\" " + mode + L" \"" + root.wstring() + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "Start directory probe child");
  const auto waited = WaitForSingleObject(process.hProcess, 10000);
  DWORD code = 0;
  const auto queried = GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  require(waited == WAIT_OBJECT_0 && queried, "Directory probe child timed out");
  return code;
}
void junctionProbe(const std::filesystem::path &root) {
  require(!std::filesystem::exists(root), "Probe root must be new");
  std::filesystem::create_directories(root / "input" / "sub");
  std::filesystem::create_directories(root / "outside");
  write(root / "input" / "sub" / "leaf.bin", "inside");
  write(root / "outside" / "leaf.bin", "outside");
  const auto held = CreateFileW((root / "input" / "sub").c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  require(held != INVALID_HANDLE_VALUE, "Hold original ancestor flags");
  const auto code = child(L"--replace-parent", root);
  const auto observed = read(root / "input" / "sub" / "leaf.bin");
  CloseHandle(held);
  std::cout << "Original ancestor flags: child=" << code << " leaf=" << observed << '\n';
  require(code == 0 && observed == "outside", "Junction replacement was not reproduced");
}
void parentLock(const std::filesystem::path &root) {
  require(!std::filesystem::exists(root), "Parent lock root must be new");
  std::filesystem::create_directories(root / "sub");
  write(root / "sub" / "leaf.bin", "inside");
  AccessScope scope; scope.input_roots = {root};
  const auto lease = InputLease::open(scope, root / "sub" / "leaf.bin", 1024);
  const auto code = child(L"--open-parent-delete", root / "sub");
  std::cout << "Held InputLease ancestor DELETE open child=" << code << '\n';
  require(code == ERROR_SHARING_VIOLATION, "Held ancestor permits a DELETE handle");
  lease.require_unchanged(lease.snapshot());
}
void run(const std::filesystem::path &root) {
  parentLock(root / "parent-lock");
  const auto input = root / "input", output = root / "output",
             outside = root / "input-neighbour";
  std::filesystem::create_directories(input);
  std::filesystem::create_directories(output);
  std::filesystem::create_directories(outside);
  write(input / L"原图.bin", "source");
  write(outside / "escape.bin", "outside");
  write(output / "existing.bin", "old");
  require(CreateHardLinkW((output / "alias.bin").c_str(),
                          (input / L"原图.bin").c_str(), nullptr) != FALSE,
          "Create hardlink");
  junction(input / "junction", outside);
  junction(output / "junction", outside);
  AccessScope scope;
  scope.input_roots = {input};
  scope.output_roots = {output};
  scope.policy_revision = "test-1";
  const std::wstring_view extensions[] = {L".bin"};
  const auto discovered = discover_files(scope, input, true, extensions);
  require(discovered.files.size() == 1 &&
              discovered.rejected_reparse_points.size() == 1,
          "Directory discovery followed a junction or missed source");
  const auto ambiguous = L"\\\\?\\" + (input / L"trailing.bin.").wstring();
  const auto ambiguousFile = CreateFileW(ambiguous.c_str(), GENERIC_WRITE, 0,
      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(ambiguousFile != INVALID_HANDLE_VALUE, "Create ambiguous-name fixture");
  CloseHandle(ambiguousFile);
  const auto withAmbiguous = discover_files(scope, input, true, extensions);
  require(withAmbiguous.files.size() == 1 && withAmbiguous.rejected_paths.size() == 1 &&
              withAmbiguous.rejected_paths[0].path.filename() == L"trailing.bin." &&
              withAmbiguous.rejected_paths[0].error.code == ErrorCode::OutOfScope,
          "An unrelated ambiguous name stopped valid directory discovery");
  rejects([&] { (void)discover_files(scope, outside, true, extensions); },
          ErrorCode::OutOfScope);
  const auto wide = root / "wide";
  std::filesystem::create_directories(wide);
  AccessScope wideScope;
  wideScope.input_roots = {wide};
  for (int i = 0; i < 511; ++i)
    std::filesystem::create_directory(wide / std::to_string(i));
  require(discover_files(wideScope, wide, true, extensions, {}, false).files.empty(),
          "Discovery should permit 512 directories including the root");
  std::filesystem::create_directory(wide / "overflow");
  rejects([&] { (void)discover_files(wideScope, wide, true, extensions, {}, false); },
          ErrorCode::ResourceLimit);
  std::stop_source discovery_stop;
  discovery_stop.request_stop();
  rejects(
      [&] {
        (void)discover_files(scope, input, true, extensions,
                             discovery_stop.get_token());
      },
      ErrorCode::Cancelled);
  InputSnapshot snapshot;
  {
    auto lease = InputLease::open(scope, input / L"原图.bin", 1024);
    snapshot = lease.snapshot();
    lease.require_unchanged(snapshot);
    require(snapshot.size_bytes == 6 && !snapshot.file_identity.empty() &&
                snapshot.content_sha256 == skeletal::sha256(bytes("source")),
            "Wrong input snapshot");
    // Network paths are outside the current local-disk authorization model,
    // including callers that explicitly put them in their requested scope.
    for (const auto *network : {
             LR"(\\localhost\formaview-no-such-share\asset.bin)",
             LR"(//localhost/formaview-no-such-share/asset.bin)",
             LR"(\\?\UNC\localhost\formaview-no-such-share\asset.bin)"}) {
      const std::filesystem::path path(network);
      AccessScope requested{{path.parent_path()}, {path.parent_path()}, true, "network-rejection"};
      rejects([&] { (void)validated_local_path(path); }, ErrorCode::OutOfScope);
      rejects([&] { (void)PrivateDirectory::open(path.parent_path()); }, ErrorCode::OutOfScope);
      rejects([&] { (void)InputLease::open(requested, path, 1024); }, ErrorCode::OutOfScope);
      rejects([&] { (void)discover_files(requested, path.parent_path(), true, extensions); }, ErrorCode::OutOfScope);
      rejects([&] { OutputTransaction t(requested, path, snapshot, ConflictPolicy::Overwrite); }, ErrorCode::OutOfScope);
    }
    std::cout << "PASS UNC rejection for validation, private journals, input, discovery and output\n";
    auto writer =
        CreateFileW((input / L"原图.bin").c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (writer != INVALID_HANDLE_VALUE)
      CloseHandle(writer);
    require(writer == INVALID_HANDLE_VALUE, "Input lease did not block writes");
    require(!MoveFileExW(input.c_str(), (root / "renamed").c_str(), 0),
            "Ancestor rename was not blocked");
    rejects(
        [&] { (void)InputLease::open(scope, outside / "escape.bin", 1024); },
        ErrorCode::OutOfScope);
    rejects(
        [&] { (void)InputIdentityLease::open(scope, outside / "escape.bin"); },
        ErrorCode::OutOfScope);
    rejects(
        [&] { (void)InputIdentityLease::open(scope, input / "junction" / "escape.bin"); },
        ErrorCode::OutOfScope);
    rejects(
        [&] {
          (void)InputLease::open(scope, input / "junction" / "escape.bin",
                                 1024);
        },
        ErrorCode::OutOfScope);
    rejects(
        [&] {
          (void)InputLease::open(
              scope, input / ".." / "input-neighbour" / "escape.bin", 1024);
        },
        ErrorCode::OutOfScope);
    rejects(
        [&] {
          (void)InputLease::open(scope, input / L"原图.bin:stream", 1024);
        },
        ErrorCode::OutOfScope);
    rejects([&] { (void)InputLease::open(scope, input / "NUL", 1024); },
            ErrorCode::OutOfScope);
    rejects([&] { (void)InputLease::open(scope, input / L"原图.bin", 2); },
            ErrorCode::ResourceLimit);
    rejects(
        [&] {
          OutputTransaction t(scope, outside / "escape.bin", snapshot,
                              ConflictPolicy::Skip);
        },
        ErrorCode::OutOfScope);
    rejects(
        [&] {
          OutputTransaction t(scope, output / "junction" / "escape.bin",
                              snapshot, ConflictPolicy::Skip);
        },
        ErrorCode::OutOfScope);
    rejects(
        [&] {
          OutputTransaction t(scope, output / "existing.bin", snapshot,
                              ConflictPolicy::Overwrite);
        },
        ErrorCode::OutOfScope);
    rejects(
        [&] {
          OutputTransaction t(scope, output / "alias.bin", snapshot,
                              ConflictPolicy::Skip);
        },
        ErrorCode::Conflict);
    {
      OutputTransaction t(scope, output / "new.bin", snapshot,
                          ConflictPolicy::Skip);
      t.write(bytes("complete"));
      auto result = t.commit();
      require(!result.skipped && read(result.path) == "complete",
              "Atomic commit failed");
    }
    {
      OutputTransaction t(scope, output / "existing.bin", snapshot,
                          ConflictPolicy::Skip);
      t.write(bytes("skip"));
      require(t.commit().skipped, "Conflict did not skip");
      require(read(output / "existing.bin") == "old",
              "Skip modified destination");
    }
    {
      const auto allocated = allocate_output(scope, output / "existing.bin",
                                             snapshot, ConflictPolicy::Rename);
      require(!allocated.exists && !std::filesystem::exists(allocated.path),
              "Rename allocation wrote a destination");
      OutputTransaction t(scope, allocated.path, snapshot,
                          ConflictPolicy::Rename);
      t.write(bytes("renamed"));
      auto result = t.commit();
      require(result.path.filename() == "existing (1).bin" &&
                  read(result.path) == "renamed",
              "Conflict rename failed");
    }
    {
      const auto allocated = allocate_output(scope, output / "existing.bin",
                                             snapshot, ConflictPolicy::Rename);
      OutputTransaction t(scope, allocated.path, snapshot,
                          ConflictPolicy::Rename);
      t.write(bytes("must-not-reallocate"));
      // Claim the exact approved path after the last validation, immediately
      // before atomic rename. The kernel must reject without rolling suffixes.
      rejects(
          [&] {
            (void)t.commit({}, [&](const OutputIntent &intent) {
              write(intent.destination, "external-owner");
            });
          },
          ErrorCode::Conflict);
      require(read(allocated.path) == "external-owner" &&
                  !std::filesystem::exists(output / "existing (3).bin"),
              "Atomic rename overwrote a claimant or chose an unapproved name");
    }
    scope.allow_overwrite = true;
    {
      const auto raced = output / "late-source-alias.bin";
      OutputTransaction t(scope, raced, snapshot, ConflictPolicy::Overwrite);
      t.write(bytes("replacement"));
      bool injected = false;
      bool rejected = false;
      try {
        (void)t.commit({}, [&](const OutputIntent &) {
          require(CreateHardLinkW(raced.c_str(), (input / L"原图.bin").c_str(), nullptr) != FALSE,
                  "Create late source hardlink");
          injected = true;
        });
      } catch (const skeletal::Failure &e) { rejected = e.error.code == ErrorCode::Conflict; }
      require(injected && rejected && read(raced) == "source" && read(input / L"原图.bin") == "source",
              "Late hardlink must not be replaced");
    }
    {
      OutputTransaction t(scope, output / "existing.bin", snapshot,
                          ConflictPolicy::Overwrite);
      t.write(bytes("replacement"));
      auto result = t.commit();
      require(read(result.path) == "replacement", "Overwrite failed");
    }
    std::stop_source stop;
    stop.request_stop();
    {
      OutputTransaction t(scope, output / "cancel.bin", snapshot,
                          ConflictPolicy::Skip);
      rejects([&] { t.write(bytes("cancel"), stop.get_token()); },
              ErrorCode::Cancelled);
    }
    {
      OutputTransaction t(scope, output / "cancel-commit.bin", snapshot,
                          ConflictPolicy::Skip);
      t.write(bytes("cancel"));
      rejects([&] { (void)t.commit(stop.get_token()); }, ErrorCode::Cancelled);
    }
    require(!std::filesystem::exists(output / "cancel.bin") &&
                !std::filesystem::exists(output / "cancel-commit.bin"),
            "Cancelled output published");
  }
  write(input / L"原图.bin", "changed");
  auto changed = InputLease::open(scope, input / L"原图.bin", 1024);
  rejects([&] { changed.require_unchanged(snapshot); },
          ErrorCode::InputChanged);
  require(read(outside / "escape.bin") == "outside",
          "Out of scope file modified");
  for (const auto &file : std::filesystem::directory_iterator(output))
    require(!file.path().filename().wstring().starts_with(L".formaview-"),
            "Temporary file leaked");
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--short-path") {
      const auto root = std::filesystem::absolute(argv[2]);
      require(std::filesystem::create_directories(root), "Create fresh short-path fixture");
      write(root / "source.bin", "short-path-source");
      std::wstring shortName(32768, L'\0'), longName(32768, L'\0');
      const auto shortSize = GetShortPathNameW(root.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
      const auto longSize = GetLongPathNameW(root.c_str(), longName.data(), static_cast<DWORD>(longName.size()));
      require(shortSize && shortSize < shortName.size() && longSize && longSize < longName.size(), "Resolve path aliases");
      shortName.resize(shortSize); longName.resize(longSize);
      if (shortName == longName) { std::cout << "SKIP volume has no short alias\n"; return 77; }
      const std::filesystem::path alias(shortName);
      AccessScope policy{{alias}, {alias}, false, "short-path"};
      const auto input = InputLease::open(policy, alias / "source.bin", 1024);
      OutputTransaction output(policy, alias / "result.bin", input.snapshot(), ConflictPolicy::Skip);
      output.write(bytes("alias-output"));
      require(!output.commit().skipped && read(root / "result.bin") == "alias-output", "Short alias output");
      std::cout << "PASS authorized 8.3 alias input and output\n"; return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--replace-parent") {
      const std::filesystem::path root(argv[2]);
      if (!MoveFileExW((root / "input" / "sub").c_str(), (root / "input" / "renamed").c_str(), 0))
        return static_cast<int>(GetLastError());
      junction(root / "input" / "sub", root / "outside"); return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--open-parent-delete") {
      const auto handle = CreateFileW(argv[2], DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
      CloseHandle(handle); return 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--junction-probe") { junctionProbe(std::filesystem::absolute(argv[2])); return 0; }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--parent-lock") { parentLock(std::filesystem::absolute(argv[2])); return 0; }
    if (argc != 2)
      throw std::runtime_error("EXTERNAL_DIRECTORY required");
    run(std::filesystem::absolute(argv[1]));
    std::cout << "PASS scope, junctions, hardlinks, held input, snapshot "
                 "change, atomic conflict modes and cancellation\n";
    return 0;
  } catch (const skeletal::Failure &e) {
    std::cerr << e.what();
    for (const auto &[k, v] : e.error.details)
      std::cerr << ' ' << k << '=' << v;
    std::cerr << '\n';
    return 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
