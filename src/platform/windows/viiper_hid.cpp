/**
 * @file src/platform/windows/viiper_hid.cpp
 * @brief VIIPER localhost client for virtual USB keyboard/mouse input.
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include "viiper_hid.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace platf {
  namespace {
    constexpr char kHost[] = "127.0.0.1";
    constexpr unsigned short kPort = 3242;
    constexpr int kWheelDelta = 120;

    bool send_all(SOCKET socket, const std::uint8_t *data, std::size_t size) {
      std::size_t sent = 0;
      while (sent < size) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(size - sent, std::numeric_limits<int>::max()));
        const int result = ::send(socket, reinterpret_cast<const char *>(data + sent), chunk, 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
      }
      return true;
    }

    bool send_all(SOCKET socket, const std::string &data, bool nul_terminate = false) {
      if (!send_all(socket, reinterpret_cast<const std::uint8_t *>(data.data()), data.size())) return false;
      if (nul_terminate) {
        const std::uint8_t zero = 0;
        return send_all(socket, &zero, 1);
      }
      return true;
    }

    SOCKET connect_localhost() {
      SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (socket == INVALID_SOCKET) return INVALID_SOCKET;

      DWORD timeout_ms = 1500;
      ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
      ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));

      sockaddr_in address {};
      address.sin_family = AF_INET;
      address.sin_port = htons(kPort);
      if (::inet_pton(AF_INET, kHost, &address.sin_addr) != 1 ||
          ::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
        ::closesocket(socket);
        return INVALID_SOCKET;
      }
      return socket;
    }

    bool api_request(const std::string &request, std::string &response) {
      SOCKET socket = connect_localhost();
      if (socket == INVALID_SOCKET) return false;

      bool ok = send_all(socket, request, true);
      if (ok) {
        ::shutdown(socket, SD_SEND);
        std::array<char, 1024> buffer {};
        for (;;) {
          const int received = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
          if (received == 0) break;
          if (received < 0) { ok = false; break; }
          response.append(buffer.data(), static_cast<std::size_t>(received));
          if (response.size() > 64 * 1024) { ok = false; break; }
        }
      }
      ::closesocket(socket);
      return ok;
    }

    bool parse_json_integer(const std::string &json, const char *field, int &value) {
      const std::string key = std::string("\"") + field + "\"";
      std::size_t pos = json.find(key);
      if (pos == std::string::npos) return false;
      pos = json.find(':', pos + key.size());
      if (pos == std::string::npos) return false;
      ++pos;
      while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
      if (pos >= json.size()) return false;

      bool quoted = false;
      if (json[pos] == '"') { quoted = true; ++pos; }

      int parsed = 0;
      bool any = false;
      while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        any = true;
        parsed = parsed * 10 + (json[pos] - '0');
        ++pos;
      }
      if (!any) return false;
      if (quoted && (pos >= json.size() || json[pos] != '"')) return false;
      value = parsed;
      return true;
    }

    SOCKET connect_device_stream(int bus_id, int device_id) {
      SOCKET socket = connect_localhost();
      if (socket == INVALID_SOCKET) return INVALID_SOCKET;
      const std::string handshake = "bus/" + std::to_string(bus_id) + "/" + std::to_string(device_id);
      if (!send_all(socket, handshake, true)) {
        ::closesocket(socket);
        return INVALID_SOCKET;
      }
      return socket;
    }

    void put_i16_le(std::uint8_t *dst, std::int16_t value) {
      const auto u = static_cast<std::uint16_t>(value);
      dst[0] = static_cast<std::uint8_t>(u & 0xFF);
      dst[1] = static_cast<std::uint8_t>((u >> 8) & 0xFF);
    }
  }  // namespace

  struct viiper_hid_t::impl_t {
    mutable std::mutex mutex;
    bool winsock_started {false};
    bool open {false};
    int bus_id {-1};
    int keyboard_id {-1};
    int mouse_id {-1};
    SOCKET keyboard_socket {INVALID_SOCKET};
    SOCKET mouse_socket {INVALID_SOCKET};
    std::array<bool, 256> pressed {};
    std::uint8_t buttons {0};
    int vscroll_remainder {0};
    int hscroll_remainder {0};

    bool start_winsock_locked() {
      if (winsock_started) return true;
      WSADATA data {};
      if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
      winsock_started = true;
      return true;
    }

    void remove_bus_locked() {
      if (bus_id < 0 || !winsock_started) return;
      std::string ignored;
      api_request("bus/remove " + std::to_string(bus_id), ignored);
      bus_id = -1;
    }

    void reset_state_locked() {
      pressed.fill(false);
      buttons = 0;
      vscroll_remainder = 0;
      hscroll_remainder = 0;
    }

    void close_locked() {
      if (keyboard_socket != INVALID_SOCKET) { ::closesocket(keyboard_socket); keyboard_socket = INVALID_SOCKET; }
      if (mouse_socket != INVALID_SOCKET) { ::closesocket(mouse_socket); mouse_socket = INVALID_SOCKET; }
      remove_bus_locked();
      keyboard_id = -1;
      mouse_id = -1;
      open = false;
      reset_state_locked();
      if (winsock_started) { ::WSACleanup(); winsock_started = false; }
    }

    bool send_keyboard_locked() {
      if (keyboard_socket == INVALID_SOCKET) return false;
      std::uint8_t modifiers = 0;
      for (std::size_t usage = 0xE0; usage <= 0xE7; ++usage) {
        if (pressed[usage]) modifiers |= static_cast<std::uint8_t>(1u << (usage - 0xE0));
      }
      std::vector<std::uint8_t> packet;
      packet.reserve(226);
      packet.push_back(modifiers);
      packet.push_back(0);
      for (std::size_t usage = 1; usage < 0xE0; ++usage) {
        if (pressed[usage]) packet.push_back(static_cast<std::uint8_t>(usage));
      }
      packet[1] = static_cast<std::uint8_t>(packet.size() - 2);
      return send_all(keyboard_socket, packet.data(), packet.size());
    }

    bool send_mouse_locked(std::int16_t x, std::int16_t y, std::int16_t wheel, std::int16_t pan) {
      if (mouse_socket == INVALID_SOCKET) return false;
      std::uint8_t packet[9] {};
      packet[0] = buttons;
      put_i16_le(packet + 1, x);
      put_i16_le(packet + 3, y);
      put_i16_le(packet + 5, wheel);
      put_i16_le(packet + 7, pan);
      return send_all(mouse_socket, packet, sizeof(packet));
    }
  };

  viiper_hid_t::viiper_hid_t(): impl_(std::make_unique<impl_t>()) {}
  viiper_hid_t::~viiper_hid_t() { close(); }

  bool viiper_hid_t::open() {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->open) return true;
    if (!impl_->start_winsock_locked()) return false;

    std::string response;
    if (!api_request("ping", response) || response.find("\"server\"") == std::string::npos || response.find("VIIPER") == std::string::npos) {
      impl_->close_locked(); return false;
    }
    response.clear();
    if (!api_request("bus/create", response) || !parse_json_integer(response, "busId", impl_->bus_id)) {
      impl_->close_locked(); return false;
    }
    response.clear();
    if (!api_request("bus/" + std::to_string(impl_->bus_id) + "/add {\"type\":\"keyboard\"}", response) ||
        !parse_json_integer(response, "devId", impl_->keyboard_id)) {
      impl_->close_locked(); return false;
    }
    impl_->keyboard_socket = connect_device_stream(impl_->bus_id, impl_->keyboard_id);
    if (impl_->keyboard_socket == INVALID_SOCKET) { impl_->close_locked(); return false; }

    response.clear();
    if (!api_request("bus/" + std::to_string(impl_->bus_id) + "/add {\"type\":\"mouse\"}", response) ||
        !parse_json_integer(response, "devId", impl_->mouse_id)) {
      impl_->close_locked(); return false;
    }
    impl_->mouse_socket = connect_device_stream(impl_->bus_id, impl_->mouse_id);
    if (impl_->mouse_socket == INVALID_SOCKET) { impl_->close_locked(); return false; }

    impl_->open = true;
    return true;
  }

  void viiper_hid_t::close() { std::scoped_lock lock(impl_->mutex); impl_->close_locked(); }
  bool viiper_hid_t::is_open() const { std::scoped_lock lock(impl_->mutex); return impl_->open; }

  bool viiper_hid_t::keyboard_usage(std::uint8_t usage, bool release) {
    if (usage == 0) return false;
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) return false;
    const bool old_state = impl_->pressed[usage];
    impl_->pressed[usage] = !release;
    if (!impl_->send_keyboard_locked()) {
      impl_->pressed[usage] = old_state;
      impl_->close_locked();
      return false;
    }
    return true;
  }

  bool viiper_hid_t::move_mouse(int delta_x, int delta_y) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) return false;
    while (delta_x != 0 || delta_y != 0) {
      const int x = std::clamp(delta_x, static_cast<int>(std::numeric_limits<std::int16_t>::min()), static_cast<int>(std::numeric_limits<std::int16_t>::max()));
      const int y = std::clamp(delta_y, static_cast<int>(std::numeric_limits<std::int16_t>::min()), static_cast<int>(std::numeric_limits<std::int16_t>::max()));
      if (!impl_->send_mouse_locked(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), 0, 0)) {
        impl_->close_locked(); return false;
      }
      delta_x -= x;
      delta_y -= y;
    }
    return true;
  }

  bool viiper_hid_t::button_mouse(int button, bool release) {
    std::uint8_t bit = 0;
    switch (button) {
      case 1: bit = 0; break;
      case 2: bit = 2; break;
      case 3: bit = 1; break;
      case 4: bit = 3; break;
      case 5: bit = 4; break;
      default: return false;
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) return false;
    const auto old_buttons = impl_->buttons;
    if (release) impl_->buttons &= static_cast<std::uint8_t>(~(1u << bit));
    else impl_->buttons |= static_cast<std::uint8_t>(1u << bit);
    if (!impl_->send_mouse_locked(0, 0, 0, 0)) {
      impl_->buttons = old_buttons;
      impl_->close_locked();
      return false;
    }
    return true;
  }

  bool viiper_hid_t::scroll(int distance, bool horizontal) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) return false;
    auto &remainder = horizontal ? impl_->hscroll_remainder : impl_->vscroll_remainder;
    remainder += distance;
    int ticks = remainder / kWheelDelta;
    remainder %= kWheelDelta;
    while (ticks != 0) {
      const int chunk = std::clamp(ticks, static_cast<int>(std::numeric_limits<std::int16_t>::min()), static_cast<int>(std::numeric_limits<std::int16_t>::max()));
      if (!impl_->send_mouse_locked(0, 0, horizontal ? 0 : static_cast<std::int16_t>(chunk), horizontal ? static_cast<std::int16_t>(chunk) : 0)) {
        impl_->close_locked(); return false;
      }
      ticks -= chunk;
    }
    return true;
  }
}  // namespace platf
