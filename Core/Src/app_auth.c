/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_auth.c
  * @brief   门禁鉴权主逻辑（阶段一刷卡 + 阶段二 ZW111 指纹）
  *          - CardTask 轮询 RC522 刷卡
  *          - FPTask   轮询 ZW111 指纹(1:N 比对)
  *          - 任一合法来源 -> 舵机开锁2.5s自动落锁；陌生 -> 拒绝+OLED
  *          - CommTask 接收上位机(Qt)指令：FACE_OK;<id>;<ROLE> 触发开锁，
  *            HOST;PING 做心跳应答，并回传 DOOR;OPEN / DOOR;CLOSED 门锁状态
  ******************************************************************************
  */
/* USER CODE END Header */
#include "app_auth.h"
#include "servo.h"
#include "rc522.h"
#include "oled.h"
#include "zw111.h"
#include "voice.h"
#include "usart.h"
#include "cmsis_os.h"
#include "string.h"

/* ---------------- 卡白名单：替换成你实际刷卡时串口打印出的 UID(4字节) ---------------- */
static const uint8_t s_allowed[][4] = {
    {0xAB, 0x77, 0x77, 0x8E},   /* 示例：管理员卡 */ //AB77778E
    {0x79, 0x26, 0xD4, 0xE6},   /* 示例：用户卡 */  //7926D4E6
};
#define ALLOWED_N  (sizeof(s_allowed) / sizeof(s_allowed[0]))

/* ---------------- 指纹用户表：录入时的 PageID 即用户序号 ----------------
   role: 0=普通用户(单因子)  1=管理员(双因子，后续阶段二用)            */
typedef struct { uint16_t pageId; const char *name; uint8_t role; } fp_user_t;
static const fp_user_t s_fpUsers[] = {
    {0, "ADMIN", 1},
    {1, "USER1", 0},
    {2, "USER2", 0},
};
#define FP_N  (sizeof(s_fpUsers) / sizeof(s_fpUsers[0]))

#define OPEN_HOLD_MS   2500    /* 开门保持时间 */
#define POLL_MS        120     /* 刷卡轮询间隔 */

/* ---------------- OLED 行坐标 ----------------
   注意：江协科技库的 OLED_ShowString(X, Y, ...) 里 **Y 是像素行(0~63)**，
   不是旧驱动那种页号(0~7)。配 OLED_8X16（每字高 16 像素）时，四行文字的
   行坐标就是 0 / 16 / 32 / 48。写成常量，避免再退化成页号。 */
#define OLED_LINE0     0       /* 第 1 行，像素行 0  */
#define OLED_LINE1     16      /* 第 2 行，像素行 16 */
#define OLED_LINE2     32      /* 第 3 行，像素行 32 */
#define OLED_LINE3     48      /* 第 4 行，像素行 48 */

/* ---------------- 串口日志：仅记录真实的开门/拒绝事件 ---------------- */
static void DBG(const char *s)//类似调试debug的效果
{
  HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 200);//调用这个函数时，当前代码停在这里，一直轮询检查发送标志，直到发完 / 超时才往下跑
}

/* 一字节按两位十六进制输出（上电自检 / 录入结果要打确认码） */
static void DBG_Hex2(uint8_t v)
{
  static const char hex[] = "0123456789ABCDEF";
  char s[2] = { hex[v >> 4], hex[v & 0x0F] };
  HAL_UART_Transmit(&huart1, (uint8_t *)s, 2, 50);
}

/* 16 位十进制输出（打印录入的 ID 号） */
static void DBG_U16(uint16_t v)
{
  char b[5];
  uint8_t n = 0;
  if (v == 0) { HAL_UART_Transmit(&huart1, (uint8_t *)"0", 1, 50); return; }
  while (v != 0 && n < sizeof(b)) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
  while (n != 0) { n--; HAL_UART_Transmit(&huart1, (uint8_t *)&b[n], 1, 50); }
}

/* 统一打印 ZW111 握手确认码（上电自检和 PING 都用它） */
static void report_fp_ack(uint8_t ack)
{
  if (ack == ZW111_OK)        DBG("OK\r\n");
  else if (ack == 0xFF)       DBG("NO REPLY\r\n");
  else                      { DBG("CONFIRM 0x"); DBG_Hex2(ack); DBG("\r\n"); }
}

static uint8_t uid_match(const uint8_t *uid)
{
  for (uint8_t i = 0; i < ALLOWED_N; i++)
    if (memcmp(uid, s_allowed[i], 4) == 0) return 1;
  return 0;
}

