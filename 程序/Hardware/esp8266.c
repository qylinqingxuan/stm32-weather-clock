#include "stm32f10x.h"
#include "Serial.h"
#include "delay.h"
#include "esp8266.h"
#include "secrets.h"      /* WiFi 与心知 key 的真值, 不进版本库 */
#include <stdio.h>
#include <string.h>

/* ==================== 全局量 ====================
 * 下面三个是早期版本留下的, 目前没有任何地方读写;
 * 后续做数据解析时可以直接复用: time 存 Unix 秒, code 存天气代码, temperature 存气温 */
long long int time;
int code;
int temperature;

/* ==================== WiFi 账号密码 ====================
 * 真值在 secrets.h 里(不进版本库), 换网络改那个文件, 本文件一行不用动 */
const u8* wifista_ssid =      WIFI_SSID;
const u8* wifista_password =  WIFI_PASSWORD;

/* ==================== 服务器地址 ====================
 * 时间接口: api.pinduoduo.com, 返回 {"server_time":毫秒时间戳}
 * 天气接口: api.seniverse.com (心知天气)
 * 端口都是 80 且只能走 http —— 裸 AT 指令不支持 TLS, 写 https 必然失败 */
#define WEATHER_PORTNUM 	"80"
#define WEATHER_SERVERIP 	"api.seniverse.com"

#define TIME_PORTNUM		"80"
#define TIME_SERVERIP		"api.pinduoduo.com"

/* ==================== 心知天气参数 ====================
 * key 和 city 在 secrets.h 里(不进版本库); days 决定返回几天 */
#define DAILY_DAYS      "3"
#define LIFE_DAYS       "1"

/* ==================== 回显开关 ====================
 * 1 = 把 ESP-01S 收到的每个字节原样转给电脑串口, 调试必开;
 * 0 = 关闭, 正式固件可以关掉少占点时间 */
u8 ESP8266_Echo = 0;
ESP8266_Diag esp_diag;  // 全部诊断状态, 谁都不打印, 交给 Dbg_Report
void (*ESP8266_Progress)(u8 step) = 0;  // 开屏进度回调, main 里挂 Boot_Screen_Step; 空指针就跳过
static uint16_t s_echo_pos = 0;

/* ESP8266_EchoPoll —— 把 ESP-01S 新回传的字节转发到电脑串口 (纯调试用)
 * 用法：放在主循环里反复调用即可; 不想看回显就把 ESP8266_Echo 置 0
 * 参数：无
 * 返回：无
 * 说明：用 s_echo_pos 记住已经转发到哪, 只发新增的部分, 不会重复也不会漏;
 * 		 缓冲被 ESP8266_Clear 清过时游标会跑到长度前面, 这里会自动归零重来 */
void ESP8266_EchoPoll (void)
{
    uint16_t len = USART2_RX_LEN;  // 当前已收到多长, 只转发新来的那一段

    if (len < s_echo_pos) s_echo_pos = 0;  // 缓冲被清过, 游标跑到了长度前面, 归零重来
    if (ESP8266_Echo == 0) { s_echo_pos = len; return; }  // 关掉回显时只更新游标, 不真的发

    if (len > s_echo_pos)
    {
        Serial_SendArray (1, (uint8_t *)&USART2_RX_BUF[s_echo_pos], (uint16_t)(len - s_echo_pos));
        s_echo_pos = len;
    }
}

/* ESP8266_Clear —— 清空 ESP-01S 的接收缓冲, 每条 AT 指令发出前都要调
 * 用法：ESP8266_Clear();  Serial_SendString(2, "AT\r\n");
 * 参数：无
 * 返回：无
 * 说明：先把还没转发的数据甩给电脑(免得丢应答尾巴), 再关中断把长度清零;
 *       关中断是为了防止"清到一半又进来一个字节", 全程 1us 左右, 不影响收数 */
void ESP8266_Clear (void)
{
    uint32_t primask;

    ESP8266_EchoPoll ();

    primask = __get_PRIMASK();  // 记住原来的中断状态, 清完要恢复, 不能把中断永久关掉
    __disable_irq();
    USART2_RX_LEN = 0;  // 长度归零就等于清空, 不用 memset 1KB
    USART2_RX_BUF[0] = '\0';
    __set_PRIMASK(primask);
}

/* ESP8266_WaitResult —— 在接收缓冲里轮询等待关键字
 * 用法：if (ESP8266_WaitResult("OK", "ERROR", 1000) == 1) 成功;
 * 参数：ack_ok  成功关键字, 如 "OK" / "ready" / "WIFI GOT IP"
 *       ack_err 失败关键字, 传 NULL 表示不关心失败
 *       timeout 最多等多少毫秒
 * 返回：1 = 等到 ack_ok; 0 = 先命中 ack_err; 2 = 超时
 * 说明：每 10ms 查一次, 查之前先转发回显, 所以电脑上能看到完整对话;
 *       判断的是"缓冲区里有没有这个词", 不区分是哪条指令的回应 */
