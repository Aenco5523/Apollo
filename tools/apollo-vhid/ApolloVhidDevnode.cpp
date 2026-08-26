#define UNICODE
#define _UNICODE

#include <windows.h>
#include <setupapi.h>

#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr GUID kSystemClassGuid = {
  0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}
};
constexpr wchar_t kHardwareId[] = L"Root\\ApolloVhid";
constexpr wchar_t kDeviceName[] = L"Apollo Virtual HID Source";

void print_last_error(const wchar_t* what) {
  const DWORD error = GetLastError();
  wchar_t* message = nullptr;
  FormatMessageW(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr,
    error,
    0,
    reinterpret_cast<wchar_t*>(&message),
    0,
    nullptr
  );
  std::wcerr << what << L" failed (" << error << L")";
  if (message) {
    std::wcerr << L": " << message;
    LocalFree(message);
  } else {
    std::wcerr << L"\n";
  }
}

bool hardware_ids_contain(HDEVINFO devices, SP_DEVINFO_DATA& info, const wchar_t* wanted) {
  DWORD type = 0;
  DWORD required = 0;
  SetupDiGetDeviceRegistryPropertyW(
    devices,
    &info,
    SPDRP_HARDWAREID,
    &type,
    nullptr,
    0,
    &required
  );

  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required < sizeof(wchar_t)) {
    return false;
  }

  std::vector<BYTE> buffer(required + sizeof(wchar_t));
  if (!SetupDiGetDeviceRegistryPropertyW(
        devices,
        &info,
        SPDRP_HARDWAREID,
        &type,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        nullptr
      )) {
    return false;
  }

  if (type != REG_MULTI_SZ && type != REG_SZ) {
    return false;
  }

  const auto* current = reinterpret_cast<const wchar_t*>(buffer.data());
  while (*current) {
    if (_wcsicmp(current, wanted) == 0) {
      return true;
    }
    current += std::wcslen(current) + 1;
  }
  return false;
}

std::wstring instance_id(HDEVINFO devices, SP_DEVINFO_DATA& info) {
  DWORD required = 0;
  SetupDiGetDeviceInstanceIdW(devices, &info, nullptr, 0, &required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
    return {};
  }

  std::wstring value(required, L'\0');
  if (!SetupDiGetDeviceInstanceIdW(devices, &info, value.data(), required, nullptr)) {
    return {};
  }
  if (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

int enumerate_matching(bool remove_devices) {
  HDEVINFO devices = SetupDiGetClassDevsW(&kSystemClassGuid, nullptr, nullptr, 0);
  if (devices == INVALID_HANDLE_VALUE) {
    print_last_error(L"SetupDiGetClassDevsW");
    return 2;
  }

  int found = 0;
  SP_DEVINFO_DATA info {};
  info.cbSize = sizeof(info);

  for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
    if (!hardware_ids_contain(devices, info, kHardwareId)) {
      continue;
    }

    ++found;
    const auto id = instance_id(devices, info);
    std::wcout << (remove_devices ? L"Removing: " : L"Found: ")
               << (id.empty() ? kHardwareId : id.c_str()) << L"\n";

    if (remove_devices) {
      SP_REMOVEDEVICE_PARAMS params {};
      params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
      params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
      params.Scope = DI_REMOVEDEVICE_GLOBAL;
      params.HwProfile = 0;

      if (!SetupDiSetClassInstallParamsW(
            devices,
            &info,
            &params.ClassInstallHeader,
            sizeof(params)
          )) {
        print_last_error(L"SetupDiSetClassInstallParamsW");
        SetupDiDestroyDeviceInfoList(devices);
        return 2;
      }

      if (!SetupDiCallClassInstaller(DIF_REMOVE, devices, &info)) {
        print_last_error(L"DIF_REMOVE");
        SetupDiDestroyDeviceInfoList(devices);
        return 2;
      }
    }

    info.cbSize = sizeof(info);
  }

  const DWORD enum_error = GetLastError();
  SetupDiDestroyDeviceInfoList(devices);
  if (enum_error != ERROR_NO_MORE_ITEMS) {
    SetLastError(enum_error);
    print_last_error(L"SetupDiEnumDeviceInfo");
    return 2;
  }

  return found > 0 ? 0 : 1;
}

int create_device() {
  const int existing = enumerate_matching(false);
  if (existing == 0) {
    std::wcout << L"Apollo V-HID root device already exists.\n";
    return 0;
  }
  if (existing != 1) {
    return existing;
  }

  HDEVINFO devices = SetupDiCreateDeviceInfoList(&kSystemClassGuid, nullptr);
  if (devices == INVALID_HANDLE_VALUE) {
    print_last_error(L"SetupDiCreateDeviceInfoList");
    return 2;
  }

  SP_DEVINFO_DATA info {};
  info.cbSize = sizeof(info);
  if (!SetupDiCreateDeviceInfoW(
        devices,
        kDeviceName,
        &kSystemClassGuid,
        kDeviceName,
        nullptr,
        DICD_GENERATE_ID,
        &info
      )) {
    print_last_error(L"SetupDiCreateDeviceInfoW");
    SetupDiDestroyDeviceInfoList(devices);
    return 2;
  }

  const wchar_t hardware_ids[] = L"Root\\ApolloVhid\0";
  if (!SetupDiSetDeviceRegistryPropertyW(
        devices,
        &info,
        SPDRP_HARDWAREID,
        reinterpret_cast<const BYTE*>(hardware_ids),
        sizeof(hardware_ids)
      )) {
    print_last_error(L"SPDRP_HARDWAREID");
    SetupDiDestroyDeviceInfoList(devices);
    return 2;
  }

  if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices, &info)) {
    print_last_error(L"DIF_REGISTERDEVICE");
    SetupDiDestroyDeviceInfoList(devices);
    return 2;
  }

  const auto id = instance_id(devices, info);
  SetupDiDestroyDeviceInfoList(devices);
  std::wcout << L"Created: " << (id.empty() ? kHardwareId : id.c_str()) << L"\n";
  return 0;
}

void usage() {
  std::wcout
    << L"ApolloVhidDevnode - minimal Root\\ApolloVhid devnode helper\n\n"
    << L"Usage:\n"
    << L"  ApolloVhidDevnode.exe status   Show matching root device(s)\n"
    << L"  ApolloVhidDevnode.exe create   Create the root device if absent\n"
    << L"  ApolloVhidDevnode.exe remove   Remove matching root device(s)\n\n"
    << L"This helper does not install, sign, or bypass validation of drivers.\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2 || _wcsicmp(argv[1], L"--help") == 0 || _wcsicmp(argv[1], L"-h") == 0) {
    usage();
    return argc == 2 ? 0 : 64;
  }

  if (_wcsicmp(argv[1], L"status") == 0) {
    return enumerate_matching(false);
  }
  if (_wcsicmp(argv[1], L"create") == 0) {
    return create_device();
  }
  if (_wcsicmp(argv[1], L"remove") == 0) {
    const int result = enumerate_matching(true);
    if (result == 1) {
      std::wcout << L"No Apollo V-HID root device found.\n";
      return 0;
    }
    return result;
  }

  usage();
  return 64;
}
