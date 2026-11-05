/* Boot_Screen.c —— 开屏初始化进度屏
 * 用法：Lcd_Init() 之后 Boot_Screen_Init(), 之后每个事件点调 Boot_Screen_Step(),
 *       成功调 Boot_Screen_Done(), 失败调 Boot_Screen_Error(code) 后停死等复位
 * 参数：见各函数
 * 返回：无
 * 说明：全部走 GUI 层(Gui_FillRegion/Gui_PushPixel/Gui_Glyph16), 不直接碰 ST7735,
 *       屏偏移留在 GUI 层; 文案全是 ASCII, 1 字符 8px, 不占汉字字库
 * 版面(128x160)：y=12 标题 / y=34 细分隔线 /
 *                 y=58 主行步骤名(青) / y=80 副行状态(灰) /
 *                 y=112 进度条框 102x10, 内部 100x8 填充 */
#include "stm32f10x.h"
#include "GUI.h"
#include "UI_Config.h"
#include "Boot_Screen.h"

#define SCR_W       128
#define SCR_H       160

#define BOOT_WHITE  0xFFFF   /* 标题: 纯白 */
#define BOOT_MAIN   0x5FFB   /* 主行: 亮青, 跟主界面湿度标签同色 */
#define BOOT_SUB    0xEF7D   /* 副行: 浅灰, 压住不抢主行 */
#define BOOT_ACC    0xFD20   /* 进度条: 暖橙, 跟主界面室内/最高温同族 */
#define BOOT_OK     0x07E0   /* 完成: 绿 */
#define BOOT_ERR    0xF800   /* 失败: 红 */
#define BOOT_HINT   0xFDE7   /* 底部提示: 浅黄 */

#define TITLE_Y     12       /* 标题文字顶边 */
#define RULE_Y      34       /* 标题下细分隔线的 y */
#define MAIN_Y      58       /* 主行文字顶边 */
#define SUB_Y       80       /* 副行文字顶边 */
#define BAR_X       13       /* 进度条外框左上角 */
#define BAR_Y       112
#define BAR_W       102      /* 外框宽(含左右两条边) */
#define BAR_H       10       /* 外框高(含上下两条边) */
#define BAR_IN_X    14       /* 内部填充区, 左右各让 1px 给边框 */
#define BAR_IN_Y    113
#define BAR_IN_W    100
#define BAR_IN_H    8

/* 一步一条记录: 主行说干什么, 副行说此刻的状态 */
typedef struct
{
    const char *name;
    const char *sub;
} Boot_Step;

/* 顺序就是 main 里真实执行的顺序, 别随手调换, 否则屏幕在撒谎 */
static const Boot_Step boot_steps[BOOT_STEP_NUM] =
{
    { "RTC sync",        "starting..."   },   /* 0 等 LSE 起振 + 配 RTC */
    { "ESP8266 init",    "connecting..." },   /* 1 AT 探测, 最多重试 5 次 */
    { "Module reset",    "please wait"   },   /* 2 AT+RST 后等固件打印 ready */
    { "Set WiFi mode",   "configuring"   },   /* 3 AT+CWMODE=1 切 Station */
    { "Connecting WiFi", "connecting..." },   /* 4 AT+CWJAP 最慢, 正常 5~15s */
    { "Setup link",      "connecting..." },   /* 5 AT+CIPMUX=0 只留单连接 */
    { "Sync time",       "fetching..."   },   /* 6 Get_Time 取网络时间 */
    { "Fetch weather",   "fetching..."   },   /* 7 Get_Weather 取实况 */
    { "DHT11 warm up",   "waiting..."    },   /* 8 上电要静置 1s 才能读 */
    { "Load UI",         "loading..."    }    /* 9 画主界面 */
};

/* 错误码 1~5 对应 ESP8266_Init 的返回值, 下标 0 不用 */
static const char *boot_err1[6] =
{
    "BOOT FAILED",
    "NO MODULE",     /* 连 AT 都没回 */
    "RESET FAIL",     /* 复位后也不理人 */
    "WIFI MODE",     /* CWMODE 失败 */
    "WIFI FAILED",    /* 账号密码不对 / 路由器不在 */
    "LINK SETUP"      /* CIPMUX 失败 */
};

static const char *boot_err2[6] =
{
    "CHECK WIFI",
    "CHECK ESP-01S",
    "CHECK ESP-01S",
    "CHECK WIFI",
    "CHECK SSID/PSWD",
    "CHECK ESP-01S"
};