u8 ESP8266_WaitResult (const char *ack_ok, const char *ack_err, uint16_t timeout)
{
    uint16_t t = 0;
    while (t < timeout)
    {
        ESP8266_EchoPoll ();
        if (ack_err != NULL && strstr (USART2_RX_BUF, ack_err) != NULL) return 0;  // 先判失败, 命中失败词立刻返回 0
        if (strstr (USART2_RX_BUF, ack_ok) != NULL) return 1;  // 再看成功词, 命中就返回 1
        delay_ms (10);
        t += 10;
    }
    return 2;
}

/* ESP8266_SendCmd —— 发一条 AT 指令并等回应, 最常用的封装
 * 用法：ESP8266_SendCmd("AT+CIPMUX=0\r\n", "OK", 1000);
 * 参数：cmd 完整指令, 必须自己带 \r\n; ack_ok 成功关键字; timeout 超时毫秒
 * 返回：1 = 成功; 0 = 收到 ERROR 或超时
 * 说明：内部会先 ESP8266_Clear, 所以只适合发"不需要看历史回应"的指令;
 *       失败关键字固定按 "ERROR" 判 */
u8 ESP8266_SendCmd (char *cmd, const char *ack_ok, uint16_t timeout)
{
    ESP8266_Clear ();  // 先清缓冲, 免得上次留下的 OK 被当成本次的
    Serial_SendString (2, cmd);  // 指令原样发, 结尾的 \r\n 由调用者自己带
    return (ESP8266_WaitResult (ack_ok, "ERROR", timeout) == 1) ? 1 : 0;
}

/* ESP8266_CopyOut —— 把接收缓冲里当前的内容整段拷到调用方给的数组
 * 用法：ESP8266_CopyOut (body, cap);   // 由 Get_xxx 在收完响应后调
 * 参数：dst 目标数组; cap 目标容量(含结尾的 '\0')
 * 返回：实际拷到的字节数
 * 说明：为什么要拷: 接收缓冲是全局的, 下一次请求的 ESP8266_Clear 就把它清了,
 *       谁也不能保证调用方什么时候才去读; 拷一份出去, 数据的命就归调用方了;
 *       超过 cap-1 就截断, 保证不越界 —— 键值都在 JSON 前部, 截尾不影响解析 */
static u16 ESP8266_CopyOut (char *dst, u16 cap)
{
    u16 n = 0;

    if (dst == 0 || cap < 2) return 0;  // 没给地方就别拷

    while (n < (u16)(cap - 1) && USART2_RX_BUF[n] != '\0')  // 遇到结束符就停, 不会跑出数组
    {
        dst[n] = USART2_RX_BUF[n];
        n++;
    }
    dst[n] = '\0';  // 补结束符, 调用方可以直接当字符串用
    return n;
}

/* ESP8266_LogHead —— 把接收缓冲开头若干个字节记进 esp_diag, 只给诊断用
 * 用法：失败 return 之前调一次, 事后由 Dbg_Report 统一打印
 * 参数：n 最多记几个字节(上限 47)
 * 返回：无
 * 说明：这里一个字都不往串口写 —— 全部攒进 esp_diag, 正式固件也能安静记录;
 *       控制字符统一换成 '.', 保证一行能打完, 不会把串口助手的光标拉回行首;
 *       fail_len 记的是 USART2_RX_LEN, 能和 fail_head 对上就知道
 *       是"没收到"还是"收到了别的" */
static void ESP8266_LogHead (u8 n)
{
    u16 i;

    if (n > 47) n = 47;
    for (i = 0; i < n && USART2_RX_BUF[i] != '\0'; i++)
    {
        esp_diag.fail_head[i] = (USART2_RX_BUF[i] < 32) ? '.' : USART2_RX_BUF[i];
    }
    esp_diag.fail_head[i] = '\0';
    esp_diag.fail_len = USART2_RX_LEN;
}

/* ESP8266_Close —— 退出透传并把 TCP 连接关干净, 四个 Get_xxx 收尾统一走这里
 * 用法：ESP8266_Close (ESP8266_WaitResult ("CLOSED", NULL, 1500));
 * 参数：closed 等 CLOSED 的结果, 1 = 服务器已经断连
 * 返回：无
 * 说明：+++ 前后各静默 1 秒才会被认成退出序列, 这是模块的硬性规定;
 *       刚收完一大包数据就发 +++, 它只当普通数据灌进 TCP 连接, 透传根本退不出来,
 *       后面的 AT+CIPCLOSE / AT+CIPSTART 就全成了发给服务器的乱码(实测回 400);
 *       服务器断连时模块自己会退出透传, 这种正常情况直接关就行, 省掉 2 秒空等 */
static void ESP8266_Close (u8 closed)
{
    if (closed != 1)  // 没等到断连: 多半还挂在透传里, 得按规矩慢慢退
    {
        delay_ms (1000);  // 前静默: 这 1 秒里 UART 上不能有任何字节
        Serial_SendString (2, "+++");  // 三个加号, 不带 \r\n
        delay_ms (1000);  // 后静默: 同样 1 秒
        ESP8266_WaitResult ("OK", NULL, 1000);  // 回 OK 才是真退出来了
    }
    Serial_SendString (2, "AT+CIPCLOSE\r\n");  // 不能走 SendCmd —— 它内部先 Clear, 会把刚收到的响应一起删掉
    ESP8266_WaitResult ("OK", NULL, 200);  // 只等 200ms: 连接早断了它会回 ERROR, 没必要干等
}

