#include "screen_data.h"
#include "DHT11.h"
#include "esp8266.h"
#include "rtc.h"        /* 周期校时: RTC_SetEpoch */
#include <stdio.h>
#include <string.h>

/* 分档切点, 单位是 I 值的 100 倍, 和 sd_comfort.index 同一刻度
 * 说明: 4180/3140/990 是用 96 个实测样本拟合的, 但当时湿度一律按 60% 代入;
 *       现在换成接口给的真实湿度, 切点还会漂, 热端尤其明显(30℃ 时湿度能拉开 9 分);
 *       所以这三个数是"先用着"的临时值, 等采样脚本补上湿度列、攒够一周样本要重拟合 */
#define SD_CUT_GOOD     4180
#define SD_CUT_FAIR     3140
#define SD_CUT_UNCOMF    990

SD_Comfort sd_comfort;

SD_Indoor sd_indoor;                      /* 室内温湿度: 屏幕只读这里 */
static u32 sd_indoor_epoch = 0xFFFFFFFF;  /* 上次读 DHT11 的秒数, 一秒只读一次 */
static u8  sd_indoor_busy  = 0;           /* 1 = DHT11 已拉低, 正等 18ms 到点 */

/* 网络响应暂存区 —— 四个接口共用, 谁用完谁解析, 只留一份
 * 由数据层持有: 开机阶段的取数和运行期的周期刷新用的是同一块, 免得吃两份 1664 字节 */
char sd_body[CW_BODY_MAX];

/* 周期刷新间隔(秒): 实况 10 分钟 / 逐日 6 小时 / 生活指数 24 小时 / 网络校时 24 小时
 * 实况取 10 分钟而不取 15: 心知约 15 分钟一更, 周期取成 15 会相位锁定, 最坏稳定慢一整拍;
 * 取 10 分钟比源更快, 天气一变最迟 10 分钟就能上屏 */
#define SD_NOW_SEC      600u
#define SD_DAILY_SEC    21600u
#define SD_LIFE_SEC     86400u
#define SD_TIME_SEC     86400u
#define SD_TIME_RETRY   600u     /* 校时没成时, 10 分钟后就再试, 不必干等满 24 小时 */
#define SD_RETRY_SEC    60u      /* 取数失败后 60 秒就重来, 不白等一个完整周期 */

static u8  sd_net_started;   /* 0 = 还没对齐过开机时刻, 第一次调用只记时间不发请求 */
static u32 sd_t_now;         /* 上次取实况的 epoch */
static u32 sd_t_daily;       /* 上次取逐日的 epoch */
static u32 sd_t_life;        /* 上次取生活指数的 epoch */
static u32 sd_t_time;        /* 上次校网络时间的 epoch */
/* 档位对应的中文, 下标就是 level, 0 号留空表示还没算过 */
static const char *const sd_comfort_text[5] =
{
    "",
    "很不舒适",
    "较不舒适",
    "较舒适",
    "舒适"
};

/* sd_isqrt —— 整数开方, 返回 floor(sqrt(n))
 * 用法：s = sd_isqrt (23300);  // 23300 是 2.33m/s 的 0.01 倍
 * 参数：n 被开方数, 最大 2^32-1
 * 返回：n 的平方根整数部分
 * 说明：公式里要 sqrt(V), 但 V 是整数化的风速, 没必要为这一处引 math.h 和浮点;
 *       这是逐位法, 从最高位开始两位两位地试商, 循环固定 16 轮, 几百个周期就完;
 *       少算的那点小数(<0.01) 对 I 值影响不到 0.03 分, 看不出来 */