/* boot_strlen —— 数纯英文串有几个字符
 * 用法：n = boot_strlen("Load UI");
 * 参数：s 以 '\0' 结尾的字符串
 * 返回：字符个数
 * 说明：开屏文案全是 ASCII, 1 字符 1 字节; 不为这事把 string.h 拖进来 */
static u8 boot_strlen(const char *s)
{
    u8 n = 0;

    while (s[n] != 0) n++;
    return n;
}

/* boot_fill —— 用纯色铺满一块矩形
 * 用法：boot_fill(0, 0, 128, 160, UI_BG);
 * 参数：x,y 左上角; w,h 真实像素宽高; c RGB565 色
 * 返回：无
 * 说明：Gui_FillRegion 只负责开窗, 像素得自己一个个送;
 *       宽 < 3 或高 < 2 时开窗会算出非法坐标(见 Lcd_SetRegion 的 +2/+1), 直接返回 */
static void boot_fill(u16 x, u16 y, u16 w, u16 h, u16 c)
{
    u32 n;

    if (w < 3 || h < 2) return;

    Gui_FillRegion(x, y, w, h);
    n = (u32)w * (u32)h;
    while (n--) Gui_PushPixel(c);
}

/* boot_line —— 先把整行抹成底色, 再居中画一行 16 号英文
 * 用法：boot_line(MAIN_Y, "Connecting WiFi", BOOT_MAIN);
 * 参数：y 文字顶边; s 文案; fc 字色
 * 返回：无
 * 说明：清 y..y+17 共 18 行, 所以新文案比旧的短也不会留残影;
 *       一个字 8px, x = (128 - 8*len)/2; 超过 16 字符会顶到边, 步骤表自己控长度 */
static void boot_line(u16 y, const char *s, u16 fc)
{
    u8  len;
    u16 x;

    len = boot_strlen(s);
    if (len == 0) return;

    boot_fill(0, y, SCR_W, 18, UI_BG);
    x = (len * 8 < SCR_W) ? (u16)((SCR_W - len * 8) / 2) : 0;
    Gui_DrawFont_GBK16(x, y, fc, UI_BG, (u8 *)s);
}

/* boot_rule —— 标题下面那条细分隔线
 * 用法：boot_rule();
 * 参数：无 / 返回：无
 * 说明：x 从 14 到 113, 比整屏线短一截, 当标题的落款线用,
 *       颜色跟主界面四条分割线一致(UI_SEP) */
static void boot_rule(void)
{
    Gui_DrawLine(14, RULE_Y, 113, RULE_Y, UI_SEP);
}

/* boot_bar_frame —— 画进度条空框(四条 1px 边)
 * 用法：boot_bar_frame();
 * 参数：无 / 返回：无
 * 说明：外框 102x10, 像素范围 x=13..114 / y=112..121; 只画框不填色 */
static void boot_bar_frame(void)
{
    Gui_DrawLine(BAR_X, BAR_Y, BAR_X + BAR_W - 1, BAR_Y, UI_SEP);
    Gui_DrawLine(BAR_X, BAR_Y + BAR_H - 1, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1, UI_SEP);
    Gui_DrawLine(BAR_X, BAR_Y, BAR_X, BAR_Y + BAR_H - 1, UI_SEP);
    Gui_DrawLine(BAR_X + BAR_W - 1, BAR_Y, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1, UI_SEP);
}

/* boot_bar_fill —— 按百分比把进度条内部填满
 * 用法：boot_bar_fill(50, BOOT_ACC);
 * 参数：pct 0~100; c 填充色
 * 返回：无
 * 说明：每次都整段重填(最宽 100x8 = 800 像素, 约 2ms), 不做增量计算,
 *       省得记旧宽度; 宽度不足 3px 时开窗非法, 直接跳过 */
static void boot_bar_fill(u8 pct, u16 c)
{
    u16 w;

    if (pct > 100) pct = 100;
    w = (u16)((u32)BAR_IN_W * (u32)pct / 100);
    if (w < 3) return;

    boot_fill(BAR_IN_X, BAR_IN_Y, w, BAR_IN_H, c);
}

/* boot_text2x —— 把英文放大 2 倍(每点变 2x2 块)画出来
 * 用法：boot_text2x(24, 34, "ERROR", BOOT_ERR);
 * 参数：x,y 左上角; s 文案(最多 8 字符); fc 字色
 * 返回：无
 * 说明：一个字符 16x32, 8 字符正好 128 宽顶满屏;
 *       整块开一个 len*16 x 32 的窗, 窗口里是行优先连续写的,
 *       所以循环必须 行 -> 字符 -> 列, 不能字符在外层 */