/* ESP8266_Init —— 初始化 ESP-01S 并连上 WiFi, 开机调用一次
 * 用法：if (ESP8266_Init() != 0) 屏幕上提示联网失败;
 * 参数：无 (SSID 和密码取文件顶部的 wifista_ssid / wifista_password)
 * 返回：0 = 成功; 1 = 模块无响应; 2 = 复位失败; 3 = STA 模式设置失败;
 *       4 = WiFi 连接失败; 5 = CIPMUX 设置失败
 * 说明：流程 = 退透传(+++) -> 等 ready -> AT 探测(10 次) -> AT+RST -> CWMODE=1
 *       -> CWJAP 等 OK -> CIPMUX=0 -> CIPMODE=0;
 *       **第一步必须是退透传**: MCU 复位时 ESP 一直挂着电, 很有可能还停在 CIPMODE=1,
 *       透传模式下模块把收到的字节全转发进 TCP, AT 指令不解析, 探多久都是死寂;
 *       顺序反了(先探 AT 再救)就是"上电偶发 NO MODULE"的真正原因,
 *       因为是否卡在透传取决于上一次复位时程序走到了哪一步;
 *       最坏会阻塞约 85 秒(探 AT 5s + 兜底 4s + 复位 5s + CWJAP 3x20s),
 *       所以要先显示"正在联网"再调它;
 *       串口由 main 在系统初始化阶段统一打开, 本函数不再碰硬件层; */
