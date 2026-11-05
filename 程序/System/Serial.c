#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"
#include "misc.h"
#include <stdio.h>
#include <stdarg.h>
#include "Serial.h"

char USART1_RX_BUF[USART1_RX_BUF_SIZE] = {0};
uint16_t USART1_RX_LEN = 0;
char USART2_RX_BUF[USART2_RX_BUF_SIZE] = {0};
uint16_t USART2_RX_LEN = 0;

/* Serial_Init —— 初始化一路串口: GPIO + USART + 接收中断
 * 用法：Serial_Init(1);  // 初始化 USART1(电脑)
 *       Serial_Init(2);  // 初始化 USART2(ESP-01S)
 * 参数：USART_Name  1 = USART1(PA9 发送 / PA10 接收, 9600 8N1)
 *                    2 = USART2(PA2 发送 / PA3 接收, 115200 8N1)
 * 返回：无
 * 说明：波特率写死在函数内的分支里, 要改波特率就改对应分支的 USART_BaudRate;
 *       收到的数据由中断存进 USARTx_RX_BUF, 长度在 USARTx_RX_LEN */
void Serial_Init (uint8_t USART_Name)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;
	if (USART_Name==1)
	{
		RCC_APB2PeriphClockCmd (RCC_APB2Periph_USART1,ENABLE);  // 开 USART1 时钟
		RCC_APB2PeriphClockCmd (RCC_APB2Periph_GPIOA,ENABLE);
		GPIO_InitStructure.GPIO_Mode=GPIO_Mode_AF_PP;
		GPIO_InitStructure.GPIO_Pin=GPIO_Pin_9;  // PA9 = TX, 复用推挽输出
		GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
		GPIO_Init(GPIOA,&GPIO_InitStructure);

		GPIO_InitStructure.GPIO_Mode=GPIO_Mode_IPU;
		GPIO_InitStructure.GPIO_Pin=GPIO_Pin_10;  // PA10 = RX, 上拉输入
		GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
		GPIO_Init(GPIOA,&GPIO_InitStructure);
		USART_InitStructure.USART_BaudRate=9600;  // 电脑端串口助手也要设 9600
		USART_InitStructure.USART_HardwareFlowControl=USART_HardwareFlowControl_None;
		USART_InitStructure.USART_Mode=USART_Mode_Tx | USART_Mode_Rx;
		USART_InitStructure.USART_Parity=USART_Parity_No;
		USART_InitStructure.USART_StopBits=USART_StopBits_1;
		USART_InitStructure.USART_WordLength=USART_WordLength_8b;
		USART_Init (USART1,&USART_InitStructure);

		USART_ITConfig (USART1,USART_IT_RXNE,ENABLE);  // 开接收非空中断, 来一个字节进一次中断

		NVIC_PriorityGroupConfig (NVIC_PriorityGroup_2);  // 2 位抢占 + 2 位子优先级
		NVIC_InitStructure.NVIC_IRQChannel=USART1_IRQn;
		NVIC_InitStructure.NVIC_IRQChannelCmd=ENABLE;
		NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority=1;
		NVIC_InitStructure.NVIC_IRQChannelSubPriority=1;
		NVIC_Init (&NVIC_InitStructure);

		USART_Cmd (USART1,ENABLE);
	}
	else if (USART_Name==2)
	{
		RCC_APB1PeriphClockCmd (RCC_APB1Periph_USART2,ENABLE);  // 开 USART2 时钟(挂在 APB1)
		RCC_APB2PeriphClockCmd (RCC_APB2Periph_GPIOA,ENABLE);
		GPIO_InitStructure.GPIO_Mode=GPIO_Mode_AF_PP;
		GPIO_InitStructure.GPIO_Pin=GPIO_Pin_2;  // PA2 = TX, 接 ESP-01S 的 RXD
		GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
		GPIO_Init(GPIOA,&GPIO_InitStructure);

		GPIO_InitStructure.GPIO_Mode=GPIO_Mode_IPU;
		GPIO_InitStructure.GPIO_Pin=GPIO_Pin_3;  // PA3 = RX, 接 ESP-01S 的 TXD
		GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
		GPIO_Init(GPIOA,&GPIO_InitStructure);
		USART_InitStructure.USART_BaudRate=115200;  // ESP-01S 固件默认 115200
		USART_InitStructure.USART_HardwareFlowControl=USART_HardwareFlowControl_None;
		USART_InitStructure.USART_Mode=USART_Mode_Tx | USART_Mode_Rx;
		USART_InitStructure.USART_Parity=USART_Parity_No;
		USART_InitStructure.USART_StopBits=USART_StopBits_1;
		USART_InitStructure.USART_WordLength=USART_WordLength_8b;
		USART_Init (USART2,&USART_InitStructure);

		USART_ITConfig (USART2,USART_IT_RXNE,ENABLE);  // 开接收中断, 模块应答全靠它收进来

		NVIC_PriorityGroupConfig (NVIC_PriorityGroup_2);  // 同上
		NVIC_InitStructure.NVIC_IRQChannel=USART2_IRQn;
		NVIC_InitStructure.NVIC_IRQChannelCmd=ENABLE;
		NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority=1;
		NVIC_InitStructure.NVIC_IRQChannelSubPriority=1;
		NVIC_Init (&NVIC_InitStructure);

		USART_Cmd (USART2,ENABLE);
	}
}

