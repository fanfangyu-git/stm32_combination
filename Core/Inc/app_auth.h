/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_auth.h
  * @brief   门禁鉴权主逻辑（阶段一：本地刷卡开锁）
  *          - FreeRTOS 任务 CardTask 轮询 RC522
  *          - 白名单比对：合法->舵机开锁2.5s自动落锁；陌生->拒绝告警
  *          - AUTH_OpenExternal() 为后续 PC 下发指令预留的钩子
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __APP_AUTH_H
#define __APP_AUTH_H

#include "main.h"

void APP_Auth_Init(void);                 /* 初始化各外设（含任务创建），须在 osKernelStart() 之前调用 */
void StartCardTask(void *argument);      /* FreeRTOS 刷卡任务体 */
void StartFPTask(void *argument);        /* FreeRTOS 指纹任务体(GetImage->GenChar->Search) */
void StartCommTask(void *argument);      /* FreeRTOS 指令任务体：USART1 收 PC 文本指令(ENROLL;<id>) */
void AUTH_OpenExternal(const char *src); /* 预留：PC/其他来源触发开锁 */

#endif /* __APP_AUTH_H */
