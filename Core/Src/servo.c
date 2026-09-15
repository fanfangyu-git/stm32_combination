/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    servo.c
  * @brief   舵机(模拟门锁)驱动 - TIM8_CH2 @ PC7, 50Hz PWM
  ******************************************************************************
  */
/* USER CODE END Header */
#include "servo.h"

void SERVO_Init(void)
{
  /* 启动 TIM8 通道2 PWM 输出 */
  HAL_TIM_PWM_Start(SERVO_TIM, SERVO_CHANNEL);
  SERVO_Close();   /* 上电默认落锁 */
}

/* ticks: 0.5us/计数值; 1.0ms=2000, 1.5ms=3000, 2.0ms=4000 */
static void SERVO_SetPulseTicks(uint32_t ticks)
{
  if (ticks > 4000) ticks = 4000;
  __HAL_TIM_SET_COMPARE(SERVO_TIM, SERVO_CHANNEL, ticks);
}

/* angle: 0~180, 线性映射到 1.0ms~2.0ms */
void SERVO_SetAngle(uint8_t angle)
{
  uint32_t ticks = 2000u + (uint32_t)angle * 2000u / 180u;
  SERVO_SetPulseTicks(ticks);
}

void SERVO_Open(void)
{
  SERVO_SetAngle(SERVO_OPEN_ANGLE);
}

void SERVO_Close(void)
{
  SERVO_SetAngle(SERVO_LOCK_ANGLE);
}
