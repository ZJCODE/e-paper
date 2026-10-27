#ifndef ZECTRIX_CANVAS_H_
#define ZECTRIX_CANVAS_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "zectrix_epd.h"

class ZectrixCanvas {
public:
    static constexpr int kWidth = ZECTRIX_EPD_PANEL_WIDTH;
    static constexpr int kHeight = ZECTRIX_EPD_PANEL_HEIGHT;
    static constexpr int kStride = kWidth / 8;

    void Clear(bool white = true);
    void Pixel(int x, int y, bool black);
    void FillRect(int x, int y, int width, int height, bool black);
    void Rect(int x, int y, int width, int height, bool black = true);
    void Line(int x0, int y0, int x1, int y1, bool black = true);
    void Text(int x, int y, const char* text, int scale = 1,
              bool inverted = false);
    void TextCentered(int y, const char* text, int scale = 1,
                      bool inverted = false);
    int TextWidth(const char* text, int scale = 1) const;

    /**
     * Blit a 1bpp MSB-first bitmap (0=black, 1=white), row stride in bytes.
     * Transparent white is not supported; every source pixel is written.
     */
    void Bitmap(int x, int y, int width, int height, const uint8_t* bits,
                size_t stride);

    uint8_t* data() { return pixels_.data(); }
    const uint8_t* data() const { return pixels_.data(); }
    size_t size() const { return pixels_.size(); }

private:
    std::array<uint8_t, ZECTRIX_EPD_1BPP_FRAME_BYTES> pixels_ = {};
};

#endif  // ZECTRIX_CANVAS_H_
