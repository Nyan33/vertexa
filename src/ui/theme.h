// Design tokens (from mockup A3). Colors are a runtime palette (Settings -> Theme); metrics are fixed.
#pragma once
#include "base.h"

enum {
    TC_BG, TC_S1, TC_S2, TC_S3, TC_S4, TC_PASTE,
    TC_LINE, TC_LINE2,
    TC_TX, TC_TX2, TC_TX3,
    TC_AC, TC_LOOP, TC_PREV, TC_NEXT, TC_TWEEN_C, TC_TWEEN_S, TC_SEL,
    TC_COUNT
};
extern Color THEME[TC_COUNT];

#define C_BG      THEME[TC_BG]
#define C_S1      THEME[TC_S1]
#define C_S2      THEME[TC_S2]
#define C_S3      THEME[TC_S3]
#define C_S4      THEME[TC_S4]
#define C_LINE    THEME[TC_LINE]
#define C_LINE2   THEME[TC_LINE2]
#define C_TX      THEME[TC_TX]
#define C_TX2     THEME[TC_TX2]
#define C_TX3     THEME[TC_TX3]
#define C_AC      THEME[TC_AC]
#define C_AC2     ((THEME[TC_AC] & 0x00ffffffu) | 0x1f000000u)
#define C_AC3     ((THEME[TC_AC] & 0x00ffffffu) | 0x66000000u)
#define C_LOOP    THEME[TC_LOOP]
#define C_PREV    THEME[TC_PREV]
#define C_NEXT    THEME[TC_NEXT]
#define C_PASTE   THEME[TC_PASTE]
#define C_TWEEN_C THEME[TC_TWEEN_C]
#define C_TWEEN_S THEME[TC_TWEEN_S]
#define C_SEL     THEME[TC_SEL]

enum {
    M_TITLE_H = 36,
    M_CTX_H = 44,
    M_TOOLS_W = 52,
    M_RIGHT_W = 300,
    M_TL_H = 316,
    M_TLBAR_H = 36,
    M_NAMES_W = 250,
    M_RULER_H = 32,
    M_ROW_H = 24,
    M_CELL_W = 18,
};