u8 ESP8266_Init (void)
{
    char cmd[128];
    u8 retry;
    u8 done;
    u8 probe;
    u8 escaped;

    if (ESP8266_Progress) ESP8266_Progress(1);  // 开屏进度 1: 开始探模块

    /* 第 1 步: 先赌一把"退透传" —— 这一步必须在等 ready / 探 AT 之前
     * MCU 复位时 ESP 一直挂着电, 它自己不会退出 CIPMODE=1;
     * 透传模式下模块把收到的字节原样转发进 TCP, AT 指令压根不解析,
     * 所以先探 AT 再退透传, 等于白等 5 秒才去救 —— 顺序反了。
     * 若本来就在 AT 模式, 这三个字节只会变成一条无效指令(模块回 ERROR), 没有任何副作用 */
    done = (strstr (USART2_RX_BUF, "ready") != NULL) ? 1 : 0;  // 顺手记下: RTC 那步可能已经把 ready 打完了
    ESP8266_Clear ();
    delay_ms (100);                        // +++ 前面必须留静默(固件要求 >=20ms, 这里给足)
    Serial_SendString (2, "+++");          // 绝不能带 \r\n: 带了就变成数据发进 TCP, 转义序列直接失效
    delay_ms (100);                        // 后面同样要静默, 模块就是在这段里回 OK
    escaped = (ESP8266_WaitResult ("OK", NULL, 300) == 1) ? 1 : 0;
    Serial_SendString (2, "\r\n");        // 补个换行: 若刚才在 AT 模式, +++ 会卡在指令缓冲里, 用 \r\n 冲掉它
    delay_ms (30);
    ESP8266_Clear ();
    esp_diag.escape = escaped;  // 1 = 复位前本来就在透传, 已经被救出来了

    /* 第 2 步: 等模块自己开口 —— 只在没退过透传、也没见过 ready 时才需要
     * 上电头 1~2 秒模块在跑启动日志, 这段时间它还没进 AT 解析器, 敲 AT 是白发;
     * 退过透传说明模块从头到尾没断过电, 启动日志早就打完了, 再等就是白等 3 秒 */
    if (escaped == 0 && done == 0)
    {
        ESP8266_WaitResult ("ready", NULL, 3000);  // 最多等 3 秒, 等不到也往下走
        ESP8266_Clear ();
    }

    /* 第 3 步: 探 AT, 10 次 x 500ms = 5 秒
     * 原来只给 2.5 秒(5 次), 碰上启动慢一点的模块就会误判成"没有模块" */
    probe = 0;
    for (retry = 0; retry < 10; retry++)
    {
        if (ESP8266_SendCmd ("AT\r\n", "OK", 500) == 1) { probe = 1; break; }
    }

    /* 第 4 步: 还是没反应, 再退一次透传重试 —— 第 1 步可能正好赶上模块还在启动日志里,
     * 静默期被日志打断, +++ 就没被认出来。这一步失败也不马上判死, 再给 3 次 AT 机会;
     * 原来只探一次就 return 1, 正是"偶尔报 NO MODULE"的直接原因 */
    if (probe == 0)
    {
        ESP8266_Clear ();
        delay_ms (1000);               // 前面 1 秒: 等模块把启动日志吐完, 顺便保证 +++ 前的静默
        Serial_SendString (2, "+++");
        delay_ms (100);                // 后面 100ms 静默, 不能省
        ESP8266_WaitResult ("OK", NULL, 600);
        Serial_SendString (2, "\r\n");
        delay_ms (30);
        ESP8266_Clear ();
        for (retry = 0; retry < 3; retry++)
        {
            if (ESP8266_SendCmd ("AT\r\n", "OK", 500) == 1) { probe = 1; break; }
        }
    }
    esp_diag.probe = probe;
    if (probe == 0) return (esp_diag.init_ret = 1);

    ESP8266_Clear ();
    if (ESP8266_Progress) ESP8266_Progress(2);  // 开屏进度 2: 复位模块
    Serial_SendString (2, "AT+RST\r\n");  // 软复位, 等固件打印 ready
    if (ESP8266_WaitResult ("ready", NULL, 5000) != 1)  // 有些固件复位后不打印 ready, 用 AT 探一下兜底
    {
        if (ESP8266_SendCmd ("AT\r\n", "OK", 1000) == 0)
        {
            /* 失败码记在 init_ret 里, 不用单独打 */
            return (esp_diag.init_ret = 2);
        }
    }
    ESP8266_Clear ();

    if (ESP8266_Progress) ESP8266_Progress(3);  // 开屏进度 3: 切 Station 模式
    for (retry = 0; retry < 3; retry++)
    {
        uint16_t t = 0;
        done = 0;
        ESP8266_Clear ();
        Serial_SendString (2, "AT+CWMODE=1\r\n");  // 1 = Station 模式, 当客户端去连路由器
        while (t < 1500)
        {
            ESP8266_EchoPoll ();
            if (strstr (USART2_RX_BUF, "no change") != NULL) { done = 1; break; }  // 本来就是 STA 模式时回 no change, 也算成功
            if (strstr (USART2_RX_BUF, "OK") != NULL)        { done = 1; break; }
            if (strstr (USART2_RX_BUF, "ERROR") != NULL)     { break; }
            delay_ms (10);
            t += 10;
        }
        if (done == 1) break;
    }
    if (retry >= 3)
    {
        return (esp_diag.init_ret = 3);
    }

    if (ESP8266_Progress) ESP8266_Progress(4);  // 开屏进度 4: 连 WiFi, 这步最慢
    for (retry = 0; retry < 3; retry++)
    {
        sprintf (cmd, "AT+CWJAP=\"%s\",\"%s\"\r\n", (const char *)wifista_ssid, (const char *)wifista_password);  // 拼入 SSID/密码, 引号要转义
        ESP8266_Clear ();
        Serial_SendString (2, cmd);
        // 这里等 "OK" 而不是等 "WIFI GOT IP": 模块上电默认会自动连 WiFi,
        // 等它连好之后再发 CWJAP, 模块只回一个 OK, 根本不会再有 GOT IP ——
        // 死等 GOT IP 的话会白等 20 秒 x3, 最后还误报成"WiFi 连接失败"
        if (ESP8266_WaitResult ("OK", "FAIL", 20000) == 1) break;
        delay_ms (500);
    }
    if (retry >= 3)
    {
        return (esp_diag.init_ret = 4);
    }

    if (ESP8266_Progress) ESP8266_Progress(5);  // 开屏进度 5: 建 TCP 链接
    for (retry = 0; retry < 3; retry++)
    {
        if (ESP8266_SendCmd ("AT+CIPMUX=0\r\n", "OK", 1000) == 1) break;  // 透传只支持单连接
        delay_ms (300);
    }
    if (retry >= 3)
    {
        return (esp_diag.init_ret = 5);
    }

    // 顺手把透传关掉: 上电残留的 CIPMODE=1 不会自己消失, 留着它后面发 AT+CIPSEND 时行为会很别扭
    ESP8266_SendCmd ("AT+CIPMODE=0\r\n", "OK", 1000);

    return (esp_diag.init_ret = 0);
}

/* ==================== 非阻塞取数: 状态机 ====================
 * 为什么要这一套: 四个 Get_xxx 原来把"建链 -> 透传 -> 发 GET -> 等响应 -> 关链"
 * 全串在一个函数里, 主循环一进去就是 0.6~1.2 秒出不来, 秒位直接跳过去、
 * 底部轮播那一行也停在半路。改成"发一步、等一步"的站点式状态机以后,
 * 主循环每圈只往里推一小步(扫一次缓冲或发一条命令), 节拍照旧。
 *
 * 用法(周期刷新):  ESP8266_ReqStart(...) 之后每圈调 ESP8266_Poll(),
 *                 返回 2 的那一圈说明跑完了, 用 ESP8266_Result() 取结果。
 * 用法(开机):     直接调 Get_xxx(), 它在里面死等 Poll, 做事和改造前一模一样。 */

/* 请求类型: 决定连哪台服务器、发哪条 GET、回来交给谁解析 */
#define ESP_REQ_NONE    0
#define ESP_REQ_TIME    1
#define ESP_REQ_NOW     2
#define ESP_REQ_DAILY   3
#define ESP_REQ_LIFE    4

