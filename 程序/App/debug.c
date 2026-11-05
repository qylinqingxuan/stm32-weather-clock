/* debug.c —— 整个工程唯一的串口输出口 (默认关闭)
 *
 * 为什么要有这一层：
 *   以前各个 .c 在自己里面 Serial_Printf, 开机要吐 [esp] escape / AT probe,
 *   每 20 分钟还要吐一行 [wx] ok, body=1187 bytes。9600 波特下每字节约 1ms,
 *   一行 26 字节就是 26ms 的主循环阻塞, 而且串口永远安静不下来。
 *   现在改成: 各层只往自己的全局量里记状态(纯赋值, 零阻塞),
 *   想看了再调本文件的 Dbg_Report() 一次性读出来。
 *
 * 怎么用：
 *   1) 把 debug.h 里的 DEBUG_EN 改成 1;
 *   2) 在 main.c 里想要的位置加一行 Dbg_Report();  (看完记得删)
 *      - 加在 Net_Init() 之后    -> 整轮开机取数的结果
 *      - 加在 while(1) 里       -> 实时状态, 一次约 1KB, 9600 下要 1 秒多, 别频繁调
 *   3) DEBUG_EN = 0 时 Dbg_Report 是个空函数, 全部字符串在编译期就没了。
 *
 * 数据从哪来：
 *   esp_diag (esp8266.c)  -> 联网各步的返回码、body 长度、失败时的响应开头
 *   cw       (cw_data.c)  -> 解析出来的天气/生活指数 + valid 标志
 *   sd_*     (screen_data.c) -> 实际显示用的舒适度/天气行/室内温湿度/6 页轮播
 *   RTC_IsValid()         -> 有没有校到时间
 *   全部是"最近一次真实发生的事", 不依赖调用时机。 */

#include "debug.h"

#if DEBUG_EN

#include <stdio.h>
#include "Serial.h"
#include "esp8266.h"
#include "rtc.h"
#include "cw_data.h"
#include "screen_data.h"

/* 生活指数四项的中文名, 顺序必须和 cw_data.h 里的 CW_LIFE_xxx 逐一对应 */
static const char *const s_life_name[CW_LIFE_NUM] =
{
    "感冒", "紫外线", "空气", "舒适度"
};

/* Dbg_Report —— 把整机当前状态一次性打到调试串口 (仅调试用)
 * 用法：在 main 里临时加一行 Dbg_Report();
 * 参数：无 / 返回：无
 * 说明：只读全局量, 不改状态、不刷屏。输出顺序就是一次取数的完整链路:
 *       底座 -> 联网 -> 实况 -> 逐日 -> 生活 -> 三块上屏数据 -> 6 页轮播。
 *       取数失败的项不跳过, 照样打出来, 用 valid 和返回码看卡在哪一环。 */
