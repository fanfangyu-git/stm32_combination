/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    zw111.c
  * @brief   ZW111 指纹模组 0xEF01 协议驱动实现
  ******************************************************************************
  */
/* USER CODE END Header */
#include "zw111.h"
#include "usart.h"
#include "cmsis_os.h"
#include <string.h>

#ifndef ZW111_UART_TIMEOUT
#define ZW111_UART_TIMEOUT   2000    /* 普通指令应答超时(ms) */
#endif

/* ---------------- 底层 UART（阻塞，专用 USART2） ---------------- */
static int uart_getc(uint8_t *b, uint32_t to)
{
  return (HAL_UART_Receive(&huart2, b, 1, to) == HAL_OK) ? 0 : -1;
}
static int uart_readn(uint8_t *dst, uint8_t n, uint32_t to)
{
  for (uint8_t i = 0; i < n; i++)
    if (uart_getc(dst + i, to)) return -1;
  return 0;
}

/* ---------------- 组帧并发送命令包 ----------------
   帧：EF01 + ADDR(FFFFFFFF) + 01(包标识)
        + L(2) + CMD(1) + PARAM(N) + SUM(2)
   L   = 指令码(1) + 参数(N) + 校验和(2)
   SUM = 从包标识(01)累加至参数末字节（大端 2 字节）            */
static void zw_send(uint8_t cmd, const uint8_t *p, uint8_t plen)
{
  uint8_t frame[16];
  frame[0] = 0xEF; frame[1] = 0x01;
  frame[2] = frame[3] = frame[4] = frame[5] = 0xFF;   /* 默认地址 FFFFFFFF */
  frame[6] = 0x01;                                    /* 包标识 = 命令 */
  uint16_t L = 1 + plen + 2;
  frame[7] = (uint8_t)(L >> 8);
  frame[8] = (uint8_t)(L & 0xFF);
  frame[9] = cmd;
  uint8_t idx = 10;
  uint16_t sum = 0x01 + frame[7] + frame[8] + cmd;    /* 校验和起点=包标识 */
  for (uint8_t i = 0; i < plen; i++) { frame[idx++] = p[i]; sum += p[i]; }
  frame[idx++] = (uint8_t)(sum >> 8);
  frame[idx++] = (uint8_t)(sum & 0xFF);
  HAL_UART_Transmit(&huart2, frame, idx, 500);
}

/* ---------------- 接收应答帧 ----------------
   同步到 EF01，再收 addr(4)+07(1)+L(2)+确认码(1)，然后收返回参数(L-3)，最后丢弃校验和。
   返回 0 成功；*confirm=确认码；*ret=返回参数；*retlen=返回参数长度。   */
static int zw_recv_frame(uint8_t *confirm, uint8_t *ret, uint8_t *retlen, uint32_t to)
{
  uint8_t b;
  int state = 0;
  for (;;) {                                    /* 同步帧头 */
    if (uart_getc(&b, to)) return -1;
    if (state == 0) { if (b == 0xEF) state = 1; }
    else { if (b == 0x01) break; else state = 0; }
  }
  uint8_t hdr[8];
  if (uart_readn(hdr, 8, to)) return -1;       /* addr(4)+包标识(1)+L(2)+确认码(1) */
  if (hdr[4] != 0x07) return -1;               /* 不是应答包 */
  uint16_t L = ((uint16_t)hdr[5] << 8) | hdr[6];
  *confirm = hdr[7];
  uint8_t rlen = (L >= 3) ? (uint8_t)(L - 3) : 0;
  *retlen = rlen;
  if (rlen) uart_readn(ret, rlen, to);
  uint8_t cs[2];
  uart_readn(cs, 2, to);                       /* 校验和，本驱动暂不严格校验 */
  return 0;
}

/* ---------------- 对外 API ----------------
   返回握手确认码：ZW111_OK(0x00)=模组在线；0xFF=无应答（没接 / 波特率不对 / TX-RX 没交叉 / 总线被抢）。
   注意：**单次握手在刚上电时很容易扑空**——模组自身也要启动时间，且上电后会先
   主动发 0x55。所以这里等一小会儿再探，失败就重试几次。 */
#define ZW111_PROBE_TRIES   3

