/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    servo.c
  * @brief   舵机(模拟门锁)驱动 - TIM2_CH2 @ PB3, 50Hz PWM（PB3 经 U13 NRF 排母座 pin5=SCK 孔引出）
  *
  *   ⚠ 行程上限：普通 SG90 是"位置型"舵机，内部电位器限位，**机械行程约 180°**。
  *     给超过 2.0~2.5ms 的脉宽不会让它多转，只会撞限位堵转发热。想要 360° 连续
  *     旋转必须换硬件（连续旋转舵机 / 步进电机），软件改不出来。
  *
  *   ⚠ 换定时器/换引脚时必看的刻度换算：
  *     TIM2 挂 APB1，APB1 的定时器时钟 = 84MHz
  *     Prescaler = 83   -> 计数频率 84MHz/(83+1) = 1MHz  ->  **1 tick = 1us**
  *     Period    = 19999 -> 周期 20000us = 20ms = 50Hz
  *   所以 tick 数就等于微秒数：1000=1.0ms(0°)  1500=1.5ms(90°)  2000=2.0ms(180°)
  *
  *   历史坑：原来挂在 TIM8(APB2，定时器时钟 168MHz) 时是 **0.5us/tick**，
  *   同样的 tick 值脉宽会翻倍。换到 TIM2 后若忘了重算，脉宽就变成 2.0ms/3.0ms，
  *   舵机要么被顶到极限堵转、要么直接不响应——表现就是"舵机没反应"。
  ******************************************************************************
  */
/* USER CODE END Header */
#include "servo.h"

void SERVO_Init(void)
{
  /* 启动 TIM2 通道2 PWM 输出 */
  HAL_TIM_PWM_Start(SERVO_TIM, SERVO_CHANNEL);
  SERVO_Close();   /* 上电默认落锁 */
}

/* 参数 ticks 的物理含义就是微秒数（1 tick = 1us，见文件头换算说明） */
static void SERVO_SetPulseTicks(uint32_t ticks)
{
  if (ticks < 500u)  ticks = 500u;    /* 0.5ms 以下非法 */
  if (ticks > 2500u) ticks = 2500u;   /* 2.5ms 以上非法（超出 SG90 有效范围） */
  __HAL_TIM_SET_COMPARE(SERVO_TIM, SERVO_CHANNEL, ticks);
}

/* angle: 0~180 线性映射到 1.0ms~2.0ms */
void SERVO_SetAngle(uint8_t angle)
{
  if (angle > 180u) angle = 180u;
  SERVO_SetPulseTicks(1000u + (uint32_t)angle * 1000u / 180u);
}

void SERVO_Open(void)
{
  SERVO_SetAngle(SERVO_OPEN_ANGLE);
}

void SERVO_Close(void)
{
  SERVO_SetAngle(SERVO_LOCK_ANGLE);
}
