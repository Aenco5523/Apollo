/**
 * @file src/platform/windows/viiper_hid.h
 * @brief User-mode bridge to VIIPER virtual USB HID devices over localhost.
 */
#pragma once

#include <cstdint>
#include <memory>

namespace platf {
  class viiper_hid_t {
  public:
    viiper_hid_t();
    viiper_hid_t(const viiper_hid_t &) = delete;
    viiper_hid_t &operator=(const viiper_hid_t &) = delete;
    ~viiper_hid_t();

    bool open();
    void close();
    bool is_open() const;

    bool keyboard_usage(std::uint8_t usage, bool release);
    bool move_mouse(int delta_x, int delta_y);
    bool button_mouse(int button, bool release);
    bool scroll(int distance, bool horizontal);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace platf