/* 站点: 一次请求从建链到关链要走的路 */
#define ESP_ST_IDLE     0    /* 空闲, 没有请求 */
#define ESP_ST_CONN     1    /* 已发 CIPSTART, 等 CONNECT */
#define ESP_ST_MODE     2    /* 已发 CIPMODE=1, 等 OK */
#define ESP_ST_PROMPT   3    /* 已发 CIPSEND, 等 '>' */
#define ESP_ST_WAIT     4    /* 已发 GET, 等 '}' */
#define ESP_ST_CLOSE    5    /* 等服务器 CLOSED */
#define ESP_ST_QUIET1   6    /* +++ 前静默 1 秒 */
#define ESP_ST_QUIET2   7    /* +++ 后静默 1 秒 */
#define ESP_ST_ESC      8    /* 等 +++ 的 OK */
#define ESP_ST_CIPCLOSE 9    /* 等 AT+CIPCLOSE 的 OK */
#define ESP_ST_END      10   /* 收尾: 算 cost, 回空闲 */

/* ESP_POLL_GAP —— 两次推进之间至少隔这么多毫秒
 * 为什么需要: 阻塞版的 WaitResult 每 10ms 才扫一次缓冲, 扫到 CONNECT / '>' / '}' 时
 *             距模块真正吐出那个串已经过了 0~10ms, 换句话说「看到回话」到「发下一条命令」
 *             之间天然带着一段间隔; 状态机在开机紧循环里每圈只有几微秒, 白送的间隔被抹成 0,
 *             模块还没把上一条收完就收到新命令, 回 busy p... 或者干脆丢掉, 于是整条链崩掉。
 *             这里把那段间隔显式补回来: 不到点就只返回, 不做事, 主循环照转。 */
#define ESP_POLL_GAP    10

static u8   s_req;       /* 正在跑的请求类型, 0 = 没请求 */
static u8   s_st;        /* 走到哪个站点 */
static u8   s_armed;     /* 0 = 这一站的动作还没发; 1 = 已发, 正在等答复 */
static u8   s_ret;       /* 结果码 0~4, 含义和 Get_xxx 的返回值一致 */
static u8   s_closed;    /* 1 = 服务器主动断的, 关链可以抄近路 */
static u16  s_got;       /* 拷出去的字节数 */
static u16  s_cost;      /* 时间接口的 cost_ms */
static u32  s_t_step;    /* 这一站的起始时刻, delay_millis 的毫秒 */
static u32  s_t_send;    /* 发出 GET 的时刻 */
static u32  s_t_recv;    /* 收到 '}' 的时刻 */
static char *s_body;     /* 调用者的响应缓冲 */
static u16  s_cap;       /* 缓冲容量 */
static char s_cmd[160];  /* 拼 AT 命令 / GET 行 */
static u8   s_esc_try;   /* 1 = 这次收尾已经试过一次 +++ 强制退透传 */
static u32  s_t_poll;   /* 上一次真正推进站点的时刻, 用来压住紧循环的轮询速度 */

/* ESP_Scan —— 在接收缓冲里找关键字, 看一眼就走, 不等待
 * 用法: r = ESP_Scan ("OK", "ERROR");
 * 参数: ok  成功关键字, 必给
 *       err 失败关键字, 给 0 表示这一站不判失败
 * 返回: 1 = 等到 ok; 0 = 等到 err; 2 = 两个都还没有
 * 说明: 和 ESP8266_WaitResult 就差在没有 delay_ms, 专供状态机每圈调;
 *       strstr 扫一遍 1KB 缓冲几十微秒, 一圈一次完全无感 */
static u8 ESP_Scan (const char *ok, const char *err)
{
    if (err != 0 && strstr (USART2_RX_BUF, err) != 0) return 0;
    if (strstr (USART2_RX_BUF, ok) != 0) return 1;
    return 2;
}

/* ESP_SendGet —— 把这一类的请求行 + Host 头发出去
 * 用法: 只在收到 '>' 之后调一次
 * 参数: 无(读 s_req)
 * 返回: 无
 * 说明: 时间接口路径固定, 三个天气接口只差一段 URL, 服务器是同一台;
 *       HTTP/1.0 不带 Content-Length, 服务器回完自己断, 靠 CLOSED 收尾 */
static void ESP_SendGet (void)
{
    const char *srv;

    srv = (s_req == ESP_REQ_TIME) ? TIME_SERVERIP : WEATHER_SERVERIP;

    if (s_req == ESP_REQ_TIME)
    {
        Serial_SendString (2, "GET /api/server/_stm HTTP/1.0\r\n");
    }
    else
    {
        if (s_req == ESP_REQ_NOW)
            sprintf (s_cmd, "GET /v3/weather/now.json?key=%s&location=%s&language=en&unit=c HTTP/1.0\r\n",
                     WEATHER_KEY, WEATHER_CITY);
        else if (s_req == ESP_REQ_DAILY)
            sprintf (s_cmd, "GET /v3/weather/daily.json?key=%s&location=%s&language=en&unit=c&start=0&days=%s HTTP/1.0\r\n",
                     WEATHER_KEY, WEATHER_CITY, DAILY_DAYS);
        else
            sprintf (s_cmd, "GET /v3/life/suggestion.json?key=%s&location=%s&language=en&days=%s HTTP/1.0\r\n",
                     WEATHER_KEY, WEATHER_CITY, LIFE_DAYS);
        Serial_SendString (2, s_cmd);
    }

    sprintf (s_cmd, "Host: %s\r\nConnection: close\r\n\r\n", srv);
    Serial_SendString (2, s_cmd);
}