static u32 sd_isqrt (u32 n)
{
    u32 r = 0;
    u32 bit = 1UL << 30;

    while (bit > n) bit >>= 2;  // 找到不超过 n 的最大 4 的幂

    while (bit != 0)  // 从高位往低位逐位试商
    {
        if (n >= r + bit)
        {
            n -= r + bit;
            r  = (r >> 1) + bit;
        }
        else
        {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* SD_CalcComfort —— 用当天的逐日预报算舒适度, 结果写进 sd_comfort
 * 用法：CW_ParseDaily 成功之后调一次, 返回 0 就是算出来了
 * 参数：无, 直接读 cw.daily[0](今天), 不去碰 DHT11 —— 室内湿度不代表室外
 * 返回：0 = 成功; 1 = 逐日数据还没解析出来, 不能算
 * 说明：公式是国标《气候宜居指数》附录 B 的人体舒适度指数
 *       I = (1.8T + 32) - 0.55(1 - RH/100)(1.8T - 26) - 3.2*sqrt(V)
 *       三段分别是: 温度基准(华氏刻度, 所以数值在 10~50 之间) / 湿度修正 / 风冷修正;
 *       注意 (1.8T - 26) 在 T = 14.4℃ 时正好为 0, 这个温度附近湿度完全不起作用 ——
 *       不是写错, 是公式本身就把"不冷不热"定义成湿度无关, 湿热和湿冷才要扣分;
 *       温度取 (high+low)/2 的日均值, 和心知 comfort "当天一个值"的口径一致;
 *       全程定点整数, 不碰 float, 也不用 math.h —— C90 + microlib 下最省事 */
u8 SD_CalcComfort (void)
{
    const CW_Daily *d = &cw.daily[0];
    s32 a;      /* (1.8T + 32) × 100 */
    s32 b;      /* (1.8T - 26) × 100 */
    s32 hum;    /* 湿度修正项 × 100 */
    s32 wind;   /* 风冷修正项 × 100 */
    s32 v100;   /* 风速, 单位 0.01m/s */
    s16 t10;    /* 日均温, 单位 0.1℃ */
    u32 s;      /* 100 × sqrt(V) */
    u8  level;

    if (d->valid == 0)  // 还没解析出逐日数据, 一个数都别写, 免得屏幕显示假值
    {
        sd_comfort.valid = 0;
        sd_comfort.level = 0;
        sd_comfort.text  = sd_comfort_text[0];
        return 1;
    }

    t10 = (s16)((s16)d->high + (s16)d->low) * 5;  /* (high+low)/2 换算成 0.1℃, 负数也成立 */

    a   = 18L * (s32)t10 + 3200L;  /* 1.8T+32, 乘 100 提成整数 */
    b   = 18L * (s32)t10 - 2600L;  /* 1.8T-26, 同上; 在 14.4℃ 附近为 0 */

    hum = (55L * (s32)(100 - (s32)d->humidity) * b) / 10000L;  /* 0.55 × (1-RH/100) × b */

    v100 = (s32)d->wind_speed * 100L / 36L;  /* 0.1km/h -> 0.01m/s, 84 -> 233 */
    s    = sd_isqrt ((u32)(v100 * 10000L));  /* 100 × sqrt(V), 2330000 -> 1526 */
    wind = 32L * (s32)s / 100L;              /* 3.2 × sqrt(V) × 100 */

    sd_comfort.index = (s16)(a - hum - wind);

    if      (sd_comfort.index >= SD_CUT_GOOD)   level = SD_LEVEL_GOOD;       /* 舒适 */
    else if (sd_comfort.index >= SD_CUT_FAIR)   level = SD_LEVEL_FAIR;       /* 较舒适 */
    else if (sd_comfort.index >= SD_CUT_UNCOMF) level = SD_LEVEL_UNCOMFORT;  /* 较不舒适 */
    else                                        level = SD_LEVEL_MISERABLE;  /* 很不舒适 */

    sd_comfort.level = level;
    sd_comfort.text  = sd_comfort_text[level];
    sd_comfort.valid = 1;
    return 0;
}

SD_Weather sd_weather;

/* SD_UpdateWeather —— 把心知的数据搬进 sd_weather, 四个值有变化才返回 1
 * 用法：主循环里每次转一圈调一次, 返回 1 就重画天气行
 * 参数：无
 * 返回：0 = 没变化(或数据还没到), 1 = 至少一个值变了
 * 说明：屏幕是给人看的, 不该跟着 30ms 的主循环抖;
 *       数据没到时也返回 0, 一次网络超时不该把屏上的温度刷成 "--";
 *       首次成功取到数据必定返回 1, 让主界面画完以后还能再补一次真值 */
u8 SD_UpdateWeather (void)
{
    static SD_Weather last;      /* 上一次真正画到屏上的四个值 */
    static u8         synced;    /* 0 = 还没画过任何真数据 */
    SD_Weather cur;

    if (cw.now.valid == 0 || cw.daily[0].valid == 0) return 0;  /* 数据没齐就保持屏上原样 */

    cur.valid = 1;
    cur.code  = cw.now.code;
    cur.temp  = cw.now.temp;
    cur.high  = cw.daily[0].high;
    cur.low   = cw.daily[0].low;

    if (synced &&
        last.code == cur.code && last.temp == cur.temp &&
        last.high == cur.high && last.low  == cur.low)
        return 0;                /* 四个值和屏上一模一样, 一个像素都不用写 */

    sd_weather = cur;
    last       = cur;
    synced     = 1;
    return 1;
}

/* SD_IndoorStore —— 把刚从 DHT11 读回的一组值判重, 变了才落到 sd_indoor
 * 用法：if (SD_IndoorStore(&th)) WeatherClock_SetIndoor();
 * 参数：th  DHT11_Poll 成功时填好的一组值
 * 返回：0 = 三个值一个没动; 1 = 有值变了, 该重画
 * 说明：原先塞在 SD_UpdateIndoor 后半段, 拆出来是因为读 DHT11 变成两段式了;
 *       第一步只管发起, 第二步拿到数就统一走这里, 判重逻辑只此一份 */
static u8 SD_IndoorStore (DHT11_TH *th)
{
    s8 t;

    t = (s8)th->temp_i;   /* temp_i 是 u8, 强转 s8 才能把 0xFB 解释成 -5(换 DHT22 才有负温) */

    if (sd_indoor.valid &&
        t          == sd_indoor.temp &&
        th->temp_d == sd_indoor.temp_d &&
        th->humi_i == sd_indoor.humi)
        return 0;                             /* 三个值一个没动, 一个像素都不用写 */

    sd_indoor.valid  = 1;
    sd_indoor.temp   = t;
    sd_indoor.temp_d = th->temp_d;
    sd_indoor.humi   = th->humi_i;
    sd_indoor.tlen   = SD_FormatTemp1(sd_indoor.tstr, t, th->temp_d);
    return 1;
}

/* SD_UpdateIndoor —— 每秒读一次 DHT11, 把室内温湿度加工成可直接上屏的数据
 * 用法：if (SD_UpdateIndoor(epoch)) WeatherClock_SetIndoor();
 * 参数：epoch 当前秒数(RTC_GetEpoch), 用来卡一秒一次
 * 返回：0 = 还没读完 / 这一秒已经发起过 / 读失败 / 三个值都没变; 1 = 有值变了, 该重画
 * 说明：一次读取横跨两圈主循环, 分两段——
 *       第一段(空闲且换秒了): DHT11_Start() 只把 DATA 拉低, 立刻返回;
 *       第二段(下一圈进来):   DHT11_Poll() 到点抬线, 读回 40 位, 约 5ms;
 *       原来是 DHT11_Read_Data() 一口气阻塞 25~30ms, 每秒把轮播那一帧顶晚一拍,
 *       现在只剩抬线后的 5ms, 12ms 帧距里看不出来;
 *       读失败一律返回 0 并保留上一帧, 不能让屏上的数突然变成 0.0 */
u8 SD_UpdateIndoor (u32 epoch)
{
    DHT11_TH th;
    u8       r;

    if (sd_indoor_busy)                       /* 刚发起的那次, 还在等 18ms */
    {
        r = DHT11_Poll(&th);
        if (r == DHT11_BUSY) return 0;        /* 没到点, 下一圈再来 */
        sd_indoor_busy  = 0;
        sd_indoor_epoch = epoch;              /* 这一秒已经用掉, 别掉头又发一次 */
        if (r != DHT11_OK) return 0;          /* 失败: 屏上保持上一次的值 */
        return SD_IndoorStore(&th);
    }

    if (epoch == sd_indoor_epoch) return 0;   /* 这一秒已经发起过了 */
    sd_indoor_epoch = epoch;

    DHT11_Start();                            /* 拉低就返回, 不阻塞 */
    sd_indoor_busy = 1;
    return 0;
}


/* SD_FormatTemp —— 把温度值格式化成不带单位的字符串, 例如 "7" "-10"
 * 用法：n = SD_FormatTemp (buf, -10);   // buf 得到 "-10", n 得到 3
 * 参数：buf 输出缓冲, 至少要 5 字节; t 温度 ℃
 * 返回：写进 buf 的字节数(不含结尾 0), 调用者拿它算显示宽度
 * 说明：只出数字和负号, 单位由调用方决定怎么画 —— 24 号字和 16 号字的单位不在同一处;
 *       超过两位一律按 99 截断: 心知的气温不可能到三位, 截断比越界写缓冲强 */
u8 SD_FormatTemp (char *buf, s16 t)
{
    u8  n = 0;
    u16 v;

    v = (t < 0) ? (u16)(-t) : (u16)t;
    if (v > 99) v = 99;
    if (t < 0) buf[n++] = '-';
    if (v >= 10) buf[n++] = (char)('0' + v / 10);
    buf[n++] = (char)('0' + v % 10);
    buf[n]   = 0;
    return n;
}

/* SD_FormatTemp1 —— 带一位小数的温度, 给室内温度用, 例 "-10.5" "21.0"
 * 用法：n = SD_FormatTemp1 (buf, -10, 5);   // buf 得到 "-10.5", n 得到 5
 * 参数：buf 输出缓冲, 至少要 8 字节; t 温度 ℃; d 小数位 0~9
 * 返回：写入 buf 的字节数(不含结尾 0)
 * 说明：整数位同样按 99 截断(室内不可能到三位数, 截断只为不越界);
 *       小数位是 DHT11 给的 temp_d, 0~9 之外按 9 处理 */
u8 SD_FormatTemp1 (char *buf, s16 t, u8 d)
{
    u8  n = 0;
    u16 v;

    v = (t < 0) ? (u16)(-t) : (u16)t;
    if (v > 99) v = 99;
    if (d > 9)  d = 9;
    if (t < 0) buf[n++] = '-';
    if (v >= 10) buf[n++] = (char)('0' + v / 10);
    buf[n++] = (char)('0' + v % 10);
    buf[n++] = '.';
    buf[n++] = (char)('0' + d);
    buf[n]   = 0;
    return n;
}

/* SD_FormatTime —— 把时分秒拼成定长 8 字符 "09:28:20"
 * 用法：SD_FormatTime (buf, dt.hour, dt.min, dt.sec);
 * 参数：buf 输出缓冲, 至少要 9 字节; h/m/s 各 0~23 / 0~59 / 0~59
 * 返回：固定 8 (不含结尾 0)
 * 说明：**必须定长**, 屏幕那一行是按"第 i 个字符放在 i*16 列"画的,
 *       少一位整个后面就错位了, 所以 9 点也输出 "09" 不是 "9" */
u8 SD_FormatTime (char *buf, u8 h, u8 m, u8 s)
{
    buf[0] = (char)('0' + h / 10);
    buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + m / 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + s / 10);
    buf[7] = (char)('0' + s % 10);
    buf[8] = 0;
    return 8;
}

/* SD_FormatDate —— 拼 "10月27日" 这种中文日期, 月和日都不补前导零
 * 用法：SD_FormatDate (buf, dt.month, dt.day);
 * 参数：buf 输出缓冲, 至少要 10 字节; mon 1~12; day 1~31
 * 返回：写入的字节数(不含结尾 0), 两位月两位日时是 8
 * 说明：输出是 **GBK 字节流**, "月"=D4 C2, "日"=C8 D5, 直接喂 Gui_DrawFont_GBK16;
 *       不补零是为了"1月5日"读着自然, 星期固定画在 x=76, 日期变窄也不会错位 */
u8 SD_FormatDate (char *buf, u8 mon, u8 day)
{
    u8 n = 0;

    if (mon >= 10) buf[n++] = (char)('0' + mon / 10);
    buf[n++] = (char)('0' + mon % 10);
    buf[n++] = (char)0xD4; buf[n++] = (char)0xC2;   /* 月 */
    if (day >= 10) buf[n++] = (char)('0' + day / 10);
    buf[n++] = (char)('0' + day % 10);
    buf[n++] = (char)0xC8; buf[n++] = (char)0xD5;   /* 日 */
    buf[n]   = 0;
    return n;
}

/* SD_FormatWeek —— 把 0~6 拼成 "星期日" / "星期一" ... "星期六"
 * 用法：SD_FormatWeek (buf, dt.week);
 * 参数：buf 输出缓冲, 至少要 8 字节; week 0=周日 1=周一 ... 6=周六 (和 rtc.c 一致)
 * 返回：固定 6 (不含结尾 0)
 * 说明：越界值当周日处理, 宁可显示错也不要读数组外 */
u8 SD_FormatWeek (char *buf, u8 week)
{
    static const char *const w[7] = { "日", "一", "二", "三", "四", "五", "六" };

    if (week > 6) week = 0;
    buf[0] = (char)0xD0; buf[1] = (char)0xC7;   /* 星 */
    buf[2] = (char)0xC6; buf[3] = (char)0xDA;   /* 期 */
    buf[4] = w[week][0];
    buf[5] = w[week][1];
    buf[6] = 0;
    return 6;
}

/* ===== 最后一行 6 页轮播的文案: 全部从 cw 和 sd_comfort 现拼 ===== */

char sd_row[SD_ROW_PAGES][SD_ROW_LEN];

/* 天气现象对照表: 天气代码 -> 屏幕上那两个汉字
 * 只用现有 69 条字模(hz16)里画得出的字, 画不出的按天气大类归并:
 *   暴雨/大暴雨/特大暴雨 -> 大雨, 暴雪 -> 大雪, 沙尘暴/强沙尘暴 -> 沙尘 (暴 22 画, 点阵必糊)
 *   雨夹雪 -> 雨雪(夹画不出), 霾 -> 自己画的横线符号(Font.h hz16 末尾那条, 不画字)
 *   晴间多云/大部多云 -> 多云, 雷阵雨伴有冰雹 -> 雷雨
 * 37 冷 / 38 热: "冷""热" 字库没有, 归无可归, 一律 --
 * 为什么用 code 而不是英文文本: 官方全表在 docs.seniverse.com/api/start/code.html,
 *   code 是稳定枚举(0~38), 英文文本随语言和版本漂, 没见过的词只能掉 -- */
static const char *const sd_wt_word[] =
{
    "晴",    /*  0 */
    "多云",  /*  1 */
    "阴",    /*  2 */
    "阵雨",  /*  3 */
    "雷雨",  /*  4 */
    "小雨",  /*  5 */
    "中雨",  /*  6 */
    "大雨",  /*  7 */
    "冻雨",  /*  8 */
    "雨雪",  /*  9 */
    "阵雪",  /* 10 */
    "小雪",  /* 11 */
    "中雪",  /* 12 */
    "大雪",  /* 13 */
    "浮尘",  /* 14 */
    "扬沙",  /* 15 */
    "沙尘",  /* 16 */
    "雾",    /* 17 */
    "大风",  /* 18 */
    "未知",  /* 19 */
    "霾"     /* 20: 不写字, 画横线符号 */
};

#define SD_WT_NUM   ((u8)(sizeof (sd_wt_word) / sizeof (sd_wt_word[0])))
#define SD_WT_NONE  0xFF

/* 天气代码 0~38 -> 词编号, 0xFF = 没有能画的词 */
static const u8 sd_wt_by_code[39] =
{
     0,  0,  0,  0,  1,  1,  1,  1,  1,  2,
     3,  4,  4,  5,  6,  7,  7,  7,  7,  8,
     9, 10, 11, 12, 13, 13, 14, 15, 16, 16,
    17, 20, 18, 18, 18, 18, 18, SD_WT_NONE, SD_WT_NONE
};

/* sd_wt_id —— 天气代码 -> 词编号
 * 用法：i = sd_wt_id (cw.daily[1].code_day);
 * 参数：code 接口的天气代码, 0~38 / 99
 * 返回：词编号 0~(SD_WT_NUM-1); 0xFF = 没有能画的词
 * 说明：返回编号而不是指针, 判重时直接比整数 —— 好几处 "--" 字面量地址不同, 比指针会误判成"变了" */
static u8 sd_wt_id (u8 code)
{
    if (code <= 38) return sd_wt_by_code[code];
    if (code == 99) return 19;          /* 99 = 未知 */
    return SD_WT_NONE;
}

/* sd_text_width —— 算一条 GBK 串在 16 号字下占多少像素
 * 用法：if (sd_text_width (sd_row[0]) > 128) ...
 * 参数：s GBK 串
 * 返回：像素宽; 空串 0
 * 说明：rules 和 QDTFT.c 的 Gui_ScrollTextWidth 完全一致(汉字 16 / ASCII 8);
 *       这里必须自己算一份, 因为数据层不能反过来依赖屏幕层的函数 */
static u16 sd_text_width (const char *s)
{
    u16 w = 0;

    while (*s)
    {
        if ((u8)*s >= 0x81) { w = (u16)(w + 16); s += 2; }   /* GBK 汉字占两字节 */
        else                { w = (u16)(w +  8); s += 1; }
    }
    return w;
}

/* sd_life_brief —— 取一项生活指数的中文简报
 * 用法：p = sd_life_brief (CW_LIFE_FLU);  if (p) ...
 * 参数：idx 生活指数下标 (CW_LIFE_FLU / UV / AIR)
 * 返回：中文简报指针; 0 = 这项没取到, 或者映射表里没这个词(此时 brief 存的是英文原文)
 * 说明：mapped == 0 一律当没有 —— 英文原文动辄十几个字母,
 *       拼进去一定超宽, 而且对着屏幕看英文也没意义 */
static const char *sd_life_brief (u8 idx)
{
    const CW_Life *l = &cw.life[idx];

    if (l->valid == 0 || l->mapped == 0 || l->brief[0] == 0) return 0;
    return l->brief;
}

/* SD_UpdateRow —— 把 6 页文案全部重新拼一遍, 有变化才返回 1
 * 用法：if (SD_UpdateRow ()) WeatherClock_RowRefresh ();
 * 参数：无, 直接读 cw 和 sd_comfort
 * 返回：0 = 和上次拼出来的一模一样; 1 = 至少一页变了, 该重画
 * 说明：六页分别是 明日天气+高低温 / 降水概率 / 感冒 / 紫外线 / 空气 / 舒适度;
 *       任何一项数据没到, 那一页就写 "--", 不写空串 ——
 *       空串会让屏幕画不出东西, 反而残留上一次的旧字;
 *       第 1 页最宽: "明日:多云 -11/-20" 有 136px, 超过 128 就去掉中间的空格再拼一次,
 *       去掉正好 128, 再长的话(三个字负温)现实中不存在 */
u8 SD_UpdateRow (void)
{
    static u8          synced;
    static u8          last_wt, last_precip, last_comfort;
    static s8          last_hi, last_lo;
    static char        last_flu[24], last_uv[24], last_air[24];

    const char *wt;                       /* 明天天气词 */
    const char *flu;
    const char *uv;
    const char *air;
    u8   wti;
    s8   hi, lo;
    u8   precip, comfort;
    char t1[8], t2[8];
    u8   changed;

    /* ---- 第 1 页: 明日 天气 + 高/低温 ---- */
    if (cw.daily[1].valid)
    {
        wti = sd_wt_id (cw.daily[1].code_day);
        wt  = (wti == SD_WT_NONE) ? "--" : sd_wt_word[wti];
        hi  = cw.daily[1].high;
        lo  = cw.daily[1].low;
        SD_FormatTemp (t1, (s16)hi);
        SD_FormatTemp (t2, (s16)lo);
        sprintf (sd_row[0], "明日:%s %s/%s", wt, t1, t2);
        if (sd_text_width (sd_row[0]) > 128)           /* 两个两位数负温才会超, 去掉空格 */
            sprintf (sd_row[0], "明日:%s%s/%s", wt, t1, t2);
    }
    else
    {
        wti = SD_WT_NONE;
        hi  = -128;
        lo  = -128;
        strcpy (sd_row[0], "--");
    }

    /* ---- 第 2 页: 本日降水概率, 低于 20% 直接说没有 ---- */
    if (cw.daily[0].valid)
    {
        precip = cw.daily[0].precip;
        if (precip < 20) strcpy (sd_row[1], "今日无降水");
        else             sprintf (sd_row[1], "降水概率:%u%%", (unsigned)precip);
    }
    else
    {
        precip = 0xFF;
        strcpy (sd_row[1], "--");
    }

    /* ---- 第 3~5 页: 感冒 / 紫外线 / 空气扩散条件 ---- */
    flu = sd_life_brief (CW_LIFE_FLU);
    uv  = sd_life_brief (CW_LIFE_UV);
    air = sd_life_brief (CW_LIFE_AIR);

    if (flu) sprintf (sd_row[2], "感冒:%s", flu);
    else     strcpy  (sd_row[2], "--");

    if (uv)  sprintf (sd_row[3], "紫外线:%s", uv);
    else     strcpy  (sd_row[3], "--");

    if (air) sprintf (sd_row[4], "空气:%s", air);
    else     strcpy  (sd_row[4], "--");

    /* ---- 第 6 页: 舒适度, 自己按国标公式算的那个 ---- */
    comfort = sd_comfort.valid ? sd_comfort.level : 0;
    if (sd_comfort.valid && sd_comfort.text[0])
        sprintf (sd_row[5], "舒适度:%s", sd_comfort.text);
    else
        strcpy (sd_row[5], "--");

    /* ---- 判重: 九个关键量全同就当没变, 一个像素都不写 ---- */
    changed = 0;
    if (synced == 0) changed = 1;
    else if (wti != last_wt || hi != last_hi || lo != last_lo ||
             precip != last_precip || comfort != last_comfort ||
             strcmp (sd_row[2], last_flu) != 0 ||
             strcmp (sd_row[3], last_uv)  != 0 ||
             strcmp (sd_row[4], last_air) != 0)
        changed = 1;

    if (changed)
    {
        last_wt      = wti;
        last_hi      = hi;
        last_lo      = lo;
        last_precip  = precip;
        last_comfort = comfort;
        strcpy (last_flu, sd_row[2]);
        strcpy (last_uv,  sd_row[3]);
        strcpy (last_air, sd_row[4]);
        synced       = 1;
    }
    return changed;
}

/* SD_NetDone —— 一次取数刚跑完, 把结果落到 cw / RTC
 * 用法: SD_NetRefresh 内部调, 外部不用管
 * 参数: 无
 * 返回: 无
 * 说明: 只有成功才解析, 失败一律等下一个周期, 不做重试风暴;
 *       校时是个例外: 失败要把 sd_t_time 往回拨, 10 分钟后再试,
 *       否则开机没校上就得干等一整天 (退避算式和改造前完全一样,
 *       这里的 sd_t_time 还是"发起那一刻"的值) */
static void SD_NetDone (void)
{
    u8  r    = ESP8266_Result ();
    u8  kind = ESP8266_Kind ();
    u32 ts;

    if (kind == ESP_REQ_TIME)
    {
        ts = (r == 0) ? CW_GetTimeStamp (sd_body) : 0;
        if (ts != 0)
        {
            /* 服务器时间点 + 网络单程 + 接口固有偏差, 四舍五入到秒再写 RTC */
            ts += (u32)((ESP8266_Cost () + TIME_BIAS_MS + 500) / 1000);
            RTC_SetEpoch (ts);
        }
        else
        {
            sd_t_time = sd_t_time - SD_TIME_SEC + SD_TIME_RETRY;
        }
        return;
    }

    if (r != 0)
    {
        /* 取数失败别白等一整个周期: 把计时起点往前拨 (周期-60) 秒, 60 秒后自然重新到期。
         * 减法在 u32 模 2^32 下是安全的: 后面 epoch - sd_t_xxx 同样是模运算, 回绕会自动抵消 */
        if      (kind == ESP_REQ_NOW)   sd_t_now   -= (SD_NOW_SEC   - SD_RETRY_SEC);
        else if (kind == ESP_REQ_DAILY) sd_t_daily -= (SD_DAILY_SEC - SD_RETRY_SEC);
        else if (kind == ESP_REQ_LIFE)  sd_t_life  -= (SD_LIFE_SEC  - SD_RETRY_SEC);
        return;
    }

    if (kind == ESP_REQ_NOW)        CW_ParseNow (sd_body);
    else if (kind == ESP_REQ_DAILY)
    {
        if (CW_ParseDaily (sd_body) == 0) SD_CalcComfort ();  /* 逐日到手才算舒适度, 6 小时一次 */
    }
    else if (kind == ESP_REQ_LIFE)  CW_ParseLife (sd_body);
}

/* SD_NetRefresh —— 该刷新就去取, 但绝不在这一圈里等网络
 * 用法：主循环每圈调一次, 传 RTC 秒;  不带返回值, 刷屏由 SD_UpdateXxx 负责
 * 参数：epoch 当前 Unix 秒
 * 返回：无
 * 说明：每圈只干一件事, 三种情况互斥:
 *       ① ESP8266_Poll 还在跑  -> 推进一步, 本圈到此为止(别的请求一个都不发);
 *       ② 刚跑完(返回 2)      -> 就地解析/校时, 本圈也到此为止;
 *       ③ 空闲                 -> 看谁到期了, 发一个就走。
 *       所以一圈最多发一条请求, 且请求从发起到完成跨了上百圈主循环,
 *       时间照跳、底部轮播照走, 不再有"卡 1.2 秒"这回事;
 *       请求里用时间全在 esp8266.c 那边自己数, 这里只管周期。
 *       实况 10 分钟 / 逐日 6 小时 / 生活指数 24 小时 / 校时 24 小时 */
void SD_NetRefresh (u32 epoch)
{
    u8 r;

    r = ESP8266_Poll ();                 /* 先把上一个请求往前推一步 */

    if (r == 2)                          /* 刚跑完: 结果就地落地 */
    {
        SD_NetDone ();
        return;
    }
    if (r == 1) return;                  /* 还在跑: 这一圈不再发新请求 */

    if (sd_net_started == 0)             /* 开机已经取过一轮, 第一次只对齐时刻 */
    {
        sd_net_started = 1;
        sd_t_now   = epoch;
        sd_t_daily = epoch;
        sd_t_life  = epoch;
        sd_t_time  = epoch;
        return;
    }

    if (epoch - sd_t_now >= SD_NOW_SEC)              /* 实况: 10 分钟 */
    {
        sd_t_now = epoch;                            /* 先记时刻再发, 失败也等下一周期 */
        ESP8266_ReqStart (ESP_REQ_NOW, sd_body, CW_BODY_MAX);
        return;
    }
    if (epoch - sd_t_daily >= SD_DAILY_SEC)          /* 逐日: 6 小时 */
    {
        sd_t_daily = epoch;
        ESP8266_ReqStart (ESP_REQ_DAILY, sd_body, CW_BODY_MAX);
        return;
    }
    if (epoch - sd_t_life >= SD_LIFE_SEC)            /* 生活指数: 24 小时 */
    {
        sd_t_life = epoch;
        ESP8266_ReqStart (ESP_REQ_LIFE, sd_body, CW_BODY_MAX);
        return;
    }
    if (epoch - sd_t_time >= SD_TIME_SEC)            /* 网络校时: 24 小时 */
    {
        sd_t_time = epoch;
        ESP8266_ReqStart (ESP_REQ_TIME, sd_body, CW_BODY_MAX);
        return;
    }
}
