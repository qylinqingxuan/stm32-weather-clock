/******************************************************************************
 * DHT11 温湿度传感器驱动 (单总线时序, GPIO 模拟收发)
 *
 * 接线 (默认 PA6, 换脚只需改 DHT11.h 里的三个宏):
 *   STM32 PA6  <-->  DHT11 DATA
 *   3.3V       --->  DHT11 VCC
 *   GND        <-->  DHT11 GND
 *   三针模块版自带上拉电阻; 四针裸 Sensor 需在 DATA 与 VCC 之间加 4.7k~10k 上拉。
 *   DHT11 上电后要稳定 1 秒以上才能读; 两次读取间隔应不小于 1 秒。
 *
 * 用法:
 *   DHT11_Init();                    // 返回 0 = 传感器在, 1 = 无应答
 *   DHT11_Read_Data(&t, &h);         // 返回 0 = 成功, 1 = 无应答, 2 = 校验错
 ******************************************************************************/

#include "stm32f10x.h"
#include "delay.h"
#include "DHT11.h"

/* DHT11_IO_OUT —— 把 DATA 脚切成推挽输出, 主机发起始信号用
 * 参数: 无 / 返回: 无 */
void DHT11_IO_OUT(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.GPIO_Pin   = DHT11_IO;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(DHT11_PORT, &GPIO_InitStructure);
}

/* DHT11_IO_IN —— 把 DATA 脚切成浮空输入, 收数据用
 * 说明: 总线上有外部上拉, 空闲时读回高电平; 浮空输入足够, 不抢总线电平
 * 参数: 无 / 返回: 无 */
void DHT11_IO_IN(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.GPIO_Pin  = DHT11_IO;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(DHT11_PORT, &GPIO_InitStructure);
}

/* DHT11_RST —— 发起始信号: 拉低 20ms 再拉高 30us
 * 原理: DHT11 规定主机至少拉低 18ms 表示"我要读了", 再放开等它应答
 * 参数: 无 / 返回: 无 */
void DHT11_RST(void)
{
    DHT11_IO_OUT();
    GPIO_ResetBits(DHT11_PORT, DHT11_IO);
    delay_ms(20);
    GPIO_SetBits(DHT11_PORT, DHT11_IO);
    delay_us(30);
}

/* DHT11_Check —— 等 DHT11 应答
 * 原理: 主机放开总线后, DHT11 会先拉低 40~80us 再拉高 40~80us;
 *       两段都要等到才算它在
 * 参数: 无 / 返回: 0 = 应答正常, 1 = 超时无应答 */
u8 DHT11_Check(void)
{
    u8 retry = 0;
    DHT11_IO_IN();
    while ((GPIO_ReadInputDataBit(DHT11_PORT, DHT11_IO) == 1) && retry < 100)
    {
        retry++;
        delay_us(1);
    }
    if (retry >= 100) return 1;
    retry = 0;
    while ((GPIO_ReadInputDataBit(DHT11_PORT, DHT11_IO) == 0) && retry < 100)
    {
        retry++;
        delay_us(1);
    }
    if (retry >= 100) return 1;
    return 0;
}

/* DHT11_Init —— 开时钟 + 发起始信号 + 等应答
 * 参数: 无 / 返回: 0 = 检测到 DHT11, 1 = 无应答 */
u8 DHT11_Init(void)
{
    RCC_APB2PeriphClockCmd(DHT11_RCC, ENABLE);
    DHT11_RST();
    return DHT11_Check();
}

/* DHT11_Read_Bit —— 读一位数据
 * 原理: 每位都以 50us 低电平开头, 之后的高电平 26~28us 是 0、70us 是 1;
 *       等高电平出现后延时 40us 再采样, 处于两者正中间, 不会认错
 * 参数: 无 / 返回: 1 或 0 */
u8 DHT11_Read_Bit(void)
{
    u8 retry = 0;
    while ((GPIO_ReadInputDataBit(DHT11_PORT, DHT11_IO) == 1) && retry < 100)
    {
        retry++;
        delay_us(1);
    }
    retry = 0;
    while ((GPIO_ReadInputDataBit(DHT11_PORT, DHT11_IO) == 0) && retry < 100)
    {
        retry++;
        delay_us(1);
    }
    delay_us(40);
    if (GPIO_ReadInputDataBit(DHT11_PORT, DHT11_IO) == 1) return 1;
    else return 0;
}

/* DHT11_Read_Byte —— 读一个字节 = 高位在前收 8 位
 * 参数: 无 / 返回: 读到的字节 */