static const char *fp_lookup(uint16_t id, uint8_t *role)
{
  for (uint8_t i = 0; i < FP_N; i++)
    if (s_fpUsers[i].pageId == id) { if (role) *role = s_fpUsers[i].role; return s_fpUsers[i].name; }
  return NULL;
}

/* ---------------- 待机画面（中文） ----------------
   注意：江协 OLED_ShowChinese 只能显示全角汉字/标点，不能混半角(ASCII/数字)。
   每次切换状态前先清除对应行，避免旧字残留。 */
static void ui_standby(void)
{
  OLED_ClearArea(0, OLED_LINE1, 128, 16);
  OLED_ClearArea(0, OLED_LINE2, 128, 16);
  OLED_ShowChinese(0, OLED_LINE1, "已锁定");
  OLED_ShowChinese(0, OLED_LINE2, "请刷卡或指纹");
  /* 第 3 行“指纹就绪”由 FPTask 维护，这里不重绘 */
  OLED_Update();
}

/* 只更新一行：清行 -> 写中文 -> 刷屏。zh 传 NULL 表示只清行。 */
static void ui_line(uint8_t y, const char *zh)
{
  OLED_ClearArea(0, y, 128, 16);
  if (zh) OLED_ShowChinese(0, y, zh);
  OLED_Update();
}

/* 同时更新 OLED 一行 + 播报对应语音，两个通道说同一句话 */
static void ui_say(uint8_t y, uint16_t track, const char *zh)
{
  ui_line(y, zh);
  VOICE_Play(track);
}

/* ============================================================
   语音播报曲目号
   ＝ JQ8900 里音频文件名的那 5 位数字（00004.mp3 -> 4）。

   ⚠ 必须**连续编号**（00001、00002、00003…），中途不能跳号！
     手册 8.4「指定曲目(07)」按**文件枚举顺序**取曲目（“指定播放当前盘符第 N 首”），
     而手册在「组合播放(1B)」一节里明确对比说：用文件名播「比文件序[号]更准确、
     不[受]顺序限制」——反过来说明 07 指令是**受顺序限制**的。
     所以只有当“文件名里的数字 == 它在盘里的第几个”时，两者才一致；
     一旦跳号（比如 00003 后面直接是 00010），VOICE;10 播到的就是第 10 个文件，
     而不是 00010.mp3 —— 表现就是“发了 x 却对不上号”。

     因此本表按"上表顺序"连续排 1~19，与磁盘上的 00001.mp3~00019.mp3 一一对应。
     以后加内容只能在末尾往后顺延，不能插队、不能跳号。
   ============================================================ */
#define V_TRACK_BOOT          1     /* 00001.mp3 系统启动完成，门禁已就绪 */
#define V_TRACK_STANDBY       2     /* 00002.mp3 请刷卡或指纹          （已定义，暂未接入） */
#define V_TRACK_LOCKED        3     /* 00003.mp3 门已关闭，系统锁定    （已定义，暂未接入） */
#define V_TRACK_CARD_OK       4     /* 00004.mp3 刷卡成功，门已打开 */
#define V_TRACK_FP_OK         5     /* 00005.mp3 指纹验证成功，门已打开 */
#define V_TRACK_FACE_OK       6     /* 00006.mp3 人脸识别成功，门已打开 */
#define V_TRACK_PWD_OK        7     /* 00007.mp3 密码正确，门已打开    （已定义，暂未接入） */
#define V_TRACK_DENY          8     /* 00008.mp3 验证失败，请重试      （已定义，暂未接入） */
#define V_TRACK_UNKNOWN_CARD  9     /* 00009.mp3 未知卡片，拒绝通行 */
#define V_TRACK_FP_NOMATCH   10     /* 00010.mp3 指纹不匹配，请重试 */
#define V_TRACK_FP_UNKNOWN   11     /* 00011.mp3 该指纹不在名册内 */
#define V_TRACK_ENROLL_START 12     /* 00012.mp3 进入指纹录入模式      （已定义，暂未接入） */
#define V_TRACK_PUT_FINGER   13     /* 00013.mp3 请将手指放在传感器上 */
#define V_TRACK_LIFT_FINGER  14     /* 00014.mp3 请抬起手指 */
#define V_TRACK_PUT_AGAIN    15     /* 00015.mp3 请再次放上同一根手指 */
#define V_TRACK_ENROLL_OK    16     /* 00016.mp3 指纹录入成功 */
#define V_TRACK_ENROLL_FAIL  17     /* 00017.mp3 指纹录入失败，请重试 */
#define V_TRACK_INTRUDER     18     /* 00018.mp3 警告，检测到陌生人    （已定义，暂未接入） */
#define V_TRACK_DEV_FAULT    19     /* 00019.mp3 设备异常，请联系管理员（已定义，暂未接入） */

