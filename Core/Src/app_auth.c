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

/* ---------------- 共用开门/拒绝动作（刷卡与指纹共用） ---------------- */
static void auth_open(const char *who)
{
  OLED_ShowString(0, OLED_LINE1, "STATUS: OPEN  ", OLED_8X16);
  OLED_ShowString(0, OLED_LINE2, who ? who : "ACCESS", OLED_8X16);
  OLED_Update();
  DBG("OPEN: "); DBG(who ? who : "?"); DBG("\r\n");
  SERVO_Open();
  osDelay(OPEN_HOLD_MS);
  SERVO_Close();
  OLED_ShowString(0, OLED_LINE1, "STATUS: LOCKED", OLED_8X16);
  OLED_ShowString(0, OLED_LINE2, "              ", OLED_8X16);
  OLED_Update();
}

static void auth_denied(const char *msg)
{
  OLED_ShowString(0, OLED_LINE1, "STATUS: DENIED", OLED_8X16);
  OLED_ShowString(0, OLED_LINE2, msg ? msg : "DENIED", OLED_8X16);
  OLED_Update();
  DBG("DENIED: "); DBG(msg ? msg : "?"); DBG("\r\n");
  osDelay(1500);
  OLED_ShowString(0, OLED_LINE1, "STATUS: LOCKED", OLED_8X16);
  OLED_ShowString(0, OLED_LINE2, "              ", OLED_8X16);
  OLED_Update();
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
  ZW111_Init();

  static const osThreadAttr_t cattr = { .name = "CardTask", .stack_size = 1024, .priority = osPriorityNormal };
  osThreadNew(StartCardTask, NULL, &cattr);

  static const osThreadAttr_t fattr = { .name = "FPTask", .stack_size = 1024, .priority = osPriorityNormal };
  osThreadNew(StartFPTask, NULL, &fattr);

  /* 上电画面 */
  OLED_Clear();
  OLED_ShowString(0, OLED_LINE0, "SMART DOOR", OLED_8X16);
  OLED_ShowString(0, OLED_LINE1, "STATUS: LOCKED", OLED_8X16);
  OLED_Update();   /* 必须调用：显示函数只写显存，否则上电是随机显存的雪花屏 */
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
      if (uid_match(uid)) auth_open("CARD OK");
      else auth_denied("UNKNOWN CARD");
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

  OLED_ShowString(0, OLED_LINE3, "FP:READY ", OLED_8X16);
  OLED_Update();
  for (;;)
  {
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
            if (who) auth_open(who);                      /* 命中白名单 -> 开门 */
            else auth_denied("FP UNKNOWN");               /* 命中库但不在用户表 */
          }
          else
          {
            auth_denied("FP NOMATCH");                    /* 库里没有 */
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
  OLED_ShowString(0, OLED_LINE1, "STATUS: OPEN  ", OLED_8X16);
  OLED_ShowString(0, OLED_LINE2, src ? src : "PC CMD", OLED_8X16);
  OLED_Update();
  SERVO_Open();
  osDelay(OPEN_HOLD_MS);
  SERVO_Close();
  OLED_ShowString(0, OLED_LINE1, "STATUS: LOCKED", OLED_8X16);
  OLED_Update();
}
