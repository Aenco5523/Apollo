#!/usr/bin/env python3
from __future__ import annotations

import shutil
import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def patch_file(path: Path, replacements: list[tuple[str, str, str]]) -> None:
    text = path.read_text(encoding="utf-8")
    for old, new, label in replacements:
        text = replace_once(text, old, new, label)
    path.write_text(text, encoding="utf-8")
    print(f"patched: {path}")


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    config_h = root / "src" / "config.h"
    config_cpp = root / "src" / "config.cpp"
    input_cpp = root / "src" / "platform" / "windows" / "input.cpp"
    vhid_h = root / "src" / "platform" / "windows" / "virtual_hid.h"
    source_header = Path(__file__).with_name("virtual_hid.h")

    for required in (config_h, config_cpp, input_cpp, source_header):
        if not required.is_file():
            raise RuntimeError(f"missing required file: {required}")

    if vhid_h.exists() and "bool virtual_hid;" in config_h.read_text(encoding="utf-8") and '#include "virtual_hid.h"' in input_cpp.read_text(encoding="utf-8"):
        print("Apollo V-HID integration is already applied")
        return 0

    if vhid_h.exists():
        raise RuntimeError(f"refusing to overwrite unexpected existing file: {vhid_h}")

    patch_file(
        config_h,
        [(
            "    bool always_send_scancodes;\n\n    bool high_resolution_scrolling;",
            "    bool always_send_scancodes;\n    bool virtual_hid;\n\n    bool high_resolution_scrolling;",
            "config.h virtual_hid field",
        )],
    )

    patch_file(
        config_cpp,
        [
            (
                "    true,  // always send scancodes\n    true,  // high resolution scrolling",
                "    true,  // always send scancodes\n    false, // virtual HID keyboard/mouse (Windows only)\n    true,  // high resolution scrolling",
                "config.cpp input defaults",
            ),
            (
                '    bool_f(vars, "always_send_scancodes", input.always_send_scancodes);\n\n    bool_f(vars, "high_resolution_scrolling", input.high_resolution_scrolling);',
                '    bool_f(vars, "always_send_scancodes", input.always_send_scancodes);\n    bool_f(vars, "virtual_hid", input.virtual_hid);\n\n    bool_f(vars, "high_resolution_scrolling", input.high_resolution_scrolling);',
                "config.cpp virtual_hid parser",
            ),
        ],
    )

    patch_file(
        input_cpp,
        [
            (
                '#include "keylayout.h"\n#include "misc.h"\n#include "src/config.h"',
                '#include "keylayout.h"\n#include "misc.h"\n#include "virtual_hid.h"\n#include "src/config.h"',
                "input.cpp virtual_hid include",
            ),
            (
                "  struct input_raw_t {\n    ~input_raw_t() {\n      delete vigem;\n    }\n\n    vigem_t *vigem;\n",
                "  struct input_raw_t {\n    ~input_raw_t() {\n      delete virtual_hid;\n      delete vigem;\n    }\n\n    vigem_t *vigem;\n    virtual_hid_t *virtual_hid;\n",
                "input.cpp input_raw_t",
            ),
            (
                "    if (raw.vigem->init()) {\n      delete raw.vigem;\n      raw.vigem = nullptr;\n    }\n\n    // Get pointers to virtual touch/pen input functions (Win10 1809+)",
                "    if (raw.vigem->init()) {\n      delete raw.vigem;\n      raw.vigem = nullptr;\n    }\n\n    if (config::input.virtual_hid) {\n      raw.virtual_hid = new virtual_hid_t {};\n      if (!raw.virtual_hid->open()) {\n        BOOST_LOG(warning) << \"Virtual HID requested, but ApolloVhid could not be opened. Falling back to SendInput.\"sv;\n        delete raw.virtual_hid;\n        raw.virtual_hid = nullptr;\n      } else {\n        BOOST_LOG(info) << \"Virtual HID keyboard/mouse backend enabled\"sv;\n      }\n    }\n\n    // Get pointers to virtual touch/pen input functions (Win10 1809+)",
                "input.cpp virtual_hid init",
            ),
            (
                "  void move_mouse(input_t &input, int deltaX, int deltaY) {\n    INPUT i {};",
                "  void move_mouse(input_t &input, int deltaX, int deltaY) {\n    auto &raw = *(input_raw_t *) input.get();\n    if (raw.virtual_hid && raw.virtual_hid->move_mouse(deltaX, deltaY)) {\n      return;\n    }\n\n    INPUT i {};",
                "input.cpp relative mouse",
            ),
            (
                "  void button_mouse(input_t &input, int button, bool release) {\n    INPUT i {};",
                "  void button_mouse(input_t &input, int button, bool release) {\n    auto &raw = *(input_raw_t *) input.get();\n    if (raw.virtual_hid && raw.virtual_hid->button_mouse(button, release)) {\n      return;\n    }\n\n    INPUT i {};",
                "input.cpp mouse buttons",
            ),
            (
                "  void scroll(input_t &input, int distance) {\n    INPUT i {};",
                "  void scroll(input_t &input, int distance) {\n    auto &raw = *(input_raw_t *) input.get();\n    if (raw.virtual_hid && raw.virtual_hid->scroll(distance, false)) {\n      return;\n    }\n\n    INPUT i {};",
                "input.cpp vertical scroll",
            ),
            (
                "  void hscroll(input_t &input, int distance) {\n    INPUT i {};",
                "  void hscroll(input_t &input, int distance) {\n    auto &raw = *(input_raw_t *) input.get();\n    if (raw.virtual_hid && raw.virtual_hid->scroll(distance, true)) {\n      return;\n    }\n\n    INPUT i {};",
                "input.cpp horizontal scroll",
            ),
            (
                "  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {\n    INPUT i {};",
                "  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {\n    auto &raw = *(input_raw_t *) input.get();\n    if (raw.virtual_hid && raw.virtual_hid->keyboard(modcode, release)) {\n      return;\n    }\n\n    INPUT i {};",
                "input.cpp keyboard",
            ),
        ],
    )

    shutil.copy2(source_header, vhid_h)
    print(f"added: {vhid_h}")
    print("Apollo V-HID integration applied; enable with: virtual_hid = true")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
