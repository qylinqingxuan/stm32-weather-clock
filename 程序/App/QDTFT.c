#include "stm32f10x.h"
#include "Lcd_Driver.h"
#include "LCD_Config.h"
#include "GUI.h"
#include "delay.h"
#include "rtc.h"
#include "UI_Picture.h"
#include "UI_Config.h"
#include "Pet_Anim.h"
#include "screen_data.h"
#include "QDTFT.h"

/* showimage_auto —— 显示带 8 字节头的图片, 自动等比缩放并居中
 * 用法：showimage_auto(gImage_girl48);
 * 参数：p 图片数组; 前两字节是格式标志(0x00 0x10), 接着宽、高(各 2 字节小端),
 *       真正的像素从第 8 字节开始, 每像素 2 字节(低字节在前);
 *       UI_Picture.h 里的 gImage_girl48 就是这个格式
 * 返回：无
 * 说明：标志不对或宽高为 0 时直接返回, 什么都不画;
 *       比屏大的图等比缩小, 比屏小的原尺寸贴中间; 内部会先清屏 */
void showimage_auto(const unsigned char* p)
{
    unsigned int imageWidth, imageHeight;
    unsigned int drawWidth, drawHeight;
    unsigned int x, y, sourceX, sourceY, i;
    unsigned char picH, picL;

    if (p[0] != 0x00 || p[1] != 0x10) // 格式标志不对就放弃, 防止按错的宽高乱画
        return;

    imageWidth = (unsigned int)p[2] | ((unsigned int)p[3] << 8);
    imageHeight = (unsigned int)p[4] | ((unsigned int)p[5] << 8);
    if (imageWidth == 0 || imageHeight == 0)
        return;

    drawWidth = imageWidth;
    drawHeight = imageHeight;
    if (drawWidth > X_MAX_PIXEL || drawHeight > Y_MAX_PIXEL) // 比屏幕大就等比缩到屏内
    {
        if ((unsigned long)X_MAX_PIXEL * imageHeight <=
                (unsigned long)Y_MAX_PIXEL * imageWidth)
        {
            drawWidth = X_MAX_PIXEL;
            drawHeight = (unsigned long)imageHeight * drawWidth / imageWidth;
        }
        else
        {
            drawHeight = Y_MAX_PIXEL;
            drawWidth = (unsigned long)imageWidth * drawHeight / imageHeight;
        }
    }
    if (drawWidth == 0)
        drawWidth = 1;  // 防止除零
    if (drawHeight == 0)
        drawHeight = 1;

    Lcd_Clear(WHITE);  // 换图前先清屏

    Gui_FillRegion((X_MAX_PIXEL - drawWidth) / 2,   // 居中开窗; 硬件偏移由 GUI 层统一补
                   (Y_MAX_PIXEL - drawHeight) / 2,
                   drawWidth, drawHeight);

    for (y = 0; y < drawHeight; y++)
    {
        sourceY = (unsigned long)y * imageHeight /
                  drawHeight;  // 缩放靠反算源坐标, 整数除法的误差肉眼看不出来
        for (x = 0; x < drawWidth; x++)
        {
            sourceX = (unsigned long)x * imageWidth / drawWidth;
            i = 8 + (sourceY * imageWidth + sourceX) * 2;  // 跳过 8 字节头, 每像素 2 字节
            picL = p[i];
            picH = p[i + 1];
            Gui_PushPixel(picH << 8 | picL);
        }
    }
}

/* showimage_xy —— 把带 8 字节头的图片原尺寸贴到指定坐标, 不清屏不缩放
 * 用法：showimage_xy(gImage_girl48, 74, 90);
 * 参数：p 图片数组(格式同 showimage_auto); x,y 图片左上角在屏上的坐标
 * 返回：无
 * 说明：showimage_auto 会清屏并强制居中, 没法往画好的界面里贴小图;
       桌宠要落在固定舞台位置, 所以拆了这个"原样搬运"的版本;
       一次开窗整幅连写, 48x48 共 2304 像素, 软件 SPI 下约 10ms;
       后面桌宠动画的脏矩形补丁也走同样的开窗思路 */
void showimage_xy(const unsigned char* p, u16 x, u16 y)
{
    unsigned int w, h, i;
    unsigned char picH, picL;

    if (p[0] != 0x00 || p[1] != 0x10) // 格式标志不对就放弃
        return;
    w = (unsigned int)p[2] | ((unsigned int)p[3] << 8);
    h = (unsigned int)p[4] | ((unsigned int)p[5] << 8);
    if (w == 0 || h == 0)
        return;

    Gui_FillRegion(x, y, w, h);  // 开窗; 硬件偏移由 GUI 层统一补
    for (i = 0; i < w * h; i++) // 每像素 2 字节, 低字节在前
    {
        picL = p[8 + i * 2];
        picH = p[8 + i * 2 + 1];
        Gui_PushPixel(picH << 8 | picL);
    }
}

/* Pet_DrawPatch —— 把一个 12 字节头的补丁块画到屏上(桌宠动画的脏矩形)
 * 用法：Pet_DrawPatch(pet_blink_closed);
 * 参数：p 补丁数组: [0]=0x00 [1]=0x20 是格式标志, w/h/x/y 各 2 字节小端
 *       (x/y 直接是屏上绝对坐标), [12] 起是像素, 低字节在前
 * 返回：无
 * 说明：和 showimage_xy 同一套开窗直写, 但只覆盖动作会变的那一小块矩形;
       标志用 0x20 区别于整图的 0x10, 防止两种数组拿混 */
void Pet_DrawPatch(const unsigned char* p)
{
    unsigned int w, h, x, y, i, n;
    unsigned char picH, picL;

    if (p[0] != 0x00 || p[1] != 0x20) // 标志不对就放弃, 不会把补丁当整图乱画
        return;
    w = (unsigned int)p[2] | ((unsigned int)p[3] << 8);
    h = (unsigned int)p[4] | ((unsigned int)p[5] << 8);
    x = (unsigned int)p[6] | ((unsigned int)p[7] << 8);
    y = (unsigned int)p[8] | ((unsigned int)p[9] << 8);

    Gui_FillRegion(x, y, w, h);  // 开窗; 硬件偏移由 GUI 层统一补
    n = w * h;
    for (i = 0; i < n; i++) // 补丁像素一次连写, 无清屏无闪烁
    {
        picL = p[12 + i * 2];
        picH = p[12 + i * 2 + 1];
        Gui_PushPixel(picH << 8 | picL);
    }
}

/* Pet_Blink —— 播一遍眨眼: 闭眼补丁 160ms 后回待机姿
 * 用法：Pet_Blink();  播完自动恢复基准帧
 * 参数：无
 * 返回：无
 * 说明：补丁只有 19x6=240 字节, 是四个动作里最省的; 阻塞式, 播放约 170ms */
void Pet_Blink(void)
{
    Pet_DrawPatch(pet_blink_closed);
    delay_ms(160);
    showimage_xy(gImage_girl48, 74, 90);  // 回待机姿: 睁眼
}

/* Pet_Bob —— 播一遍呼吸浮动: 整图下移 1px 停 600ms 再回来, 零补丁数据
 * 用法：Pet_Bob();
 * 参数：无
 * 返回：无
 * 说明：呼吸只是位移, 不存新图; 代价是贴边墨迹要手工抹: 下移后 y90 行会留着
       原来第 0 行的发丝, y138 行会多出基准第 47 行的 2 个墨点, 各画一条
       背景线抹掉, 结果与算法合成帧逐像素一致 */
void Pet_Bob(void)
{
    showimage_xy(gImage_girl48, 74, 91);
    Gui_DrawLine(74, 90, 121, 90, UI_BG);   // 抹掉悬在原位的发丝
    Gui_DrawLine(74, 138, 121, 138, UI_BG); // 抹掉越界的脚底墨点
    delay_ms(600);
    showimage_xy(gImage_girl48, 74, 90);    // 浮回来, y138 已是背景无需再抹
}

