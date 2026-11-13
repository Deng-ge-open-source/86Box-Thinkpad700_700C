#ifndef EMU_VID_LCD_H
#define EMU_VID_LCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side LCD controls; neutral values preserve every guest RGB value. */
extern int tp700_lcd_brightness;
extern int tp700_lcd_contrast;
void tp700_lcd_adjust(int control);

static inline int
tp700_lcd_clamp(int value, int minimum, int maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static inline void
tp700_lcd_build_lut(uint8_t lut[256], int brightness, int contrast)
{
    for (int i = 0; i < 256; ++i) {
        int value = 128 + ((i - 128) * contrast) / 100;
        value = (value * brightness) / 100;
        lut[i] = (uint8_t) tp700_lcd_clamp(value, 0, 255);
    }
}

static inline uint32_t
tp700_lcd_pixel(uint32_t pixel, const uint8_t lut[256])
{
    return (pixel & 0xff000000U) |
           ((uint32_t) lut[(pixel >> 16) & 255] << 16) |
           ((uint32_t) lut[(pixel >> 8) & 255] << 8) |
           lut[pixel & 255];
}

#ifdef __cplusplus
}
#endif
#endif
