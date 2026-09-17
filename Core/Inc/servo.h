/* USER CODE BEGIN Header */
/**
  ******************************************************************************
 * @file    servo.h
 * @brief   舵机(模拟门锁)驱动 - TIM2_CH2 @ PB3, 50Hz PWM
 *          1.0ms=上锁(0°)  1.5ms=开锁(90°)
 *          刻度：TIM2@APB1 84MHz，Prescaler=83 -> 1MHz -> **1 tick = 1us**，ARR=19999 -> 20ms
 *          PB3 从开发板 U13(NRF24L01) 排母座第 5 脚(SCK 孔)引出，公头舵机可直接插排母孔
 ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __SERVO_H
#define __SERVO_H

#include "main.h"
#include "tim.h"    /* SERVO_TIM 宏展开出 htim2，必须在此可见 */

/* 舵机挂在 TIM2 通道2 (PB3，经 U13 NRF 排母座 pin5=SCK 孔引出) */
#define SERVO_TIM       (&htim2)
#define SERVO_CHANNEL   TIM_CHANNEL_2

/* 角度定义（0~180，普通 SG90 的物理行程上限就是约 180°，做不到 360°）
   LOCK = 上锁位，OPEN = 开锁位。
   ⚠ OPEN 不要设到 180 以上：超出范围舵机会撞限位堵转、嗡嗡响、发热，久了会烧。
   若机械结构在 180° 处顶死（能听到持续嗡嗡声），就往下调到 150 / 120 试。 */
#define SERVO_LOCK_ANGLE  0
#define SERVO_OPEN_ANGLE  180

void SERVO_Init(void);            /* 启动PWM并默认上锁 */
void SERVO_SetAngle(uint8_t angle);
void SERVO_Open(void);            /* 转到开锁角 */
void SERVO_Close(void);           /* 转回上锁角 */

#endif /* __SERVO_H */
