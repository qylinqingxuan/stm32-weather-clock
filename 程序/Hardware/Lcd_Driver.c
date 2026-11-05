
#include "stm32f10x.h"
#include "Lcd_Driver.h"
#include "LCD_Config.h"
#include "delay.h"

/* LCD_GPIO_Init —— 屏幕引脚分工: CS/RS/RST/背光仍是普通 GPIO, SCL/SDA 交给硬件 SPI1
 * 用法：由 Lcd_Init 内部调用, 一般不用自己调
 * 参数：无
 * 返回：无
 * 说明：PA5/PA7 正好是 SPI1 的 SCK/MOSI, 切硬件不用动一根线;
 *       PA6(MISO) 屏幕用不到(只写不读), 留给 DHT11, 不给 SPI 配置;
 *       CS(PA4) 保持手动 GPIO 控制, 比硬件 NSS 直观可控 */
void LCD_GPIO_Init(void)
{
	GPIO_InitTypeDef  GPIO_InitStructure;
	SPI_InitTypeDef   SPI_InitStructure;

	RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOB ,ENABLE);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0| GPIO_Pin_1;  // PB0=RES  PB1=DC
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;  // PB10=背光, 交给 TIM2_CH3 PWM, 改复用推挽
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	RCC_APB2PeriphClockCmd( RCC_APB2Periph_GPIOA | RCC_APB2Periph_SPI1 ,ENABLE);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;  // PA4=CS  手动片选, 保持普通推挽输出
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5| GPIO_Pin_7;  // PA5=SCK  PA7=MOSI, 归 SPI1 硬件管
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;  // 只发不收, MISO 不接屏
	SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
	SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
	SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;  // ST7735 是 SPI 模式0
	SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
	SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;  // 片选自己拉 PA4, 不用硬件 NSS
	SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;  // 72M/8=9MHz, 屏标称上限 15M, 稳了再提
	SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;  // 高位在前, 和屏一致
	SPI_InitStructure.SPI_CRCPolynomial = 7;
	SPI_Init(SPI1, &SPI_InitStructure);
	SPI_Cmd(SPI1, ENABLE);
}

/* SPI_WriteData —— 硬件 SPI1 送出一个字节(高位在前)
 * 用法：SPI_WriteData(0x2C);
 * 参数：Data 要发送的字节
 * 返回：无
 * 说明：CPU 只负责把字节扔进移位寄存器, 数拍子由 SPI1 硬件完成;
 *       等 BSY 清零保证 CS 拉高时字节已经真正发完, 不丢尾 */
void  SPI_WriteData(u8 Data)
{
	while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);  // 等发送缓冲空
	SPI_I2S_SendData(SPI1, Data);  // 扔给硬件, 之后 CPU 可以干别的
	while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);  // 等移位真正结束
}

/* Lcd_WriteIndex —— 给屏幕发一条命令 (RS=0)
 * 用法：Lcd_WriteIndex(0x2C);   // 写显存命令
 * 参数：Index 命令字节, 具体含义查 ST7735 数据手册
 * 返回：无
 * 说明：命令和数据的区别只在 RS 脚电平, 命令后通常还要跟若干参数 */
void Lcd_WriteIndex(u8 Index)
{
   LCD_CS_CLR;  // 片选拉低, 选中屏幕
   LCD_RS_CLR;  // RS=0 表示接下来这个字节是命令
	 SPI_WriteData(Index);
   LCD_CS_SET;
}

/* Lcd_WriteData —— 给屏幕发一个数据字节 (RS=1)
 * 用法：Lcd_WriteData(0x05);
 * 参数：Data 数据字节
 * 返回：无
 * 说明：一般跟在 Lcd_WriteIndex 后面作为参数 */
void Lcd_WriteData(u8 Data)
{
   LCD_CS_CLR;
   LCD_RS_SET;  // RS=1 表示接下来这个字节是数据
   SPI_WriteData(Data);
   LCD_CS_SET;
}

/* LCD_WriteData_16Bit —— 发一个 RGB565 像素 (2 字节, 高字节在前)
 * 用法：LCD_WriteData_16Bit(RED);
 * 参数：Data 16 位颜色值
 * 返回：无
 * 说明：调用前必须已经用 Lcd_SetRegion / Lcd_SetXY 设好窗口,
 *       否则像素会落在上一次窗口里; 连续调用即自动按行填充 */
