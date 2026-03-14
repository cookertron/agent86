#pragma once
#include <cstdint>
#include <string>

struct VideoState {
    bool active = false;
    std::string mode_name;        // "CGA80" etc
    int cols = 80, rows = 25;
    int cursor_row = 0, cursor_col = 0;
    uint8_t cursor_start = 6, cursor_end = 7; // scan line shape
    bool cursor_hidden = false;
    uint32_t vram_base = 0xB8000; // physical addr in cpu.memory

    // VRAM address for a given cell
    uint32_t cellAddr(int row, int col) const {
        return vram_base + (uint32_t)(row * cols + col) * 2;
    }

    // Video mode number for INT 10h AH=0Fh
    uint8_t modeNumber() const {
        if (mode_name == "MDA")   return 0x07;
        if (mode_name == "CGA40") return 0x01;
        if (mode_name == "CGA80") return 0x03;
        if (mode_name == "VGA50") return 0x03; // same as CGA80, 50-line variant
        return 0x03;
    }
};

struct MouseState {
    bool present = false;    // driver installed
    bool visible = false;    // cursor shown
    uint16_t x = 0, y = 0;  // pixel coordinates (text mode: col*8, row*8)
    uint16_t buttons = 0;    // button state bitmask (bit0=left, bit1=right, bit2=middle)
    uint16_t h_min = 0, h_max = 639;  // horizontal range (pixels)
    uint16_t v_min = 0, v_max = 199;  // vertical range (pixels)
};