/* 曲目号 -> OLED 中文详情。auth_open()/auth_denied() 共用这一份表，
   避免"屏幕上写的"和"喇叭里念的"不一致。 */
typedef struct { uint16_t track; const char *zh; } track_text_t;
static const track_text_t s_trackText[] = {
    { V_TRACK_CARD_OK,      "刷卡成功" },
    { V_TRACK_FP_OK,        "指纹成功" },
    { V_TRACK_FACE_OK,      "人脸成功" },
    { V_TRACK_PWD_OK,       "验证成功" },
    { V_TRACK_DENY,         "请重试"   },
    { V_TRACK_UNKNOWN_CARD, "未知卡片" },
    { V_TRACK_FP_NOMATCH,   "指纹不符" },
    { V_TRACK_FP_UNKNOWN,   "指纹未知" },
    { V_TRACK_DEV_FAULT,    "设备异常" },
    { V_TRACK_INTRUDER,     "警告"     },
};

static const char *track_zh(uint16_t track)
{
  for (uint8_t i = 0; i < sizeof(s_trackText) / sizeof(s_trackText[0]); i++)
    if (s_trackText[i].track == track) return s_trackText[i].zh;
  return NULL;
}

/* ============================================================
   分步录入指纹（不用模组的 AutoEnroll 黑盒——它只在最后回一个确认码，
   中途无法给提示，手指没按上就直接回 0x26 超时）。
   自己控制节奏：采图 -> 生成特征 -> (抬手 -> 再采图 -> 生成特征 -> 合并)
   ============================================================ */
#define FP_ENROLL_TIMES     3       /* 总按压次数（首次 + 后续合并轮数） */
#define FP_CAP_TIMEOUT_MS   15000   /* 单次等手指放上的超时 */
#define FP_LIFT_TIMEOUT_MS   5000   /* 等手指抬起的超时 */

/* 等手指放上并采到图；返回确认码，0x00 = 采到 */
static uint8_t fp_capture(uint32_t timeout_ms)
{
  uint32_t t = 0;
  while (t < timeout_ms)
  {
    uint8_t r = ZW111_GetImage();
    if (r == ZW111_OK) return ZW111_OK;                    /* 采到指纹图 */
    if (r == ZW111_ERR_NOFINGER || r == 0xFF)              /* 还没放 / 通信抖动 -> 继续等 */
    {
      osDelay(100); t += 100; continue;
    }
    return r;                                             /* 其它错误码直接上报 */
  }
  return ZW111_ERR_TIMEOUT;
}

/* 等手指抬起：传感器上没手指时 GetImage 回 0x02 */
static void fp_wait_lift(uint32_t timeout_ms)
{
  uint32_t t = 0;
  while (t < timeout_ms)
  {
    if (ZW111_GetImage() == ZW111_ERR_NOFINGER) return;
    osDelay(150); t += 150;
  }
}

/* 完整录入一次；返回确认码，0x00 = 入库成功 */
static uint8_t fp_enroll(uint16_t pageId, uint8_t times)
{
  uint8_t r;

  ui_line(OLED_LINE1, "录入指纹中");
  ui_say(OLED_LINE2, V_TRACK_PUT_FINGER, "请放手指");
  ui_line(OLED_LINE3, NULL);

  /* 第 1 次：采图 -> 特征存入缓冲区 1 */
  r = fp_capture(FP_CAP_TIMEOUT_MS);
  if (r != ZW111_OK) return r;
  r = ZW111_GenChar(1);
  if (r != ZW111_OK) return r;

  /* 第 2..times 次：抬手 -> 再采图 -> 特征存缓冲区 2 -> 合并回缓冲区 1 */
  for (uint8_t n = 2; n <= times; n++)
  {
    ui_say(OLED_LINE2, V_TRACK_LIFT_FINGER, "请抬手指");
    fp_wait_lift(FP_LIFT_TIMEOUT_MS);

    ui_say(OLED_LINE2, V_TRACK_PUT_AGAIN, "请再放一次");
    r = fp_capture(FP_CAP_TIMEOUT_MS);
    if (r != ZW111_OK) return r;
    r = ZW111_GenChar(2);
    if (r != ZW111_OK) return r;
    r = ZW111_RegModel();                 /* 0x0A = 两次不是同一根手指 */
    if (r != ZW111_OK) return r;
  }

  ui_line(OLED_LINE2, NULL);
  ui_line(OLED_LINE3, NULL);
  return ZW111_StoreChar(1, pageId);
}

