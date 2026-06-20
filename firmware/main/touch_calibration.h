#pragma once

#include <stdint.h>

#define TOUCH_CAL_TARGET_COUNT 4
#define TOUCH_CAL_TARGET_LEFT   24
#define TOUCH_CAL_TARGET_RIGHT  216
#define TOUCH_CAL_TARGET_TOP    32
#define TOUCH_CAL_TARGET_BOTTOM 288
#define TOUCH_CAL_MIN_SPAN      40

typedef struct {
    uint16_t x;
    uint16_t y;
} touch_cal_sample_t;