uint8_t ZW111_Init(void)
{
  uint8_t ack = 0xFF;

  /* 必须用 HAL_Delay 而非 osDelay：本函数经 APP_Auth_Init() 在
     osKernelStart() 之前调用，此刻 pxCurrentTCB 仍为 NULL，
     vTaskDelay 会解引用空指针（tasks.c prvAddCurrentTaskToDelayedList）。 */
  HAL_Delay(100);                        /* 给模组一点上电稳定时间 */

  for (uint8_t i = 0; i < ZW111_PROBE_TRIES; i++)
  {
    ack = ZW111_HandShake();
    if (ack != 0xFF) break;              /* 只要有应答就采纳 */
    HAL_Delay(200);
  }

  HAL_Delay(50);
  return ack;
}

uint8_t ZW111_HandShake(void)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  zw_send(0x35, NULL, 0);
  if (zw_recv_frame(&confirm, ret, &rl, 1000)) return 0xFF;
  return confirm;
}

uint8_t ZW111_GetImage(void)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  zw_send(0x01, NULL, 0);
  if (zw_recv_frame(&confirm, ret, &rl, ZW111_UART_TIMEOUT)) return 0xFF;
  return confirm;
}

uint8_t ZW111_GenChar(uint8_t bufId)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  uint8_t p[1] = { bufId };
  zw_send(0x02, p, 1);
  if (zw_recv_frame(&confirm, ret, &rl, ZW111_UART_TIMEOUT)) return 0xFF;
  return confirm;
}

/* 合并特征(生成模板)：把缓冲区1、2 的特征融合，模板存回缓冲区1。
   分步录入靠它把多次按压逐轮合并成模板；确认码 0x0A = 合并失败(两次按的不是同一根手指)。 */
uint8_t ZW111_RegModel(void)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  zw_send(0x05, NULL, 0);
  if (zw_recv_frame(&confirm, ret, &rl, ZW111_UART_TIMEOUT)) return 0xFF;
  return confirm;
}

uint8_t ZW111_Search(uint8_t bufId, uint16_t startPage, uint16_t pageNum,
                     uint16_t *pageId, uint16_t *score)
{
  uint8_t confirm, ret[4]; uint8_t rl;
  uint8_t p[5] = { bufId,
                   (uint8_t)(startPage >> 8), (uint8_t)(startPage & 0xFF),
                   (uint8_t)(pageNum >> 8),   (uint8_t)(pageNum & 0xFF) };
  zw_send(0x04, p, 5);
  if (zw_recv_frame(&confirm, ret, &rl, ZW111_UART_TIMEOUT)) return 0xFF;
  if (confirm == ZW111_OK && rl >= 4) {
    *pageId = ((uint16_t)ret[0] << 8) | ret[1];
    *score  = ((uint16_t)ret[2] << 8) | ret[3];
  }
  return confirm;
}

uint8_t ZW111_StoreChar(uint8_t bufId, uint16_t pageId)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  uint8_t p[3] = { bufId, (uint8_t)(pageId >> 8), (uint8_t)(pageId & 0xFF) };
  zw_send(0x06, p, 3);
  if (zw_recv_frame(&confirm, ret, &rl, ZW111_UART_TIMEOUT)) return 0xFF;
  return confirm;
}

uint8_t ZW111_DeleteChar(uint16_t pageId, uint16_t n)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  uint8_t p[4] = { (uint8_t)(pageId >> 8), (uint8_t)(pageId & 0xFF),
                    (uint8_t)(n >> 8),      (uint8_t)(n & 0xFF) };
  zw_send(0x0C, p, 4);
  if (zw_recv_frame(&confirm, ret, &rl, ZW111_UART_TIMEOUT)) return 0xFF;
  return confirm;
}

uint8_t ZW111_Empty(void)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  zw_send(0x0D, NULL, 0);
  if (zw_recv_frame(&confirm, ret, &rl, ZW111_UART_TIMEOUT)) return 0xFF;
  return confirm;
}

uint8_t ZW111_AutoEnroll(uint16_t pageId, uint8_t times)
{
  uint8_t confirm, ret[2]; uint8_t rl;
  /* param = 0x0004：bit2=1 不要求返回中间步骤，模组只回最终确认码；
     录入过程由模组 LED 引导用户按压 times 次。超时给 20s。 */
  uint8_t p[5] = { (uint8_t)(pageId >> 8), (uint8_t)(pageId & 0xFF),
                    times, 0x00, 0x04 };
  zw_send(0x31, p, 5);
  if (zw_recv_frame(&confirm, ret, &rl, 20000)) return 0xFF;
  return confirm;
}