/* ============================================================
   USART1 指令通道（中断接收，不占用任务轮询）
   上位机(Qt)下行指令：
     FACE_OK;<id>;<ROLE>  人脸识别命中，开门
     HOST;PING            心跳请求，回 HOST;PONG
   调试指令（串口助手手敲）：
     ENROLL;<id>   录入 <id> 号指纹（按压 3 次，OLED 逐步提示）
     PING          重测指纹模组握手（不用重启板子）
     SERVO;<角> / VOICE;<曲> / VOL;<量> / VDISC;<盘> / VLOOP;<模式>
   上行回传（给上位机）：
     DOOR;OPEN / DOOR;CLOSED   门锁状态（auth_open() 里发出）
   自检结果与录入结果都回打在同一个串口上。
   ⚠ 与模组对话期间必须独占 USART2：fp_take_bus() / fp_release_bus()
   ============================================================ */
#define CMD_LINE_MAX   24

static volatile char    s_cmdBuf[CMD_LINE_MAX];
static volatile uint8_t s_cmdLen   = 0;
static volatile uint8_t s_cmdReady = 0;
static volatile uint8_t s_fpBusy   = 0;    /* 1 = 正在录入/探测，FPTask 让出 USART2 */
static volatile uint8_t s_fpParked = 0;    /* FPTask 已停在循环顶（确实让出总线的确认） */
static uint8_t          s_rxByte;

/* 独占/释放 USART2：任何要直接和模组对话的地方（录入、PING）都要成对调用。
   s_fpParked 是 FPTask「已停在循环顶」的确认——它可能正卡在一次 GetImage 里
   （模组异常时最长 2s），只靠固定延时会两边同时收发、把协议帧撕碎。 */
static void fp_take_bus(void)
{
  s_fpBusy = 1;
  for (uint8_t w = 0; w < 40 && !s_fpParked; w++) osDelay(50);
}

static void fp_release_bus(void)
{
  s_fpBusy = 0;
}

/* HAL 接收完成回调：逐字节收，遇 CR/LF 视为一行结束。
   中断上下文，禁止任何 RTOS 调用。 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    char c = (char)s_rxByte;
    if (c == '\r' || c == '\n')
    {
      if (s_cmdLen > 0) { s_cmdBuf[s_cmdLen] = '\0'; s_cmdReady = 1; }
    }
    else if (s_cmdLen < CMD_LINE_MAX - 1)
    {
      s_cmdBuf[s_cmdLen++] = c;
    }
    HAL_UART_Receive_IT(&huart1, &s_rxByte, 1);   /* 必须重新挂上，否则只收一个字节 */
  }
}