/* Pet_Lift —— 播一遍举爱心: 抬1/2/3 -> 定格500ms -> 放回, 共约 1.2s
 * 用法：Pet_Lift();
 * 参数：无
 * 返回：无
 * 说明：三个补丁是同一个 32x26 包围盒(爱心+蓝手的活动范围), 逐帧贴上去
       不会有残影; 最后一位是放回待机姿, 直接重画基准帧 */
void Pet_Lift(void)
{
    Pet_DrawPatch(pet_lift_1);
    delay_ms(140);
    Pet_DrawPatch(pet_lift_2);
    delay_ms(140);
    Pet_DrawPatch(pet_lift_3);
    delay_ms(500);
    Pet_DrawPatch(pet_lift_2);
    delay_ms(140);
    Pet_DrawPatch(pet_lift_1);
    delay_ms(140);
    showimage_xy(gImage_girl48, 74, 90);  // 放下爱心, 回待机姿
}

/* Pet_Sleep —— 播一遍入睡: 闭眼 -> Z 字从头旁往左上飘三步 -> 收尾
 * 用法：Pet_Sleep();
 * 参数：无
 * 返回：无
 * 说明：闭眼和三个 Z 位置共用一个 26x25 包围盒(眼睛区+Z 的活动区),
       共 4 个补丁; 真正的睡眠状态机(22:00-08:00 定格)是后续工作,
       这版只是把动作本身跑通 */
void Pet_Sleep(void)
{
    Pet_DrawPatch(pet_sleep_close);
    delay_ms(450);
    Pet_DrawPatch(pet_sleep_z1);
    delay_ms(450);
    Pet_DrawPatch(pet_sleep_z2);
    delay_ms(450);
    Pet_DrawPatch(pet_sleep_z3);
    delay_ms(600);
    showimage_xy(gImage_girl48, 74, 90);  // 醒来, 回待机姿
}

/* ==================== 桌宠动画播放器 ====================
 * 原来四个动作是 delay_ms 串起来的阻塞函数, 一轮 3.8 秒就把主循环拖到 0.26Hz。
 * 现在改成"帧表 + 每圈推进一步": 一次动作 = 若干帧, 每帧 = 一块补丁 + 停留毫秒,
 * 到点才画下一帧, 期间主循环照常跑, 时间和底部轮播一点不受影响。
 *
 * 调度规则:
 *   醒着        -> 常驻呼吸(1.8 秒一个来回); 呼吸间隙随机眨眼、随机举爱心
 *   每个整点    -> 必举一次爱心(压掉这一次的随机抬手)
 *   22:00~08:00 -> 睡觉: 闭眼 + Z 字循环, 不呼吸、不眨眼、不举爱心
 */

#define PET_X        74        /* 桌宠左上角在屏幕上的坐标 */
#define PET_Y        90
#define PET_W        48        /* 基准帧(待机姿)的宽高 */
#define PET_H        48

#define PET_ACT_BLINK  0       /* 眨眼 */
#define PET_ACT_LIFT   1       /* 举爱心 */
#define PET_ACT_SLEEP  2       /* 睡觉: 只由时段触发, 不进随机池 */
#define PET_ACT_NUM    3

#define PET_SLEEP_FRAME 4      /* 睡觉那组帧没有收尾帧, 播完从头再来 */

#define PET_BREATH_MS  		900u    	/* 呼吸: 沉下去、浮上来各停 900ms, 一个来回 1.8 秒 */
#define PET_BLINK_LO  		2500u    	/* 随机眨眼间隔 2.5~6 秒 */
#define PET_BLINK_HI  		6000u
#define PET_LIFT_LO  		60000u    	/* 随机举爱心间隔 1~3 分钟, 平均两分钟一次 */
#define PET_LIFT_HI 		180000u
#define PET_SLEEP_ON    	22u    		/* 22:00 入睡 */
#define PET_SLEEP_OFF    	7u    		/* 07:00 起床, 和亮度一起切 */
#define PET_NIGHT_BRIGHT 	20u  		/* 夜里的背光: 20% */
#define PET_DAY_BRIGHT 		100u  		/* 白天的背光: 100% */
#define PET_FADE_MS      	50u  		/* 渐暗/渐亮每步隔 50ms */
#define PET_FADE_STEP    	4u  		/* 每步 4 个百分点, 100->20 共 20 步 = 1 秒 */

/* 一帧的完整描述; ms = 0 表示序列结束, 这时 rx/ry/rw/rh 就是收尾要恢复的矩形 */
typedef struct
{
    const unsigned char* patch;   /* 补丁帧(自带 x/y); 0 只出现在收尾标记帧上 */
    u16                  ms;      /* 这一帧停留多少毫秒 */
    u16 rx, ry, rw, rh;           /* 收尾时从基准帧搬回来的矩形 */
} Pet_Step;

/* 眨眼: 只有一块 19x6 的闭眼补丁 */
static const Pet_Step pet_seq_blink[] =
{
    { pet_blink_closed, 160, 86, 108, 19,  6 },
    { 0,                   0, 86, 108, 19,  6 }
};

/* 抬手: 三张 31x26 补丁, 抬 -> 定格 500ms -> 放回; 连举三下, 一下 0.92 秒共 2.9 秒
 * 三下之间的 lift_1 既是这一下的"放回", 也是下一上的"起手", 所以中间不重复写 */
static const Pet_Step pet_seq_lift[] =
{
    { pet_lift_1, 140, 76, 112, 31, 26 },
    { pet_lift_2, 140, 76, 112, 31, 26 },
    { pet_lift_3, 500, 76, 112, 31, 26 },
    { pet_lift_2, 140, 76, 112, 31, 26 },
    { pet_lift_1, 140, 76, 112, 31, 26 },
    { pet_lift_2, 140, 76, 112, 31, 26 },
    { pet_lift_3, 500, 76, 112, 31, 26 },
    { pet_lift_2, 140, 76, 112, 31, 26 },
    { pet_lift_1, 140, 76, 112, 31, 26 },
    { pet_lift_2, 140, 76, 112, 31, 26 },
    { pet_lift_3, 500, 76, 112, 31, 26 },
    { pet_lift_2, 140, 76, 112, 31, 26 },
    { pet_lift_1, 140, 76, 112, 31, 26 },
    { 0,           0, 76, 112, 31, 26 }
};

/* 睡觉: 闭眼 + Z 字往左上飘三步; 醒着切回来时才收尾, 所以这里不放结束帧 */
static const Pet_Step pet_seq_sleep[] =
{
    { pet_sleep_close, 450, 81, 90, 26, 25 },
    { pet_sleep_z1,    450, 81, 90, 26, 25 },
    { pet_sleep_z2,    450, 81, 90, 26, 25 },
    { pet_sleep_z3,    600, 81, 90, 26, 25 }
};

static const Pet_Step* const pet_seq[PET_ACT_NUM] =
{
    pet_seq_blink, pet_seq_lift, pet_seq_sleep
};

static const Pet_Step* s_pet_seq;    /* 正在播的序列; 0 = 空闲(在呼吸节拍上) */
static u8               s_pet_idx;   /* 播到第几帧 */
static u32              s_pet_t0;    /* 这一帧 / 这个呼吸相位是什么时候开始的(ms) */
static u8               s_pet_sink;  /* 呼吸: 0 = 浮到位 / 1 = 沉下去 1px */
static u8               s_pet_mode;  /* 0 = 醒着 / 1 = 睡觉 */
static u8               s_pet_ready; /* 0 = 随机计时还没起过 */
static u8               s_pet_hour;  /* 上一个已举过爱心的整点, 防止整分钟里反复触发 */
static u32              s_pet_t_blink;  /* 下一次随机眨眼的时刻 */
static u32              s_pet_t_lift;   /* 下一次随机举爱心的时刻 */
static u8               s_bl_cur    = PET_DAY_BRIGHT;  /* 当前背光, 上电全亮 */
static u8               s_bl_target = PET_DAY_BRIGHT;  /* 目标背光, 睡觉时段压到 20% */
static u32              s_bl_t0;                      /* 上一次改亮度的时间 */
static u32              s_pet_rand = 0x2A57u;  /* 32 位线性同余, 不引 stdlib 的 rand */