/* Serial_SendByte —— 通过指定串口发送 1 个字节
 * 用法：Serial_SendByte(1, 'A');  // 从 USART1 发出字符 A
 * 参数：USART_Name 1 或 2 选串口; Byte 要发的字节 (0x00~0xFF 都可以)
 * 返回：无
 * 说明：发完死等 TXE 标志, 波特率低时频繁调用会拖慢主循环 */
void Serial_SendByte (uint8_t USART_Name,uint8_t Byte)
{
	if (USART_Name==1)
	{
		USART_SendData (USART1,Byte);
		while (USART_GetFlagStatus (USART1,USART_FLAG_TXE) == RESET);  // 等发送寄存器空, 否则下一字节会覆盖
	}
	else if (USART_Name==2)
	{
		USART_SendData (USART2,Byte);
		while (USART_GetFlagStatus (USART2,USART_FLAG_TXE) == RESET);  // 同上
	}
}

/* Serial_SendArray —— 连续发送一段内存, 常用于原样转发接收缓冲区
 * 用法：Serial_SendArray(1, (uint8_t *)USART2_RX_BUF, USART2_RX_LEN);
 * 参数：Array 数据首地址; Length 字节数
 * 返回：无
 * 说明：按长度发送, 中途遇到 0x00 不会停, 适合发二进制数据 */
void Serial_SendArray (uint8_t USART_Name, uint8_t *Array, uint16_t Length)
{
	uint16_t i;
	for (i=0;i<Length;i++)  // 按长度逐字节发
	{
		Serial_SendByte (USART_Name,Array[i]);
	}
}

/* Serial_SendString —— 发送以 '\0' 结尾的字符串
 * 用法：Serial_SendString(2, "AT\r\n");  // 给 ESP-01S 发一条 AT 指令
 * 参数：String 字符串首地址, 必须以 '\0' 结束
 * 返回：无
 * 说明：AT 指令必须自己带 \r\n, 本函数不会自动补 */
void Serial_SendString (uint8_t USART_Name,char *String)
{
	uint16_t i;
	for (i=0;String[i]!='\0';i++)  // 遇到结束符停下
	{
		Serial_SendByte (USART_Name,String[i]);
	}
}
uint32_t Serial_Pow (uint32_t x,uint32_t y)
{
	uint32_t Resuit =1;
	while (y--)
	{
		Resuit*=x;
	}
	return Resuit;
}

/* Serial_SendNumber —— 按固定位数发送十进制数, 不足的高位补 0
 * 用法：Serial_SendNumber(1, 65, 2);   // 发出 "65"
 *       Serial_SendNumber(1, 5, 3);    // 发出 "005"
 * 参数：Number 要发的数 (32 位无符号); Length 总位数, 超过 Number 实际位数才会补 0
 * 返回：无
 * 说明：只适合正数, 要显示负号或空格请自己拼字符串再用 Serial_SendString */
void Serial_SendNumber (uint8_t USART_Name,uint32_t Number,uint16_t Length)
{
	uint16_t i;
	for (i=0;i<Length;i++)  // 从高位到低位逐位取出数字
	{
		Serial_SendByte (USART_Name,Number/Serial_Pow(10,Length-i-1)%10+0x30);
	}
}

/* Serial_Printf —— 格式化打印, 用法和 printf 一样, 只是输出到指定串口
 * 用法：Serial_Printf(1, "temp = %d C\r\n", t);
 * 参数：format 格式字符串; 后面接可变参数
 * 返回：无
 * 注意：内部缓冲只有 100 字节, 一次输出别超过 100 个字符, 否则会踩内存 */
void Serial_Printf (uint8_t USART_Name,char *format,...)
{
	char String [100];  // 格式化缓冲, 超出会越界
	va_list arg;
	va_start (arg,format);
	vsprintf (String,format,arg);  // 先把可变参数格式化进缓冲, 再整串发出去
	va_end (arg);
	Serial_SendString (USART_Name,String);
}

void USART1_IRQHandler(void) {
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        if (USART1_RX_LEN < USART1_RX_BUF_SIZE - 1) {  // 留 1 字节给结束符, 满了就丢新字节
            USART1_RX_BUF[USART1_RX_LEN++] = USART_ReceiveData(USART1);
            USART1_RX_BUF[USART1_RX_LEN] = '\0';  // 保证 BUF 永远是合法字符串, strstr 才安全
        } else {
            USART_ReceiveData(USART1);  // 满了也必须读走, 否则 RXNE 一直置位
        }
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
}
void USART2_IRQHandler(void) {
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET) {
        if (USART2_RX_LEN < USART2_RX_BUF_SIZE - 1) {  // 同上, 留 1 字节给结束符
            USART2_RX_BUF[USART2_RX_LEN++] = USART_ReceiveData(USART2);
            USART2_RX_BUF[USART2_RX_LEN] = '\0';  // 同上
        } else {
            USART_ReceiveData(USART2);  // 满了也必须读走
        }
        USART_ClearITPendingBit(USART2, USART_IT_RXNE);
    }
}
