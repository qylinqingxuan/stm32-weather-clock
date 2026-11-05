/*****************************************************************************
 * 整机接线总表 (改动了引脚记得连这里和对应驱动头文件一起改)
 *
 * ============================================================================
 * 【LCD 屏幕】QDtech TFT 128x160 —— 硬件 SPI1(9MHz)，宏在 Hardware/Lcd_Driver.h
 *     STM32 PA5  (SCL)    --->  LCD SCL
 *     STM32 PA7  (SDA)    --->  LCD SDA
 *     STM32 PA4  (CS)     --->  LCD CS
 *     STM32 PB1  (RS/DC)  --->  LCD RS
 *     STM32 PB0  (RST)    --->  LCD RST
 *     STM32 PB10 (背光)   --->  LCD LED
 *     3.3V ---> LCD VCC,  GND <--> LCD GND
 *
 * ============================================================================
 * 【DHT11 温湿度】单总线，宏在 Hardware/DHT11.h
 *     STM32 PA6 (DATA)    <-->  DHT11 DATA
 *     3.3V ---> DHT11 VCC,  GND <--> DHT11 GND
 *     三针模块自带上拉；四针裸 Sensor 需在 DATA 与 VCC 间加 4.7k~10k 上拉
 *
 * ============================================================================
 * 【串口一】USART1 —— 接电脑（USB-TTL 模块）
 *   参数：9600，8 数据位，无校验，1 停止位（9600 8N1）
 *
 *     STM32 PA9  (USART1_TX)  --->  USB-TTL 模块的 RXD
 *     STM32 PA10 (USART1_RX)  <---  USB-TTL 模块的 TXD
 *     STM32 GND               <-->  USB-TTL 模块的 GND      【必须共地】
 *
 * ============================================================================
 * 【串口二】USART2 —— 接 ESP-01S
 *   参数：115200，8 数据位，无校验，1 停止位（115200 8N1）
 *
 *     STM32 PA2  (USART2_TX)  --->  ESP-01S 的 RXD
 *     STM32 PA3  (USART2_RX)  <---  ESP-01S 的 TXD
 *     STM32 GND               <-->  ESP-01S 的 GND          【必须共地】
 *
 *   注意：TX/RX 必须交叉连接（STM32 的 TX 接模块的 RX），接成直连会完全无响应。
 *
 * ============================================================================
 * 【ESP-01S 供电】是 3.3V 器件，绝对不能接 5V
 *
 *     VCC    <---  3.3V   电源需能提供 500mA 峰值电流
 *                         建议在模块 VCC-GND 之间就近并 100uF 电解 + 0.1uF 瓷片电容
 *     CH_PD  <---  3.3V   使能脚（EN），必须拉高，悬空时模块完全不工作
 *     GND    --->  GND
 *     RST    --->  可悬空（内部上拉）；需手动复位时接按键到 GND
 *
 *   如果 USB-TTL 是 5V 电平，PA3（接收）不要直连 5V，需确认模块 TXD 为 3.3V 电平
 *   或加电平转换，否则可能损坏 STM32 引脚。
 *
 *****************************************************************************/
#include "stm32f10x.h"
#include "delay.h"
#include "QDTFT.h"
#include "Lcd_Driver.h"
#include "GUI.h"
#include "Serial.h"
#include "esp8266.h"
#include "rtc.h"
#include "cw_data.h"
#include "screen_data.h"
#include "debug.h"      // 唯一的串口输出口, 平时是空壳
#include "UI_Config.h"
#include "Boot_Screen.h"
#include "DHT11.h"

/* Board_Init —— 系统底座初始化: 时基 + 双串口
 * 用法: main 里第一个调用, 顺序不能动
 * 参数: 无 / 返回: 无
 * 说明: delay 必须最先初始化(后面所有人都用 delay_ms); 串口也必须在这里
 *       打开, 否则 Dbg_Report 里第一句 Serial_Printf 就死在等 TXE 上 */
static void Board_Init(void)
{
    SystemInit();
    delay_init(72);      // delay_ms 依赖 SysTick, 必须最先初始化
    delay_dwt_init();    // 再开 DWT 周期计数器, 给 delay_millis 用(校时算网络耗时)

    Serial_Init(1);  // 调试串口, Dbg_Report 走这一路; 不初始化会死在等 TXE 上
    Serial_Init(2);  // ESP-01S 这一路 115200
}

/* Screen_Init —— 点亮屏 + 画开屏首帧 + 测滚动条带长度
 * 用法: 在 Board_Init 之后调用
 * 参数: 无 / 返回: 无 */
static void Screen_Init(void)
{
    Lcd_Init();          // 屏先点亮, 后面才有地方画开屏
    Boot_Screen_Init();  // 开屏第一帧: 标题 + 分割线 + 空进度条

}

/* Net_Init —— 联网阶段: 配 RTC + 连 WiFi + 取时间 + 取天气, 全程喂开屏进度
 * 用法: main 里 Screen_Init 之后调一次
 * 参数: 无 / 返回: 无
 * 说明: 失败就直接画 ERROR 页并停在这儿, 不返回 —— 带着坏网络往下跑,
 *       主界面只会显示一堆假数据, 不如把故障码亮在屏上;
 *       ESP8266_Progress 是驱动层留的回调口, 这里把开屏函数挂上去,
 *       esp8266.c 因此不需要知道有屏这么个东西 */
