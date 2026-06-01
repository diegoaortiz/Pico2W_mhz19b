/*
 * Minimal LVGL v9 configuration for neuma mini (Pico 2W + GC9A01).
 * Only the options we deviate from are set here; every other LVGL option
 * falls back to its built-in default (see lv_conf_internal.h).
 *
 * Activated via build flag -D LV_CONF_INCLUDE_SIMPLE (see platformio.ini),
 * which makes LVGL include this file from the project's include/ path.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Guard: this header is also pulled in when assembling LVGL's .S files,
 * where C typedefs from <stdint.h> are invalid. */
#ifndef __ASSEMBLY__
#include <stdint.h>
#endif

/*====================
   COLOR
 *====================*/
#define LV_COLOR_DEPTH 16

/*=========================
   MEMORY
 *=========================*/
#define LV_MEM_SIZE (48 * 1024U)

/*====================
   HAL / REFRESH
 *====================*/
#define LV_DEF_REFR_PERIOD 16 /* ~60 Hz refresh attempts */

/*=======================
   LOG / DEBUG
 *=======================*/
#define LV_USE_LOG 0

/*==================
   FONTS
 *==================*/
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#endif /* LV_CONF_H */