/* ESP_Abort —— 某一站失败, 记下结果码并选收尾路线
 * 用法: ESP_Abort (3);
 * 参数: ret 结果码 1~4
 * 返回: 无
 * 说明: 1/2 还没进透传, 链原样留着, 直接收尾;
 *       3/4 已经在透传里了, 必须走 +++ 那一套把链拆干净, 否则下次 CIPSTART
 *       撞 "ALREADY CONNECTED", 一撞就次次失败, 只能复位 */
static void ESP_Abort (u8 ret)
{
    ESP8266_LogHead (40);            /* 把缓冲开头几个字节留档, 事后能看出是什么错 */
    s_ret    = ret;
    s_got    = 0;
    s_armed  = 0;
    s_closed = 0;
    /* 1/2 失败时大概率是模块还赖在透传里(上一条 AT 被当成 TCP 数据吞了),
     * 直接 END 会把它留在"连接没关 + 透传没退"的状态, 下一次 CIPSTART 必炸;
     * 所以也走一遍 +++ 退出+关链的收尾, 让下一个周期有个干净的开始 */
    s_esc_try = (ret >= 3) ? 0 : 1;
    s_st      = (ret >= 3) ? ESP_ST_CLOSE : ESP_ST_QUIET1;
}

/* ESP8266_ReqStart —— 发一次取数请求, 立刻返回
 * 用法: ESP8266_ReqStart (ESP_REQ_NOW, sd_body, CW_BODY_MAX);
 * 参数: kind 请求类型 ESP_REQ_xxx
 *       body 响应缓冲(状态机跑完会往里拷)
 *       cap  缓冲容量
 * 返回: 无
 * 说明: 上一个还没走完就直接不理, 免得两套状态互相踩;
 *       这里只负责"把这次请求跑到底", 多长时间刷一次由调用者自己数 */
void ESP8266_ReqStart (u8 kind, char *body, u16 cap)
{
    if (s_st != ESP_ST_IDLE) return;

    s_req    = kind;
    s_body   = body;
    s_cap    = cap;
    s_ret    = 0;
    s_got    = 0;
    s_closed = 0;
    s_cost   = 0;
    s_armed  = 0;
    s_esc_try = 0;
    s_t_poll  = delay_millis ();
    s_st     = ESP_ST_CONN;
}

/* ESP8266_Poll —— 把当前请求往前推进一步, 主循环每圈调一次
 * 用法: r = ESP8266_Poll ();
 * 参数: 无
 * 返回: 0 = 空闲(没有请求); 1 = 还在跑; 2 = 刚跑完(结果用 ESP8266_Result 取)
 * 说明: 每次调用最多发一条命令或扫一次缓冲, 全程不等, 主循环节拍不受影响;
 *       计时用 delay_millis, 不碰 SysTick, 也不影响 delay_ms;
 *       返回 2 只在收尾那一圈出现一次, 下一圈就回到 0 */
