#include "stm32f10x.h"
#include "Lcd_Driver.h"
#include "GUI.h"
#include "delay.h"
#include "font.h"

/* Gui_BGR2RGB —— 把 BGR 顺序的 RGB565 颜色翻成 RGB 顺序
 * 用法：c = Gui_BGR2RGB(0xF800);   // 原来的红会变成蓝
 * 参数：c 按 BGR 排列的颜色值
 * 返回：按 RGB 排列的颜色值
 * 说明：有些屏读回来是 BGR、写进去是 RGB, 靠它对齐;
 *       正常画图用不到, 只有读点取色时才需要 */
u16 Gui_BGR2RGB(u16 c)
{
  u16  r,g,b,rgb;
  b=(c>>0)&0x1f;  // 按 BGR 拆出三个通道
  g=(c>>5)&0x3f;
  r=(c>>11)&0x1f;
  rgb=(b<<11)+(g<<5)+(r<<0);  // 再按 RGB 顺序拼回去
  return(rgb);
}

/* Gui_Circle —— 画空心圆, 只画圆周, 内部不填充
 * 用法：Gui_Circle(64, 80, 20, YELLOW);   // 圆心(64,80) 半径 20
 * 参数：X,Y 圆心坐标; R 半径(像素); fc 圆周颜色 RGB565
 * 返回：无
 * 说明：Bresenham 画圆, 一圈只算 1/8 再对称补 8 个点, 很快 */
void Gui_Circle(u16 X,u16 Y,u16 R,u16 fc)
{
    u16  a,b;
    int c;
    a=0;
    b=R;
    c=3-2*R;  // Bresenham 判别式初值
    while (a<b)
    {
        Lcd_DrawPoint(X+a,Y+b,fc);
        Lcd_DrawPoint(X-a,Y+b,fc);
        Lcd_DrawPoint(X+a,Y-b,fc);
        Lcd_DrawPoint(X-a,Y-b,fc);
        Lcd_DrawPoint(X+b,Y+a,fc);
        Lcd_DrawPoint(X-b,Y+a,fc);
        Lcd_DrawPoint(X+b,Y-a,fc);
        Lcd_DrawPoint(X-b,Y-a,fc);

        if(c<0) c=c+4*a+6;  // 判别式选点: 不换步就只加 x 项
        else
        {
            c=c+4*(a-b)+10;
            b-=1;
        }
       a+=1;
    }
    if (a==b)  // 走到 45 度时补一次对角线上的点
    {
        Lcd_DrawPoint(X+a,Y+b,fc);
        Lcd_DrawPoint(X+a,Y+b,fc);
        Lcd_DrawPoint(X+a,Y-b,fc);
        Lcd_DrawPoint(X-a,Y-b,fc);
        Lcd_DrawPoint(X+b,Y+a,fc);
        Lcd_DrawPoint(X-b,Y+a,fc);
        Lcd_DrawPoint(X+b,Y-a,fc);
        Lcd_DrawPoint(X-b,Y-a,fc);
    }
}

/* Gui_DrawLine —— 两点之间画一条 1 像素宽的直线
 * 用法：Gui_DrawLine(4, 41, 123, 41, 0x7BEF);   // 画一条横向分隔线
 * 参数：x0,y0 起点; x1,y1 终点 (两端都会画上); Color RGB565 颜色
 * 返回：无
 * 说明：Bresenham 算法, 任意方向都能画, 起点终点不用自己排序;
 *       逐点写屏, 画满屏长的线要几毫秒 */
void Gui_DrawLine(u16 x0, u16 y0,u16 x1, u16 y1,u16 Color)
{
int dx,
    dy,
    dx2,
    dy2,
    x_inc,
    y_inc,
    error,
    index;

	Lcd_SetXY(x0,y0);  // 先把光标定位到起点
	dx = x1-x0;  // 算 x/y 方向的距离
	dy = y1-y0;

	if (dx>=0)  // 先统一成正数, 方向用 x_inc/y_inc 记住
	{
		x_inc = 1;
	}
	else
	{
		x_inc = -1;
		dx    = -dx;
	}

	if (dy>=0)
	{
		y_inc = 1;
	}
	else
	{
		y_inc = -1;
		dy    = -dy;
	}

	dx2 = dx << 1;
	dy2 = dy << 1;

	if (dx > dy)  // x 距离大: 以 x 为步进, 每步最多进 1 个 y
	{
		error = dy2 - dx;  // 斜率小于 1 时的判别式初值

		for (index=0; index <= dx; index++)
		{
			Lcd_DrawPoint(x0,y0,Color);

			if (error >= 0)
			{
				error-=dx2;

				y0+=y_inc;
			}

			error+=dy2;

			x0+=x_inc;
		}
	}
	else
	{
		error = dx2 - dy;

		for (index=0; index <= dy; index++)
		{
			Lcd_DrawPoint(x0,y0,Color);

			if (error >= 0)
			{
				error-=dy2;

				x0+=x_inc;
			}

			error+=dx2;

			y0+=y_inc;
		}
	}
}