/* 执行一条指令（跑在 CommTask 上下文，允许阻塞和 osDelay） */
static void comm_handle(char *line)
{
  /* ---- FACE_OK;<id>;<ROLE>：上位机(Qt)人脸识别命中，请求开门 ----
     这是 AUTH_OpenExternal() 预留钩子的最终接入点：认证在 PC 侧完成，
     开门动作仍由本板执行（策略边界与计划书一致）。
     id   = 上位机人员库主键（仅用于日志，本板不需要人员表）
     ROLE = USER / ADMIN（阶段四做管理员双因子时再细分，现在只透传打印） */
  if (strncmp(line, "FACE_OK;", 8) == 0)
  {
    const char *q = line + 8;
    uint16_t id = 0;
    while (*q >= '0' && *q <= '9') { id = (uint16_t)(id * 10 + (uint16_t)(*q - '0')); q++; }
    if (*q == ';') q++;                       /* 跳到 ROLE 字段 */
    DBG("FACE_OK "); DBG_U16(id); DBG(" "); DBG(q); DBG("\r\n");
    AUTH_OpenExternal(q);                     /* OLED + 语音 + 舵机 + 自动落锁 */
    return;
  }

  /* ---- HOST;PING：上位机心跳请求 ----
     ⚠ 与下面的 PING 区分开：PING 的原义是「重测 ZW111 指纹模组握手」，
     是调试指纹用的；这条只做连通性应答，两者不要混用。 */
  if (strncmp(line, "HOST;PING", 9) == 0)
  {
    DBG("HOST;PONG\r\n");
    return;
  }

  /* ---- SERVO;<角度>：单独测试舵机（0~180），不依赖指纹，用来隔离硬件/供电问题 ---- */
  if (strncmp(line, "SERVO;", 6) == 0)
  {
    const char *q = line + 6;
    uint16_t ang = 0;
    if (*q < '0' || *q > '9') { DBG("SERVO: bad angle\r\n"); return; }
    while (*q >= '0' && *q <= '9') { ang = (uint16_t)(ang * 10 + (uint16_t)(*q - '0')); q++; }
    if (ang > 180) { DBG("SERVO: use 0..180\r\n"); return; }
    DBG("SERVO "); DBG_U16(ang); DBG("\r\n");
    SERVO_SetAngle((uint8_t)ang);
    return;
  }

  /* ---- VOICE;<曲目号>：单独测试语音播报（1~65535；填 0 = 停止播放）----
     曲目号就是音频文件名里的 5 位数字，如 00010.mp3 -> 发 VOICE;10。
     VOL;<0~30> 可以调音量。两条都只测硬件链路，不碰认证流程。 */
  if (strncmp(line, "VOICE;", 6) == 0)
  {
    const char *q = line + 6;
    uint32_t tr = 0;
    if (*q < '0' || *q > '9') { DBG("VOICE: bad track\r\n"); return; }
    while (*q >= '0' && *q <= '9') { tr = tr * 10u + (uint32_t)(*q - '0'); q++; }
    if (tr > 65535u) { DBG("VOICE: use 1..65535 (0=stop)\r\n"); return; }
    if (tr == 0) { DBG("VOICE stop\r\n"); VOICE_Stop(); return; }
    DBG("VOICE "); DBG_U16((uint16_t)tr); DBG("\r\n");
    VOICE_Play((uint16_t)tr);
    return;
  }

  /* ---- VOL;<0~30>：设置播报音量 ---- */
  if (strncmp(line, "VOL;", 4) == 0)
  {
    const char *q = line + 4;
    uint16_t v = 0;
    if (*q < '0' || *q > '9') { DBG("VOL: bad value\r\n"); return; }
    while (*q >= '0' && *q <= '9') { v = (uint16_t)(v * 10 + (uint16_t)(*q - '0')); q++; }
    if (v > VOICE_VOL_MAX) { DBG("VOL: use 0..30\r\n"); return; }
    DBG("VOL "); DBG_U16(v); DBG("\r\n");
    VOICE_SetVolume((uint8_t)v);
    return;
  }

  /* ---- VDISC;<0|1|2>：切换模块盘符（0=U盘 1=TF卡 2=FLASH）----
     07 指定曲目播的是"当前盘符"。文件拷在 FLASH 里却从 U 盘播，就会完全对不上号。
     手册建议切换前先查询当前盘符，但查询要读模块回复（本驱动暂未做接收），
     所以这里先给个"盲切"，够用来验证盘符假设。 */
  if (strncmp(line, "VDISC;", 6) == 0)
  {
    const char *q = line + 6;
    if (*q < '0' || *q > '2' || q[1] != '\0')
    {
      DBG("VDISC: use 0=USB 1=SD 2=FLASH\r\n");
      return;
    }
    DBG("VDISC "); DBG_U16((uint16_t)(*q - '0')); DBG("\r\n");
    VOICE_SwitchDisc((uint8_t)(*q - '0'));
    return;
  }

  /* ---- VLOOP;<0~7>：设置播放模式（0=全盘循环 1=单曲循环 2=单曲停止 3=全盘随机
     4=目录循环 5=目录随机 6=目录顺序 7=顺序播放）----
     用来在线试出「单曲停止」到底是 02 还是 03（手册 §8.3 表写 02，§8.4 例子写 03，
     自相矛盾）。试法：发 VLOOP;2 后再发 VOICE;4 听一句，若播完还继续往下播，
     换个值再试；哪次播完就安静了，就是它。不用重编固件。 */
  if (strncmp(line, "VLOOP;", 6) == 0)
  {
    const char *q = line + 6;
    if (*q < '0' || *q > '7' || q[1] != '\0') { DBG("VLOOP: use 0..7\r\n"); return; }
    DBG("VLOOP "); DBG_U16((uint16_t)(*q - '0')); DBG("\r\n");
    VOICE_SetPlayMode((uint8_t)(*q - '0'));
    return;
  }

  /* ---- PING：重测指纹模组握手，不用重启板子 ---- */
  if (strncmp(line, "PING", 4) == 0)
  {
    DBG("ZW111: ");
    fp_take_bus();
    uint8_t a = ZW111_HandShake();
    fp_release_bus();
    report_fp_ack(a);
    return;
  }

  if (strncmp(line, "ENROLL;", 7) != 0) { DBG("CMD? "); DBG(line); DBG("\r\n"); return; }

  const char *p = line + 7;
  uint16_t id = 0;
  if (*p < '0' || *p > '9') { DBG("ENROLL: bad id\r\n"); return; }
  while (*p >= '0' && *p <= '9') { id = (uint16_t)(id * 10 + (uint16_t)(*p - '0')); p++; }
  if (id >= ZW111_CAPACITY) { DBG("ENROLL: id out of range\r\n"); return; }

  DBG("ENROLL "); DBG_U16(id); DBG(" ...\r\n");

  fp_take_bus();                                  /* 独占 USART2 */

  uint8_t ack = fp_enroll(id, FP_ENROLL_TIMES);   /* 逐步给提示，详见 fp_enroll() */

  fp_release_bus();

  OLED_ClearArea(0, OLED_LINE1, 128, 16);
  OLED_ClearArea(0, OLED_LINE2, 128, 16);
  OLED_ClearArea(0, OLED_LINE3, 128, 16);
  if (ack == ZW111_OK)
  {
    OLED_ShowChinese(0, OLED_LINE1, "录入成功");
    VOICE_Play(V_TRACK_ENROLL_OK);
    DBG("ENROLL OK\r\n");
  }
  else
  {
    OLED_ShowChinese(0, OLED_LINE1, "录入失败");
    OLED_ShowChinese(0, OLED_LINE2, "请重试");
    VOICE_Play(V_TRACK_ENROLL_FAIL);
    DBG("ENROLL FAIL 0x"); DBG_Hex2(ack); DBG("\r\n");
  }
  OLED_ShowChinese(0, OLED_LINE3, "指纹就绪");
  OLED_Update();

  osDelay(1500);
  ui_standby();
}

