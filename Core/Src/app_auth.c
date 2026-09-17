/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_auth.c
  * @brief   门禁鉴权主逻辑（阶段一刷卡 + 阶段二 ZW111 指纹）
  *          - CardTask 轮询 RC522 刷卡
  *          - FPTask   轮询 ZW111 指纹(1:N 比对)
  *          - 任一合法来源 -> 舵机开锁2.5s自动落锁；陌生 -> 拒绝+OLED
  *          - 后续阶段二: PC 下发 FACE_OK;UID;ROLE 由 AUTH_OpenExternal() 触发
  ******************************************************************************
  */
/* USER CODE END Header */
#include "app_auth.h"
#include "servo.h"
#include "rc522.h"
#include "oled.h"
#include "zw111.h"
#include "usart.h"
#include "cmsis_os.h"
#include "string.h"

/* ---------------- 卡白名单：替换成你实际刷卡时串口打印出的 UID(4字节) ---------------- */
static const uint8_t s_allowed[][4] = {
    {0xDE, 0xAD, 0xBE, 0xEF},   /* 示例：管理员卡 */
    {0x12, 0x34, 0x56, 0x78},   /* 示例：用户卡 */
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
static void DBG(const char *s)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 200);
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
  ui_line(OLED_LINE2, "请放手指");
  ui_line(OLED_LINE3, NULL);

  /* 第 1 次：采图 -> 特征存入缓冲区 1 */
  r = fp_capture(FP_CAP_TIMEOUT_MS);
  if (r != ZW111_OK) return r;
  r = ZW111_GenChar(1);
  if (r != ZW111_OK) return r;

  /* 第 2..times 次：抬手 -> 再采图 -> 特征存缓冲区 2 -> 合并回缓冲区 1 */
  for (uint8_t n = 2; n <= times; n++)
  {
    ui_line(OLED_LINE2, "请抬手指");
    fp_wait_lift(FP_LIFT_TIMEOUT_MS);

    ui_line(OLED_LINE2, "请再放一次");
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
   指令：
     ENROLL;<id>   录入 <id> 号指纹（按压 3 次，OLED 逐步提示）
     PING          重测指纹模组握手（不用重启板子）
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
    DBG("ENROLL OK\r\n");
  }
  else
  {
    OLED_ShowChinese(0, OLED_LINE1, "录入失败");
    OLED_ShowChinese(0, OLED_LINE2, "请重试");
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

/* ---------------- 共用开门/拒绝动作（刷卡与指纹共用） ----------------
   zh_detail: 全中文详情(显示在 LINE2)，如 “刷卡成功”/“指纹成功”/“人脸成功” */
static void auth_open(const char *zh_detail)
{
  OLED_ClearArea(0, OLED_LINE1, 128, 16);
  OLED_ClearArea(0, OLED_LINE2, 128, 16);
  OLED_ShowChinese(0, OLED_LINE1, "已开锁");
  OLED_ShowChinese(0, OLED_LINE2, zh_detail);
  OLED_Update();
  DBG("OPEN\r\n");
  SERVO_Open();
  osDelay(OPEN_HOLD_MS);
  SERVO_Close();
  ui_standby();
}

static void auth_denied(const char *zh_detail)
{
  OLED_ClearArea(0, OLED_LINE1, 128, 16);
  OLED_ClearArea(0, OLED_LINE2, 128, 16);
  OLED_ShowChinese(0, OLED_LINE1, "验证失败");
  OLED_ShowChinese(0, OLED_LINE2, zh_detail);
  OLED_Update();
  DBG("DENIED\r\n");
  osDelay(1500);
  ui_standby();
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
      if (uid_match(uid)) { DBG("CARD OK\r\n"); auth_open("刷卡成功"); }
      else auth_denied("未知卡片");
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
            if (who) { DBG("FP: "); DBG(who); DBG("\r\n"); auth_open("指纹成功"); }
            else auth_denied("指纹未知");                 /* 命中库但不在用户表 */
          }
          else
          {
            auth_denied("指纹不符");                      /* 库里没有 */
          }
        }
      }
#ifdef ZW111_USE_TOUCH_PIN
    }
#endif
    osDelay(250);
  }
}

/* ---------------- 预留：PC(Qt)下发 FACE_OK;UID;ROLE 指令时调用 ---------------- */
void AUTH_OpenExternal(const char *src)
{
  (void)src;
  OLED_ClearArea(0, OLED_LINE1, 128, 16);
  OLED_ClearArea(0, OLED_LINE2, 128, 16);
  OLED_ShowChinese(0, OLED_LINE1, "已开锁");
  OLED_ShowChinese(0, OLED_LINE2, "人脸成功");
  OLED_Update();
  DBG("FACE OPEN\r\n");
  SERVO_Open();
  osDelay(OPEN_HOLD_MS);
  SERVO_Close();
  ui_standby();
}