static void Net_Init(void)
{
    u8 ret;
    u32 ts;
    u16 cost;

    Boot_Screen_Step(BOOT_STEP_RTC);  // 第 0 步: 等 LSE 起振 + 配 RTC
    ret = RTC_Config();               // 0 = 用外部晶振, 1 = 退回内部 LSI

    ESP8266_Progress = Boot_Screen_Step;  // 挂上回调, 之后 1~5 步由驱动层自己报
    ret = ESP8266_Init();

    if (ret != 0)
    {
        Boot_Screen_Error(ret);           // 红 ERROR + 故障名 + 复位提示
        while (1) { }                     // 停死, 等人按复位
    }

    Boot_Screen_Step(BOOT_STEP_NTP);              // 第 6 步: 取网络时间
    cost = 0;
    ret = Get_Time(sd_body, CW_BODY_MAX, &cost);   // 响应拷进 sd_body; cost 带回单程网络 + 收尾耗时

    ts = 0;                                       // 失败时保持 0, 一眼看出"没试过"
    if (ret == 0) ts = CW_GetTimeStamp(sd_body);   // 解析交给 cw_data, 驱动层不碰 JSON
    if (ts != 0)
    {
        // 服务器给的是"它生成那一刻"的秒数, 到写进 RTC 已经过去好几秒, 得补回来:
        // cost 是实测的网络单程 + 收尾, TIME_BIAS_MS 是接口自身偏差(实测值), +500 是四舍五入到秒
        ts = ts + (u32)((cost + TIME_BIAS_MS + 500) / 1000);
        RTC_SetEpoch(ts);                         // 拿到真时间才写, 否则保留 RTC 原来的走时
    }

    Boot_Screen_Step(BOOT_STEP_WEATHER);       // 第 7 步: 取实况天气
    ret = Get_Weather(sd_body, CW_BODY_MAX);   // 同一个暂存区, 上一条时间响应已经用完, 可以覆盖

    if (ret == 0)  // 请求成功才有得解析; 结果落在 cw.now 里, 后面主界面直接读
    {
        ret = CW_ParseNow(sd_body);
    }

    // 逐日预报紧跟着实况跑, 开屏进度不用新加一步 —— 对用户来说都算"在取天气"
    ret = Get_Daily(sd_body, CW_BODY_MAX);

    if (ret == 0)
    {
        ret = CW_ParseDaily(sd_body);
    }

    ret = SD_CalcComfort ();  // 逐日到手就算舒适度: 输入全来自 daily[0], 不碰 DHT11

    // 生活指数紧跟着逐日跑, 同样不占开屏新的一步
    ret = Get_Life(sd_body, CW_BODY_MAX);

    if (ret == 0)
    {
        ret = CW_ParseLife(sd_body);
    }
}

/* main —— 程序入口: 三段初始化 + 主循环三任务, 具体实现都在上面的函数里
 * 用法: 烧录后自动运行, 不需要调用
 * 参数: 无 / 返回: 不返回 (最后是 while(1) 死循环)
 * 说明: 想知道卡在哪一步, 把 debug.h 里的 DEBUG_EN 改成 1, 再在 Net_Init 后加一行 Dbg_Report() */
int main(void)
{
	u32 epoch;

    Board_Init();   // 时钟 + 双串口, 最先做
    Screen_Init();  // 屏点亮 + 开屏首帧, 之后每一步都有地方显示进度

    Net_Init();     // RTC + WiFi + 取数; 失败会停在 ERROR 页, 不返回

    Boot_Screen_Step(BOOT_STEP_DHT11);  // 第 8 步: DHT11 上电要静置 1 秒
	delay_ms(1500);
    DHT11_Init();                    // 返回值早就不用了, 失败也就是读不到数

    Boot_Screen_Step(BOOT_STEP_UI);  // 第 9 步: 该画主界面了
    delay_ms(250);
    Boot_Screen_Done();              // 满格 + All done, 让人看清收尾
    delay_ms(700);
    SD_UpdateWeather();               // 先把真值装进 sd_weather, 主界面一上来就是真数据
    WeatherClock_UI();               // 整屏覆盖, 开屏到此结束

    while (1)
    {

		epoch = RTC_GetEpoch();
		WeatherClock_Tick(epoch);     /* 时间/日期: 内部判重, 没变一个像素都不写 */

		SD_NetRefresh(epoch);         // 到点才去取, 一圈最多发一个请求

		if (SD_UpdateIndoor(epoch))       /* 室内温湿度: 同一秒读一次 DHT11, 值变了才重画 */
			WeatherClock_SetIndoor();

        if (SD_UpdateWeather())          /* 天气有变化才重画那一行, 没变一个像素都不写 */
            WeatherClock_RefreshWeather();

        if (SD_UpdateRow())              /* 6 页轮播文案重建过才重画最后一行 */
            WeatherClock_RowRefresh();

        ESP8266_EchoPoll();              /* 桌宠动画已停, 这里只转发模块回显 */
        WeatherClock_PollRow(epoch); // 最后一行 6 页轮播, 每圈推一帧, 不阻塞
        WeatherClock_PollPet(epoch);   // 桌宠: 呼吸/眨眼/整点爱心/22-7 点睡觉(同时渐暗到 20%)
		
        delay_ms(12);   // 主循环节拍: 也是换页的帧间隔, 17 帧合计约 0.3 秒
    }
}