u8 DHT11_Read_Byte(void)
{
    u8 i, dat;
    dat = 0;
    for (i = 0; i < 8; i++)
    {
        dat <<= 1;
        dat |= DHT11_Read_Bit();
    }
    return dat;
}

/* DHT11_Read_Data —— 读一次完整数据 (湿度 16bit + 温度 16bit + 校验 8bit)
 * 说明: 40 位数据帧 = [湿度整数][湿度小数][温度整数][温度小数][校验],
 *       四个有效字节全部带出; 温度小数字节 DHT11 一般恒为 0, 读到 0 是正常的
 * 参数: out = 结果结构体指针, 失败时 *out 不做任何修改
 * 返回: 0 = 成功, 1 = 无应答, 2 = 校验错
 * 用法: DHT11_TH th;  DHT11_Read_Data(&th);  温度 = th.temp_i.th.temp_d 度
 */
u8 DHT11_Read_Data(DHT11_TH *out)
{
    u8 buf[5];
    u8 i;
    DHT11_RST();
    if (DHT11_Check() == 1) return 1;
    for (i = 0; i < 5; i++)
    {
        buf[i] = DHT11_Read_Byte();
    }
    if ((((u16)buf[0] + buf[1] + buf[2] + buf[3]) & 0xFF) != buf[4]) return 2;
    out->humi_i = buf[0];
    out->humi_d = buf[1];
    out->temp_i = buf[2];
    out->temp_d = buf[3];
    return 0;
}

static u8  s_dht_busy = 0;   /* 0 = 空闲, 1 = 已拉低 DATA 正在计时 */
static u32 s_dht_t0;         /* 拉低那一刻的毫秒时基 */

/* DHT11_Start —— 拉低 DATA 发起始信号, 拉完就返回, 不等 18ms
 * 用法: DHT11_Start();  之后主循环反复调 DHT11_Poll() 往前推
 * 参数: 无 / 返回: 无
 * 说明: 手册要求主机拉低至少 18ms 表示"要取数"; 原来这 18ms 是死等,
 *       每秒把主循环卡住一次, 现在是三分之一个阻塞里最长的一段, 先把它拆掉;
 *       拉低之后引脚由我们保持, 中间去干别的事不会影响 DHT11 */
void DHT11_Start (void)
{
    DHT11_IO_OUT();
    GPIO_ResetBits(DHT11_PORT, DHT11_IO);
    s_dht_t0   = delay_millis();
    s_dht_busy = 1;
}

/* DHT11_Poll —— 拉低满 18ms 就抬线, 并一口气读回 40 位数据
 * 用法: r = DHT11_Poll(&th);  // 反复调, 直到 r != DHT11_BUSY
 * 参数: out 结果结构体指针, 只有返回 DHT11_OK 时才被写
 * 返回: DHT11_BUSY 还没到点 / DHT11_OK 成功 / DHT11_NORES 无响应 / DHT11_CRC 校验错
 * 说明: 抬线后的 40 位不能拆——位间隔只有 50us, 中间撒手就丢帧,
 *       所以这里仍有约 5ms 占着 CPU, 但比原来 25~30ms 短了一个数量级;
 *       拉低超过 100ms 说明主循环被别的事情拖住了, 这一次直接放弃,
 *       抬线走人, 等下一秒的 DHT11_Start 重来 */
u8 DHT11_Poll (DHT11_TH *out)
{
    u8  buf[5];
    u8  i;
    u32 dt;

    if (s_dht_busy == 0) return DHT11_BUSY;   /* 没开工, 当还在忙 */

    dt = delay_millis() - s_dht_t0;           /* 无符号减, 计数器回绕也成立 */
    if (dt < 18) return DHT11_BUSY;           /* 还没拉够 18ms, 下一圈再看 */

    s_dht_busy = 0;
    GPIO_SetBits(DHT11_PORT, DHT11_IO);       /* 抬线, 把总线交给 DHT11 */
    DHT11_IO_IN();                            /* 立刻切输入, 别和 DHT11 抢线 */
    if (dt > 100) return DHT11_NORES;         /* 拖太久, 这次不读了 */

    delay_us(30);                             /* DHT11 在 20~40us 内拉低应答 */
    if (DHT11_Check() == 1) return DHT11_NORES;
    for (i = 0; i < 5; i++)
    {
        buf[i] = DHT11_Read_Byte();
    }
    if ((((u16)buf[0] + buf[1] + buf[2] + buf[3]) & 0xFF) != buf[4]) return DHT11_CRC;

    out->humi_i = buf[0];
    out->humi_d = buf[1];
    out->temp_i = buf[2];
    out->temp_d = buf[3];
    return DHT11_OK;
}