/* ---------------- 指令任务：把中断收好的整行交给 comm_handle ---------------- */
void StartCommTask(void *argument)
{
  (void)argument;

  HAL_UART_Receive_IT(&huart1, &s_rxByte, 1);   /* 挂上第一字节 */

  uint8_t lastLen = 0, idle = 0;

  for (;;)
  {
    /* 兼容不补 CR/LF 的串口工具（很多「发送」按钮只发框里的原文）：
       收到字节后若连续 100ms 没有新字节，也当作一行结束。 */
    if (!s_cmdReady && s_cmdLen > 0)
    {
      if (s_cmdLen == lastLen)
      {
        if (++idle >= 5)
        {
          taskENTER_CRITICAL();
          if (s_cmdLen < CMD_LINE_MAX) s_cmdBuf[s_cmdLen] = '\0';
          s_cmdReady = 1;
          taskEXIT_CRITICAL();
          idle = 0;
        }
      }
      else { lastLen = s_cmdLen; idle = 0; }
    }
    else { lastLen = s_cmdLen; idle = 0; }

    if (s_cmdReady)
    {
      char line[CMD_LINE_MAX];
      /* 复制整行时进临界区，避免 ISR 同时改写缓冲 */
      taskENTER_CRITICAL();
      for (uint8_t i = 0; i < CMD_LINE_MAX; i++) line[i] = (char)s_cmdBuf[i];
      s_cmdLen = 0;
      s_cmdReady = 0;
      taskEXIT_CRITICAL();

      comm_handle(line);
      lastLen = 0;
      idle = 0;
    }
    osDelay(20);
  }
}

/* ============================================================
   开门/拒绝流程的独占闸门
   ------------------------------------------------------------
   为什么需要：auth_open() 里 osDelay(OPEN_HOLD_MS) 会阻塞 2.5s，
   这期间任务处于 Blocked 态、让出 CPU，而同优先级的另外两个任务
   照常运行。auth_open() 本身没有「是否已在执行」的概念，于是指纹
   可以在刷卡那条还睡着的时候再进来一次，两条流程同时操作舵机和串口：
     - 先到期的那个先 SERVO_Close()，把后一次的开门时间提前掐断
     - DOOR;OPEN / DOOR;CLOSED 各上报两次，上位机事件表重复记一条
     - 屏幕被先到期的 ui_standby() 刷回「已锁定」，可门其实还开着
   这里用一把闸门把所有「独占人机通道(OLED+语音+舵机)」的流程串起来，
   后到者直接丢弃——门禁场景下重复的开门请求本就无意义。

   auth_denied() 也走同一把闸门：它同样独占 OLED 和语音。若不拦，
   门开着时刷一张非法卡，屏幕会被改写成「验证失败」并随后刷回待机，
   造成屏幕显示与实际门锁状态不一致。
   ============================================================ */