void LCD_WriteData_16Bit(u16 Data)
{
   LCD_CS_CLR;
   LCD_RS_SET;
	 SPI_WriteData(Data>>8);  // 高字节先发, 屏幕按大端接收
	 SPI_WriteData(Data);  // 再发低字节
   LCD_CS_SET;
}

/* Lcd_WriteReg —— 先发命令再发一个参数, 两句合一句
 * 用法：Lcd_WriteReg(0x36, 0xC0);
 * 参数：Index 命令; Data 该命令的第一个参数
 * 返回：无 */
void Lcd_WriteReg(u8 Index,u8 Data)
{
	Lcd_WriteIndex(Index);
  Lcd_WriteData(Data);
}

/* Lcd_Reset —— 硬复位屏幕: RST 拉低 100ms 再拉高 50ms
 * 用法：由 Lcd_Init 内部调用; 屏幕花屏时也可以手动再调一次
 * 参数：无
 * 返回：无
 * 说明：复位后屏幕寄存器全部回到默认值, 必须重新跑 Lcd_Init 的初始化序列 */
void Lcd_Reset(void)
{
	LCD_RST_CLR;  // 拉低复位脚
	delay_ms(100);
	LCD_RST_SET;
	delay_ms(50);
}

/* Lcd_Init —— 初始化 ST7735 屏幕, 开机调用一次
 * 用法：Lcd_Init();  Lcd_Clear(BLACK);
 * 参数：无
 * 返回：无
 * 说明：先配 GPIO、硬复位, 再按 ST7735R 的推荐值写一串寄存器
 *       (退出休眠 / 帧率 / 电源 / 灰度 / 显存窗口 / 65K 色 / 开显示);
 *       这些数值是厂家例程调好的, 不要随便改
 *
 * 【接线】STM32F103C8T6 与 1.8 寸 TFT (128x160)
 *   GND <-> GND        VCC <-> 3.3V
 *   SCL -> PA5         SDA -> PA7        CS  -> PA4
 *   RES -> PB0         DC(RS) -> PB1     BL(背光) -> PB10
 *   背光不亮时先把 BL 直接接 3.3V 排除模块极性差异 */
/* LCD_PWM_Init 初始化 —— 把背光脚 PB10 交给 TIM2_CH3 输出 PWM, 默认全亮
 * 用法: Lcd_Init 内部调用, 一般不用自己碰
 * 参数: 无
 * 返回: 无
 * 说明: TIM2 的 CH3 默认在 PA2(被 ESP 的 USART2 TX 占着), 所以做完全重映射搬到 PB10;
 *       重映射只搬 TIM2 自己的通道, PA2/PA3 的串口功能一点不受影响;
 *       PWM 频率 72MHz/7200 = 10kHz, 人眼和手机相机都看不出闪;
 *       TIM 标准库没进工程, 这里直接写寄存器, 每行后面标了位含义 */
static void LCD_PWM_Init (void)
{
    RCC->APB1ENR |= (1 << 0);                       /* TIM2 时钟开 */
    RCC->APB2ENR |= (1 << 0);                       /* AFIO 时钟开, 重映射靠它 */

    GPIO_PinRemapConfig(GPIO_FullRemap_TIM2, ENABLE);   /* TIM2 完全重映射: CH3 落到 PB10 */

    TIM2->PSC   = 0;                                /* 不分频, 72MHz */
    TIM2->ARR   = 7200 - 1;                         /* 计到 7200 一轮 = 10kHz */
    TIM2->CCR3  = 7200;                             /* 占空比先给满, 开机默认全亮 */
    TIM2->CCMR2 = (0x6 << 4) | (1 << 3);            /* OC3M=110(PWM1) + OC3PE(影子寄存器) */
    TIM2->CCER  = (1 << 8);                         /* CC3E: CH3 输出使能 */
    TIM2->CR1   = (1 << 7) | (1 << 0);              /* ARPE(ARR 影子) + CEN(启动) */
}

/* LCD_SetBacklight 设置 —— 调背光亮度
 * 用法: LCD_SetBacklight (30);   // 30% 亮度
 * 参数: percent 0~100, 0=全灭 100=最亮, 超出按边界处理
 * 返回: 无
 * 说明: 占空比线性映射进 CCR, 一条语句, 主循环里随时可调不阻塞;
 *       要掉电记住亮度的话, 让配置层开机读出来再调一次 */
