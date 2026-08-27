#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\ApolloVIIPERBackgroundLauncher";
constexpr wchar_t kViiperRelative[] = L"VIIPER\\viiper.exe";
constexpr wchar_t kApolloRelative[] = L"Apollo\\sunshine.exe";

std::wstring module_directory() {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  std::wstring path(buffer.data(), length);
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  path.resize(slash);
  return path;
}

std::wstring join_path(const std::wstring &base, const wchar_t *relative) {
  if (base.empty()) return {};
  return base + L"\\" + relative;
}

bool file_exists(const std::wstring &path) {
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring normalize_path(const std::wstring &path) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  if (length == 0 || length >= buffer.size()) return path;
  std::wstring result(buffer.data(), length);
  std::transform(result.begin(), result.end(), result.begin(), towlower);
  return result;
}

bool process_matches_path(DWORD pid, const std::wstring &expected_normalized) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;

  std::vector<wchar_t> buffer(32768);
  DWORD chars = static_cast<DWORD>(buffer.size());
  const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &chars);
  CloseHandle(process);
  if (!ok) return false;

  std::wstring actual(buffer.data(), chars);
  std::transform(actual.begin(), actual.end(), actual.begin(), towlower);
  return actual == expected_normalized;
}

bool process_running(const std::wstring &path) {
  const std::wstring expected = normalize_path(path);
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (process_matches_path(entry.th32ProcessID, expected)) {
        found = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

void stop_processes_for_path(const std::wstring &path) {
  const std::wstring expected = normalize_path(path);
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (!process_matches_path(entry.th32ProcessID, expected)) continue;
      HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
      if (!process) continue;
      TerminateProcess(process, 0);
      WaitForSingleObject(process, 3000);
      CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
}

bool start_hidden(const std::wstring &exe, const std::wstring &arguments, const std::wstring &working_dir) {
  std::wstring command = L"\"" + exe + L"\"";
  if (!arguments.empty()) command += L" " + arguments;

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};

  const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
  const BOOL ok = CreateProcessW(
    exe.c_str(),
    command.data(),
    nullptr,
    nullptr,
    FALSE,
    flags,
    nullptr,
    working_dir.c_str(),
    &startup,
    &process
  );

  if (!ok) return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool viiper_api_ready() {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(3242);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

  const bool ok = connect(s, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
  closesocket(s);
  return ok;
}

bool wait_for_viiper() {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;

  bool ready = false;
  for (int i = 0; i < 50; ++i) {
    if (viiper_api_ready()) {
      ready = true;
      break;
    }
    Sleep(100);
  }

  WSACleanup();
  return ready;
}

int run_background(const std::wstring &root) {
  const std::wstring viiper = join_path(root, kViiperRelative);
  const std::wstring apollo = join_path(root, kApolloRelative);
  const std::wstring apollo_dir = join_path(root, L"Apollo");
  const std::wstring viiper_dir = join_path(root, L"VIIPER");

  if (!file_exists(viiper) || !file_exists(apollo)) return 2;

  HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
  if (!mutex) return 3;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return 0;
  }

  if (!process_running(viiper)) {
    if (!start_hidden(viiper, L"server", viiper_dir)) {
      CloseHandle(mutex);
      return 4;
    }
  }

  if (!wait_for_viiper()) {
    CloseHandle(mutex);
    return 5;
  }

  if (!process_running(apollo)) {
    if (!start_hidden(apollo, L"", apollo_dir)) {
      CloseHandle(mutex);
      return 6;
    }
  }

  CloseHandle(mutex);
  return 0;
}

int stop_background(const std::wstring &root) {
  const std::wstring apollo = join_path(root, kApolloRelative);
  const std::wstring viiper = join_path(root, kViiperRelative);
  stop_processes_for_path(apollo);
  stop_processes_for_path(viiper);
  return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
  const std::wstring root = module_directory();
  if (root.empty()) return 1;

  const std::wstring args = command_line ? command_line : L"";
  if (args.find(L"--stop") != std::wstring::npos) {
    return stop_background(root);
  }

  return run_background(root);
}