u8 ESP8266_Poll (void)
{
    u32 now;
    u8  r;

    if (s_st == ESP_ST_IDLE) return 0;

    ESP8266_EchoPoll ();          /* 请求期间照样能看透传原文(Echo 打开时) */
    now = delay_millis ();

    /* 站间节流: 刚推进过就只让出 CPU, 不碰状态也不发命令 */
    if ((u32)(now - s_t_poll) < ESP_POLL_GAP) return 1;
    s_t_poll = now;

    switch (s_st)
    {
    /* ---- 第 1 站: 建 TCP 连接 ---- */
    case ESP_ST_CONN:
        if (s_armed == 0)
        {
            ESP8266_Clear ();
            sprintf (s_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s\r\n",
                     (s_req == ESP_REQ_TIME) ? TIME_SERVERIP : WEATHER_SERVERIP,
                     (s_req == ESP_REQ_TIME) ? TIME_PORTNUM  : WEATHER_PORTNUM);
            Serial_SendString (2, s_cmd);
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        r = ESP_Scan ("CONNECT", "ERROR");
        if (r == 1)
        {
            s_st    = ESP_ST_MODE;
            s_armed = 0;
        }
        else if (r == 0 || (u32)(now - s_t_step) >= 10000u)
        {
            ESP_Abort (1);
        }
        break;

    /* ---- 第 2 站: 切透传 ---- */
    case ESP_ST_MODE:
        if (s_armed == 0)
        {
            ESP8266_Clear ();
            Serial_SendString (2, "AT+CIPMODE=1\r\n");
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        r = ESP_Scan ("OK", "ERROR");
        if (r == 1)
        {
            s_st    = ESP_ST_PROMPT;
            s_armed = 0;
        }
        else if (r == 0 || (u32)(now - s_t_step) >= 2000u)
        {
            ESP_Abort (2);
        }
        break;

    /* ---- 第 3 站: 等 '>' 提示符 ---- */
    case ESP_ST_PROMPT:
        if (s_armed == 0)
        {
            ESP8266_Clear ();
            Serial_SendString (2, "AT+CIPSEND\r\n");
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        r = ESP_Scan (">", 0);
        if (r == 1)
        {
            s_st    = ESP_ST_WAIT;
            s_armed = 0;
        }
        else if ((u32)(now - s_t_step) >= 2000u)
        {
            ESP_Abort (3);
        }
        break;

    /* ---- 第 4 站: 发 GET, 等 JSON 里的第一个 '}' ---- */
    case ESP_ST_WAIT:
        if (s_armed == 0)
        {
            ESP8266_Clear ();
            s_t_send = now;           /* 时间接口算单程用, 从现在开始掐表 */
            ESP_SendGet ();
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        r = ESP_Scan ("}", 0);
        if (r == 1)
        {
            s_t_recv = now;
            s_st     = ESP_ST_CLOSE;
            s_armed  = 0;
        }
        else if ((u32)(now - s_t_step) >= 10000u)
        {
            ESP_Abort (4);
        }
        break;

    /* ---- 第 5 站: 等服务器断连, 顺手把响应拷出去 ---- */
    case ESP_ST_CLOSE:
        if (s_armed == 0)
        {
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        r = ESP_Scan ("CLOSED", 0);
        if (r == 1)
        {
            if (s_closed == 0)        /* 第一次看到 CLOSED: 从这一刻重新起表 */
            {
                s_closed = 1;
                s_t_step = now;
            }
            if ((u32)(now - s_t_step) < 100u) break;  /* 打印 CLOSED 到真正退出透传有几毫秒, 等一等 */
        }
        else if ((u32)(now - s_t_step) < 1500u)
        {
            break;                    /* 还没到, 接着等下一圈 */
        }
        /* 到了, 或者等超了: 先把数据拷出来(失败路径没数据), 再定怎么关链 */
        if (s_ret == 0)
        {
            s_got = ESP8266_CopyOut (s_body, s_cap);
            esp_diag.ok_len = s_got;
        }
        s_st    = (s_closed == 1) ? ESP_ST_CIPCLOSE : ESP_ST_QUIET1;
        s_armed = 0;
        break;

    /* ---- 关链 A: 服务器已经断了, +++ 省掉 ---- */
    case ESP_ST_QUIET1:
        if (s_armed == 0)
        {
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        if ((u32)(now - s_t_step) < 1000u) break;   /* 静默没满 1 秒 */
        Serial_SendString (2, "+++");
        s_st    = ESP_ST_QUIET2;
        s_armed = 0;
        break;

    /* ---- 关链 B: +++ 之后还得静默 1 秒, 否则 OK 会被吞 ---- */
    case ESP_ST_QUIET2:
        if (s_armed == 0)
        {
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        if ((u32)(now - s_t_step) < 1000u) break;
        s_st    = ESP_ST_ESC;
        s_armed = 0;
        break;

    /* ---- 关链 C: 等 +++ 的 OK, 等不到也往下走 ---- */
    case ESP_ST_ESC:
        if (s_armed == 0)
        {
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        r = ESP_Scan ("OK", 0);
        if (r == 1 || (u32)(now - s_t_step) >= 1000u)
        {
            s_st    = ESP_ST_CIPCLOSE;
            s_armed = 0;
        }
        break;

    /* ---- 关链 D: 关连接, 200ms 等不到就算了 ---- */
    case ESP_ST_CIPCLOSE:
        if (s_armed == 0)
        {
            /* 必须先清: 缓冲区里还压着刚收的 HTTP 响应, 头一行就是 "HTTP/1.0 200 OK",
             * 不清的话 ESP_Scan("OK") 立刻假命中, 等于根本没等模块回话 */
            ESP8266_Clear ();
            Serial_SendString (2, "AT+CIPCLOSE\r\n");
            s_armed  = 1;
            s_t_step = now;
            break;
        }
        r = ESP_Scan ("OK", "ERROR");   /* 链接已经断了时模块回 ERROR, 一样算收干净 */
        if (r != 2)
        {
            s_st    = ESP_ST_END;
            s_armed = 0;
            break;
        }
        if ((u32)(now - s_t_step) < 300u) break;
        if (s_esc_try == 0)             /* 一句都不回: 模块还在透传里, 走一次 +++ 把它拽出来 */
        {
            s_esc_try = 1;
            s_st      = ESP_ST_QUIET1;
            s_armed   = 0;
            break;
        }
        s_st    = ESP_ST_END;           /* 拽过一次还不行就别耗着了, 交给下一个周期 */
        s_armed = 0;
        break;

    /* ---- 收尾: 算 cost, 回空闲 ---- */
    case ESP_ST_END:
        if (s_req == ESP_REQ_TIME && s_ret == 0)
        {
            /* 往返时间取一半算单程, 再加"收到 } 到关完链"的尾巴 */
            s_cost = (u16)((s_t_recv - s_t_send) / 2) + (u16)(now - s_t_recv);
            esp_diag.time_cost = s_cost;
        }
        esp_diag.last_ret = s_ret;
        s_armed = 0;
        s_st    = ESP_ST_IDLE;
        return 2;

    default:
        s_st = ESP_ST_IDLE;
        return 2;
    }

    return 1;
}

/* ESP8266_Result —— 取上一次请求的结果码
 * 用法: if (ESP8266_Poll () == 2) r = ESP8266_Result ();
 * 参数: 无
 * 返回: 0 = 成功; 1 = TCP 建链失败; 2 = 透传失败; 3 = CIPSEND 失败; 4 = 10 秒没收全
 * 说明: 值一直保留到下次 ReqStart, 所以 Poll 返回 2 之后什么时候取都行 */
u8 ESP8266_Result (void)
{
    return s_ret;
}

/* ESP8266_Kind —— 取上一次请求的类型
 * 用法: switch (ESP8266_Kind ()) { case ESP_REQ_NOW: ... }
 * 参数: 无
 * 返回: ESP_REQ_xxx
 * 说明: 轮询式调用不知道刚跑完的是哪一个, 靠这个分派解析函数 */
u8 ESP8266_Kind (void)
{
    return s_req;
}

/* ESP8266_Cost —— 取时间接口的 cost_ms
 * 用法: ts += (ESP8266_Cost () + TIME_BIAS_MS + 500) / 1000;
 * 参数: 无
 * 返回: 网络单程 + 收尾, 毫秒; 只有 ESP_REQ_TIME 成功时才有意义
 * 说明: 别的类型一律 0 */
u16 ESP8266_Cost (void)
{
    return s_cost;
}

/* ==================== 取数接口: 状态机薄封装 ====================
 * 四个 Get_xxx 不再自带一套同步流程, 全部交给上面的状态机:
 *   发起 -> 原地死等 ESP8266_Poll -> 取结果
 * 开机 Net_Init 走这条(阻塞无所谓, 一次性); 主循环走 SD_NetRefresh 的非阻塞路径,
 * 同一个状态机, 只是不在这里死等。两处行为完全一致, 不存在"两套实现" */

/* Get_Time 函数 —— 取一次网络时间(同步壳, 只给开机用)
 * 用法: u16 cost;  if (Get_Time (body, CW_BODY_MAX, &cost) == 0) ...
 * 参数: body 响应缓冲; cap 缓冲容量; cost_ms 可选, 填往返延时(可传 0)
 * 返回: 0 成功; 1 建链失败; 2 透传失败; 3 CIPSEND 失败; 4 没收到 }
 * 说明: 里面的 while (Poll() == 1) 就是"原地等网络", 网络那几步全在状态机里走 */
u8 Get_Time (char *body, u16 cap, u16 *cost_ms)
{
    while (ESP8266_Poll () == 1) { }            // 上一条的收尾还没走完, 先让它收干净
    ESP8266_ReqStart (ESP_REQ_TIME, body, cap); // 发起, 立刻返回
    while (ESP8266_Poll () == 1) { }            // 死等这一条走完(开机路径, 等得起)
    if (cost_ms != 0) *cost_ms = ESP8266_Cost ();  // 失败时 Cost 是 0, 不会污染校时
    return ESP8266_Result ();
}

/* Get_Weather 函数 —— 取一次实况天气(心知 now.json, language=en)
 * 用法: if (Get_Weather (body, CW_BODY_MAX) == 0) CW_ParseNow (body);
 * 参数: body 响应缓冲; cap 缓冲容量(超了截断)
 * 返回: 同 Get_Time
 * 说明: en 是因为心知中文返回 UTF-8, 而屏幕字库是 GBK, 直接显示就是乱码 */
u8 Get_Weather (char *body, u16 cap)
{
    while (ESP8266_Poll () == 1) { }
    ESP8266_ReqStart (ESP_REQ_NOW, body, cap);
    while (ESP8266_Poll () == 1) { }
    return ESP8266_Result ();
}

/* Get_Daily 函数 —— 取逐日预报(daily.json, 从今天起 DAILY_DAYS 天)
 * 用法: if (Get_Daily (body, CW_BODY_MAX) == 0) CW_ParseDaily (body);
 * 参数: body 响应缓冲; cap 缓冲容量
 * 返回: 同 Get_Time
 * 说明: start=0 表示从今天开始, 第 0 个元素就是今天; 3 天约 998 字节 */
u8 Get_Daily (char *body, u16 cap)
{
    while (ESP8266_Poll () == 1) { }
    ESP8266_ReqStart (ESP_REQ_DAILY, body, cap);
    while (ESP8266_Poll () == 1) { }
    return ESP8266_Result ();
}

/* Get_Life 函数 —— 取生活指数(suggestion.json, days=1 只取当天)
 * 用法: if (Get_Life (body, CW_BODY_MAX) == 0) CW_ParseLife (body);
 * 参数: body 响应缓冲; cap 缓冲容量
 * 返回: 同 Get_Time
 * 说明: 一次返回 20 多项, 每项有 brief(短词) 和 details(长句), 屏幕只取 brief */
u8 Get_Life (char *body, u16 cap)
{
    while (ESP8266_Poll () == 1) { }
    ESP8266_ReqStart (ESP_REQ_LIFE, body, cap);
    while (ESP8266_Poll () == 1) { }
    return ESP8266_Result ();
}