void LCD_SetBacklight (u8 percent)
{
    if (percent > 100) percent = 100;
    TIM2->CCR3 = (u16)((u16)percent * 72u);         /* 7200 / 100 = 72, 每 1% 走 72 个计数 */
}

void Lcd_Init(void)
{
	LCD_GPIO_Init();
	Lcd_Reset();

	Lcd_WriteIndex(0x11);  // 退出休眠, 之后要等 120ms
	delay_ms (120);

	Lcd_WriteIndex(0xB1);  // 帧率设置
	Lcd_WriteData(0x01);
	Lcd_WriteData(0x2C);
	Lcd_WriteData(0x2D);

	Lcd_WriteIndex(0xB2);
	Lcd_WriteData(0x01);
	Lcd_WriteData(0x2C);
	Lcd_WriteData(0x2D);

	Lcd_WriteIndex(0xB3);
	Lcd_WriteData(0x01);
	Lcd_WriteData(0x2C);
	Lcd_WriteData(0x2D);
	Lcd_WriteData(0x01);
	Lcd_WriteData(0x2C);
	Lcd_WriteData(0x2D);

	Lcd_WriteIndex(0xB4);  // 列翻转, 能消掉竖条纹
	Lcd_WriteData(0x07);

	Lcd_WriteIndex(0xC0);  // 电源电压参数
	Lcd_WriteData(0xA2);
	Lcd_WriteData(0x02);
	Lcd_WriteData(0x84);
	Lcd_WriteIndex(0xC1);
	Lcd_WriteData(0xC5);

	Lcd_WriteIndex(0xC2);
	Lcd_WriteData(0x0A);
	Lcd_WriteData(0x00);

	Lcd_WriteIndex(0xC3);
	Lcd_WriteData(0x8A);
	Lcd_WriteData(0x2A);
	Lcd_WriteIndex(0xC4);
	Lcd_WriteData(0x8A);
	Lcd_WriteData(0xEE);

	Lcd_WriteIndex(0xC5);
	Lcd_WriteData(0x0E);

	Lcd_WriteIndex(0x36);  // 扫描方向 + RGB 顺序, 0xC0 = 行列都翻转
	Lcd_WriteData(0xC0);

	Lcd_WriteIndex(0xe0);  // 正极性灰度表
	Lcd_WriteData(0x0f);
	Lcd_WriteData(0x1a);
	Lcd_WriteData(0x0f);
	Lcd_WriteData(0x18);
	Lcd_WriteData(0x2f);
	Lcd_WriteData(0x28);
	Lcd_WriteData(0x20);
	Lcd_WriteData(0x22);
	Lcd_WriteData(0x1f);
	Lcd_WriteData(0x1b);
	Lcd_WriteData(0x23);
	Lcd_WriteData(0x37);
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x07);
	Lcd_WriteData(0x02);
	Lcd_WriteData(0x10);

	Lcd_WriteIndex(0xe1);  // 负极性灰度表
	Lcd_WriteData(0x0f);
	Lcd_WriteData(0x1b);
	Lcd_WriteData(0x0f);
	Lcd_WriteData(0x17);
	Lcd_WriteData(0x33);
	Lcd_WriteData(0x2c);
	Lcd_WriteData(0x29);
	Lcd_WriteData(0x2e);
	Lcd_WriteData(0x30);
	Lcd_WriteData(0x30);
	Lcd_WriteData(0x39);
	Lcd_WriteData(0x3f);
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x07);
	Lcd_WriteData(0x03);
	Lcd_WriteData(0x10);

	Lcd_WriteIndex(0x2a);  // 列地址范围 0~127
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x7f);

	Lcd_WriteIndex(0x2b);  // 行地址范围 0~159
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x00);
	Lcd_WriteData(0x9f);

	Lcd_WriteIndex(0xF0);  // 厂家要求的测试命令, 少了会花屏
	Lcd_WriteData(0x01);
	Lcd_WriteIndex(0xF6);
	Lcd_WriteData(0x00);

	Lcd_WriteIndex(0x3A);  // 像素格式 = 65K 色(16 位)
	Lcd_WriteData(0x05);

	Lcd_WriteIndex(0x29);  // 打开显示, 不写这句屏幕一直黑

	LCD_PWM_Init();  // 背光交给 TIM2_CH3 PWM, 默认全亮 (原 LCD_LED_SET 拉高废弃)
}

