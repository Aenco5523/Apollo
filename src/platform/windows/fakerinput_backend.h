/**
 * @file src/platform/windows/fakerinput_backend.h
 * @brief User-mode transport for an already-installed FakerInput virtual HID device.
 *
 * This backend does not install, remove, stage, sign, or modify any driver.
 * It only discovers the public FakerInput HID collections and submits output
 * reports using normal Windows HID APIs.
 */
#pragma once

#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace platf {
  class fakerinput_backend_t {
  public:
    fakerinput_backend_t() = default;
    fakerinput_backend_t(const fakerinput_backend_t &) = delete;
    fakerinput_backend_t &operator=(const fakerinput_backend_t &) = delete;

    ~fakerinput_backend_t() {
      close();
    }

    bool open() {
      if (is_open()) {
        return true;
      }

      close();

      control_ = find_endpoint(usage_page_vendor_, usage_control_);
      if (control_ == INVALID_HANDLE_VALUE) {
        return false;
      }

      method_ = find_endpoint(usage_page_vendor_, usage_method_);
      if (method_ == INVALID_HANDLE_VALUE) {
        close();
        return false;
      }

      // FakerInput API-version handshake. The reference client writes a
      // 65-byte output report with Report ID 0x41 and UINT32 API version at
      // offset 4 (natural alignment of the original structure).
      std::array<std::uint8_t, control_report_size_> report {};
      report[0] = report_check_api_version_;
      const std::uint32_t api_version = 1;
      std::memcpy(report.data() + 4, &api_version, sizeof(api_version));

      if (!write_report(method_, report)) {
        close();
        return false;
      }

      return true;
    }

    void close() {
      if (control_ != INVALID_HANDLE_VALUE) {
        CloseHandle(control_);
        control_ = INVALID_HANDLE_VALUE;
      }
      if (method_ != INVALID_HANDLE_VALUE) {
        CloseHandle(method_);
        method_ = INVALID_HANDLE_VALUE;
      }
    }

    bool is_open() const {
      return control_ != INVALID_HANDLE_VALUE && method_ != INVALID_HANDLE_VALUE;
    }

    bool keyboard(std::uint8_t modifiers, const std::uint8_t keys[6]) {
      if (!is_open()) {
        return false;
      }

      std::array<std::uint8_t, control_report_size_> report {};
      report[0] = report_control_;
      report[1] = 9;  // FakerInputKeyboardReport size
      report[2] = report_keyboard_;
      report[3] = modifiers;
      report[4] = 0;  // reserved
      std::memcpy(report.data() + 5, keys, 6);
      return write_report(control_, report);
    }

    bool relative_mouse(std::uint8_t buttons,
                        std::int16_t x,
                        std::int16_t y,
                        std::int8_t wheel,
                        std::int8_t horizontal_wheel) {
      if (!is_open()) {
        return false;
      }

      std::array<std::uint8_t, control_report_size_> report {};
      report[0] = report_control_;
      report[1] = 8;  // FakerInputRelativeMouseReport size
      report[2] = report_relative_mouse_;
      report[3] = buttons;
      std::memcpy(report.data() + 4, &x, sizeof(x));
      std::memcpy(report.data() + 6, &y, sizeof(y));
      report[8] = static_cast<std::uint8_t>(wheel);
      report[9] = static_cast<std::uint8_t>(horizontal_wheel);
      return write_report(control_, report);
    }

  private:
    static constexpr USHORT vendor_id_ = 0xFE0F;
    static constexpr USHORT product_id_ = 0x00FF;
    static constexpr USAGE usage_page_vendor_ = 0xFF00;
    static constexpr USAGE usage_control_ = 0x0001;
    static constexpr USAGE usage_method_ = 0x0002;

    static constexpr std::size_t control_report_size_ = 0x41;
    static constexpr std::uint8_t report_keyboard_ = 0x01;
    static constexpr std::uint8_t report_relative_mouse_ = 0x03;
    static constexpr std::uint8_t report_control_ = 0x40;
    static constexpr std::uint8_t report_check_api_version_ = 0x41;

    static bool matches_device(HANDLE file, USAGE usage_page, USAGE usage) {
      HIDD_ATTRIBUTES attributes {};
      attributes.Size = sizeof(attributes);
      if (!HidD_GetAttributes(file, &attributes)) {
        return false;
      }

      if (attributes.VendorID != vendor_id_ || attributes.ProductID != product_id_) {
        return false;
      }

      PHIDP_PREPARSED_DATA preparsed = nullptr;
      if (!HidD_GetPreparsedData(file, &preparsed)) {
        return false;
      }

      HIDP_CAPS caps {};
      const auto status = HidP_GetCaps(preparsed, &caps);
      HidD_FreePreparsedData(preparsed);

      return status == HIDP_STATUS_SUCCESS &&
             caps.UsagePage == usage_page &&
             caps.Usage == usage;
    }

    static HANDLE open_interface(HDEVINFO info,
                                 SP_DEVICE_INTERFACE_DATA &interface_data,
                                 USAGE usage_page,
                                 USAGE usage) {
      DWORD required = 0;
      SetupDiGetDeviceInterfaceDetailW(
        info,
        &interface_data,
        nullptr,
        0,
        &required,
        nullptr
      );

      if (required == 0) {
        return INVALID_HANDLE_VALUE;
      }

      auto *detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(std::malloc(required));
      if (!detail) {
        return INVALID_HANDLE_VALUE;
      }

      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (!SetupDiGetDeviceInterfaceDetailW(
            info,
            &interface_data,
            detail,
            required,
            &required,
            nullptr
          )) {
        std::free(detail);
        return INVALID_HANDLE_VALUE;
      }

      HANDLE file = CreateFileW(
        detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      std::free(detail);

      if (file == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
      }

      if (!matches_device(file, usage_page, usage)) {
        CloseHandle(file);
        return INVALID_HANDLE_VALUE;
      }

      return file;
    }

    static HANDLE find_endpoint(USAGE usage_page, USAGE usage) {
      GUID hid_guid {};
      HidD_GetHidGuid(&hid_guid);

      HDEVINFO info = SetupDiGetClassDevsW(
        &hid_guid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
      );
      if (info == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
      }

      HANDLE result = INVALID_HANDLE_VALUE;
      for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data {};
        interface_data.cbSize = sizeof(interface_data);

        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, index, &interface_data)) {
          break;
        }

        result = open_interface(info, interface_data, usage_page, usage);
        if (result != INVALID_HANDLE_VALUE) {
          break;
        }
      }

      SetupDiDestroyDeviceInfoList(info);
      return result;
    }

    template<std::size_t N>
    static bool write_report(HANDLE file, const std::array<std::uint8_t, N> &report) {
      DWORD written = 0;
      return WriteFile(
               file,
               report.data(),
               static_cast<DWORD>(report.size()),
               &written,
               nullptr
             ) != FALSE && written == report.size();
    }

    HANDLE control_ {INVALID_HANDLE_VALUE};
    HANDLE method_ {INVALID_HANDLE_VALUE};
  };
}  // namespace platf
