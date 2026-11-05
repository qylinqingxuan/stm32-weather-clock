#include "rtc.h"
#include "delay.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_rtc.h"
#include "stm32f10x_bkp.h"
#include "stm32f10x_pwr.h"

#define RTC_VALID_FLAG       0xA5A5

#define RTC_MIN_VALID_EPOCH  1600000000u

#define RTC_TZ_OFFSET_SEC    (8u * 3600u)
#define SECS_PER_DAY         86400u

/* civil_from_days —— 内部工具: 把"1970-01-01 起的天数"换算成 年/月/日
 * 用法：只在 RTC_DateTimeFromEpoch 内部用, 应用层不用管
 * 参数：z 自 1970-01-01 起的天数; yy/mm/dd 分别带回年、月、日
 * 返回：无 (结果通过指针带回)
 * 说明：Howard Hinnant 的纯整数算法, 不调任何 C 库函数,
 *       所以没有 localtime 那套时区/locale 的坑; 2100 这类非闰世纪年也正确 */
static void civil_from_days (u32 z, u32 *yy, u32 *mm, u32 *dd)
{
    u32 era, doe, yoe, doy, mp;

    z    += 719468;  // 把纪元起点从 1970 挪到 0000-03-01, 算法要求
    era   = z / 146097;  // 400 年一个 era, 一个 era 共 146097 天
    doe   = z - era * 146097;
    yoe   = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy   = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp    = (5 * doy + 2) / 153;

    *dd   = doy - (153 * mp + 2) / 5 + 1;
    *mm   = (mp < 10) ? (mp + 3) : (mp - 9);  // 内部以 3 月为 1 月, 这里换回正常月份
    *yy   = (yoe + era * 400) + (u32)(*mm <= 2);
}

/* RTC_Config —— 初始化 RTC 走时, 开机调用一次
 * 用法：rtc_src = RTC_Config();   // 0 表示用上了外部晶振
 * 参数：无
 * 返回：0 = 用外部 32.768kHz 晶振 (准确)
 *       1 = 外部晶振 3 秒内没起振, 已退化为内部 LSI (误差大, 一天可能差几十秒)
 * 说明：蓝色小板晶振虚焊 / 负载电容不匹配很常见, 所以必须做超时退化,
 *       否则会死在等 LSERDY 上, 现象就是开机卡住不往下跑 */
u8 RTC_Config (void)
{
    u8  use_lsi = 0;
    u16 timeout = 0;

    RCC_APB1PeriphClockCmd (RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);  // RTC 在备份域, 必须先开 PWR 和 BKP 时钟
    PWR_BackupAccessCmd (ENABLE);  // 解锁备份域, 不解锁 RTC 写不进去

    RCC_LSEConfig (RCC_LSE_ON);  // 启动外部 32.768kHz 晶振
    while (RCC_GetFlagStatus (RCC_FLAG_LSERDY) == RESET)
    {
        delay_ms (10);
        if (++timeout > 300) { use_lsi = 1; break; }  // 等 3 秒还没起振就换内部 LSI, 不能死等
    }

    if (use_lsi == 1)
    {
        RCC_LSEConfig (RCC_LSE_OFF);
        RCC_LSICmd (ENABLE);  // 退化方案: 打开内部低速时钟
        timeout = 0;
        while (RCC_GetFlagStatus (RCC_FLAG_LSIRDY) == RESET)
        {
            delay_ms (1);
            if (++timeout > 200) break;
        }
        RCC_RTCCLKConfig (RCC_RTCCLKSource_LSI);
        RCC_RTCCLKCmd (ENABLE);
        RTC_WaitForSynchro ();
        RTC_WaitForLastTask ();
        RTC_SetPrescaler (40000 - 1);  // LSI 标称 40kHz, 分频到 1Hz; 实际偏差大, 只能兜底
    }
    else
    {
        RCC_RTCCLKConfig (RCC_RTCCLKSource_LSE);
        RCC_RTCCLKCmd (ENABLE);
        RTC_WaitForSynchro ();
        RTC_WaitForLastTask ();
        RTC_SetPrescaler (32768 - 1);  // 32768Hz / 32768 = 1Hz, 正好每秒加 1
    }
    RTC_WaitForLastTask ();

    return use_lsi;
}

