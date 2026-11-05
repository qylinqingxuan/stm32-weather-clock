#ifndef __SCREEN_DATA_H__
#define __SCREEN_DATA_H__

#include "stm32f10x.h"
#include "cw_data.h"

#define SD_LEVEL_MISERABLE   1
#define SD_LEVEL_UNCOMFORT   2
#define SD_LEVEL_FAIR        3
#define SD_LEVEL_GOOD        4

typedef struct
{
    u8          valid;
    u8          level;
    s16         index;
    const char *text;
} SD_Comfort;

extern SD_Comfort sd_comfort;

/* 主屏天气行要显示的四个数: 实况拿一个, 当天逐日拿三个
 * 屏幕不直接读 cw, 一律走这个结构, 这样"哪几个值需要一起刷新"就写在一处 */
typedef struct
{
    u8  valid;   /* 1 = 实况和当天逐日都已经解析成功 */
    u8  code;    /* 天气现象码 0~38 */
    s8  temp;    /* 当前气温 ℃ */
    s8  high;    /* 今日最高 ℃ */
    s8  low;     /* 今日最低 ℃ */
} SD_Weather;

extern SD_Weather sd_weather;

/* 左下角室内温湿度: DHT11 每秒读一次, 文本也在这里拼好, 屏幕只管照着画
 * tstr 是 "-10.5" 这种成品串, 屏幕层不再格式化, 负号占几个像素这种细节也不用它管 */
typedef struct
{
    u8   valid;    /* 1 = 至少成功读到过一次 */
    s8   temp;     /* 整数部分 ℃, 可为负(换 DHT22 才有负温) */
    u8   temp_d;   /* 小数部分 0~9 */
    u8   humi;     /* 湿度 %RH 整数 */
    u8   tlen;     /* tstr 的有效字节数(不含结尾 0) */
    char tstr[8];  /* 温度文本, 如 "21.0" / "-10.5" */
} SD_Indoor;

extern SD_Indoor sd_indoor;

/* 网络响应暂存区: 开机取数(main)和周期刷新(SD_NetRefresh)共用同一块 */
extern char sd_body[CW_BODY_MAX];
void SD_NetRefresh (u32 epoch);

/* 最后一行轮播: 页数 + 每页文本缓冲
 * 40 字节 = 最宽 128px 能放 16 个 ASCII 或 8 个汉字, 再留足余量 */
#define SD_ROW_PAGES   6
#define SD_ROW_LEN     40

extern char sd_row[SD_ROW_PAGES][SD_ROW_LEN];

u8 SD_CalcComfort (void);
u8 SD_UpdateWeather (void);
u8 SD_UpdateRow (void);
u8 SD_UpdateIndoor (u32 epoch);
u8 SD_FormatTemp (char *buf, s16 t);
u8 SD_FormatTemp1 (char *buf, s16 t, u8 d);
u8 SD_FormatTime (char *buf, u8 h, u8 m, u8 s);
u8 SD_FormatDate (char *buf, u8 mon, u8 day);
u8 SD_FormatWeek (char *buf, u8 week);

#endif
