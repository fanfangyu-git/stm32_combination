/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    zw111.h
  * @brief   ZW111 半导体指纹模组驱动（标准 0xEF01 协议，与 AS608 同族）
  *          接口：USART2 (PA2/TX -> 模组RX, PA3/RX -> 模组TX)，57600 8N1
  *          说明：本驱动只做"协议封装+收发"，不含 RTOS；由 app_auth.c 的
  *                FPTask 调用。ZW111 自带半导体活体检测，V_SENSOR 须常供 3.3V。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __ZW111_H
#define __ZW111_H

#include "main.h"

/* ---------------- 确认码（常用） ---------------- */
#define ZW111_OK            0x00
#define ZW111_ERR_PKG       0x01   /* 收包有错 */
#define ZW111_ERR_NOFINGER  0x02   /* 传感器上无手指 */
#define ZW111_ERR_IMGFAIL   0x03   /* 录入图像失败 */
#define ZW111_ERR_MISMATCH  0x08   /* 指纹不匹配 */
#define ZW111_ERR_NOTFOUND  0x09   /* 没搜索到 */
#define ZW111_ERR_ENROLL    0x25   /* 录入次数设置错误 */
#define ZW111_ERR_TIMEOUT   0x26   /* 超时 */
#define ZW111_ERR_DUP       0x27   /* 指纹已存在 */
#define ZW111_ERR_DBEMPTY   0x24   /* 指纹库为空 */

/* 指纹库容量：小面积 ZW111=50，中面积=320；按你手上的模组改 */
#ifndef ZW111_CAPACITY
#define ZW111_CAPACITY      50
#endif

/* 可选：把 ZW111 的 TOUCH_OUT(活体/触摸信号) 接 PC1，并在 CubeMX 把 PC1
   配成输入，再取消下一行注释，可避免无手指时反复空轮询 GetImage。
   不接也能用——默认走 GetImage 轮询。 */
/* #define ZW111_USE_TOUCH_PIN */

uint8_t ZW111_Init(void);                                       /* 返回握手确认码：00=正常，FF=模组无应答 */
uint8_t ZW111_HandShake(void);                                  /* 探活，返回确认码 */
uint8_t ZW111_GetImage(void);                                   /* 00=采到指纹图, 02=无手指 */
uint8_t ZW111_GenChar(uint8_t bufId);                           /* 生成特征到缓冲区 bufId */
uint8_t ZW111_RegModel(void);                                   /* 合并特征：缓冲区1+2 -> 模板(存回缓冲区1) */
uint8_t ZW111_Search(uint8_t bufId, uint16_t startPage,
                     uint16_t pageNum, uint16_t *pageId, uint16_t *score);
uint8_t ZW111_StoreChar(uint8_t bufId, uint16_t pageId);
uint8_t ZW111_DeleteChar(uint16_t pageId, uint16_t n);
uint8_t ZW111_Empty(void);
uint8_t ZW111_AutoEnroll(uint16_t pageId, uint8_t times);       /* 一键录入(模组 LED 引导) */

#endif /* __ZW111_H */