static volatile uint8_t s_authBusy = 0;    /* 1 = 有流程正在占用人机通道 */

/* 拿闸门：成功返回 1；已有流程在跑返回 0（调用方放弃本次动作）。
   读改写在临界区内完成——tick 中断可能正好插在「判断」与「置位」之间，
   一旦在那被切走，另一个任务就会看到 s_authBusy 仍是 0 而一同进入。 */
static uint8_t auth_lock(void)
{
  uint8_t ok = 1;
  taskENTER_CRITICAL();
  if (s_authBusy) ok = 0; else s_authBusy = 1;
  taskEXIT_CRITICAL();
  return ok;
}

static void auth_unlock(void)
{
  s_authBusy = 0;
}

/* ---------------- 共用开门/拒绝动作（刷卡与指纹共用） ----------------
   参数是**语音曲目号**：OLED 第二行的中文详情从 track_zh() 取，
   所以屏幕上写的和喇叭里念的永远是同一件事。 */
static void auth_open(uint16_t track)
{
  if (!auth_lock()) return;              /* 已有开门/拒绝流程在跑，本次忽略 */

  const char *zh = track_zh(track);

  OLED_ClearArea(0, OLED_LINE1, 128, 16);
  OLED_ClearArea(0, OLED_LINE2, 128, 16);
  OLED_ShowChinese(0, OLED_LINE1, "已开锁");
  OLED_ShowChinese(0, OLED_LINE2, zh ? zh : "验证成功");
  OLED_Update();
  VOICE_Play(track);
  DBG("OPEN\r\n");
  SERVO_Open();
  /* 回传门锁状态给上位机（Qt 侧据此写 door_open 事件并入库）。
     单次 Transmit 一次发完，尽量减少与其它任务打印交错的窗口。 */
  DBG("DOOR;OPEN\r\n");
  osDelay(OPEN_HOLD_MS);
  SERVO_Close();
  DBG("DOOR;CLOSED\r\n");
  ui_standby();

  auth_unlock();                         /* 门已落锁，释放人机通道 */
}

static void auth_denied(uint16_t track)
{
  if (!auth_lock()) return;              /* 不打断正在进行的开门/拒绝流程 */

  const char *zh = track_zh(track);

  OLED_ClearArea(0, OLED_LINE1, 128, 16);
  OLED_ClearArea(0, OLED_LINE2, 128, 16);
  OLED_ShowChinese(0, OLED_LINE1, "验证失败");
  OLED_ShowChinese(0, OLED_LINE2, zh ? zh : "请重试");
  OLED_Update();
  VOICE_Play(track);
  DBG("DENIED\r\n");
  osDelay(1500);
  ui_standby();

  auth_unlock();                         /* 提示播完，释放人机通道 */
}

/* ---------------- 初始化 + 建任务 ----------------
   本函数由 main() 在 osKernelInitialize() 之前调用。
   注意两条约束：
   1) osThreadNew/xTaskCreate 在调度器启动前是合法用法（任务进就绪表，
      osKernelStart() 后才被调度），所以这里建任务没问题；
   2) 但 osDelay/vTaskDelay 在 pxCurrentTCB 仍为 NULL 时会踩空指针，
      **调度器启动前禁止任何 osDelay**。故各驱动的初始化里只能用 HAL_Delay。 */