/* Lcd_SetRegion —— 设置矩形显示窗口, 之后连续写的像素会自动换行
 * 用法：Lcd_SetRegion(0, 0, 127, 159);   // 开整屏窗口
 * 参数：x_start,y_start 左上角; x_end,y_end 右下角 (都是屏幕真实坐标)
 * 返回：无
 * 说明：函数内部对 x_end 加了 +2、y_end 加了 +1, 是这块屏固有的坐标偏移补偿,
 *       所以调用时按真实右下角传, 不要自己再减;
 *       画点、清屏、贴图全靠它定位 */
void Lcd_SetRegion(u16 x_start,u16 y_start,u16 x_end,u16 y_end)
{
	Lcd_WriteIndex(0x2a);
	Lcd_WriteData(0x00);
	Lcd_WriteData(x_start);
	Lcd_WriteData(0x00);
	Lcd_WriteData(x_end+2);  // 列结束地址要 +2, 这块屏的固定偏移

	Lcd_WriteIndex(0x2b);
	Lcd_WriteData(0x00);
	Lcd_WriteData(y_start+0);
	Lcd_WriteData(0x00);
	Lcd_WriteData(y_end+1);  // 行结束地址要 +1

	Lcd_WriteIndex(0x2c);  // 0x2C = 开始写显存, 之后来的都是像素
}

/* Lcd_SetXY —— 把光标定位到某个像素 (等于开一个单点窗口)
 * 用法：Lcd_SetXY(10, 20);
 * 参数：x,y 屏幕坐标
 * 返回：无
 * 说明：画单点前用它定位, 比每次自己写窗口命令省事 */
void Lcd_SetXY(u16 x,u16 y)
{
  	Lcd_SetRegion(x,y,x,y);
}

/* Lcd_DrawPoint —— 在(x,y)画一个像素点
 * 用法：Lcd_DrawPoint(10, 20, RED);
 * 参数：x,y 屏幕坐标; Data RGB565 颜色
 * 返回：无
 * 说明：每次都重设窗口, 单点调用很慢;
 *       画大量点(整屏填充、贴图)请用 Lcd_SetRegion + LCD_WriteData_16Bit */
void Lcd_DrawPoint(u16 x,u16 y,u16 Data)
{
	Lcd_SetRegion(x,y,x+1,y+1);  // 开 2x2 小窗口, 比单点窗口少发一半指令
	LCD_WriteData_16Bit(Data);
}

/* Lcd_ReadPoint —— 读某个像素的颜色 【当前实现是坏的, 不要调用】
 * 用法：暂不可用
 * 参数：x,y 屏幕坐标
 * 返回：名义上返回颜色, 现在返回的是未初始化值
 * 说明：读数据的代码已被注释掉, 最后还把未初始化的 Data 当数据写回屏幕,
 *       既读不到颜色又可能改坏显存; 真要读点得按 ST7735 的读时序重写 */
unsigned int Lcd_ReadPoint(u16 x,u16 y)
{
  unsigned int Data;
  Lcd_SetXY(x,y);

  Lcd_WriteData(Data);
  return Data;
}

/* Lcd_Clear —— 用单一颜色铺满整屏
 * 用法：Lcd_Clear(BLACK);
 * 参数：Color RGB565 颜色, 可用 BLUE/WHITE/SKY_BULE 等宏
 * 返回：无
 * 说明：开整屏窗口后连写 128x160 个像素, 约几十毫秒;
 *       频繁整屏清会看到闪烁, 局部刷新请自己算小窗口 */
void Lcd_Clear(u16 Color)
{
   unsigned int i,m;
   Lcd_SetRegion(0,0,X_MAX_PIXEL-1,Y_MAX_PIXEL-1);
   Lcd_WriteIndex(0x2C);
   for(i=0;i<X_MAX_PIXEL;i++)  // 列 0~127
    for(m=0;m<Y_MAX_PIXEL;m++)  // 行 0~159, 一共 20480 个像素
    {
	  	LCD_WriteData_16Bit(Color);
    }
}