static void boot_text2x(u16 x, u16 y, const char *s, u16 fc)
{
    const u8 *g;
    u8  cw, stride;
    u8  len, i;
    u16 row, c, col;
    u8  bits;

    len = boot_strlen(s);
    if (len == 0 || len > 8) return;

    Gui_FillRegion(x, y, (u16)(len * 16), 32);
    for (row = 0; row < 32; row++)
    {
        for (i = 0; i < len; i++)
        {
            g = Gui_Glyph16((const unsigned char *)s + i, &cw, &stride);
            for (c = 0; c < 16; c++)
            {
                col  = c >> 1;                 /* 横向放大 2 倍: 两列共用一个字模位 */
                bits = 0;
                if (g != 0) bits = g[(row >> 1) * (u16)stride + (col >> 3)];
                Gui_PushPixel((bits & (0x80 >> (col & 7))) ? fc : UI_BG);
            }
        }
    }
    (void)cw;                                  /* 这里只用位图, 宽度用不上 */
}

/* Boot_Screen_Init —— 画开屏第一帧: 标题 + 分割线 + 空进度条
 * 用法：Lcd_Init() 之后立刻调一次
 * 参数：无 / 返回：无
 * 说明：整屏铺 UI_BG 打底; 之后每一步只刷主行/副行/进度条三小块,
 *       不整屏重画, 所以不会闪 */
void Boot_Screen_Init(void)
{
    boot_fill(0, 0, SCR_W, SCR_H, UI_BG);

    boot_line(TITLE_Y, "WEATHER CLOCK", BOOT_WHITE);
    boot_rule();

    boot_line(MAIN_Y, "Booting", BOOT_SUB);
    boot_line(SUB_Y,  "starting...", BOOT_SUB);

    boot_fill(BAR_IN_X, BAR_IN_Y, BAR_IN_W, BAR_IN_H, UI_BG);
    boot_bar_frame();
}

/* Boot_Screen_Step —— 切到第 idx 步: 换两行文案 + 推进度条
 * 用法：Boot_Screen_Step(BOOT_STEP_ESP_JOIN);
 * 参数：idx 步骤编号, 取 BOOT_STEP_xxx
 * 返回：无
 * 说明：进度按 (idx+1)/N 算, 一进这步条子就先到位, 人看着是"这步干完了",
 *       实际还在等 —— 等待期间条子不动最让人焦虑;
 *       可以直接当函数指针塞给 ESP8266_Progress, 签名一致 */
void Boot_Screen_Step(u8 idx)
{
    u8 pct;

    if (idx >= BOOT_STEP_NUM) idx = BOOT_STEP_NUM - 1;   /* 越界夹到最后一格, 防查表飞出 */

    boot_line(MAIN_Y, boot_steps[idx].name, BOOT_MAIN);
    boot_line(SUB_Y,  boot_steps[idx].sub,  BOOT_SUB);

    pct = (u8)(((u16)(idx + 1) * 100) / BOOT_STEP_NUM);
    boot_bar_fill(pct, BOOT_ACC);
}

/* Boot_Screen_Done —— 全部搞定: 绿字 + 满格
 * 用法：Boot_Screen_Done();
 * 参数：无 / 返回：无
 * 说明：画完建议停 0.5~1 秒再切主界面, 太快人看不清 */
void Boot_Screen_Done(void)
{
    boot_line(MAIN_Y, "All done", BOOT_OK);
    boot_line(SUB_Y,  "ready",    BOOT_SUB);
    boot_bar_fill(100, BOOT_OK);
}

/* Boot_Screen_Error —— 起不来: 红 ERROR + 故障名 + 复位提示
 * 用法：if (ret != 0) { Boot_Screen_Error(ret); while (1); }
 * 参数：code 1~5, 与 ESP8266_Init 返回码一一对应
 * 返回：无
 * 说明：屏只能写不能读, 先整屏铺底色把进度条抹掉;
 *       code 越界按 1 处理, 免得查表飞出去 */
void Boot_Screen_Error(u8 code)
{
    if (code < 1 || code > 5) code = 1;

    boot_fill(0, 0, SCR_W, SCR_H, UI_BG);

    boot_text2x(24, 34, "ERROR", BOOT_ERR);   /* 5 字 x 16 = 80px, 居中 x=24 */
    boot_line(84,  boot_err1[code], BOOT_SUB);
    boot_line(102, boot_err2[code], BOOT_SUB);
    boot_line(128, "PRESS RESET",   BOOT_HINT);
}
