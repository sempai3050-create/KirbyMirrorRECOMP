#pragma once
#include <algorithm>
#include <cstdint>

// Shared native color arithmetic; moved unchanged from the reference compositor.
namespace gba::color {
inline void to_rgb888(uint16_t c, uint8_t* out) {
    // 5 → 8 bit expansion: ((v << 3) | (v >> 2)) gives full-range 0..255.
    uint8_t r = (c >>  0) & 0x1F;
    uint8_t g = (c >>  5) & 0x1F;
    uint8_t b = (c >> 10) & 0x1F;
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

inline uint16_t blend_alpha_gba555(uint16_t top,
                                   uint16_t bottom,
                                   uint32_t eva,
                                   uint32_t evb) {
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    const uint32_t tr = top & 31u;
    const uint32_t tg = ((top >> 4) & 62u) | (top >> 15);
    const uint32_t tb = (top >> 10) & 31u;
    const uint32_t br = bottom & 31u;
    const uint32_t bg = ((bottom >> 4) & 62u) | (bottom >> 15);
    const uint32_t bb = (bottom >> 10) & 31u;

    // The GBA blends in its native color domain, rounds to nearest (+8 before
    // the 4-bit division), and carries a sixth green precision bit in palette
    // bit 15. Only after blending is green reduced to the displayed 5 bits.
    // Doing this on already-expanded RGB888 values produces subtly wrong colors.
    const uint32_t r = std::min(31u, (tr * eva + br * evb + 8u) >> 4);
    const uint32_t g6 = std::min(63u, (tg * eva + bg * evb + 8u) >> 4);
    const uint32_t b = std::min(31u, (tb * eva + bb * evb + 8u) >> 4);
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

inline uint16_t brighten_gba555(uint16_t color, uint32_t evy) {
    if (evy > 16) evy = 16;
    uint32_t r = color & 31u;
    uint32_t g6 = ((color >> 4) & 62u) | (color >> 15);
    uint32_t b = (color >> 10) & 31u;
    r += ((31u - r) * evy + 8u) >> 4;
    g6 += ((63u - g6) * evy + 8u) >> 4;
    b += ((31u - b) * evy + 8u) >> 4;
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

inline uint16_t darken_gba555(uint16_t color, uint32_t evy) {
    if (evy > 16) evy = 16;
    uint32_t r = color & 31u;
    uint32_t g6 = ((color >> 4) & 62u) | (color >> 15);
    uint32_t b = (color >> 10) & 31u;
    // Hardware rounds the subtractive term with +7 rather than +8.
    r -= (r * evy + 7u) >> 4;
    g6 -= (g6 * evy + 7u) >> 4;
    b -= (b * evy + 7u) >> 4;
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

} // namespace gba::color
