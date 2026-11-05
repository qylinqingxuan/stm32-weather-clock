#ifndef __CW_DATA_H__
#define __CW_DATA_H__

#include "stm32f10x.h"

#define CW_DAILY_NUM   3
#define CW_LIFE_NUM    4
#define CW_BODY_MAX    1664

#define CW_LIFE_FLU      0
#define CW_LIFE_UV       1
#define CW_LIFE_AIR      2
#define CW_LIFE_COMFORT  3

typedef struct
{
    u8   valid;
    u8   code;
    s8   temp;
    char text[24];
} CW_Now;

typedef struct
{
    u8   valid;
    u8   code_day;
    u8   code_night;
    u8   precip;     // 降水概率 0~100 (接口给的是 0.00~1.00 的小数)
    u8   humidity;   // 当天平均相对湿度 0~100 %
    u8   wind_scale; // 风力等级 0~9, 接口直接给整数
    u16  wind_speed; // 当天风速, 单位 0.1km/h (接口给 "8.4")
    u16  wind_deg;   // 风向角度 0~359 (270 = 西), 显示时再查表换成"西风"
    u16  rainfall;   // 当天降雨量, 单位 0.1mm (接口 "27.97" 只取到 27.9)
    s8   high;
    s8   low;
    char date[11];
    char text_day[24];
    char text_night[24];
} CW_Daily;

typedef struct
{
    u8   valid;
    u8   idx;
    u8   mapped;
    char en[20];
    char brief[20];
} CW_Life;

typedef struct
{
    CW_Now   now;
    CW_Daily daily[CW_DAILY_NUM];
    CW_Life  life[CW_LIFE_NUM];
    char now_update[26];    // 实况接口的 last_update 原文, 判重用 (25 字符 + 结尾)
    char daily_update[26];  // 逐日接口的 "2026-10-29T08:00:00+08:00"
    char life_update[26];   // 生活指数的
} CW_Data;

extern CW_Data cw;

void CW_Reset (void);
u32  CW_GetTimeStamp (const char *json);
u8   CW_ParseNow (const char *json);
u8   CW_ParseDaily (const char *json);
u8   CW_ParseLife (const char *json);
u8   CW_UpdateChanged (const char *last, const char *cur);

#endif
