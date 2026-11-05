#ifndef __ESP8266_H__
#define __ESP8266_H__

#include "stm32f10x.h"

extern long long int time;
extern int code;                        /* 与 esp8266.c 中的定义保持一致（原来误写成 uint16_t）*/
extern int temperature;

/* ---- 基础通信层：清缓冲 / 等应答 / 发命令 ---- */
void ESP8266_Clear (void);
u8   ESP8266_WaitResult (const char *ack_ok, const char *ack_err, uint16_t timeout);
u8   ESP8266_SendCmd (char *cmd, const char *ack_ok, uint16_t timeout);

/* ---- 测试用：把 USART2 收到的原始数据转发到 USART1（电脑串口助手） ---- */
extern u8 ESP8266_Echo;

/* ---- 开屏进度回调: 每走到一个关键节点就调一次, 参数是步骤号 1~5 ---- */
extern void (*ESP8266_Progress)(u8 step);
void ESP8266_EchoPoll (void);

/* ---- 诊断记录: 各层不再直接打印, 状态攒在这里, 由 App/debug.c 的 Dbg_Report 统一输出 ---- */
typedef struct
{
    u8   escape;
    u8   probe;
    u8   init_ret;
    u8   last_ret;
    u16  ok_len;
    u16  fail_len;
    u16  time_cost;
    char fail_head[48];
} ESP8266_Diag;

extern ESP8266_Diag esp_diag;

/* ---- 非阻塞取数: 发起 / 每圈推进 / 取结果 ---- */
#define ESP_REQ_TIME    1   /* 网络时间 */
#define ESP_REQ_NOW     2   /* 实况 */
#define ESP_REQ_DAILY   3   /* 逐日预报 */
#define ESP_REQ_LIFE    4   /* 生活指数 */

void ESP8266_ReqStart (u8 kind, char *body, u16 cap);
u8   ESP8266_Poll (void);
u8   ESP8266_Result (void);
u8   ESP8266_Kind (void);
u16  ESP8266_Cost (void);

/* ---- 初始化并接入 WiFi，返回 0 表示成功 ---- */
u8   ESP8266_Init (void);

/* 接口自身的时间偏差(毫秒): 实测 api.pinduoduo.com 的 server_time 比真实时间慢约 1.47 秒 */
#define TIME_BIAS_MS   1470

u8   Get_Time (char *body, u16 cap, u16 *cost_ms);  /* 0=完整走完 1=建链 2=透传 3=CIPSEND 4=无响应; body 收响应, cost_ms 带回收尾耗时(毫秒) */
u8   Get_Weather (char *body, u16 cap);     /* 同上; 响应拷进 body, cap 是容量 */
u8   Get_Daily (char *body, u16 cap);     /* 同上; 逐日预报, 响应拷进 body */
u8   Get_Life (char *body, u16 cap);                       /* 生活指数，返回码同上 */

#endif