/* RTC_IsValid —— 判断 RTC 里现在是不是真实时间
 * 用法：if (RTC_IsValid() == 0) 时间显示 "--:--";
 * 参数：无
 * 返回：1 = 已校过时且数值合理; 0 = 没校过时
 * 说明：两个条件都满足才算有效: 备份寄存器里有校时标志 + 计数器不早于 2020-09-13;
 *       板子没接 VBAT 电池, 掉电后计数器归零, 所以上电必然是 0, 联网校时后才变 1 */
u8 RTC_IsValid (void)
{
    if (BKP_ReadBackupRegister (BKP_DR1) != RTC_VALID_FLAG) return 0;  // 没写过校时标志 = 从没校过时
    if (RTC_GetCounter () < RTC_MIN_VALID_EPOCH)            return 0;  // 数值比 2020 年还早, 说明不是真时间
    return 1;
}

/* RTC_SetEpoch —— 校时: 把 Unix 秒写进 RTC 计数器
 * 用法：RTC_SetEpoch(1792994999);   // 拿到网络时间后调用
 * 参数：epoch 自 1970-01-01 00:00:00 UTC 起的秒数
 * 返回：无
 * 说明：注意单位是秒, 服务器给的 13 位毫秒要先去掉末 3 位;
 *       写完硬件每秒自动 +1, 主循环只读计数器就行, 不用再喂任何东西 */
void RTC_SetEpoch (u32 epoch)
{
    RTC_WaitForLastTask ();
    RTC_SetCounter (epoch);
    RTC_WaitForLastTask ();
    BKP_WriteBackupRegister (BKP_DR1, RTC_VALID_FLAG);  // 置校时标志, 供 RTC_IsValid 判断
}

/* RTC_GetEpoch —— 读当前 Unix 秒
 * 用法：u32 now = RTC_GetEpoch();
 * 参数：无
 * 返回：自 1970-01-01 起的秒数; 未校时时是上电后的累计秒数, 没有意义 */
u32 RTC_GetEpoch (void)
{
    return RTC_GetCounter ();
}

/* RTC_DateTimeFromEpoch —— Unix 秒换算成日历时间 (固定按 UTC+8)
 * 用法：RTC_DateTimeFromEpoch(epoch, &dt);
 *       Serial_Printf(1, "%04d-%02d-%02d\r\n", dt.year, dt.month, dt.day);
 * 参数：epoch 秒; dt 输出结构体, 不能传 NULL
 * 返回：无 (结果写进 *dt)
 * 说明：接口给的是 UTC 秒, 这里先加 8 小时再换算, 所以 dt 里直接是北京时间;
 *       week 编码 0=周日 1=周一 ... 6=周六 */
void RTC_DateTimeFromEpoch (u32 epoch, DateTime *dt)
{
    u32 secs = epoch + RTC_TZ_OFFSET_SEC;  // 先加上 8 小时时区偏移
    u32 days = secs / SECS_PER_DAY;  // 拆成"第几天"和"当天第几秒"
    u32 rem  = secs % SECS_PER_DAY;
    u32 y, m, d;

    civil_from_days (days, &y, &m, &d);

    dt->year  = (u16)y;
    dt->month = (u8)m;
    dt->day   = (u8)d;
    dt->hour  = (u8)(rem / 3600);
    dt->min   = (u8)((rem % 3600) / 60);
    dt->sec   = (u8)(rem % 60);
    dt->week  = (u8)((days + 4) % 7);  // 1970-01-01 是周四, 加 4 让编码变成 0=周日
}

/* RTC_GetDateTime —— 直接读当前日历时间 (读计数器 + 换算的合并版)
 * 用法：DateTime dt; RTC_GetDateTime(&dt);
 * 参数：dt 输出结构体, 调用前不需要初始化
 * 返回：无 */
void RTC_GetDateTime (DateTime *dt)
{
    RTC_DateTimeFromEpoch (RTC_GetEpoch (), dt);
}