/* Pet_RestoreRect —— 从待机姿里把一小块矩形搬回屏幕
 * 用法：Pet_RestoreRect(86, 108, 19, 6);
 * 参数：x,y 屏幕左上角; w,h 宽高
 * 返回：无
 * 说明：动作收尾只补"被改过的那一小块", 不重画整只 ——
 *       整只 48x48 是 2304 像素约 10ms, 看得见闪; 眨眼那块只有 114 像素, 约 0.5ms;
 *       矩形出到图外的部分(呼吸下移多出来那一行)填底色, 不让源图越界读 */
static void Pet_RestoreRect(u16 x, u16 y, u16 w, u16 h)
{
    const unsigned char* p = gImage_girl48 + 8;   /* 跳过 8 字节图片头 */
    u16 i, j;
    u32 k;

    Gui_FillRegion(x, y, w, h);
    for (j = 0; j < h; j++)
    {
        for (i = 0; i < w; i++)
        {
            if ((u32)(y - PET_Y + j) < PET_H)               // 这一行还在图里
            {
                k = ((u32)(y - PET_Y + j) * PET_W + (x - PET_X + i)) * 2u;
                Gui_PushPixel((u16)((p[k + 1] << 8) | p[k]));
            }
            else
            {
                Gui_PushPixel(UI_BG);                        // 图外那行填底色
            }
        }
    }
}

/* Pet_DrawFrame —— 把当前这一帧贴到屏上
 * 用法：内部调用, 每到一个帧的起点调一次
 * 参数：无, 直接读 s_pet_seq / s_pet_idx
 * 返回：无
 * 说明：帧表里每一帧都自带补丁和坐标, 直接贴;
 *       同一个动作的几帧矩形完全一致, 帧与帧之间直接覆盖, 不用先擦 */
static void Pet_DrawFrame(void)
{
    Pet_DrawPatch(s_pet_seq[s_pet_idx].patch);
}

/* Pet_Sink —— 呼吸下半程: 整只下移 1px
 * 用法：内部调用
 * 参数：无 / 返回：无
 * 说明：原位顶边会露出上半截头发, 新位底边会露出越界的鞋, 各抹一条线 */
static void Pet_Sink(void)
{
    showimage_xy(gImage_girl48, PET_X, PET_Y + 1);
    Gui_DrawLine(PET_X, PET_Y,         PET_X + PET_W - 1, PET_Y,         UI_BG);
    Gui_DrawLine(PET_X, PET_Y + PET_H, PET_X + PET_W - 1, PET_Y + PET_H, UI_BG);
}

/* Pet_Rise —— 呼吸上半程: 把整只搬回原位
 * 用法：内部调用
 * 参数：无 / 返回：无
 * 说明：高度取 PET_H + 1, 多出来那一行由 Pet_RestoreRect 填底色,
 *       所以画面不会残留一条下移时的鞋印 */
static void Pet_Rise(void)
{
    Pet_RestoreRect(PET_X, PET_Y, PET_W, PET_H + 1);
}

/* Pet_Rand —— 取一个 [lo, hi] 之间的伪随机数(毫秒)
 * 用法：Pet_Rand(2500, 6000);
 * 参数：lo 下限, hi 上限(含); 单位毫秒, 可以超过 65535
 * 返回：lo ~ hi
 * 说明：32 位线性同余(经典 1103515245/12345), 取中间 24 位当随机源 ——
 *       最大跨度 180000 只占 24 位容量的百分之一, 取模的偏斜看不出来;
 *       参数必须是 u32, 用 u16 装 180000 会被截断成 4864, 区间就反了 */
static u32 Pet_Rand(u32 lo, u32 hi)
{
    s_pet_rand = s_pet_rand * 1103515245u + 12345u;
    return lo + (((s_pet_rand >> 8) & 0xFFFFFFu) % (hi - lo + 1u));
}

/* Pet_SleepEnter —— 进入睡眠状态
 * 用法：内部调用
 * 参数：now 当前毫秒时刻 / 返回：无
 * 说明：先把整只身子重画一遍, 顺手抹掉可能只播了一半的眨眼/抬手补丁,
 *       再从闭眼那一帧开始循环 */
static void Pet_SleepEnter(u32 now)
{
    Pet_RestoreRect(PET_X, PET_Y, PET_W, PET_H);
    s_pet_sink = 0;
    s_pet_idx  = 0;
    s_pet_seq  = pet_seq[PET_ACT_SLEEP];
    s_pet_t0   = now;
    s_pet_mode = 1;
    Pet_DrawFrame();
}

/* Pet_SleepExit —— 退出睡眠状态, 回到呼吸
 * 用法：内部调用
 * 参数：now 当前毫秒时刻 / 返回：无
 * 说明：只补闭眼和 Z 字那一块, 整只不用重画; 两个随机计时重新起表,
 *       免得刚醒就立刻眨眼 */
static void Pet_SleepExit(u32 now)
{
    Pet_RestoreRect(81, 90, 26, 25);
    s_pet_seq  = 0;
    s_pet_mode = 0;
    s_pet_sink = 0;
    s_pet_t0   = now;
    s_pet_t_blink = now + Pet_Rand(PET_BLINK_LO, PET_BLINK_HI);
    s_pet_t_lift  = now + Pet_Rand(PET_LIFT_LO,  PET_LIFT_HI);
}

/* WeatherClock_PetPlay —— 发起一个桌宠动作, 立刻返回
 * 用法：WeatherClock_PetPlay(PET_ACT_LIFT);
 * 参数：act PET_ACT_BLINK / PET_ACT_LIFT
 * 返回：无
 * 说明：正在播就直接忽略 —— 不排队也不打断; 排队会两个动作叠着播, 打断会半截停住;
 *       睡觉不走这个入口; 身子沉在低位时先浮回来, 否则补丁会比眼睛低 1px */
void WeatherClock_PetPlay(u8 act)
{
    if (act >= PET_ACT_NUM)
        return;   // 动作号越界
    if (act == PET_ACT_SLEEP)
        return;   // 睡觉只由 22:00~08:00 时段触发
    if (s_pet_seq != 0)
        return;   // 上一个还没播完

    if (s_pet_sink)
    {
        Pet_Rise();
        s_pet_sink = 0;
    }

    s_pet_seq = pet_seq[act];
    s_pet_idx = 0;
    s_pet_t0  = delay_uptime();

    Pet_DrawFrame();                      // 第 0 帧当场画, 不能等到下一圈
}

/* Pet_BrightnessPoll —— 把背光一步一步挪向目标值, 主循环每圈调一次
 * 用法：Pet_BrightnessPoll(now);   // now 就是 delay_uptime() 的值
 * 参数：now 当前毫秒时刻
 * 返回：无
 * 说明：当前值已经等于目标值时立刻返回, 一个寄存器都不写;
 *       每 50ms 走 4 个百分点, 100->20 正好 1 秒, 看起来是渐暗而不是跳变;
 *       不阻塞 —— 每次只写一个 CCR, 写完立刻回主循环 */
