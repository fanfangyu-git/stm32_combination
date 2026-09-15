/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    servo.h
  * @brief   舵机(模拟门锁)驱动 - TIM8_CH2 @ PC7, 50Hz PWM
  *          1.0ms=上锁(0°)  1.5ms=开锁(90°)  （tick=0.5us, ARR=19999）
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __SERVO_H
#define __SERVO_H

#include "main.h"
#include "tim.h"    /* SERVO_TIM 宏展开出 htim8，必须在此可见 */

/* 舵机挂在 TIM8 通道2 (PC7) */
#define SERVO_TIM       (&htim8)
#define SERVO_CHANNEL   TIM_CHANNEL_2

/* 角度定义：0°=上锁(1.0ms)，90°=开锁(1.5ms)，可按机械结构微调 */
#define SERVO_LOCK_ANGLE  0
#define SERVO_OPEN_ANGLE  90

void SERVO_Init(void);            /* 启动PWM并默认上锁 */
void SERVO_SetAngle(uint8_t angle);
void SERVO_Open(void);            /* 转到开锁角 */
void SERVO_Close(void);           /* 转回上锁角 */

#endif /* __SERVO_H */
