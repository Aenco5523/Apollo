#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kClassName[] = L"ApolloRawInputProbeWindow";
constexpr wchar_t kNeedle[] = L"VID_2E8A&PID_0011";
HWND g_window = nullptr;
std::vector<HANDLE> g_targets;
std::uint64_t g_event_count = 0;

std::wstring device_name(HANDLE device) {
  UINT chars = 0;
  if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &chars) == static_cast<UINT>(-1) || chars == 0) {
    return {};
  }
  std::wstring name(chars, L'\0');
  if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &chars) == static_cast<UINT>(-1)) {
    return {};
  }
  if (!name.empty() && name.back() == L'\0') name.pop_back();
  return name;
}

bool is_target(HANDLE device) {
  return std::find(g_targets.begin(), g_targets.end(), device) != g_targets.end();
}

void enumerate_targets() {
  g_targets.clear();
  UINT count = 0;
  if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
    std::wcerr << L"GetRawInputDeviceList(count) failed: " << GetLastError() << L"\n";
    return;
  }

  std::vector<RAWINPUTDEVICELIST> devices(count);
  if (count && GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
    std::wcerr << L"GetRawInputDeviceList(data) failed: " << GetLastError() << L"\n";
    return;
  }

  std::wcout << L"Raw Input mice containing " << kNeedle << L":\n";
  for (const auto &entry : devices) {
    if (entry.dwType != RIM_TYPEMOUSE) continue;
    auto name = device_name(entry.hDevice);
    if (name.find(kNeedle) == std::wstring::npos) continue;

    g_targets.push_back(entry.hDevice);
    std::wcout << L"  [FOUND] " << name << L"\n";

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT info_size = sizeof(info);
    if (GetRawInputDeviceInfoW(entry.hDevice, RIDI_DEVICEINFO, &info, &info_size) != static_cast<UINT>(-1) && info.dwType == RIM_TYPEMOUSE) {
      std::wcout << L"          buttons=" << info.mouse.dwNumberOfButtons
                 << L" sampleRate=" << info.mouse.dwSampleRate
                 << L" hasHWheel=" << info.mouse.fHasHorizontalWheel << L"\n";
    }
  }

  if (g_targets.empty()) {
    std::wcout << L"  [NOT FOUND] VIIPER mouse is not currently exposed to Raw Input.\n";
  }
}

void print_mouse(const RAWINPUT &raw) {
  if (!is_target(raw.header.hDevice) || raw.header.dwType != RIM_TYPEMOUSE) return;

  const auto &m = raw.data.mouse;
  const bool absolute = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
  ++g_event_count;

  std::wcout << L"[" << std::setw(8) << g_event_count << L"] "
             << (absolute ? L"ABS" : L"REL")
             << L" dx=" << m.lLastX
             << L" dy=" << m.lLastY
             << L" btnFlags=0x" << std::hex << std::setw(4) << std::setfill(L'0') << m.usButtonFlags
             << L" btnData=0x" << std::setw(4) << m.usButtonData
             << std::dec << std::setfill(L' ') << L"\n";
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_INPUT: {
      UINT size = 0;
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return 0;
      }
      std::vector<std::uint8_t> buffer(size);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) == size) {
        print_mouse(*reinterpret_cast<const RAWINPUT *>(buffer.data()));
      }
      return 0;
    }
    case WM_INPUT_DEVICE_CHANGE:
      enumerate_targets();
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

BOOL WINAPI console_handler(DWORD type) {
  if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) && g_window) {
    PostMessageW(g_window, WM_CLOSE, 0, 0);
    return TRUE;
  }
  return FALSE;
}
}  // namespace

int wmain() {
  SetConsoleOutputCP(CP_UTF8);
  std::wcout << L"Apollo/VIIPER Raw Input Probe\n"
             << L"Target: HID mouse VID_2E8A PID_0011\n"
             << L"This tool only observes WM_INPUT; it does not inject or modify input.\n\n";

  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    std::wcerr << L"RegisterClassW failed: " << GetLastError() << L"\n";
    return 1;
  }

  g_window = CreateWindowExW(0, kClassName, L"Apollo Raw Input Probe", 0,
                             0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  if (!g_window) {
    std::wcerr << L"CreateWindowExW failed: " << GetLastError() << L"\n";
    return 1;
  }

  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01; // Generic Desktop
  rid.usUsage = 0x02;     // Mouse
  rid.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
  rid.hwndTarget = g_window;
  if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
    std::wcerr << L"RegisterRawInputDevices failed: " << GetLastError() << L"\n";
    return 1;
  }

  SetConsoleCtrlHandler(console_handler, TRUE);
  enumerate_targets();

  std::wcout << L"\nMove the Artemis/VIIPER mouse now.\n"
             << L"Then launch the game and keep this window open.\n"
             << L"If REL dx/dy lines continue while the game is focused, Windows is still delivering VIIPER Raw Input.\n"
             << L"If they stop exactly when the game opens, note that behavior.\n"
             << L"Press Ctrl+C to exit.\n\n";

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  return 0;
}