static void Pet_BrightnessPoll(u32 now)
{
    if (s_bl_cur == s_bl_target)
        return;                 // 已经到位, 什么都不做
    if ((u32)(now - s_bl_t0) < PET_FADE_MS)
        return;      // 这一档还没停够 50ms

    s_bl_t0 = now;
    if (s_bl_cur < s_bl_target)                         // 往亮里走
    {
        s_bl_cur = (u8)(s_bl_cur + PET_FADE_STEP);
        if (s_bl_cur > s_bl_target)
            s_bl_cur = s_bl_target;
    }
    else                                                // 往暗里走
    {
        s_bl_cur = (u8)(s_bl_cur - PET_FADE_STEP);
        if (s_bl_cur < s_bl_target)
            s_bl_cur = s_bl_target;
    }
    LCD_SetBacklight(s_bl_cur);
}

/* WeatherClock_PollPet —— 推进桌宠动画, 主循环每圈调一次
 * 用法：主循环里 WeatherClock_PollPet(epoch);
 * 参数：epoch 当前秒级时间, 用来判睡眠时段和整点
 * 返回：无
 * 说明：整圈只做几次比较, 没到点立刻返回, 一个像素都不碰;
 *       帧耗时靠每帧重取时间戳对齐, 某圈被取数拖慢也不会累积漂移 */
void WeatherClock_PollPet(u32 epoch)
{
    DateTime dt;
    const Pet_Step *st;
    u32 now;
    u8  want;

    now = delay_uptime();                 /* 不能用 delay_millis: 它 59.6 秒回绕一次, 存未来时刻会被吃掉 */
    Pet_BrightnessPoll(now);              /* 背光跟着昼夜走: 到点渐暗或渐亮, 每 50ms 一步 */

    dt.hour = 12;                         /* RTC 没校过时按白天处理, 不跟着 1970 跑 */
    dt.min  = 0;
    dt.sec  = 0;
    if (RTC_IsValid() != 0)
        RTC_DateTimeFromEpoch(epoch, &dt);

    if (s_pet_ready == 0)                 // 开机第一圈: 随机计时起表
    {
        s_pet_ready   = 1;
        s_pet_hour    = (u8)((RTC_IsValid() != 0) ? dt.hour : 0xFFu);
        s_pet_t0      = now;
        s_pet_t_blink = now + Pet_Rand(PET_BLINK_LO, PET_BLINK_HI);
        s_pet_t_lift  = now + Pet_Rand(PET_LIFT_LO,  PET_LIFT_HI);
    }

    /* 1. 时段: 22:00~07:00 睡觉, 进出各做一次; 背光目标跟着一起切, 实际变化交给渐变 */
    want = (u8)((dt.hour >= PET_SLEEP_ON || dt.hour < PET_SLEEP_OFF) ? 1 : 0);
    if (want != s_pet_mode)
    {
        if (want)
        {
            Pet_SleepEnter(now);
            s_bl_target = PET_NIGHT_BRIGHT;
        }
        else
        {
            Pet_SleepExit(now);
            s_bl_target = PET_DAY_BRIGHT;
        }
        return;
    }

    /* 2. 睡觉: 四帧循环, Z 字飘走再从眼前重新出来 */
    if (s_pet_mode != 0)
    {
        st = &s_pet_seq[s_pet_idx];
        if ((u32)(now - s_pet_t0) < (u32)st->ms)
            return;   // 这帧还没停够

        s_pet_idx++;
        if (s_pet_idx >= PET_SLEEP_FRAME)
            s_pet_idx = 0;
        s_pet_t0 = now;
        Pet_DrawFrame();
        return;
    }

    /* 3. 醒着: 正在播的动作先推完, 推完就回到呼吸节拍 */
    if (s_pet_seq != 0)
    {
        st = &s_pet_seq[s_pet_idx];
        if ((u32)(now - s_pet_t0) < (u32)st->ms)
            return;

        s_pet_idx++;
        st = &s_pet_seq[s_pet_idx];

        if (st->ms == 0)                  // 收尾帧: 只补被改过的那块
        {
            Pet_RestoreRect(st->rx, st->ry, st->rw, st->rh);
            s_pet_seq = 0;
            s_pet_t0  = now;              // 呼吸从这一圈重新起表
            return;
        }

        s_pet_t0 = now;
        Pet_DrawFrame();
        return;
    }

    /* 4. 准点举爱心: 整点那一分钟里举一次; s_pet_hour 记住已处理过的整点 */
    if (RTC_IsValid() != 0 && dt.min == 0 && s_pet_hour != dt.hour)
    {
        s_pet_hour   = dt.hour;
        s_pet_t_lift = now + Pet_Rand(PET_LIFT_LO, PET_LIFT_HI);
        WeatherClock_PetPlay(PET_ACT_LIFT);
        return;
    }

    /* 5. 随机事件: 眨眼比抬手密得多, 抬手十几秒才来一次 */
    if ((s32)(now - s_pet_t_blink) >= 0)
    {
        s_pet_t_blink = now + Pet_Rand(PET_BLINK_LO, PET_BLINK_HI);
        WeatherClock_PetPlay(PET_ACT_BLINK);
        return;
    }
    if ((s32)(now - s_pet_t_lift) >= 0)
    {
        s_pet_t_lift = now + Pet_Rand(PET_LIFT_LO, PET_LIFT_HI);
        WeatherClock_PetPlay(PET_ACT_LIFT);
        return;
    }

    /* 6. 空闲: 常驻呼吸, 每 900ms 翻一次相位 */
    if ((u32)(now - s_pet_t0) >= PET_BREATH_MS)
    {
        s_pet_t0   = now;
        s_pet_sink = (u8)(s_pet_sink ^ 1);
        if (s_pet_sink)
            Pet_Sink();
        else
            Pet_Rise();
    }
}

/* 高低温行用的两个配色, 整屏背景统一走 UI_Picture.h 里的 UI_BG */
#define UI_TXT_HIGH  0xFC00   /* 最高温: 橙红 */
#define UI_TXT_LOW   0x87FF   /* 最低温: 浅青 */
#define UI_TXT_IN    0xFD20   /* 室内标签: 暖橙, 与最高温同族 */
#define UI_TXT_HUM   0x5FFB   /* 湿度标签: 亮青, 与最低温同族 */
#define UI_TXT_WEEK  0xFDE7   /* 日期里的星期二字: 浅黄点缀 */

/* UI_FillRect —— 用一整块纯色盖住矩形
 * 用法：UI_FillRect(0, 48, 128, 37, UI_BG);
 * 参数：x,y 左上角; w,h 宽高; c RGB565 颜色
 * 返回：无
 * 说明：GUI 层只有 Gui_FillRegion(只开窗不写) 和 Gui_box(只画边框),
 *       想刷底色只能自己开窗后逐点灌, 和 Lcd_Clear 是同一个套路 */
static void UI_FillRect(u16 x, u16 y, u16 w, u16 h, u16 c)
{
    u32 n;

    Gui_FillRegion(x, y, w, h);
    for (n = 0; n < (u32)w * h; n++)
        Gui_PushPixel(c);
}

/* UI_DrawArrow —— 画一个 7 像素宽的实心三角箭头
 * 用法：UI_DrawArrow(101, 54, 1, UI_TXT_HIGH);   // 上箭头
 * 参数：cx 尖端所在列; y 顶行; up 1=尖端朝上, 0=尖端朝下; c 颜色
 * 返回：无
 * 说明：原来四条线的坐标是写死的, 现在按 cx 参数化;
 *       温度位数一变(比如 -10), 数字起点会左移, 箭头得跟着挪才不会顶到数字 */
static void UI_DrawArrow(u16 cx, u16 y, u8 up, u16 c)
{
    if (up)
    {
        Gui_DrawLine(cx,     y,     cx,     y,     c);
        Gui_DrawLine(cx - 1, y + 1, cx + 1, y + 1, c);
        Gui_DrawLine(cx - 2, y + 2, cx + 2, y + 2, c);
        Gui_DrawLine(cx - 3, y + 3, cx + 3, y + 3, c);
    }
    else
    {
        Gui_DrawLine(cx - 3, y,     cx + 3, y,     c);
        Gui_DrawLine(cx - 2, y + 1, cx + 2, y + 1, c);
        Gui_DrawLine(cx - 1, y + 2, cx + 1, y + 2, c);
        Gui_DrawLine(cx,     y + 3, cx,     y + 3, c);
    }
}

