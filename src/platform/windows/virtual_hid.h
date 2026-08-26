/**
 * @file src/platform/windows/virtual_hid.h
 * @brief Optional Apollo keyboard/mouse bridge to the ApolloVhid VHF driver.
 */
#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <iterator>
#include <mutex>

namespace platf {
  namespace apollo_vhid {
    constexpr wchar_t DEVICE_PATH[] = L"\\\\.\\ApolloVhid";

    constexpr DWORD IOCTL_KEYBOARD = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA);
    constexpr DWORD IOCTL_MOUSE = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA);

#pragma pack(push, 1)
    struct keyboard_report_t {
      std::uint8_t modifiers;
      std::uint8_t reserved;
      std::uint8_t keys[6];
    };

    struct mouse_report_t {
      std::uint8_t buttons;
      std::int16_t x;
      std::int16_t y;
      std::int8_t wheel;
      std::int8_t horizontal_wheel;
    };
#pragma pack(pop)
  }  // namespace apollo_vhid

  class virtual_hid_t {
  public:
    virtual_hid_t() = default;
    virtual_hid_t(const virtual_hid_t &) = delete;
    virtual_hid_t &operator=(const virtual_hid_t &) = delete;

    ~virtual_hid_t() {
      close();
    }

    bool open() {
      std::scoped_lock lock(mutex_);
      if (handle_ != INVALID_HANDLE_VALUE) {
        return true;
      }

      handle_ = CreateFileW(
        apollo_vhid::DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );

      return handle_ != INVALID_HANDLE_VALUE;
    }

    void close() {
      std::scoped_lock lock(mutex_);
      close_locked();
    }

    bool keyboard(std::uint16_t modcode, bool release) {
      const auto usage = vk_to_hid_usage(static_cast<std::uint8_t>(modcode & 0xFF));
      if (usage == 0) {
        return false;
      }

      std::scoped_lock lock(mutex_);
      if (handle_ == INVALID_HANDLE_VALUE) {
        return false;
      }

      const bool old_state = pressed_[usage];
      pressed_[usage] = !release;

      apollo_vhid::keyboard_report_t report {};
      std::size_t normal_key_count = 0;

      for (std::size_t i = 0xE0; i <= 0xE7; ++i) {
        if (pressed_[i]) {
          report.modifiers |= static_cast<std::uint8_t>(1u << (i - 0xE0));
        }
      }

      for (std::size_t i = 1; i < 0xE0; ++i) {
        if (!pressed_[i]) {
          continue;
        }

        if (normal_key_count < std::size(report.keys)) {
          report.keys[normal_key_count] = static_cast<std::uint8_t>(i);
        }
        ++normal_key_count;
      }

      if (normal_key_count > std::size(report.keys)) {
        std::fill(std::begin(report.keys), std::end(report.keys), 0x01);
      }

      if (!ioctl_locked(apollo_vhid::IOCTL_KEYBOARD, &report, sizeof(report))) {
        pressed_[usage] = old_state;
        close_locked();
        return false;
      }

      return true;
    }

    bool move_mouse(int delta_x, int delta_y) {
      std::scoped_lock lock(mutex_);
      if (handle_ == INVALID_HANDLE_VALUE) {
        return false;
      }

      while (delta_x != 0 || delta_y != 0) {
        const int x = std::clamp(delta_x,
                                 static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                                 static_cast<int>(std::numeric_limits<std::int16_t>::max()));
        const int y = std::clamp(delta_y,
                                 static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                                 static_cast<int>(std::numeric_limits<std::int16_t>::max()));

        apollo_vhid::mouse_report_t report {};
        report.buttons = buttons_;
        report.x = static_cast<std::int16_t>(x);
        report.y = static_cast<std::int16_t>(y);

        if (!ioctl_locked(apollo_vhid::IOCTL_MOUSE, &report, sizeof(report))) {
          close_locked();
          return false;
        }

        delta_x -= x;
        delta_y -= y;
      }

      return true;
    }

    bool button_mouse(int button, bool release) {
      std::uint8_t bit = 0;
      switch (button) {
        case 1: bit = 0; break;
        case 2: bit = 2; break;
        case 3: bit = 1; break;
        case 4: bit = 3; break;
        case 5: bit = 4; break;
        default: return false;
      }

      std::scoped_lock lock(mutex_);
      if (handle_ == INVALID_HANDLE_VALUE) {
        return false;
      }

      const auto old_buttons = buttons_;
      if (release) {
        buttons_ &= static_cast<std::uint8_t>(~(1u << bit));
      } else {
        buttons_ |= static_cast<std::uint8_t>(1u << bit);
      }

      apollo_vhid::mouse_report_t report {};
      report.buttons = buttons_;

      if (!ioctl_locked(apollo_vhid::IOCTL_MOUSE, &report, sizeof(report))) {
        buttons_ = old_buttons;
        close_locked();
        return false;
      }

      return true;
    }

    bool scroll(int distance, bool horizontal) {
      std::scoped_lock lock(mutex_);
      if (handle_ == INVALID_HANDLE_VALUE) {
        return false;
      }

      auto &accumulator = horizontal ? hscroll_remainder_ : vscroll_remainder_;
      accumulator += distance;
      int ticks = accumulator / WHEEL_DELTA;
      accumulator %= WHEEL_DELTA;

      while (ticks != 0) {
        const int chunk = std::clamp(ticks, -127, 127);

        apollo_vhid::mouse_report_t report {};
        report.buttons = buttons_;
        if (horizontal) {
          report.horizontal_wheel = static_cast<std::int8_t>(chunk);
        } else {
          report.wheel = static_cast<std::int8_t>(chunk);
        }

        if (!ioctl_locked(apollo_vhid::IOCTL_MOUSE, &report, sizeof(report))) {
          close_locked();
          return false;
        }

        ticks -= chunk;
      }

      return true;
    }

  private:
    static std::uint8_t vk_to_hid_usage(std::uint8_t vk) {
      if (vk >= 'A' && vk <= 'Z') {
        return static_cast<std::uint8_t>(0x04 + (vk - 'A'));
      }

      if (vk >= '1' && vk <= '9') {
        return static_cast<std::uint8_t>(0x1E + (vk - '1'));
      }
      if (vk == '0') {
        return 0x27;
      }

      if (vk >= VK_F1 && vk <= VK_F12) {
        return static_cast<std::uint8_t>(0x3A + (vk - VK_F1));
      }
      if (vk >= VK_F13 && vk <= VK_F24) {
        return static_cast<std::uint8_t>(0x68 + (vk - VK_F13));
      }

      if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) {
        return static_cast<std::uint8_t>(0x59 + (vk - VK_NUMPAD1));
      }

      switch (vk) {
        case VK_LCONTROL:
        case VK_CONTROL: return 0xE0;
        case VK_LSHIFT:
        case VK_SHIFT: return 0xE1;
        case VK_LMENU:
        case VK_MENU: return 0xE2;
        case VK_LWIN: return 0xE3;
        case VK_RCONTROL: return 0xE4;
        case VK_RSHIFT: return 0xE5;
        case VK_RMENU: return 0xE6;
        case VK_RWIN: return 0xE7;
        case VK_RETURN: return 0x28;
        case VK_ESCAPE: return 0x29;
        case VK_BACK: return 0x2A;
        case VK_TAB: return 0x2B;
        case VK_SPACE: return 0x2C;
        case VK_OEM_MINUS: return 0x2D;
        case VK_OEM_PLUS: return 0x2E;
        case VK_OEM_4: return 0x2F;
        case VK_OEM_6: return 0x30;
        case VK_OEM_5: return 0x31;
        case VK_OEM_1: return 0x33;
        case VK_OEM_7: return 0x34;
        case VK_OEM_3: return 0x35;
        case VK_OEM_COMMA: return 0x36;
        case VK_OEM_PERIOD: return 0x37;
        case VK_OEM_2: return 0x38;
        case VK_CAPITAL: return 0x39;
        case VK_SNAPSHOT: return 0x46;
        case VK_SCROLL: return 0x47;
        case VK_PAUSE: return 0x48;
        case VK_INSERT: return 0x49;
        case VK_HOME: return 0x4A;
        case VK_PRIOR: return 0x4B;
        case VK_DELETE: return 0x4C;
        case VK_END: return 0x4D;
        case VK_NEXT: return 0x4E;
        case VK_RIGHT: return 0x4F;
        case VK_LEFT: return 0x50;
        case VK_DOWN: return 0x51;
        case VK_UP: return 0x52;
        case VK_NUMLOCK: return 0x53;
        case VK_DIVIDE: return 0x54;
        case VK_MULTIPLY: return 0x55;
        case VK_SUBTRACT: return 0x56;
        case VK_ADD: return 0x57;
        case VK_NUMPAD0: return 0x62;
        case VK_DECIMAL: return 0x63;
        case VK_OEM_102: return 0x64;
        case VK_APPS: return 0x65;
        case VK_SLEEP: return 0x66;
        case VK_KANA: return 0x90;
        case VK_HANJA: return 0x91;
        default: return 0;
      }
    }

    bool ioctl_locked(DWORD code, void *buffer, DWORD size) {
      DWORD bytes_returned = 0;
      return DeviceIoControl(
               handle_,
               code,
               buffer,
               size,
               nullptr,
               0,
               &bytes_returned,
               nullptr
             ) != FALSE;
    }

    void close_locked() {
      if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
      }
      pressed_.fill(false);
      buttons_ = 0;
      vscroll_remainder_ = 0;
      hscroll_remainder_ = 0;
    }

    HANDLE handle_ {INVALID_HANDLE_VALUE};
    std::mutex mutex_;
    std::array<bool, 256> pressed_ {};
    std::uint8_t buttons_ {0};
    int vscroll_remainder_ {0};
    int hscroll_remainder_ {0};
  };
}  // namespace platf