/* Gui_box —— 画一个带立体效果的矩形框 (上/左亮, 下/右暗)
 * 用法：Gui_box(0, 0, 128, 40, SKY_BULE);
 * 参数：x,y 左上角坐标; w,h 宽和高; bc 框内那一圈底线的颜色
 * 返回：无
 * 说明：明暗色写死为 0xEF7D / 0x2965, 只适合做凸起的面板;
 *       注意后两个参数是宽高, 不是右下角坐标 */
void Gui_box(u16 x, u16 y, u16 w, u16 h,u16 bc)
{
	Gui_DrawLine(x,y,x+w,y,0xEF7D);
	Gui_DrawLine(x+w-1,y+1,x+w-1,y+1+h,0x2965);
	Gui_DrawLine(x,y+h,x+w,y+h,0x2965);
	Gui_DrawLine(x,y,x,y+h,0xEF7D);
    Gui_DrawLine(x+1,y+1,x+1+w-2,y+1+h-2,bc);
}

/* Gui_box2 —— 画矩形边框, 用 mode 切换三种立体效果
 * 用法：Gui_box2(10, 10, 60, 30, 1);   // 按下效果
 * 参数：x,y 左上角; w,h 宽高; mode 0=凸起 1=凹陷 2=纯白边框
 * 返回：无
 * 说明：只画四条边, 不填内部; 做按钮的两种状态最省事 */
void Gui_box2(u16 x,u16 y,u16 w,u16 h, u8 mode)
{
	if (mode==0)	{
		Gui_DrawLine(x,y,x+w,y,0xEF7D);
		Gui_DrawLine(x+w-1,y+1,x+w-1,y+1+h,0x2965);
		Gui_DrawLine(x,y+h,x+w,y+h,0x2965);
		Gui_DrawLine(x,y,x,y+h,0xEF7D);
		}
	if (mode==1)	{
		Gui_DrawLine(x,y,x+w,y,0x2965);
		Gui_DrawLine(x+w-1,y+1,x+w-1,y+1+h,0xEF7D);
		Gui_DrawLine(x,y+h,x+w,y+h,0xEF7D);
		Gui_DrawLine(x,y,x,y+h,0x2965);
	}
	if (mode==2)	{
		Gui_DrawLine(x,y,x+w,y,0xffff);
		Gui_DrawLine(x+w-1,y+1,x+w-1,y+1+h,0xffff);
		Gui_DrawLine(x,y+h,x+w,y+h,0xffff);
		Gui_DrawLine(x,y,x,y+h,0xffff);
	}
}

/* DisplayButtonDown —— 画按钮"未按下"的立体边框 (上/左深灰, 下/右白)
 * 用法：DisplayButtonDown(10, 50, 100, 90);
 * 参数：x1,y1 左上角; x2,y2 右下角 (右下角坐标, 不是宽高)
 * 返回：无
 * 说明：函数名和视觉效果是反的 —— 它画出的是凸起(未按下)的样子,
 *       想让按钮看起来被按下, 要调 DisplayButtonUp */
void DisplayButtonDown(u16 x1,u16 y1,u16 x2,u16 y2)
{
	Gui_DrawLine(x1,  y1,  x2,y1, GRAY2);
	Gui_DrawLine(x1+1,y1+1,x2,y1+1, GRAY1);
	Gui_DrawLine(x1,  y1,  x1,y2, GRAY2);
	Gui_DrawLine(x1+1,y1+1,x1+1,y2, GRAY1);
	Gui_DrawLine(x1,  y2,  x2,y2, WHITE);
	Gui_DrawLine(x2,  y1,  x2,y2, WHITE);
}