/* UI_DrawMinus3 —— 画一个 3x2 的短横, 当数字的负号用
 * 用法：UI_DrawMinus3(37, 64, WHITE);
 * 参数：x,y 左上角; c 颜色
 * 返回：无
 * 说明：24 号数字一格 16px, 但字身只占 12px, 每格右边恒有 4px 空白,
 *       所以 3px 短横塞得进去, 数字不用降字号;
 *       厚度 2px 和数字的横画一样粗, y 取数字 x 高度的中线:
 *       16 号字墨迹在 y+2 ~ y+11, 传 y+6; 24 号字墨迹在 y+2 ~ y+17, 传 y+9 */
static void UI_DrawMinus3(u16 x, u16 y, u16 c)
{
    Gui_DrawLine(x,     y, (u16)(x + 2), y,     c);
    Gui_DrawLine(x, (u16)(y + 1), (u16)(x + 2), (u16)(y + 1), c);
}

/* UI_DrawTempBig —— 把当前气温按 24 号字右对齐画到 xRight 之前
 * 用法：UI_DrawTempBig(72, cw.now.temp);
 * 参数：xRight 右边界(不含); t 气温 ℃
 * 返回：无
 * 说明：24 号数字一格 16 像素, 右对齐能让个位始终贴在 ℃ 左边, 换数时不会左右跳;
 *       图标右缘 x=36 到 ℃ 左缘 x=72 只有 36px, "-10" 若连负号都占一格 16px 就要 48px,
 *       放不下, 所以负号改用 3px 短横: "-10" = 3 + 16 + 16 = 35px, 起点 x=37, 零下也能保持 24 号 */
static void UI_DrawTempBig(u16 xRight, s8 t)
{
    u8  buf[6];
    u8  n;
    u16 x;

    n = SD_FormatTemp((char*)buf, t);
    if (t >= 0)
    {
        Gui_DrawNum24String((u16)(xRight - (u16)n * 16), 55, WHITE, UI_BG, (const char*)buf);
        return;
    }

    x = (u16)(xRight - (u16)(n - 1) * 16 - 3);   /* 数字位数*16 再加 3px 负号 */
    UI_DrawMinus3(x, 64, WHITE);                 /* 55 + 9, 对齐 24 号字的 x 高度中线 */
    Gui_DrawNum24String((u16)(x + 3), 55, WHITE, UI_BG, (const char*)(buf + 1));
}

/* UI_DrawTempSmall —— 把最高/最低温按 16 号字右对齐画到 xRight 之前
 * 用法：x0 = UI_DrawTempSmall(122, 50, UI_TXT_HIGH, d->high);
 * 参数：xRight 右边界(不含); y 顶行; fc 字色; t 温度 ℃
 * 返回：数字起始列, 调用者拿它定位左边的箭头
 * 说明：右边界定 122 是为了和原来的 "28" 位置完全重合(x=106 起, 右边留 6px);
 *       负号同样用 3px 短横, "-21" 只占 3 + 2*8 = 19px, 起点 103,
 *       离 ℃ 右缘(95) 还有 8px, 不会顶上去; 箭头按返回的起点再左移 5px */
static u16 UI_DrawTempSmall(u16 xRight, u16 y, u16 fc, s8 t)
{
    u8  buf[6];
    u8  n;
    u16 w;

    n = SD_FormatTemp((char*)buf, t);
    if (t >= 0)
    {
        w = (u16)n * 8;
        Gui_DrawFont_GBK16((u16)(xRight - w), y, fc, UI_BG, buf);
        return (u16)(xRight - w);
    }

    w = (u16)((u16)(n - 1) * 8 + 3);             /* 数字位数*8 再加 3px 负号 */
    UI_DrawMinus3((u16)(xRight - w), (u16)(y + 6), fc);
    Gui_DrawFont_GBK16((u16)(xRight - w + 3), y, fc, UI_BG, (u8*)(buf + 1));
    return (u16)(xRight - w);
}

/* UI_DrawWeatherRow —— 整块重画天气行(图标 + 气温 + ℃ + 最高最低温)
 * 用法：WeatherClock_UI 里画一次; 之后由 WeatherClock_RefreshWeather 重画
 * 参数：无
 * 返回：无
 * 说明：先铺一遍底色再画, 否则 "26" 变成 "9" 的时候左边会留半截旧墨迹;
 *       y 取 48~84, 正好夹在 y=47 和 y=85 两条分割线中间, 不会把线擦掉;
 *       一整块 128x37 共 4736 像素, 软件 SPI 下约 20ms, 一天只跑几次,
 *       不值得为省这点时间去做脏矩形 */
static void UI_DrawWeatherRow(void)
{
    u16 x0;

    UI_FillRect(0, 48, 128, 37, UI_BG);   /* 清掉上一次画的数字和图标 */

    if (sd_weather.valid == 0)            /* 数据还没到: 问号图标 + 三条横杠 */
    {
        Gui_DrawWeatherIcon(4, 50, 99);
        Gui_DrawFont_GBK16(56, 59, WHITE, UI_BG, (u8*)"--");
        Gui_DrawFont_GBK24(72, 55, WHITE, UI_BG, (u8*)"\xa1\xe6");
        Gui_DrawFont_GBK16(106, 50, UI_TXT_HIGH, UI_BG, (u8*)"--");
        Gui_DrawFont_GBK16(106, 66, UI_TXT_LOW,  UI_BG, (u8*)"--");
        return;
    }

    Gui_DrawWeatherIcon(4, 50, sd_weather.code);   /* 天气码直接给图标层, 越界它自己走"未知" */
    UI_DrawTempBig(72, sd_weather.temp);
    Gui_DrawFont_GBK24(72, 55, WHITE, UI_BG, (u8*)"\xa1\xe6");

    x0 = UI_DrawTempSmall(122, 50, UI_TXT_HIGH, sd_weather.high);
    UI_DrawArrow((u16)(x0 - 5), 54, 1, UI_TXT_HIGH);

    x0 = UI_DrawTempSmall(122, 66, UI_TXT_LOW, sd_weather.low);
    UI_DrawArrow((u16)(x0 - 5), 70, 0, UI_TXT_LOW);
}

/* WeatherClock_SetIndoor —— 把 sd_indoor 里拼好的室内温湿度画到左下角
 * 用法：if (SD_UpdateIndoor(epoch)) WeatherClock_SetIndoor();
 * 参数：无 (数据全在 sd_indoor, 格式化已经在 screen_data.c 做完)
 * 返回：无
 * 说明：温度格式 "-10.5" 最多 5 个字符, 16 号字一个 8px, 负号只占 3px,
 *       所以最宽 = 3 + 4*8 = 35px; 右对齐到 x=73(桌宠从 74 起, 差 1px 刚好不压);
 *       正温 "21.0" 只有 32px, 右对齐后左端 41, 正好落在标签"室内 "的空格上, 不会挤到"内"字;
 *       画之前先把 x=38~73 铺一遍底色, 否则从 "-5.0" 跳回 "5.0" 时旧负号会留在屏上;
 *       湿度固定两位, DHT11 量程 20~90% 不会出现一位或三位;
 *       还没读到过一次数据(valid=0)就直接返回, 不画 0.0 骗人 */
