#include "stm32f10x.h"
#include "delay.h"

/* DWT 是 Cortex-M3 内核里的调试观察单元, 其中 CYCCNT 是个自由运行的周期计数器,
 * 每个 HCLK 加 1; 本工程的 CMSIS 没带 DWT 结构体, 直接按手册给的地址访问 */
#define DWT_CTRL        (*(volatile u32 *)0xE0001000)  // 控制寄存器, bit0 = CYCCNT 使能
#define DWT_CYCCNT      (*(volatile u32 *)0xE0001004)  // 周期计数, 只读
#define DWT_CYCCNTENA   (1u << 0)                      // CTRL 里的启动位
static u8  fac_us=0;
static u16 fac_ms=0;

/* delay_uptime 用的状态: 把 DWT 周期计数的增量累加成毫秒 */
static u32 up_ms;      /* 累计毫秒, 49.7 天才回绕 */
static u32 up_cyc;     /* 上次读到的周期计数 */
static u32 up_rem;     /* 不足 1ms 的周期零头, 留着下次一起算 */
static u8  up_ok;      /* 0 = 还没起过表 */

/* delay_init —— 初始化 SysTick 延时基准, delay_ms / delay_us 之前必须调用一次
 * 用法：SystemInit() 之后调用 delay_init(72);
 * 参数：SYSCLK 系统主频, 单位 MHz (72MHz 主频就填 72)
 * 返回：无
 * 说明：把 SysTick 时钟源设为 HCLK/8, 并算出微秒/毫秒所需计数值;
 *       全工程只调一次, 重复调用会覆盖 fac_us / fac_ms */
void delay_init(u8 SYSCLK)
{
	SysTick->CTRL&=0xfffffffb;  // bit2 清零 = SysTick 时钟源选 HCLK/8
	fac_us=SYSCLK/8;  // 每 1us 需要的计数值
	fac_ms=(u16)fac_us*1000;  // 每 1ms 需要的计数值
}

/* delay_ms —— 阻塞式毫秒延时
 * 用法：delay_ms(500);  // 等 500ms
 * 参数：nms 毫秒数, 72MHz 下单次最大 1864ms (SysTick 是 24 位计数器, 更大会溢出)
 * 返回：无
 * 说明：忙等, 期间 CPU 干不了别的; 但中断照常响应, 串口接收不会丢 */
void delay_ms(u16 nms)
{
	u32 temp;
	SysTick->LOAD=(u32)nms*fac_ms;  // 装载本次延时需要的计数
	SysTick->VAL =0x00;
	SysTick->CTRL=0x01 ;
	do
	{
		temp=SysTick->CTRL;
	}
	while(temp&0x01&&!(temp&(1<<16)));  // bit16 置位说明倒数到 0, 退出等待
	SysTick->CTRL=0x00;
	SysTick->VAL =0X00;
}

/* delay_us —— 阻塞式微秒延时, 用于总线时序等短延时
 * 用法：delay_us(10);
 * 参数：nus 微秒数, 同样受 24 位计数器限制, 不要超过 1000000
 * 返回：无
 * 说明：几十微秒到几毫秒用它, 长时间等待请用手 delay_ms */
void delay_us(u32 nus)
{
	u32 temp;
	SysTick->LOAD=nus*fac_us;  // 装载本次延时需要的计数
	SysTick->VAL=0x00;
	SysTick->CTRL=0x01 ;
	do
	{
		temp=SysTick->CTRL;
	}
	while(temp&0x01&&!(temp&(1<<16)));  // 同上, 等倒数结束
	SysTick->CTRL=0x00;
	SysTick->VAL =0X00;
}

/* delay_dwt_init —— 打开 DWT 周期计数器, 给 delay_millis 提供时基
 * 用法：delay_init 之后调用一次
 * 参数：无
 * 返回：无
 * 说明：DWT 是 Cortex-M3 内核自带的周期计数器, 每个 HCLK 加 1, 独立于 SysTick;
 *       72MHz 下 32 位能数 59.6 秒, 校时这种场景够用;
 *       不动 SysTick, 所以和 delay_ms / delay_us 互不干扰 */
void delay_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA;  // 使能内核跟踪单元, 不开这个 DWT 计不上数
    DWT_CYCCNT = 0;                              // 从 0 开始数
    DWT_CTRL  |= DWT_CYCCNTENA;                  // 启动计数
}

/* delay_millis —— 读 DWT 计数器换算成毫秒
 * 用法：u32 t0 = delay_millis();  ...  u32 cost = delay_millis() - t0;
 * 参数：无
 * 返回：上电以来的毫秒数, 每 59652ms 回绕一次
 * 说明：只适合算"两次采样之间过了多久", 而且要保证这段间隔不跨回绕点 ——
 *       它除完 72000 才回绕, 回绕点不是 2^32, 所以跨过回绕点的差值会算成接近 49.7 天;
 *       存"未来时刻"(now + 180000) 更是直接失效, 那种场合用 delay_uptime */
u32 delay_millis(void)
{
    // fac_us = SYSCLK/8, 是 SysTick(9MHz) 每微秒的计数; DWT 走满速 HCLK, 比它快 8 倍,
    // 所以每毫秒的 DWT 周期数 = fac_us * 8 * 1000。用 fac_us 换算的好处是跟 delay_init 传的主频严格一致
    return DWT_CYCCNT / ((u32)fac_us * 8000);
}

/* delay_uptime —— 取上电以来的毫秒数, 不会 59.6 秒回绕
 * 用法：u32 t0 = delay_uptime();  ...  u32 d = delay_uptime() - t0;
 * 参数：无
 * 返回：上电以来的毫秒数, 约 49.7 天才回绕
 * 说明：delay_millis 是"周期数先除 72000 再返回", 除完的值每 59652ms 回绕一次,
 *       拿来存未来时刻(now + 180000)会变成永远到不了的值;
 *       这里改成累加"周期计数的增量": CYCCNT 本身按 2^32 回绕, 无符号减法在回绕点
 *       算出来的差值严格正确; 不足 1ms 的零头留着下次一起算, 不丢时间
 *       (前提: 调用间隔小于 59 秒, 否则一次增量会溢出; 本工程最短 12ms 一圈) */
u32 delay_uptime(void)
{
	u32 cyc = DWT_CYCCNT;
	u32 div = (u32)fac_us * 8000;   /* 每毫秒的周期数, 72MHz 下 72000 */
	u32 dc;

	if(up_ok == 0)                  /* 第一次调用只记基准, 返回 0 */
	{
		up_ok  = 1;
		up_cyc = cyc;
		return 0;
	}

	dc = (u32)(cyc - up_cyc);       /* 模 2^32 的差值, 跨回绕点也对 */
	up_cyc = cyc;

	up_ms  += dc / div;
	up_rem += dc % div;
	if(up_rem >= div)               /* 零头攒够 1ms 就进位 */
	{
		up_rem -= div;
		up_ms++;
	}
	return up_ms;
}