void APP_Auth_Init(void)
{
  SERVO_Init();
  OLED_Init();
  RC522_Init();
  VOICE_Init();   /* 把 JQ8900 音量拉回默认值（纯发送，没接模块也不阻塞） */

  /* 上电画面（中文）—— 先推上去，别让指纹模组探活（会重试几百 ms）拖住屏幕 */
  OLED_Clear();
  OLED_ShowChinese(0, OLED_LINE0, "智能门禁系统");
  OLED_ShowChinese(0, OLED_LINE1, "已锁定");
  OLED_ShowChinese(0, OLED_LINE2, "请刷卡或指纹");
  OLED_Update();   /* 必须调用：显示函数只写显存，否则上电是随机显存的雪花屏 */

  /* ---- ZW111 上电自检：把握手确认码打到 USART1 ----
     00   = 模组在线
     FF   = 无应答（没接 / 波特率不对 / TX-RX 没交叉 / 总线被 RS485 抢）
     其它 = 模组回了非 0 确认码，按协议查错
     注意：这只是「此刻」的探测结果，不阻塞任何功能；随时可再发 PING 重测。 */
  DBG("ZW111: ");
  report_fp_ack(ZW111_Init());

  static const osThreadAttr_t cattr = { .name = "CardTask", .stack_size = 1024, .priority = osPriorityNormal };
  osThreadNew(StartCardTask, NULL, &cattr);

  static const osThreadAttr_t fattr = { .name = "FPTask", .stack_size = 1024, .priority = osPriorityNormal };
  osThreadNew(StartFPTask, NULL, &fattr);

  static const osThreadAttr_t matt = { .name = "CommTask", .stack_size = 1024, .priority = osPriorityNormal };
  osThreadNew(StartCommTask, NULL, &matt);

  /* ---- 开机播报 ----
     放在最后、并且先等 400ms：JQ8900 和指纹模组一样上电需要时间才响应，
     太早发会丢帧（掉电后第一次尤其明显）。 */
  HAL_Delay(400);
  VOICE_Play(V_TRACK_BOOT);
}

/* ---------------- 刷卡任务 ---------------- */
void StartCardTask(void *argument)
{
  uint8_t uid[5];
  uint8_t uid_len;
  (void)argument;

  for (;;)
  {
    if (RC522_Check(uid, &uid_len) == 0)
    {
      /* 每次刷卡都打印 UID(4字节/8位十六进制)，用于填白名单 s_allowed */
      DBG("UID: ");
      DBG_Hex2(uid[0]); DBG_Hex2(uid[1]); DBG_Hex2(uid[2]); DBG_Hex2(uid[3]);
      DBG("\r\n");
      if (uid_match(uid)) { DBG("CARD OK\r\n"); auth_open(V_TRACK_CARD_OK); }
      else auth_denied(V_TRACK_UNKNOWN_CARD);
    }
    osDelay(POLL_MS);
  }
}

/* ---------------- 指纹任务：GetImage -> GenChar -> Search(1:N) ---------------- */
void StartFPTask(void *argument)
{
  uint16_t pageId = 0, score = 0;
  uint8_t role = 0;
  (void)argument;

  OLED_ShowChinese(0, OLED_LINE3, "指纹就绪");
  OLED_Update();
  for (;;)
  {
    /* 录入(ENROLL)期间独占 USART2，这里必须停手，否则协议帧会被撕碎；
       s_fpParked 是「已让出总线」的确认，CommTask 靠它判断可以安全开始录入 */
    if (s_fpBusy) { s_fpParked = 1; osDelay(100); continue; }
    s_fpParked = 0;

#ifdef ZW111_USE_TOUCH_PIN
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_1) == GPIO_PIN_SET) {   /* 有手指才采图 */
#endif
      uint8_t r = ZW111_GetImage();
      if (r == ZW111_OK)                                  /* 采到指纹图 */
      {
        if (ZW111_GenChar(1) == ZW111_OK)
        {
          if (ZW111_Search(1, 0, ZW111_CAPACITY, &pageId, &score) == ZW111_OK)
          {
            const char *who = fp_lookup(pageId, &role);
            if (who) { DBG("FP: "); DBG(who); DBG("\r\n"); auth_open(V_TRACK_FP_OK); }
            else auth_denied(V_TRACK_FP_UNKNOWN);         /* 命中库但不在用户表 */
          }
          else
          {
            auth_denied(V_TRACK_FP_NOMATCH);              /* 库里没有 */
          }
        }
      }
#ifdef ZW111_USE_TOUCH_PIN
    }
#endif
    osDelay(250);
  }
}

/* ---------------- 上位机(Qt)下发 FACE_OK;id;ROLE 时调用（comm_handle 里已接入） ----------------
   复用同一条开门链路：OLED + 语音 + 舵机 + 自动落锁 + DOOR 状态回传。
   src 为 ROLE 字段（USER/ADMIN），本板暂不据此区分策略。 */
void AUTH_OpenExternal(const char *src)
{
  (void)src;
  auth_open(V_TRACK_FACE_OK);
}