void Dbg_Report (void)
{
    DateTime dt;
    u8 k;

    Serial_Printf(1, "\r\n===== dbg report: begin =====\r\n");

    /* 一、底座: flash 容量是芯片出厂固化的, 只读寄存器; RTC 只看有没有校到时间 */
    Serial_Printf(1, "boot: flash = %d KB\r\n",
                  *(volatile unsigned short *)0x1FFFF7E0);
    Serial_Printf(1, "rtc: valid = %u (0 表示还没校到时间, 屏幕显示 --:--:--)\r\n",
                  RTC_IsValid());

    /* 二、联网: 全部状态都从 esp_diag 里读, 驱动层自己一个字都不打印 */
    Serial_Printf(1, "esp: init = %u, escape = %u, probe = %u\r\n",
                  esp_diag.init_ret, esp_diag.escape, esp_diag.probe);
    Serial_Printf(1, "esp last: ret = %u, time cost = %u ms\r\n",
                  esp_diag.last_ret, esp_diag.time_cost);
    if (esp_diag.last_ret == 0)
        Serial_Printf(1, "esp body: %u bytes\r\n", esp_diag.ok_len);
    else
        Serial_Printf(1, "esp FAIL: %u bytes in buf, head = %s\r\n",
                      esp_diag.fail_len, esp_diag.fail_head);

    /* 三、当前时间 */
    RTC_GetDateTime(&dt);
    Serial_Printf(1, "clock: %04d-%02d-%02d %02d:%02d:%02d week = %u, epoch = %lu\r\n",
                  dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec, dt.week,
                  RTC_GetEpoch());

    /* 四、实况 */
    Serial_Printf(1, "now: valid = %u | text = %s, code = %u, temp = %d\r\n",
                  cw.now.valid, cw.now.text, cw.now.code, cw.now.temp);
    Serial_Printf(1, "now update: %s\r\n", cw.now_update);

    /* 五、逐日: 每天两行, 第二行那几个字段解析了但没上屏 */
    for (k = 0; k < CW_DAILY_NUM; k++)
    {
        Serial_Printf(1, "daily[%u]: %s, code = %u, high = %d, low = %d, precip = %u%%, humi = %u%%, wind = %u.%u km/h\r\n",
                      k, cw.daily[k].date, cw.daily[k].code_day,
                      cw.daily[k].high, cw.daily[k].low, cw.daily[k].precip,
                      cw.daily[k].humidity,
                      cw.daily[k].wind_speed / 10, cw.daily[k].wind_speed % 10);
        Serial_Printf(1, "           deg = %u, scale = %u, rain = %u.%u mm\r\n",
                      cw.daily[k].wind_deg, cw.daily[k].wind_scale,
                      cw.daily[k].rainfall / 10, cw.daily[k].rainfall % 10);
    }
    Serial_Printf(1, "daily update: %s\r\n", cw.daily_update);

    /* 六、生活指数: 英文原词 + 翻出来的中文, 一眼看清哪里缺映射 */
    for (k = 0; k < CW_LIFE_NUM; k++)
    {
        if (cw.life[k].valid == 0)
            Serial_Printf(1, "life[%u] %s: 接口没返回\r\n", k, s_life_name[k]);
        else if (cw.life[k].mapped)
            Serial_Printf(1, "life[%u] %s: \"%s\" -> %s\r\n",
                          k, s_life_name[k], cw.life[k].en, cw.life[k].brief);
        else
            Serial_Printf(1, "life[%u] %s: \"%s\" [?] 没见过的值, 待补映射表\r\n",
                          k, s_life_name[k], cw.life[k].en);
    }
    Serial_Printf(1, "life update: %s\r\n", cw.life_update);

    /* 七、上屏数据: 屏幕读到什么就打什么, 和上面 cw 里的值对照着看 */
    Serial_Printf(1, "comfort: I = %d.%02d -> %s (level %u)\r\n",
                  sd_comfort.index / 100, sd_comfort.index % 100,
                  sd_comfort.text, sd_comfort.level);
    Serial_Printf(1, "weather row: valid = %u, code = %u, temp = %d, high = %d, low = %d\r\n",
                  sd_weather.valid, sd_weather.code, sd_weather.temp,
                  sd_weather.high, sd_weather.low);
    Serial_Printf(1, "indoor: valid = %u, temp = %s, humi = %u%%\r\n",
                  sd_indoor.valid, sd_indoor.tstr, sd_indoor.humi);

    /* 八、最后一行轮播: 6 页文案, 空串表示这页数据还没到 */
    for (k = 0; k < SD_ROW_PAGES; k++)
        Serial_Printf(1, "row[%u]: %s\r\n", k, sd_row[k]);

    Serial_Printf(1, "===== dbg report: end =====\r\n");
}

#else  /* DEBUG_EN == 0 */

/* 关掉时留个空壳: 主程序里写 Dbg_Report(); 照样编得过, 不用再套 #if */
void Dbg_Report (void)
{
}

#endif