/* DisplayButtonUp —— 画按钮"已按下"的立体边框 (上/左白, 下/右深灰)
 * 用法：DisplayButtonUp(10, 50, 100, 90);
 * 参数：x1,y1 左上角; x2,y2 右下角
 * 返回：无
 * 说明：和 DisplayButtonDown 成对使用, 一按一抬就有立体反馈 */
void DisplayButtonUp(u16 x1,u16 y1,u16 x2,u16 y2)
{
	Gui_DrawLine(x1,  y1,  x2,y1, WHITE);
	Gui_DrawLine(x1,  y1,  x1,y2, WHITE);

	Gui_DrawLine(x1+1,y2-1,x2,y2-1, GRAY1);
	Gui_DrawLine(x1,  y2,  x2,y2, GRAY2);
	Gui_DrawLine(x2-1,y1+1,x2-1,y2, GRAY1);
    Gui_DrawLine(x2  ,y1  ,x2,y2, GRAY2);
}

/* Gui_DrawFont_GBK16 —— 显示 16 像素高的混合字符串 (ASCII 8x16 + 汉字 16x16)
 * 用法：Gui_DrawFont_GBK16(2, 24, WHITE, SKY_BULE, (u8 *)"10月22日 星期四");
 * 参数：x,y 文字左上角起始坐标
 *       fc 前景色(字的颜色); bc 背景色(底色, 传成和 fc 相同则只画字不填底)
 *       s  字符串, 必须是 GBK 编码 —— Keil 里源码存成 GBK, 字面量就是 GBK
 * 返回：无
 * 说明：汉字靠 hz16[] 表按内码线性查, 表里没有的字不会显示(Font.h 只收了十几个常用字),
 *       要显示新汉字必须先补点阵; 遇到 0x0D 当回车换行并把 x 拉回起始位置;
 *       ASCII 每字前进 8 像素, 汉字前进 16 像素 */
void Gui_DrawFont_GBK16(u16 x, u16 y, u16 fc, u16 bc, u8 *s)
{
	u8 i,j;
	u16 k,x0;
	x0=x;  // 记住起始 x, 回车换行时回到行首

	while(*s)
	{
		if((*s) < 128)  // 小于 128 的是单字节字符, 按 ASCII 画
		{
			k=*s;
			if (k==13)  // 0x0D 当回车: 换到下一行并回到行首
			{
				x=x0;
				y+=16;
			}
			else
			{
				if (k>32) k-=32; else k=0;  // 前 32 个控制字符不占字模, 下标要减 32

			    for(i=0;i<16;i++)
				for(j=0;j<8;j++)
					{
				    	if(asc16[k*16+i]&(0x80>>j))	Lcd_DrawPoint(x+j,y+i,fc);
						else
						{
							if (fc!=bc) Lcd_DrawPoint(x+j,y+i,bc);
						}
					}
				x+=8;  // ASCII 每个宽 8 像素
			}
			s++;
		}

		else
		{
			for (k=0;k<hz16_num;k++)  // 在字库里线比内码找这个汉字
			{
			  if ((hz16[k].Index[0]==*(s))&&(hz16[k].Index[1]==*(s+1)))
			  {
				    for(i=0;i<16;i++)
				    {
						for(j=0;j<8;j++)
							{
						    	if(hz16[k].Msk[i*2]&(0x80>>j))	Lcd_DrawPoint(x+j,y+i,fc);
								else {
									if (fc!=bc) Lcd_DrawPoint(x+j,y+i,bc);
								}
							}
						for(j=0;j<8;j++)
							{
						    	if(hz16[k].Msk[i*2+1]&(0x80>>j))	Lcd_DrawPoint(x+j+8,y+i,fc);
								else
								{
									if (fc!=bc) Lcd_DrawPoint(x+j+8,y+i,bc);
								}
							}
				    }
				}
			  }
			s+=2;x+=16;  // 汉字占 2 字节, 宽 16 像素
		}
	}
}

/* Gui_DrawFont_GBK24 —— 显示 24 像素高的汉字 (ASCII 部分仍然是 8x16)
 * 用法：Gui_DrawFont_GBK24(36, 44, WHITE, SKY_BULE, (u8 *)"℃");
 * 参数：同 Gui_DrawFont_GBK16; 一个汉字占 24x24
 * 返回：无
 * 说明：hz24[] 里的字更少, 适合放"℃"这类单个大字符;
 *       汉字前进 24 像素, 而 ASCII 仍只前进 8 像素, 混排时注意对齐 */
