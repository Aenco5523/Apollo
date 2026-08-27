/**
 * @file tools/sunshinesvc.cpp
 * @brief Handles launching Sunshine.exe and bundled VIIPER into user sessions as SYSTEM
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <Windows.h>
#include <WtsApi32.h>

#include <format>
#include <string>

// PROC_THREAD_ATTRIBUTE_JOB_LIST is currently missing from MinGW headers
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

SERVICE_STATUS_HANDLE service_status_handle;
SERVICE_STATUS service_status;
HANDLE stop_event;
HANDLE session_change_event;

#define SERVICE_NAME "ApolloService"

DWORD WINAPI HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
  switch (dwControl) {
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      // If a new session connects to the console, restart Apollo and VIIPER
      // to allow both to spawn inside the new console session.
      if (dwEventType == WTS_CONSOLE_CONNECT) {
        SetEvent(session_change_event);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_PRESHUTDOWN:
      // The system is shutting down
    case SERVICE_CONTROL_STOP:
      // Let SCM know we're stopping in up to 30 seconds
      service_status.dwCurrentState = SERVICE_STOP_PENDING;
      service_status.dwControlsAccepted = 0;
      service_status.dwWaitHint = 30 * 1000;
      SetServiceStatus(service_status_handle, &service_status);

      // Trigger ServiceMain() to start cleanup
      SetEvent(stop_event);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

HANDLE CreateJobObjectForChildProcess() {
  HANDLE job_handle = CreateJobObjectW(nullptr, nullptr);
  if (!job_handle) {
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit_info = {};

  // Kill Apollo/VIIPER when the final job object handle is closed.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  // Allow Apollo to use CREATE_BREAKAWAY_FROM_JOB when spawning streamed applications.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;

  if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &job_limit_info, sizeof(job_limit_info))) {
    CloseHandle(job_handle);
    return nullptr;
  }

  return job_handle;
}

LPPROC_THREAD_ATTRIBUTE_LIST AllocateProcThreadAttributeList(DWORD attribute_count) {
  SIZE_T size;
  InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &size);

  auto list = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
  if (list == nullptr) {
    return nullptr;
  }

  if (!InitializeProcThreadAttributeList(list, attribute_count, 0, &size)) {
    HeapFree(GetProcessHeap(), 0, list);
    return nullptr;
  }

  return list;
}

HANDLE DuplicateTokenForSession(DWORD console_session_id) {
  HANDLE current_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current_token)) {
    return nullptr;
  }

  // Duplicate our own LocalSystem token
  HANDLE new_token;
  if (!DuplicateTokenEx(current_token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &new_token)) {
    CloseHandle(current_token);
    return nullptr;
  }

  CloseHandle(current_token);

  // Change the duplicated token to the console session ID
  if (!SetTokenInformation(new_token, TokenSessionId, &console_session_id, sizeof(console_session_id))) {
    CloseHandle(new_token);
    return nullptr;
  }

  return new_token;
}

HANDLE OpenLogFileHandle() {
  WCHAR log_file_name[MAX_PATH];

  // Create sunshine.log in the Temp folder (usually %SYSTEMROOT%\Temp)
  GetTempPathW(_countof(log_file_name), log_file_name);
  wcscat_s(log_file_name, L"sunshine.log");

  // The file handle must be inheritable for our child process to use it
  SECURITY_ATTRIBUTES security_attributes = {sizeof(security_attributes), nullptr, TRUE};

  // Overwrite the old sunshine.log
  return CreateFileW(log_file_name, GENERIC_WRITE, FILE_SHARE_READ, &security_attributes, CREATE_ALWAYS, 0, nullptr);
}

std::wstring InstalledPath(const wchar_t *relative_path) {
  wchar_t current_dir[32768];
  const DWORD length = GetCurrentDirectoryW(_countof(current_dir), current_dir);
  if (length == 0 || length >= _countof(current_dir)) {
    return {};
  }

  std::wstring result(current_dir, length);
  if (!result.empty() && result.back() != L'\\') {
    result += L'\\';
  }
  result += relative_path;
  return result;
}

bool WaitForViiperApi() {
  WSADATA data = {};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return false;
  }

  bool ready = false;
  for (int attempt = 0; attempt < 50 && !ready; ++attempt) {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle != INVALID_SOCKET) {
      sockaddr_in address = {};
      address.sin_family = AF_INET;
      address.sin_port = htons(3242);
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

      ready = connect(socket_handle, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
      closesocket(socket_handle);
    }

    if (!ready) {
      Sleep(100);
    }
  }

  WSACleanup();
  return ready;
}

bool LaunchSessionProcess(HANDLE console_token,
                          STARTUPINFOEXW &startup_info,
                          const std::wstring &application,
                          std::wstring command_line,
                          PROCESS_INFORMATION &process_info) {
  LPWSTR mutable_command = command_line.empty() ? nullptr : command_line.data();
  return CreateProcessAsUserW(
           console_token,
           application.c_str(),
           mutable_command,
           nullptr,
           nullptr,
           TRUE,
           CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
           nullptr,
           nullptr,
           reinterpret_cast<LPSTARTUPINFOW>(&startup_info),
           &process_info
         ) != FALSE;
}

bool RunTerminationHelper(HANDLE console_token, DWORD pid) {
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(nullptr, module_path, _countof(module_path));
  std::wstring command;

  command += L'"';
  command += module_path;
  command += L'"';
  command += std::format(L" --terminate {}", pid);

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  // Execute ourselves as a detached process in the user session with the --terminate argument.
  // This will allow us to attach to Apollo's console and send it a Ctrl-C event.
  PROCESS_INFORMATION process_info;
  if (!CreateProcessAsUserW(console_token, module_path, (LPWSTR) command.c_str(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS, nullptr, nullptr, &startup_info, &process_info)) {
    return false;
  }

  // Wait for the termination helper to complete
  WaitForSingleObject(process_info.hProcess, INFINITE);

  // Check the exit status of the helper process
  DWORD exit_code;
  GetExitCodeProcess(process_info.hProcess, &exit_code);

  // Cleanup handles
  CloseHandle(process_info.hProcess);
  CloseHandle(process_info.hThread);

  // If the helper process returned 0, it succeeded
  return exit_code == 0;
}

VOID WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv) {
  service_status_handle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, HandlerEx, nullptr);
  if (service_status_handle == nullptr) {
    ExitProcess(GetLastError());
    return;
  }

  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwServiceSpecificExitCode = 0;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwWaitHint = 0;
  service_status.dwControlsAccepted = 0;
  service_status.dwCheckPoint = 0;
  service_status.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(service_status_handle, &service_status);

  stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (stop_event == nullptr) {
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  session_change_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (session_change_event == nullptr) {
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  auto log_file_handle = OpenLogFileHandle();
  if (log_file_handle == INVALID_HANDLE_VALUE) {
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  STARTUPINFOEXW startup_info = {};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  startup_info.StartupInfo.lpDesktop = (LPWSTR) L"winsta0\\default";
  startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_info.StartupInfo.hStdInput = nullptr;
  startup_info.StartupInfo.hStdOutput = log_file_handle;
  startup_info.StartupInfo.hStdError = log_file_handle;

  startup_info.lpAttributeList = AllocateProcThreadAttributeList(2);
  if (startup_info.lpAttributeList == nullptr) {
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log_file_handle, sizeof(log_file_handle), nullptr, nullptr);

  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
  service_status.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(service_status_handle, &service_status);

  const auto viiper_path = InstalledPath(L"viiper\\viiper.exe");
  const auto apollo_path = InstalledPath(L"sunshine.exe");

  // Loop every 3 seconds until the stop event is set or Apollo is running.
  while (WaitForSingleObject(stop_event, 3000) != WAIT_OBJECT_0) {
    auto console_session_id = WTSGetActiveConsoleSessionId();
    if (console_session_id == 0xFFFFFFFF) {
      continue;
    }

    auto console_token = DuplicateTokenForSession(console_session_id);
    if (console_token == nullptr) {
      continue;
    }

    auto job_handle = CreateJobObjectForChildProcess();
    if (job_handle == nullptr) {
      CloseHandle(console_token);
      continue;
    }

    UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_handle, sizeof(job_handle), nullptr, nullptr);

    // VIIPER must be ready before Apollo initializes its virtual HID backend.
    PROCESS_INFORMATION viiper_process_info = {};
    std::wstring viiper_command = L"\"" + viiper_path + L"\" server";
    if (!LaunchSessionProcess(console_token, startup_info, viiper_path, viiper_command, viiper_process_info)) {
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }

    if (!WaitForViiperApi()) {
      TerminateProcess(viiper_process_info.hProcess, ERROR_PROCESS_ABORTED);
      CloseHandle(viiper_process_info.hThread);
      CloseHandle(viiper_process_info.hProcess);
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }

    PROCESS_INFORMATION process_info = {};
    if (!LaunchSessionProcess(console_token, startup_info, apollo_path, L"", process_info)) {
      TerminateProcess(viiper_process_info.hProcess, ERROR_PROCESS_ABORTED);
      CloseHandle(viiper_process_info.hThread);
      CloseHandle(viiper_process_info.hProcess);
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }

    bool still_running;
    do {
      // Wait for stop, Apollo exit, console session change, or VIIPER exit.
      const HANDLE wait_objects[] = {stop_event, process_info.hProcess, session_change_event, viiper_process_info.hProcess};
      switch (WaitForMultipleObjects(_countof(wait_objects), wait_objects, FALSE, INFINITE)) {
        case WAIT_OBJECT_0 + 2:
          if (WTSGetActiveConsoleSessionId() == console_session_id) {
            still_running = true;
            continue;
          }
          // Fall-through when the active console session actually changed.
        case WAIT_OBJECT_0:
          if (!RunTerminationHelper(console_token, process_info.dwProcessId) ||
              WaitForSingleObject(process_info.hProcess, 20000) != WAIT_OBJECT_0) {
            TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
          }
          still_running = false;
          break;

        case WAIT_OBJECT_0 + 1:
          {
            DWORD exit_code;
            if (GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == ERROR_SHUTDOWN_IN_PROGRESS) {
              SetEvent(stop_event);
            }
            still_running = false;
            break;
          }

        case WAIT_OBJECT_0 + 3:
          // VIIPER exited unexpectedly. Restart both processes so Apollo can reopen the HID backend cleanly.
          if (!RunTerminationHelper(console_token, process_info.dwProcessId) ||
              WaitForSingleObject(process_info.hProcess, 5000) != WAIT_OBJECT_0) {
            TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
          }
          still_running = false;
          break;

        default:
          still_running = false;
          break;
      }
    } while (still_running);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(viiper_process_info.hThread);
    CloseHandle(viiper_process_info.hProcess);
    CloseHandle(console_token);

    // Closing the job kills any remaining process in the pair, including VIIPER.
    CloseHandle(job_handle);
  }

  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(service_status_handle, &service_status);
}

// This will run in a child process in the user session
int DoGracefulTermination(DWORD pid) {
  if (!AttachConsole(pid)) {
    return GetLastError();
  }

  SetConsoleCtrlHandler(nullptr, TRUE);

  if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
    return GetLastError();
  }

  return 0;
}

int main(int argc, char *argv[]) {
  static const SERVICE_TABLE_ENTRY service_table[] = {
    {(LPSTR) SERVICE_NAME, ServiceMain},
    {nullptr, nullptr}
  };

  if (argc == 3 && strcmp(argv[1], "--terminate") == 0) {
    return DoGracefulTermination(atol(argv[2]));
  }

  // Services default to %SYSTEMROOT%\System32. Use the directory containing sunshine.exe instead.
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(nullptr, module_path, _countof(module_path));
  for (auto i = 0; i < 2; i++) {
    auto last_sep = wcsrchr(module_path, '\\');
    if (last_sep) {
      *last_sep = 0;
    }
  }
  SetCurrentDirectoryW(module_path);

  return StartServiceCtrlDispatcher(service_table);
}
