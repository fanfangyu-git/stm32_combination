/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rc522.h
  * @brief   RC522 RFID 驱动 (SPI2, 软件CS=PB12, RST=PC0)
  *          仅实现：寻卡 + 防冲撞读UID，足够阶段一(刷卡开锁)使用
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __RC522_H
#define __RC522_H

#include "main.h"

/* 引脚：软件片选 + 复位（注意：PC0 原标签 STEP_IN1，此处复用为 RC522_RST） */
#define RC522_CS_PORT    GPIOB
#define RC522_CS_PIN     GPIO_PIN_12
#define RC522_RST_PORT   GPIOC
#define RC522_RST_PIN    GPIO_PIN_0

/* 状态返回 */
#define MI_OK            0
#define MI_NOTAGERR      1
#define MI_ERR           2

/* PICC 命令 */
#define PICC_REQIDL      0x26
#define PICC_ANTICOLL    0x93
#define PICC_HALT        0x50

void RC522_Init(void);                       /* 配置CS/RST引脚、降速SPI、复位并使能天线 */
void RC522_AntennaOn(void);                  /* 打开天线（TxControlReg 的 TX1/TX2） */
void RC522_WriteReg(uint8_t reg, uint8_t val);
uint8_t RC522_ReadReg(uint8_t reg);
uint8_t RC522_Request(uint8_t req_mode, uint8_t *tag_type);
uint8_t RC522_Anticoll(uint8_t *ser_num);
void RC522_Halt(void);
void RC522_CalcCRC(uint8_t *pindata, uint8_t len, uint8_t *poutdata);
/* 便捷接口：读到卡返回0并填充4字节UID；未读到返回非0 */
uint8_t RC522_Check(uint8_t *uid4, uint8_t *uid_len);

#endif /* __RC522_H */