void Gui_DrawFont_GBK24(u16 x, u16 y, u16 fc, u16 bc, u8 *s)
{
	u8 i,j;
	u16 k;

	while(*s)
	{
		if( *s < 0x80 )
		{
			k=*s;
			if (k>32) k-=32; else k=0;  // 同上, 跳过控制字符

		    for(i=0;i<16;i++)
			for(j=0;j<8;j++)
				{
			    	if(asc16[k*16+i]&(0x80>>j))
					Lcd_DrawPoint(x+j,y+i,fc);
					else
					{
						if (fc!=bc) Lcd_DrawPoint(x+j,y+i,bc);
					}
				}
			s++;x+=8;
		}
		else
		{
			for (k=0;k<hz24_num;k++)  // 在 24 号字库里找这个汉字
			{
			  if ((hz24[k].Index[0]==*(s))&&(hz24[k].Index[1]==*(s+1)))
			  {
				    for(i=0;i<24;i++)
				    {
						for(j=0;j<8;j++)
							{
						    	if(hz24[k].Msk[i*3]&(0x80>>j))
								Lcd_DrawPoint(x+j,y+i,fc);
								else
								{
									if (fc!=bc) Lcd_DrawPoint(x+j,y+i,bc);
								}
							}
						for(j=0;j<8;j++)
							{
						    	if(hz24[k].Msk[i*3+1]&(0x80>>j))	Lcd_DrawPoint(x+j+8,y+i,fc);
								else {
									if (fc!=bc) Lcd_DrawPoint(x+j+8,y+i,bc);
								}
							}
						for(j=0;j<8;j++)
							{
						    	if(hz24[k].Msk[i*3+2]&(0x80>>j))
								Lcd_DrawPoint(x+j+16,y+i,fc);
								else
								{
									if (fc!=bc) Lcd_DrawPoint(x+j+16,y+i,bc);
								}
							}
				    }
			  }
			}
			s+=2;x+=24;  // 汉字宽 24 像素
		}
	}
}

/* Gui_DrawFont_Num32 —— 按 sz32[] 的序号画一个 32x32 的大字模
 * 用法：Gui_DrawFont_Num32(10, 10, WHITE, BLACK, 0);
 * 参数：num 字模序号(从 0 开始, 表里共 15 个字模, 传大会读到别的数据)
 *       fc 前景色; bc 背景色
 * 返回：无
 * 说明：sz32[] 在 Font.h 里, 数据来源是本工程自带的小字库, 不是标准数字字形;
 *       想要整齐的数字时钟请用 Gui_DrawNum24String */
//void Gui_DrawFont_Num32(u16 x, u16 y, u16 fc, u16 bc, u16 num)
//{
//	unsigned char i,j,k,c;

//    for(i=0;i<32;i++)
//	{
//		for(j=0;j<4;j++)
//		{
//			c=*(sz32+num*32*4+i*4+j);  // 32x32 字模每行 4 字节, 逐字节取
//			for (k=0;k<8;k++)
//			{
//		    	if(c&(0x80>>k))	Lcd_DrawPoint(x+j*8+k,y+i,fc);
//				else {
//					if (fc!=bc) Lcd_DrawPoint(x+j*8+k,y+i,bc);
//				}
//			}
//		}
//	}
//}

/* Gui_DrawFont_Num24 —— 按 number_24[] 的序号画一个 16x24 的数字或冒号
 * 用法：Gui_DrawFont_Num24(0, 0, WHITE, SKY_BULE, 3);   // 画数字 3
 * 参数：num 0~9 对应数字, 10 对应冒号; fc 前景色; bc 背景色
 * 返回：无
 * 说明：表里只有 11 个字模(0~9 和冒号), 序号超过 10 一律不画, 判据用 sizeof 跟着数组走;
 *       一格宽 16 像素, 高度正好适合时钟界面 */
void Gui_DrawFont_Num24(u16 x, u16 y, u16 fc, u16 bc, u16 num)
{
    u8 row, byte_index, bit;
    u8 data;
    const u8 *glyph;

    if ((u16)num >= (u16)(sizeof (number_24) / 48)) return;  // 每字 48 字节, 数组里有几个就画到几, 越界直接跳过
    glyph = number_24 + num * 24 * 2;  // 定位到第 num 个字模, 每字 48 字节

    for (row = 0; row < 24; row++)  // 逐行逐位画, 位为 1 涂字色否则涂底色
    {
        for (byte_index = 0; byte_index < 2; byte_index++)
        {
            data = glyph[row * 2 + byte_index];
            for (bit = 0; bit < 8; bit++)
            {
                if (data & (0x80 >> bit))
                    Lcd_DrawPoint(x + byte_index * 8 + bit, y + row, fc);
                else if (fc != bc)
                    Lcd_DrawPoint(x + byte_index * 8 + bit, y + row, bc);
            }
        }
    }
}

