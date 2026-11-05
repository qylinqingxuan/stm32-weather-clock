#include "cw_data.h"
#include <string.h>

/* ==================== 全局数据 ====================
 * 整个工程只有这一份: 解析函数往里写, 显示层往外读;
 * 某项解析失败时不动它, 保留上一次的旧值, 屏幕上不会突然跳成 0 */
CW_Data cw;

/* CW_Reset —— 把所有天气数据和有效标志清零
 * 用法：CW_Reset();
 * 参数：无
 * 返回：无
 * 说明：结构体里没有指针, 全 0 就是合法状态;
 *       valid 为 0 表示"这项还没取到", 显示层据此决定画真值还是画 --;
 *       全局变量本来就落在 ZI 段由启动代码清零, 这个函数是给"重取数据前先清空"用的 */
void CW_Reset (void)
{
    memset (&cw, 0, sizeof (cw));
}

/* cw_skip_space —— 跳过 JSON 里的空白字符
 * 用法：p = cw_skip_space (p);
 * 参数：p 当前位置
 * 返回：往后第一个非空白字符的地址
 * 说明：冒号后面常带一个空格 (如 "code": 0), 不跳过去就会把空格当成值的开头;
 *       只处理空格和制表符, 网络响应里本来也不会出现换行缩进 */