void WeatherClock_SetIndoor(void)
{
    u8  hb[4];
    u8  neg;
    u16 w, x;

    if (sd_indoor.valid == 0)
        return;

    neg = (u8)((sd_indoor.tstr[0] == '-') ? 1 : 0);
    w   = (u16)((u16)(sd_indoor.tlen - neg) * 8 + (neg ? 3 : 0));
    x   = (u16)(73 - w);

    UI_FillRect(38, 94, 36, 16, UI_BG);   /* 值区整段铺底, 位数或正负变了都不留旧墨 */
    if (neg)
        UI_DrawMinus3(x, 100, WHITE); /* 94 + 6, 对齐 16 号字的 x 高度中线 */
    Gui_DrawFont_GBK16((u16)(x + (neg ? 3 : 0)), 94, WHITE, UI_BG,
                       (u8*)(sd_indoor.tstr + neg));

    hb[0] = (u8)('0' + sd_indoor.humi / 10);  /* 十位 */
    hb[1] = (u8)('0' + sd_indoor.humi % 10);  /* 个位 */
    hb[2] = 0;
    Gui_DrawFont_GBK16(42, 118, WHITE, UI_BG, (u8*)hb);
}

/* WeatherClock_RefreshWeather —— 天气数据变了就重画那一行, 别处一概不动
 * 用法：if (SD_UpdateWeather()) WeatherClock_RefreshWeather();
 * 参数：无
 * 返回：无
 * 说明：只覆盖 y=48~84, 桌宠静态帧在 y=90 以下, 室内温湿度在 y=94 以下,
 *       三者互不重叠, 所以刷新时那两块一个像素都不会闪 */
void WeatherClock_RefreshWeather(void)
{
    UI_DrawWeatherRow();
}

/* ---- 时间/日期上屏的判重缓存, 只有 WeatherClock_Tick 会动 ---- */
static char s_time_str[9];      /* 上次上屏的 "HH:MM:SS", 逐位比较用 */
static u8   s_date_mon;         /* 上次上屏的月 */
static u8   s_date_day;         /* 上次上屏的日 */
static u8   s_date_week;        /* 上次上屏的星期 */
static u8   s_date_valid;       /* 0 = 缓存还没填过, 下一次必须整行重画 */
static u8   s_clock_force;      /* 1 = 屏刚被清过, 下一次 Tick 必须全量重画 */

/* WeatherClock_Tick —— 把当前时间画到主屏顶部, 每秒调一次
 * 用法：WeatherClock_Tick(RTC_GetEpoch());
 * 参数：epoch Unix 秒(UTC), 内部自己加 8 小时时区
 * 返回：无
 * 说明：时间行 8 个 24 号字正好 128 宽, 每个字符占 i*16 列, **逐位比较只重画变了的那几位**,
 *       一秒只写 2 个字符(32x24=768 像素), 而不是每次刷 128x24, 主循环几乎不受影响;
 *       日期行跨天才重画, 重画前先铺一遍底色 —— "10月3日" 换成 "10月27日" 位数变了,
 *       不铺底会留下半个旧数字;
 *       星期固定画在 x=76, 不跟着日期宽度走, 免得每天左右跳 */
void WeatherClock_Tick(u32 epoch)
{
    DateTime dt;
    char buf[16];
    u8 i, d;

    if (s_clock_force)                 /* 整屏刚清过, 缓存作废, 下面必须全画 */
    {
        s_clock_force  = 0;
        s_date_valid   = 0;
        for (i = 0; i < 9; i++)
            s_time_str[i] = 0;
    }

    /* 没校过时: RTC 计数器还是 0, 硬算会得到 1970-01-01, 不能摆到屏上。
     * 时间位画 "--:--:--"(冒号照常, 数字位用 10px 长横顶替), 日期整行留空;
     * s_time_str 里存 '-' 当"这一位已经是短横"的标记 —— 校时成功后 buf[i] 是
     * 数字, 必然不等于 '-', 于是每一位自动重画一遍, 不需要额外的状态变量 */
    if (RTC_IsValid() == 0)
    {
        for (i = 0; i < 8; i++)
        {
            if (s_time_str[i] == '-')
                continue;   /* 这一位已经是短横了 */
            s_time_str[i] = '-';
            if (i == 2 || i == 5)
            {
                Gui_DrawFont_Num24((u16)(i * 16), 2, WHITE, UI_BG, 10);  /* 冒号在字库里是 10 号 */
            }
            else
            {
                UI_FillRect((u16)(i * 16), 2, 16, 24, UI_BG);            /* 先擦掉可能残留的数字 */
                UI_FillRect((u16)(i * 16 + 3), 11, 10, 2,
                            WHITE);        /* 10px 长横顶替一个数字位, 2px 粗和数字横画一致 */
            }
        }
        if (s_date_valid)   /* 只在刚进无效态时清一次日期, 不必每秒铺底 */
        {
            s_date_valid = 0;
            UI_FillRect(0, 30, 128, 16, UI_BG);
        }
        return;
    }

    RTC_DateTimeFromEpoch(epoch, &dt);

    /* 时间: 逐位比较, 只重画变了的字符; ':' 在字库里是 10 号 */
    SD_FormatTime(buf, dt.hour, dt.min, dt.sec);
    for (i = 0; i < 8; i++)
    {
        if (buf[i] == s_time_str[i])
            continue;
        s_time_str[i] = buf[i];
        d = (buf[i] == ':') ? (u8)10 : (u8)(buf[i] - '0');
        Gui_DrawFont_Num24((u16)(i * 16), 2, WHITE, UI_BG, (u16)d);
    }

    /* 日期: 跨天才重画 */
    if (s_date_valid && dt.month == s_date_mon && dt.day == s_date_day && dt.week == s_date_week)
        return;

    s_date_valid = 1;
    s_date_mon   = dt.month;
    s_date_day   = dt.day;
    s_date_week  = dt.week;

    UI_FillRect(0, 30, 128, 16, UI_BG);
    SD_FormatDate(buf, dt.month, dt.day);
    Gui_DrawFont_GBK16(4, 30, GRAY0, UI_BG, (u8*)buf);
    SD_FormatWeek(buf, dt.week);
    Gui_DrawFont_GBK16(76, 30, UI_TXT_WEEK, UI_BG, (u8*)buf);
}

/* WeatherClock_UI —— 画一整屏框架, 天气行已经是真数据
 * 用法：Lcd_Init() 之后调一次 WeatherClock_UI();
 * 参数：无
 * 返回：无
 * 说明：整屏只画一次框架, 之后各块按需局部刷新, 不做整屏重绘;
 *       天气行(图标 + 三个温度)已经是真数据, 变了由 WeatherClock_RefreshWeather 重画;
 *       时间 09:28:20 / 日期 10月27日 / 室内温湿度 / 滚动行仍是固定值, 下一批再接;
 *       整屏背景统一用 UI_BG, 和天气图标里烘焙的透明底色同色, 不会出现色块;
 *       区块之间用 UI_SEP 色画了 4 条 1px 分割线(y=27/47/85/140) */
void WeatherClock_UI(void)
{
    Lcd_Clear(UI_BG);  // 整屏铺底色

    /* 区块分割线: 时间/日期/天气/室内桌宠/滚动 五个区块之间各一条 1px;
       Gui_DrawLine 是逐点写的, 4 条共 512 点只在初始化画一次, 速度无所谓;
       画线不碰滚动行(y144~159), main 循环里每 30ms 重画滚动不会擦掉它们 */
    Gui_DrawLine(0, 27, 127, 27, UI_SEP);
    Gui_DrawLine(0, 47, 127, 47, UI_SEP);
    Gui_DrawLine(0, 85, 127, 85, UI_SEP);
    Gui_DrawLine(0, 140, 127, 140, UI_SEP);


    /* 时间行 / 日期行不在这里画: 交给 WeatherClock_Tick, main 紧接着会调一次;
       这里只把判重缓存作废, 保证 Tick 一定会整行重画一遍 */
    s_clock_force = 1;

    /* 天气行: 图标 + 气温 + ℃ + 高低温, 画法统一在 UI_DrawWeatherRow 里 */
    UI_DrawWeatherRow();

    /* 左栏: 室内温湿度, 上行到 x73 为止, 不压 x74 起的桌宠舞台 */
    /* 标签着色 + 数字留白: GBK16 空白处写底色, 两段绘制区不能重叠 */
    Gui_DrawFont_GBK16(2, 94, UI_TXT_IN, UI_BG, (u8*)"室内 ");
//	Gui_DrawFont_GBK16(42, 94, WHITE, UI_BG, (u8 *)"24.6");
    Gui_DrawFont_GBK16(2, 118, UI_TXT_HUM, UI_BG, (u8*)"湿度 ");
//	Gui_DrawFont_GBK16(42, 118, WHITE, UI_BG, (u8 *)"50");

    /* 桌宠舞台: 48x48 底帧贴在左栏右侧, x74 起正好与文字相邻不重叠 */
    showimage_xy(gImage_girl48, 74, 90);

    /* 底部最后一行: 先画第 0 页, 之后 main 每圈调 WeatherClock_PollRow 推进 */
    WeatherClock_RowFirst();
}

