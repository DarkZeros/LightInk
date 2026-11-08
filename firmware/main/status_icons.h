#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "display.h"

// Status icons in a strip at the top left of the watchface, 16x16 each. A shown icon
// makes the strip part of the overlay: see Core::draw() and digitsClearOfOverlay()
namespace StatusIcons {

enum Id : uint8_t { WIFI, LORA, BT, GPS, NOTIFY, COUNT };

constexpr uint8_t kSize = 16;
constexpr Rect kArea{0, 0, kSize * COUNT, kSize};

using Bitmap = std::array<uint8_t, kSize * kSize / 8>; // Rows MSB first, as drawBitmap()

void rowNot16Pixels(); // Not constexpr: a bad row fails the compile
consteval Bitmap bitmap(std::array<std::string_view, kSize> art) {
    Bitmap b{};
    for (size_t y = 0; y < kSize; y++) {
        if (art[y].size() != kSize)
            rowNot16Pixels();
        for (size_t x = 0; x < kSize; x++)
            if (art[y][x] == 'X')
                b[y * 2 + x / 8] |= 0x80 >> (x & 7);
    }
    return b;
}

// Drafts
constexpr std::array<Bitmap, COUNT> kBitmaps{
    bitmap({
        "................",
        "................",
        ".....XXXXXX.....",
        "...XX......XX...",
        "..X..XXXXXX..X..",
        ".X.XX......XX.X.",
        "X.X...XXXX...X.X",
        "...X.X....X.X...",
        "....X..XX..X....",
        "......X..X......",
        ".......XX.......",
        "......XXXX......",
        "......XXXX......",
        ".......XX.......",
        "................",
        "................",
    }),
    bitmap({
        "................",
        ".X............X.",
        "X..X........X..X",
        "X.X..X....X..X.X",
        "X.X.X..XX..X.X.X",
        "X.X.X.XXXX.X.X.X",
        "X.X..X.XX.X..X.X",
        ".X..X..XX..X..X.",
        ".......XX.......",
        ".......XX.......",
        ".......XX.......",
        ".......XX.......",
        ".......XX.......",
        ".....XXXXXX.....",
        "................",
        "................",
    }),
    bitmap({
        "................",
        ".......X........",
        ".......XX.......",
        ".......X.X......",
        "...X...X..X.....",
        "....X..X.X......",
        ".....X.XX.......",
        "......XX........",
        ".....X.XX.......",
        "....X..X.X......",
        "...X...X..X.....",
        ".......X.X......",
        ".......XX.......",
        ".......X........",
        "................",
        "................",
    }),
    bitmap({
        "................",
        ".....XXXXXX.....",
        "....XX....XX....",
        "...XX......XX...",
        "...X...XX...X...",
        "...X..XXXX..X...",
        "...X..XXXX..X...",
        "...X...XX...X...",
        "...XX......XX...",
        "....XX....XX....",
        ".....XX..XX.....",
        "......XXXX......",
        ".......XX.......",
        "................",
        "................",
        "................",
    }),
    bitmap({
        ".......XX.......",
        "......XXXX......",
        ".....XX..XX.....",
        "....XX....XX....",
        "....X......X....",
        "....X......X....",
        "....X......X....",
        "...XX......XX...",
        "...X........X...",
        "..XX........XX..",
        "..XXXXXXXXXXXX..",
        "................",
        "......XXXX......",
        ".......XX.......",
        "................",
        "................",
    }),
};

} // namespace StatusIcons