static const char *cw_skip_space (const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* cw_find_key —— 在 JSON 里定位键名, 返回它对应值的第一个字符的地址
 * 用法：p = cw_find_key (body, "server_time");
 * 参数：json 要搜索的字符串, 整个响应带不带 HTTP 头都行
 *       key  键名, 不带引号, 最长 24 字符
 * 返回：值的起始地址; 找不到返回 0
 * 说明：先只找 "键名" 这个锚点, 再回头确认它后面紧跟一个冒号 —— 这一步不能省:
 *       找 "code" 会先命中 "code_day" 的前半截, 靠冒号校验挡掉;
 *       "text":"code" 这种键名出现在值里的情况同样被挡掉;
 *       冒号两边允许空格, 因为不同服务器格式化 JSON 的习惯不一样 */
static const char *cw_find_key (const char *json, const char *key)
{
    char pat[32];
    const char *hit;
    const char *q;

    if (json == 0 || key == 0) return 0;
    if (strlen (key) > 24) return 0;

    pat[0] = '"';
    strcpy (&pat[1], key);
    strcat (pat, "\"");
    hit = strstr (json, pat);
    if (hit == 0) return 0;  // 整份响应里根本没有这个键

    q = cw_skip_space (hit + strlen (pat));  // 锚点后面先跳过空白
    if (*q != ':') return 0;  // 不是冒号, 说明命中的是更长的键(如 code_day)
    return cw_skip_space (q + 1);  // 跨过冒号再跳一次空白, 才是值的开头
}

/* cw_read_str —— 把当前位置引号里的字符串拷进目标缓冲
 * 用法：p = cw_find_key (body, "text"); cw_read_str (&p, cw.now.text, 16);
 * 参数：pp 字符串指针的地址, 读完自动前进到右引号后面
 *       dst 目标缓冲; cap 缓冲总字节数(含结尾的 '\0')
 * 返回：无
 * 说明：超长的部分直接丢掉, 保证不写越界 —— 天气描述最长也就 20 来个字符;
 *       当前位置不是引号时把 dst 置成空串、指针原地不动, 让调用方自己去判断;
 *       不做转义处理(\" 之类), 心知返回的描述里不会出现 */
static void cw_read_str (const char **pp, char *dst, u8 cap)
{
    const char *p = *pp;
    u8 n = 0;

    if (dst == 0 || cap == 0) return;
    dst[0] = '\0';  // 先给个空串, 下面任何一条路提前返回都不会留野值
    if (*p != '"') return;  // 不是字符串, 交回调用方处理

    p++;  // 跳过左引号
    while (*p != '\0' && *p != '"')
    {
        if (n < (u8)(cap - 1)) dst[n++] = *p;  // 满了就只前进不写, 防止越界
        p++;
    }
    dst[n] = '\0';
    if (*p == '"') p++;  // 跳过右引号
    *pp = p;
}

/* cw_read_int —— 从当前位置读一个十进制整数, 允许带负号和引号
 * 用法：p = cw_find_key (body, "temperature"); v = cw_read_int (&p);
 * 参数：pp 字符串指针的地址, 读完自动前进到数字后面
 * 返回：读到的值; 一个数字都没有时返回 0
 * 说明：心知返回的数值全是带引号的字符串 ("temperature":"18"), 所以要先吃掉左引号,
 *       读完再吃掉右引号 —— 不吃的话下一次搜索会卡在引号上;
 *       负号单独处理, 冬天的高低温会是 "-3" 这种;
 *       逐位乘 10 累加, 不用 atoi: microlib 下 atoi 会拉库, 而且它认不了引号 */
static s32 cw_read_int (const char **pp)
{
    const char *p = *pp;
    s32 sign = 1;
    s32 v = 0;

    if (*p == '"') p++;  // 字符串包裹的左引号
    if (*p == '-') { sign = -1; p++; }  // 负号在引号里面
    while (*p >= '0' && *p <= '9')
    {
        v = v * 10 + (s32)(*p - '0');
        p++;
    }
    if (*p == '"') p++;  // 右引号
    *pp = p;
    return v * sign;
}

/* cw_read_pct —— 把 0~1 的小数读成 0~100 的百分比整数
 * 用法：p = cw_find_key (body, "precip"); v = cw_read_pct (&p);
 * 参数：pp 字符串指针的地址, 函数自己往前推
 * 返回：0~100; 一个数字都没有时返回 0
 * 说明：接口给的是 "0.55" 这种两位小数, cw_read_int 只吃整数部分会丢精度;
 *       这里整数位乘 100, 再补两位小数, 不做四舍五入 —— 屏幕上 1% 的差别看不出来;
 *       万一将来给成 100 这种整数也接得住, 最后统一夹到 100 以内
 */
static u8 cw_read_pct (const char **pp)
{
    const char *p = *pp;
    u16 v = 0;

    if (*p == '"') p++;  // 值也是带引号的字符串

    while (*p >= '0' && *p <= '9')  // 整数部分, 正常是 0 或 1
    {
        v = (u16)(v * 10 + (u16)(*p - '0'));
        p++;
        if (v >= 100) break;  // 防止 "100.00" 这种把 u16 撑爆
    }
    v = (u16)(v * 100);  // 整数位换算成百分位

    if (*p == '.')  // 小数部分只取两位, 再多也没意义
    {
        p++;
        if (*p >= '0' && *p <= '9') { v = (u16)(v + (u16)(*p - '0') * 10); p++; }
        if (*p >= '0' && *p <= '9') { v = (u16)(v + (u16)(*p - '0'));      p++; }
    }

    while (*p != '\0' && *p != '"' && *p != ',' && *p != '}') p++;  // 丢掉尾巴, 停在分隔符上
    if (*p == '"') p++;
    *pp = p;

    return (v > 100) ? 100 : (u8)v;  // 超过 100 一律当 100
}

/* cw_read_tenths —— 把 "8.4" 这种一位小数读成放大 10 倍的整数
 * 用法：p = cw_find_key (body, "wind_speed"); v = cw_read_tenths (&p);
 * 参数：pp 字符串指针的地址, 函数自己往前推
 * 返回：8.4 -> 84, 32.8 -> 328; 一个数字都没有时返回 0
 * 说明：不能复用 cw_read_pct —— 那个是"小数转百分比", 整数位乘的是 100,
 *       而且结果夹在 100 以内, 风速 8.4km/h 进去会被夹成 100;
 *       这里整数位只乘 10, 也不夹上限(阵风能到几十 km/h);
 *       小数位只取一位, 第二位直接丢 —— 0.04km/h 对舒适度的影响是 0.001 分
 */
static u16 cw_read_tenths (const char **pp)
{
    const char *p = *pp;
    u16 v = 0;

    if (*p == '"') p++;  // 值也是带引号的字符串

    while (*p >= '0' && *p <= '9')  // 整数部分
    {
        v = (u16)(v * 10 + (u16)(*p - '0'));
        p++;
        if (v >= 1000) break;  // 三位数已经 100km/h, 再大不是风了
    }
    v = (u16)(v * 10);  // 腾出个位给小数

    if (*p == '.')  // 小数部分只取一位
    {
        p++;
        if (*p >= '0' && *p <= '9') { v = (u16)(v + (u16)(*p - '0')); p++; }
        while (*p >= '0' && *p <= '9') p++;  // 后面还剩的位直接跳过
    }

    while (*p != '\0' && *p != '"' && *p != ',' && *p != '}') p++;  // 丢掉尾巴
    if (*p == '"') p++;
    *pp = p;

    return v;
}

/* CW_GetTimeStamp —— 从时间接口的响应里取出 Unix 秒
 * 用法：ts = CW_GetTimeStamp ((const char *)USART2_RX_BUF);
 *       if (ts != 0) RTC_SetEpoch (ts);
 * 参数：json 整个响应, 带不带 HTTP 头都行
 * 返回：成功返回秒数; 0 表示没找到或数据不可信 —— 0 是 1970 年, 永远不可能出现
 * 说明：接口给的是 13 位毫秒(1792994999527), 最大 1.8e12, 远超 u32 上限 42.9 亿;
 *       所以不能"先累加成毫秒再除以 1000" —— 累加那一步就已经溢出了;
 *       做法是先数数字的位数, 再只累加前面 10 位(等价于丢掉末 3 位), 全程不超 u32;
 *       10 位(本身已是秒)原样保留, 接口哪天改格式也能兜住 */
u32 CW_GetTimeStamp (const char *json)
{
    const char *p;
    u32 sec = 0;
    u8 len = 0;
    u8 keep;
    u8 i;

    p = cw_find_key (json, "server_time");
    if (p == 0) return 0;  // 不是时间接口的正常回复
    if (*p == '"') p++;  // 值被包成字符串时的兼容写法

    while (len < 20 && p[len] >= '0' && p[len] <= '9') len++;  // 这一趟只数位数, 不碰数值
    if (len < 10) return 0;  // 不足 10 位, 不可能是 2001 年之后的时间

    keep = (u8)((len >= 13) ? (len - 3) : len);  // 13 位毫秒丢末 3 位; 10 位秒原样留
    if (keep > 10) keep = 10;  // 10 位已经到 2286 年, 再多也没意义, 顺便防溢出
    for (i = 0; i < keep; i++) sec = sec * 10 + (u32)(p[i] - '0');

    return sec;
}

/* CW_ParseNow —— 从实况天气的响应里取出描述、天气代码、温度
 * 用法：if (Get_Weather (body, CW_BODY_MAX) == 0 && CW_ParseNow (body) == 0)
 *           printf ("%s %u %d", cw.now.text, cw.now.code, cw.now.temp);
 * 参数：json 整个响应, 带不带 HTTP 头都行
 * 返回：0 = 成功; 1 = 没有 now 对象; 2 = 缺 text; 3 = 缺 code; 4 = 缺 temperature
 * 说明：响应是两层嵌套 {"results":[{"location":{...},"now":{...}}]},
 *       先定位 "now" 再在它后面找字段, 这样 "location" 里的同名字段不会串进来;
 *       三项全部拿到才整体写进 cw.now —— 用局部结构体攒着, 中途失败就原样丢弃,
 *       免得屏幕上一半新数据一半旧数据;
 *       失败不动 cw.now, 保留上一次的值, 一次网络抖动不会把天气清成空白 */
u8 CW_ParseNow (const char *json)
{
    const char *p;
    const char *q;
    CW_Now tmp;
    char upd[26];  // last_update 完整 25 字符 + 结尾

    p = cw_find_key (json, "now");
    if (p == 0) return 1;  // 没有 now, 多半是错误体(无效 key / 超限)

    memset (&tmp, 0, sizeof (tmp));  // 局部攒着, 三项都成立才写进全局

    q = cw_find_key (p, "text");  // 天气描述, 英文字符串如 "Sunny"
    if (q == 0) return 2;
    cw_read_str (&q, tmp.text, (u8)sizeof (tmp.text));

    q = cw_find_key (p, "code");  // 天气代码, 带引号的数字如 "0"
    if (q == 0) return 3;
    tmp.code = (u8)cw_read_int (&q);

    q = cw_find_key (p, "temperature");  // 温度, 带引号, 可能带负号
    if (q == 0) return 4;
    tmp.temp = (s8)cw_read_int (&q);

    upd[0] = '\0';  // 判重用的更新时间, 拿不到就留空, 不影响三项主体
    q = cw_find_key (p, "last_update");
    if (q != 0) cw_read_str (&q, upd, (u8)sizeof (upd));

    tmp.valid = 1;
    cw.now = tmp;  // 结构体整体赋值, 不会出现"一半新一半旧"
    strcpy (cw.now_update, upd);
    return 0;
}

/* CW_ParseDaily —— 从逐日预报的响应里取最近 CW_DAILY_NUM 天
 * 用法：if (Get_Daily (body, CW_BODY_MAX) == 0 && CW_ParseDaily (body) == 0)
 *           Gui_DrawWeatherIcon (x, y, cw.daily[0].code_day);
 * 参数：json 完整响应, 允许带 HTTP 头
 * 返回：0 = 成功(至少解析到 1 天); 1 = 没有 daily 数组;
 *       2 = 某天的 date/code_day/high/low 缺字段
 * 说明：daily 是数组, 每个元素以 "date" 开头, 所以拿 "date" 当每轮的入口;
 *       code_day 直接就是 Gui_DrawWeatherIcon 要的天气码, 不用再换算;
 *       逐天先写进局部 tmp, 四项齐全才落到 cw.daily[i] ——
 *       中途失败不会留下半天的脏数据;
 *       只取 4 项: date / code_day / high / low, 降水概率有就拿, 没有算 0
 */
u8 CW_ParseDaily (const char *json)
{
    const char *p;
    const char *q;
    const char *arr;  // daily 数组起点, 循环里 p 一直往前跑, 最后靠它找 last_update
    CW_Daily tmp;
    u8 i;

    p = cw_find_key (json, "daily");
    if (p == 0) return 1;  // 没有 daily 数组(错误体或 key 无效)
    arr = p;

    for (i = 0; i < CW_DAILY_NUM; i++)
    {
        p = cw_find_key (p, "date");  // 每一天的对象都从 date 开始
        if (p == 0) break;            // 天数用完了, 正常结束

        memset (&tmp, 0, sizeof (tmp));  // 局部变量, 保证四项都写全再落地
        cw_read_str (&p, tmp.date, (u8)sizeof (tmp.date));

        q = cw_find_key (p, "code_day");  // 白天天气码, 直接喂图标函数
        if (q == 0) return 2;
        tmp.code_day = (u8)cw_read_int (&q);

        q = cw_find_key (p, "code_night");  // 夜间码, 先存着, 夜间图标要用
        if (q != 0) tmp.code_night = (u8)cw_read_int (&q);

        q = cw_find_key (p, "high");  // 最高温, 可能带负号
        if (q == 0) return 2;
        tmp.high = (s8)cw_read_int (&q);

        q = cw_find_key (p, "low");  // 最低温
        if (q == 0) return 2;
        tmp.low = (s8)cw_read_int (&q);

        q = cw_find_key (p, "precip");  // 降水概率 0.00~1.00, 有就取, 没有留 0
        if (q != 0) tmp.precip = cw_read_pct (&q);

        q = cw_find_key (p, "humidity");  // 当天平均湿度, 整数 0~100, 算舒适度要用
        if (q != 0) tmp.humidity = (u8)cw_read_int (&q);

        q = cw_find_key (p, "wind_speed");  // 当天风速, 单位 km/h 带一位小数
        if (q != 0) tmp.wind_speed = cw_read_tenths (&q);

        q = cw_find_key (p, "wind_scale");  // 风力等级, 整数, 屏幕要显示"2级"直接用
        if (q != 0) tmp.wind_scale = (u8)cw_read_int (&q);

        q = cw_find_key (p, "wind_direction_degree");  // 风向角度; 不取 "W" 那个字符串,
        if (q != 0) tmp.wind_deg = (u16)cw_read_int (&q);  // 数字能查表, 字母得另建 16 项映射

        q = cw_find_key (p, "rainfall");  // 降雨量 mm, 带两位小数, 只留到 0.1mm
        if (q != 0) tmp.rainfall = cw_read_tenths (&q);

        tmp.valid = 1;
        cw.daily[i] = tmp;  // 四项齐全才算这一天有效
    }

    if (i > 0)  // 响应级更新时间, 整份只有一条, 取到就存
    {
        q = cw_find_key (arr, "last_update");
        cw.daily_update[0] = '\0';
        if (q != 0) cw_read_str (&q, cw.daily_update, (u8)sizeof (cw.daily_update));
    }

    return (i > 0) ? 0 : 2;  // 一天都没解析出来算失败
}

/* ==================== 生活指数: 英文 -> 中文 ====================
 * 心知只公布了中文值域(21 个), 英文值域一个字都没有, 所以这张表全是实测对拍来的:
 * 同一时刻对同一城市分别请求 language=en 和 language=zh-Hans, 逐项比对;
 * 补新值的做法就是照现有行的格式加一行, 中文照抄 zh-Hans 那份的返回值。
 * 千万不要按字面直译: Rarely/Likely/Probably/Certainly 的轻重顺序和字面是反的,
 * Comfortable 那组里最好的一档反而叫 Very Comfortable */
typedef struct
{
    u8          item;  // 属于哪一项, 取 CW_LIFE_FLU 这类
    const char *en;    // 接口返回的英文原词
    const char *zh;    // 对应的中文
} CW_LifeMap;

static const CW_LifeMap cw_life_map[] =
{
    { CW_LIFE_FLU,     "Rarely",           "少发"   },  // 最低一档
    { CW_LIFE_FLU,     "Likely",           "较易发" },  // 注意: 字面比 Probably 轻, 实际更轻一档
    { CW_LIFE_FLU,     "Probably",         "易发"   },  // 字面轻、实际比 Likely 重
    { CW_LIFE_FLU,     "Certainly",        "极易发" },  // 最高一档

    { CW_LIFE_UV,      "Very Low",         "最弱"   },
    { CW_LIFE_UV,      "Low",              "弱"     },
    { CW_LIFE_UV,      "Moderate",         "中等"   },
    { CW_LIFE_UV,      "High",             "强"     },
    { CW_LIFE_UV,      "Very High",        "很强"   },

    { CW_LIFE_AIR,     "Excellent",        "优"     },  // 说的是扩散条件, 不是空气质量的优
    { CW_LIFE_AIR,     "Good",             "良"     },
    { CW_LIFE_AIR,     "Moderate",         "中"     },  // 别照着英文写成"中度污染"
    { CW_LIFE_AIR,     "Unhealthy",        "较差"   },
    { CW_LIFE_AIR,     "Hazardous",        "很差"   },

    { CW_LIFE_COMFORT, "Very Comfortable", "舒适"   },  // 最好的一档, 反而带 Very
    { CW_LIFE_COMFORT, "Comfortable",      "较舒适" },
    { CW_LIFE_COMFORT, "Uncomfortable",    "较不舒适" }  // 比它更差的"不舒适"是另一档, 不是这个

    /* 舒适度还差 4 档没测到: 不舒适 / 很不舒适 / 极不舒适 / 非常不舒适。
     * 中文名官方有, 英文怎么写的没见过, 所以这里空着 —— 实测碰到时照上面格式补一行即可,
     * 不用改任何逻辑; 补上之前, 这些词会原样以英文显示出来并打 [?] 标记, 不会静默出错 */
};

#define CW_LIFE_MAP_NUM  ((u8)(sizeof (cw_life_map) / sizeof (cw_life_map[0])))

/* 四项在响应里的键名, 下标和 CW_LIFE_xxx 一致 */
static const char *const cw_life_key[CW_LIFE_NUM] =
{
    "flu", "uv", "air_pollution", "comfort"
};

/* cw_life_lookup —— 拿英文原词去表里换中文
 * 用法：zh = cw_life_lookup (CW_LIFE_FLU, "Rarely");
 * 参数：item 哪一项; en 接口给的英文原词
 * 返回：匹配到的中文; 0 表示表里没有这个词
 * 说明：返回 0 不是错误, 是"又发现了一个没见过的值" —— 调用方要原样保留英文,
 *       绝对不能自己挑一个中文顶上, 否则错的值会一直错下去还看不出来;
 *       表只有 21 项, 顺序查找足够, 不值得为它建哈希 */
static const char *cw_life_lookup (u8 item, const char *en)
{
    u8 i;

    for (i = 0; i < CW_LIFE_MAP_NUM; i++)
    {
        if (cw_life_map[i].item != item) continue;  // 先按项筛, 少做几次 strcmp
        if (strcmp (cw_life_map[i].en, en) == 0) return cw_life_map[i].zh;
    }
    return 0;  // 没见过这个词
}

/* CW_ParseLife —— 从生活指数的响应里取出四项的 brief 并翻成中文
 * 用法：if (Get_Life (body, CW_BODY_MAX) == 0 && CW_ParseLife (body) == 0) ...
 * 参数：json 完整响应, 允许带 HTTP 头
 * 返回：0 = 至少拿到 1 项; 1 = 没有 suggestion 对象; 2 = 四项一个都没拿到
 * 说明：只认 flu / uv / air_pollution / comfort 四项, 其余 19 项一概不管 ——
 *       屏幕最后一行只要这四样, 多解析就是白占 RAM 和 flash;
 *       先定位 suggestion 再在它里面找键, 免得撞上 location 或 details 里的同名词;
 *       每项都用局部变量接, 拿到有效值才写进 cw.life —— 某一项缺了只跳过它,
 *       其余照写, 不会因为一项缺失就把整组扔掉;
 *       空字符串一律当没拿到(渭南实测出现过 200 但四字段全空, 是当天的"暂缺") */
u8 CW_ParseLife (const char *json)
{
    const char *p;
    const char *q;
    const char *r;
    const char *zh;
    char en[20];
    u8   item;
    u8   got;

    p = cw_find_key (json, "suggestion");
    if (p == 0) return 1;  // 错误体或接口没权限

    got = 0;
    for (item = 0; item < CW_LIFE_NUM; item++)
    {
        q = cw_find_key (p, cw_life_key[item]);  // 这一项的对象起点
        if (q == 0) continue;                    // 没返回这一项, 保留旧值

        r = cw_find_key (q, "brief");  // 只要 brief, details 是长句, 屏幕用不上
        if (r == 0) continue;

        cw_read_str (&r, en, (u8)sizeof (en));
        if (en[0] == '\0') continue;  // 空串 / 暂缺, 当没拿到

        strcpy (cw.life[item].en, en);  // 原词先存下来, 串口和以后的补表都要用
        zh = cw_life_lookup (item, en);
        if (zh != 0)
        {
            strcpy (cw.life[item].brief, zh);  // 表里有, 用中文
            cw.life[item].mapped = 1;
        }
        else
        {
            strcpy (cw.life[item].brief, en);  // 表里没有, 原样显示英文, 等人工补表
            cw.life[item].mapped = 0;
        }

        cw.life[item].idx   = item;
        cw.life[item].valid = 1;
        got++;
    }

    if (got > 0)
    {
        r = cw_find_key (p, "last_update");
        cw.life_update[0] = '\0';
        if (r != 0) cw_read_str (&r, cw.life_update, (u8)sizeof (cw.life_update));
    }

    return (got > 0) ? 0 : 2;  // 一项都没拿到才算失败
}

/* CW_UpdateChanged —— 判断这批数据比上次是不是新的
 * 用法：if (CW_UpdateChanged (s_last, cw.daily_update)) 才刷屏;
 * 参数：last 上次存下来的 last_update 原文; cur 这次刚解析出来的
 * 返回：1 = 有变化, 该刷新; 0 = 没变化, 别刷
 * 说明：心知的 last_update 是 "2026-10-29T08:00:00+08:00" 这种完整时间串,
 *       判重只需要"跟上次一不一样", 不需要知道差多少秒, 所以直接存原文比对,
 *       省掉时间串转 Unix 秒那套时区换算 —— 那个只在开机校时时做一次就够了;
 *       这次没取到(cur 为空)一律当"没变化", 不能因为一次网络失败就把屏幕全刷一遍;
 *       上次是空(第一次跑)则当"有变化", 不然第一次永远不刷新 */
u8 CW_UpdateChanged (const char *last, const char *cur)
{
    if (cur == 0 || cur[0] == '\0') return 0;   // 这次没拿到, 不算变化
    if (last == 0 || last[0] == '\0') return 1; // 头一次拿到, 算变化
    return (strcmp (last, cur) != 0) ? 1 : 0;
}