/* Gui_DrawWeatherIcon —— 按天气码画一个 32x32 天气图标
 * 用法：Gui_DrawWeatherIcon(4, 50, code);   code 是心知天气返回的天气现象码
 * 参数：x,y 图标左上角坐标; code 天气现象码(0~38, 99 及其他越界值都当"未知")
 * 返回：无
 * 说明：16 色索引色, 每字节装两个像素(高 4 位是左边那个), 查调色板换成 RGB565 写屏;
 *       数据在 UI_Picture.h; 背景色已按 UI_BG(0x1149) 烘焙, 所以图标必须画在同色背景上,
 *       否则会看到一个方形色块; 以后改 UI_BG 记得同步重生成整批图标 */
void Gui_DrawWeatherIcon(u16 x, u16 y, u8 code)
{
    const u8  *px;     // 这个图标的索引数据
    const u16 *pal;    // 这个图标的 16 色调色板
    u16 i;
    u8  b, id;

    id  = (code > 38) ? ICON_UNKNOWN : ic_code_map[code];  // 越界码走"未知"桶, 防止读数组外
    pal = ic_pal_of[id];
    px  = ic_px_of[id];

    Gui_FillRegion(x, y, ICON_SIZE, ICON_SIZE);  // 开窗; 硬件偏移由 GUI 层统一补
    for (i = 0; i < ICON_BYTES; i++)  // 512 次循环, 每次写 2 个像素
    {
        b = px[i];
        Gui_PushPixel(pal[b >> 4]);      // 高 4 位 = 左边那个像素
        Gui_PushPixel(pal[b & 0x0F]);    // 低 4 位 = 右边那个像素
    }
}

/* Gui_ScrollTextWidth —— 算一条混合字符串在 16 号字下横着占多少像素
 * 用法：total = Gui_ScrollTextWidth((u8 *)"穿衣 较舒适");
 * 参数：s 字符串, 必须是 GBK 编码
 * 返回：总宽度(像素); 空串或空指针返回 0
 * 说明：汉字 16、ASCII 8, 和 Gui_DrawFont_GBK16 的前进量完全一致;
 *       字库里缺字的汉字也照样算 16 宽, 条带长度不会因为少字而变短 */
u16 Gui_ScrollTextWidth(const unsigned char* s)
{
    u16 total = 0;
    u8  w, stride;

    if (s == 0)
        return 0;
    while (*s)
    {
        Gui_Glyph16(s, &w, &stride);
        total += w;
        s += (w == 16) ? 2 : 1;   // 汉字吃 2 字节, ASCII 吃 1 字节
    }
    return total;
}

/* Gui_ScrollText —— 在 w 宽的窗口里画滚动字幕的第 offset 帧
 * 用法：total = Gui_ScrollTextWidth(str);
 *       offset++; if (offset >= total) offset = 0;        // 每帧推 1 像素
 *       Gui_ScrollText(0, 144, 128, WHITE, UI_BG, str, offset);
 * 参数：x,y 窗口左上角; w 窗口宽度(高度固定 16 像素, 等于一个汉字高);
 *       fc 字色; bc 底色; s 字符串(GBK); offset 已经滚过去多少像素
 * 返回：无
 * 说明：屏幕只能写不能读, 所以每帧都得把可见的 w x 16 重新算一遍再送出去;
 *       但字符在条带里是首尾相接的, 可见区一定被字符盖满,
 *       所以不必先刷一遍底色 —— 一帧刚好只写 w*16 个像素;
 *       offset 超过条带长度时会从右边接回开头, 看起来是无限循环, 中间没有空档 */
void Gui_ScrollText(u16 x, u16 y, u16 w, u16 fc, u16 bc,
                    const u8 *s, u16 offset)
{
    const u8 *g;
    u16 total, p;
    int sx, x0, x1, cols, row, c, col, wi;
    u8  cw, stride, bits;

    if (s == 0 || w == 0)
        return;
    total = Gui_ScrollTextWidth(s);
    if (total == 0)
        return;
    if (offset >= total)
        offset %= total;   // 越界就绕回去, 免得算出屏幕外坐标

    wi = (int)w;
    p  = 0;
    while (*s)
    {
        g  = Gui_Glyph16(s, &cw, &stride);
        s += (cw == 16) ? 2 : 1;

        sx = (int)p - (int)offset;          // 这个字符此刻落在屏幕第几列(可为负)
        while (sx + (int)cw <= 0)           // 整段还在窗口左边, 接到右边去
            sx += (int)total;

        while (sx < wi && g != 0)           // 缺字的只占宽度不画, 所以 g 为空直接跳过
        {
            x0 = (sx < 0) ? 0 : sx;                          // 与窗口求交后的左边界
            x1 = (sx + (int)cw > wi) ? wi : sx + (int)cw;    // 右边界
            cols = x1 - x0;

            if (cols >= 3)                  // 够宽才开窗口整段写, 驱动偏移由 GUI 层补掉了
            {
                Gui_FillRegion(x + (u16)x0, y, (u16)cols, 16);
                for (row = 0; row < 16; row++)
                {
                    for (c = 0; c < cols; c++)
                    {
                        col  = x0 - sx + c;                  // 换算成字模里的列号
                        bits = (u8)g[row * (int)stride + (col >> 3)];
                        Gui_PushPixel((bits & (0x80 >> (col & 7))) ? fc : bc);
                    }
                }
            }
            else                            // 只剩 1~2 列的碎片, 开窗会算出非法坐标, 逐点画
            {
                for (row = 0; row < 16; row++)
                {
                    for (c = 0; c < cols; c++)
                    {
                        col  = x0 - sx + c;
                        bits = (u8)g[row * (int)stride + (col >> 3)];
                        Lcd_DrawPoint(x + (u16)(x0 + c), y + (u16)row,
                                      (bits & (0x80 >> (col & 7))) ? fc : bc);
                    }
                }
            }
            sx += (int)total;               // 条带短于窗口时, 同一字符还会再绕一圈出现
        }
        p += cw;
    }
}


/* ===== 最后一行: 6 页轮播 + 垂直上移换页 ===== */

#define UI_ROW_Y        144   /* 最后一行窗口顶边, 高度固定 16(一个汉字高) */
#define UI_ROW_W        128   /* 窗口宽度, 占满整屏 */
#define UI_ROW_HOLD_SEC 4     /* 一页停多少秒: 用 RTC 秒数计时, 和主循环节拍无关 */
#define UI_ROW_FRAMES   16    /* 换页动画的步数, 0~16 共 17 帧, 每帧上移 1 像素 */

static u8  s_row_page;        /* 当前停在哪一页 0~SD_ROW_PAGES-1 */
static u8  s_row_frame;       /* 换页动画走到第几帧 0~16; 0xFF 表示静止没在动 */
static u32 s_row_sec;         /* 进入静止那一刻的 epoch 秒, 0xFFFFFFFF 表示还没记过 */
static u8  s_row_dirty;       /* 1 = 表内容更新过, 等这屏画完要补画一次 */