/* Gui_DrawNum24String —— 直接画一串数字和冒号, 例如 "09:28:20"
 * 用法：Gui_DrawNum24String(0, 0, WHITE, SKY_BULE, "09:28:20");
 * 参数：s 只认 '0'~'9' 和 ':', 其他字符(空格、'-')会被跳过
 * 返回：无
 * 说明：每画一个字自动右移 16 像素, 想居中请自己算起始 x;
 *       显示 "--:--" 时注意 '-' 会被跳过, 只会画出冒号 */
void Gui_DrawNum24String(u16 x, u16 y, u16 fc, u16 bc, const char *s)
{
    u16 glyph;

    if (s == 0) return;  // 空指针保护
    while (*s != '\0')
    {
        if (*s >= '0' && *s <= '9')
            glyph = (u16)(*s - '0');
        else if (*s == ':')  // 冒号在表里是 10 号
            glyph = 10;
        else
        {
            s++;
            continue;
        }

        Gui_DrawFont_Num24(x, y, fc, bc, glyph);  // 画完一个字, x 右移 16 像素
        x += 16;
        s++;
    }
}
/* Gui_FillRegion —— 开一个 w x h 的像素窗口, 之后可以用 Gui_PushPixel 连续灌像素
 * 用法：Gui_FillRegion(x, y, 32, 32);
 * 参数：x,y 窗口左上角; w,h 窗口宽高(像素)
 * 返回：无
 * 说明：ST7735 的地址窗口有固定偏移(列 +2 / 行 +1), 这里统一减掉,
 *       上层再也不用自己写 -3 / -2 这种魔法数, 硬件怪癖封在本层 */
void Gui_FillRegion(u16 x, u16 y, u16 w, u16 h)
{
  Lcd_SetRegion(x, y, x + w - 3, y + h - 2);
}

/* Gui_PushPixel —— 往 Gui_FillRegion 刚开的窗口里写一个 RGB565 像素
 * 用法：Gui_PushPixel(WHITE);
 * 参数：c RGB565 颜色
 * 返回：无
 * 说明：窗口内地址自动递增, 按"先行后列"的顺序灌就行;
 *       它就是驱动层 LCD_WriteData_16Bit 的门面, 逻辑层不要越过本层直调驱动 */
void Gui_PushPixel(u16 c)
{
  LCD_WriteData_16Bit(c);
}

/* Gui_Glyph16 —— 按当前字符取 16 号点阵字模, 顺带告诉调用者这个字多宽
 * 用法：g = Gui_Glyph16(s, &w, &stride);
 * 参数：s 指向当前字符(GBK 或 ASCII); w 输出字宽(ASCII 8 / 汉字 16);
 *       stride 输出字模每行占几字节(ASCII 1 / 汉字 2)
 * 返回：字模首地址; 字库里没有这个字时返回 0(不画, 但宽度照样算)
 * 说明：hz16 表尾放着一个全 0 的哨兵项, 所以能靠 Index[0]==0 判断结束, 不必知道表长;
 *       字库只在 GUI 层 include, 逻辑层不要再 extern 一份,
 *       免得同一个数组被两套结构体声明 */
const unsigned char *Gui_Glyph16(const unsigned char *s, u8 *w, u8 *stride)
{
	u16 k;

	*w      = 8;              // 先按 ASCII 准备: 8 像素宽, 每行 1 字节
	*stride = 1;
	if (*s < 0x80)
	{
		k = (u16)(*s);
		if (k > 32) k -= 32; else k = 0;   // 前 32 个控制码在字库里没有字模
		return asc16 + k * 16;
	}

	*w      = 16;             // 双字节当汉字处理: 16 像素宽, 每行 2 字节
	*stride = 2;
	k = 0;
	while (hz16[k].Index[0] != 0)          // 走到全 0 哨兵为止
	{
		if (hz16[k].Index[0] == s[0] && hz16[k].Index[1] == s[1])
			return (const unsigned char *)hz16[k].Msk;
		k++;
	}
	return 0;                 // 缺字: 画不出来, 但宽度还得占, 否则整排会挤在一起
}
