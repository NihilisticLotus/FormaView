#include "formaview/file_access.hpp"
#include "formaview/skeletal.hpp"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <windows.h>
#include <psapi.h>
#include <winioctl.h>

using namespace formaview;
namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F f, ErrorCode code) {
  try { f(); } catch (const skeletal::Failure &e) {
    require(e.error.code == code, "Unexpected failure code"); return;
  }
  throw std::runtime_error("Expected structured rejection");
}
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void sparse(const std::filesystem::path &path, std::uint64_t size) {
  Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, nullptr)};
  require(file.value != INVALID_HANDLE_VALUE, "Create isolated sparse fixture");
  DWORD returned{};
  require(DeviceIoControl(file.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                          &returned, nullptr) != FALSE, "Mark sparse fixture");
  LARGE_INTEGER end{}; end.QuadPart = static_cast<LONGLONG>(size);
  require(SetFilePointerEx(file.value, end, nullptr, FILE_BEGIN) && SetEndOfFile(file.value),
          "Set sparse logical size");
}
PROCESS_MEMORY_COUNTERS_EX memory() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  require(K32GetProcessMemoryInfo(GetCurrentProcess(),
      reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)) != FALSE,
      "Read process memory counters");
  return counters;
}
IO_COUNTERS io() {
  IO_COUNTERS counters{};
  require(GetProcessIoCounters(GetCurrentProcess(), &counters) != FALSE, "Read IO counters");
  return counters;
}
void run(const std::filesystem::path &root) {
  constexpr std::uint64_t mib = 1024 * 1024, largeSize = 8 * 1024 * mib;
  std::filesystem::create_directories(root);
  const auto large = root / "sparse-8g.bin", hashed = root / "sparse-256m.bin";
  sparse(large, largeSize); sparse(hashed, 256 * mib);
  const AccessScope scope{{root}, {root}, true, "bounded-lease-test"};
  const auto alias = root / "alias.bin";
  require(CreateHardLinkW(alias.c_str(), large.c_str(), nullptr) != FALSE, "Create fixture alias");
  const auto baseline = memory();
  const auto readBefore = io().ReadTransferCount;
  {
    const auto pin = InputIdentityLease::open(scope, large);
    require(pin.snapshot().size_bytes == largeSize && pin.snapshot().content_sha256.empty(),
            "Identity lease must not claim a content digest");
    const auto readBytes = io().ReadTransferCount - readBefore;
    require(readBytes < mib, "Identity pin must not scan file content");
    Handle writer{CreateFileW(alias.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
                              FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
    require(writer.value == INVALID_HANDLE_VALUE, "Pinned source blocks hardlink writes");
    require(!DeleteFileW(large.c_str()) && !MoveFileW(large.c_str(), (root / "moved.bin").c_str()),
            "Pinned source blocks delete and rename");
    rejects([&] { (void)inspect_output(scope, alias, pin.snapshot(), ConflictPolicy::Overwrite); },
            ErrorCode::Conflict);
    rejects([&] { (void)InputLease::open(scope, large, 8 * mib); }, ErrorCode::ResourceLimit);
    std::stop_source cancelled; cancelled.request_stop();
    rejects([&] { (void)InputIdentityLease::open(scope, large, cancelled.get_token()); }, ErrorCode::Cancelled);
    rejects([&] { (void)InputLease::open(scope, large, largeSize, cancelled.get_token()); }, ErrorCode::Cancelled);
    std::cout << "PASS 8GiB identity pin, budget, pre-cancel, hardlink/write/delete/rename protection; read_bytes=" << readBytes << '\n';
  }
  {
    const auto lease = InputLease::open(scope, hashed, 256 * mib);
    require(lease.snapshot().content_sha256 == "a6d72ac7690f53be6ae46ba88506bd97302a093f7108472bd9efc3cefda06484",
            "256MiB streamed SHA256 must match independent hashlib result");
  }
  // Include SHA padding boundaries, exact block alignment, and a partial read
  // after a full IO chunk, using the existing portable SHA implementation.
  for (const auto size : {0u, 1u, 55u, 56u, 63u, 64u, 65u, 1048593u}) {
    const auto path = root / ("hash-" + std::to_string(size));
    std::vector<std::byte> data(size);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>(i % 251);
    { std::ofstream file(path, std::ios::binary);
      if (!data.empty()) file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
      require(static_cast<bool>(file), "Write hash fixture"); }
    const auto lease = InputLease::open(scope, path, size);
    require(lease.snapshot().content_sha256 == skeletal::sha256(data), "Streamed SHA compatibility");
  }
  const auto readStart = io().ReadTransferCount;
  std::promise<ErrorCode> completion; auto result = completion.get_future();
  std::jthread worker([&](std::stop_token stop) {
    try { (void)InputLease::open(scope, large, largeSize, stop); completion.set_value(ErrorCode::Internal); }
    catch (const skeletal::Failure &e) { completion.set_value(e.error.code); }
    catch (...) { completion.set_exception(std::current_exception()); }
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (io().ReadTransferCount == readStart && result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto sawRead = io().ReadTransferCount > readStart;
  const auto cancelStart = std::chrono::steady_clock::now();
  worker.request_stop(); worker.join();
  const auto cancelMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cancelStart).count();
  require(sawRead && result.get() == ErrorCode::Cancelled, "Cancel while streaming, after a real read");
  require(cancelMs < 2000, "Streaming cancellation must be prompt");
  const auto peak = memory();
  const auto peakDelta = peak.PeakPagefileUsage - baseline.PeakPagefileUsage;
  require(peakDelta < 64 * mib, "Hashing and identity pins must not allocate whole files");
  // All leases are gone: the source must be available for write again.
  Handle writer{CreateFileW(large.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
  require(writer.value != INVALID_HANDLE_VALUE, "Cancelled hashing releases the source");
  // A captured unavailable dependency must not silently enter the graph later.
  const auto late = root / "late.bin";
  auto frozen = scope; frozen.unavailable_inputs.push_back(late);
  { std::ofstream file(late, std::ios::binary); file << "later"; }
  rejects([&] { (void)InputLease::open(frozen, late, mib); }, ErrorCode::Io);
  require(InputLease::open(scope, late, mib).snapshot().size_bytes == 5, "Only captured scope excludes the late file");
  auto deep = root;
  for (int i = 0; i != 8; ++i) deep /= "nested";
  std::filesystem::create_directories(deep);
  std::vector<std::filesystem::path> paths;
  for (int i = 0; i != 128; ++i) {
    const auto path = deep / (std::to_string(i) + ".bin");
    { std::ofstream file(path, std::ios::binary); file << 'x'; } paths.push_back(path);
  }
  DWORD beforeHandles{}, heldHandles{}, afterHandles{};
  require(GetProcessHandleCount(GetCurrentProcess(), &beforeHandles), "Read baseline handle count");
  {
    std::vector<InputIdentityLease> held;
    for (const auto &path : paths) held.push_back(InputIdentityLease::open(scope, path));
    require(GetProcessHandleCount(GetCurrentProcess(), &heldHandles), "Read pinned handle count");
  }
  require(GetProcessHandleCount(GetCurrentProcess(), &afterHandles), "Read released handle count");
  require(heldHandles >= beforeHandles + 128 && afterHandles == beforeHandles, "Deep-path leases release all handles");
  std::cout << "PASS captured missing dependency; handle sample unique_sources=128 extra_depth=8 held_delta="
            << heldHandles - beforeHandles << " released_delta=" << afterHandles - beforeHandles << '\n';
  std::cout << "PASS streamed SHA256 and cancellation; peak_commit_delta=" << peakDelta
            << " peak_working_set=" << peak.PeakWorkingSetSize << " cancel_ms=" << cancelMs << '\n';
}
}
int wmain(int argc, wchar_t **argv) {
  try {
    require(argc == 2, "External isolated fixture directory required");
    run(std::filesystem::absolute(argv[1])); return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