/* UI_RowDrawOne —— 把一页文案按垂直偏移 dy 画进最后一行窗口
 * 用法：UI_RowDrawOne("感冒:少发", 0);    // 静态整页
 *       UI_RowDrawOne("感冒:少发", -5);   // 往上飘 5 像素, 底部空出 5 行
 * 参数：s   GBK 文案; dy 文字顶边相对窗口顶边的偏移, -16~16 之外等于不画
 * 返回：无
 * 说明：屏幕只能写不能读, 所以每一帧都得把可见的 128x16 重新算一遍再送出去;
 *       只写落在窗口里的那几行字模, 不会溢到 y=140 的分割线或屏外 —— 这正是
 *       Gui_DrawFont_GBK16 做不到的, 它只会老老实实从 y 开始画 16 行;
 *       文案按像素宽度居中, 宽度超过 128 时从 x=0 起画、右边被裁掉(现在最宽 120, 碰不到);
 *       缺字(g == 0)只占宽度不画, 和 Gui_DrawFont_GBK16 的规矩一致 */
static void UI_RowDrawOne(const char* s, s16 dy)
{
    const u8 *p;
    const u8 *g;
    u16 total, x;
    s16 sx, r0, r1, r;
    u8  cw, stride, bits, c;

    if (s == 0)
        return;

    total = Gui_ScrollTextWidth((const u8*)s);
    x = (u16)((total < UI_ROW_W) ? ((UI_ROW_W - total) / 2) : 0);

    r0 = (s16)(-dy);                        /* 第一个还落在窗口里的字模行 */
    if (r0 < 0)
        r0 = 0;
    r1 = (s16)(15 - dy);                    /* 最后一个还落在窗口里的字模行 */
    if (r1 > 15)
        r1 = 15;
    if (r0 > r1)
        return;                    /* 这页整块都在窗口外, 这一帧没有它 */

    p  = (const u8*)s;
    sx = (s16)x;
    while (*p)
    {
        g = Gui_Glyph16(p, &cw, &stride);
        p += (cw == 16) ? 2 : 1;            /* 汉字吃 2 字节, ASCII 吃 1 字节 */

        if (sx >= (s16)UI_ROW_W)
            break;     /* 右边已经被裁光了, 后面的不用画 */
        if (g != 0)
        {
            Gui_FillRegion((u16)sx, (u16)(UI_ROW_Y + dy + r0), cw, (u16)(r1 - r0 + 1));
            for (r = r0; r <= r1; r++)
            {
                for (c = 0; c < cw; c++)
                {
                    bits = g[r * (s16)stride + (c >> 3)];
                    Gui_PushPixel((bits & (0x80 >> (c & 7))) ? WHITE : UI_BG);
                }
            }
        }
        sx += (s16)cw;
    }
}

/* WeatherClock_RowScroll —— 画出换页动画的第 t 帧: 旧页往上走, 新页从底下来
 * 用法：for (t = 0; t <= 16; t++) WeatherClock_RowScroll(a, b, t);
 * 参数：old_s 正在离开的那页; new_s 要顶上来的一页; t 0~16
 *       t = 0 全是旧页, t = 16 全是新页, 中间是上下各占一段
 * 返回：无
 * 说明：第 t 帧旧页占窗口上 16-t 行、新页占下 t 行, 两段严丝合缝拼满 16 行,
 *       既不露底色也不重叠, 所以每帧正好写满 128x16, 不用先刷一遍底;
 *       接缝处只是字被切断, 不会有半像素错位(两个页都是从自己的字模里按行取的);
 *       两页文案一样时也会照常走一遍, 上层想省这点开销就自己判重 */
void WeatherClock_RowScroll(const char* old_s, const char* new_s, u8 t)
{
    if (t > 16)
        t = 16;
    UI_RowDrawOne(old_s, (s16)(-(s16)t));      /* 旧页顶边跑到窗口上方 t 像素 */
    UI_RowDrawOne(new_s, (s16)(16 - (s16)t));/* 新页顶边压在窗口下方 16-t 像素处 */
}

/* WeatherClock_RowFirst —— 静态画第 0 页, 给开机首帧和整屏重绘用
 * 用法：WeatherClock_UI 末尾调一次
 * 参数：无 / 返回：无
 * 说明：顺便把计时器和页号归零, 整屏被清过之后一切从头开始 */
void WeatherClock_RowFirst(void)
{
    s_row_page  = 0;
    s_row_frame = 0xFF;            /* 0xFF = 静止, 不在动画里 */
    s_row_sec   = 0xFFFFFFFF;      /* 下一圈 PollRow 重新记基准时间 */
    s_row_dirty = 0;               /* 刚整屏画过, 没有欠账 */
    UI_RowDrawOne(sd_row[0], 0);
}

/* WeatherClock_RowRefresh —— 数据变了, 让最后一行重画一次
 * 用法：if (SD_UpdateRow ()) WeatherClock_RowRefresh ();
 * 参数：无 / 返回：无
 * 说明：这里只置一个标志, 真正的重画交给下一圈的 PollRow;
 *       换页动画正走到一半时不能插进去画整页, 会把画面跳一下,
 *       等这屏画完(帧号回到 0xFF)再补, 最迟一圈 18ms 就上屏 */
void WeatherClock_RowRefresh(void)
{
    s_row_dirty = 1;
}

/* WeatherClock_PollRow —— 主循环每圈调一次: 动画往前挪一帧, 或者到点起一次新动画
 * 用法：while (1) { ... epoch = RTC_GetEpoch(); WeatherClock_PollRow(epoch); }
 * 参数：epoch 当前秒数(RTC_GetEpoch), 只用来算"这一页停了多久"
 * 返回：无
 * 说明：不再用 delay 卡帧 —— 一帧的耗时就是主循环一圈, 17 圈走完整段;
 *       换页那 0.3 秒里时钟/室内/DHT 每圈照常跑, 主循环不会停摆;
 *       代价是帧间隔被主循环节拍定死(一圈约 18ms, 整段约 0.3 秒),
 *       想改快慢调 main.c 的 delay_ms, 想改停多久调 UI_ROW_HOLD_SEC;
 *       停多久用秒数算而不是数圈, 是因为一圈 = delay + 任务耗时并不固定,
 *       以后主循环再加活儿, 数圈会让"停 4 秒"悄悄漂掉;
 *       换页只有一帧在画, 每帧仍然写满 128x16, 接缝和阻塞版完全一样 */
void WeatherClock_PollRow(u32 epoch)
{
    u8 next;

    if (s_row_frame <= UI_ROW_FRAMES)          /* 还在动画里: 这一圈只画第 frame 帧 */
    {
        next = (u8)(s_row_page + 1);
        if (next >= SD_ROW_PAGES)
            next = 0;

        WeatherClock_RowScroll(sd_row[s_row_page], sd_row[next], s_row_frame);
        s_row_frame++;

        if (s_row_frame > UI_ROW_FRAMES)       /* 最后一帧画完, 正式翻页 */
        {
            s_row_page  = next;
            s_row_frame = 0xFF;
            s_row_sec   = epoch;               /* 从这一秒重新计 4 秒 */
        }
        return;
    }

    if (s_row_dirty)                           /* 表内容更新过, 补画当前这一页 */
    {
        s_row_dirty = 0;
        UI_RowDrawOne(sd_row[s_row_page], 0);
    }

    if (s_row_sec == 0xFFFFFFFF)               /* 开机或刚整屏重绘过, 先记基准 */
    {
        s_row_sec = epoch;
        return;
    }

    if (epoch < s_row_sec)                     /* 校时把表按回去了, 从现在重新计 */
    {
        s_row_sec = epoch;
        return;
    }

    if ((epoch - s_row_sec) < UI_ROW_HOLD_SEC)
        return;   /* 这一页还没停够 */

    s_row_frame = 0;                           /* 到点: 下一圈从第 0 帧开画 */
}
