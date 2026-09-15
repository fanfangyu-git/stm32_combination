/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under Ultimate Liberty license
  * SLA0044, the "License"; You may not use this file except in compliance with
  * the License. You may obtain a copy of the License at:
  *                             www.st.com/SLA0044
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define KEY2_Pin GPIO_PIN_2
#define KEY2_GPIO_Port GPIOE
#define KEY1_Pin GPIO_PIN_3
#define KEY1_GPIO_Port GPIOE
#define KEY0_Pin GPIO_PIN_4
#define KEY0_GPIO_Port GPIOE
#define BEEP_Pin GPIO_PIN_8
#define BEEP_GPIO_Port GPIOF
#define LED0_Pin GPIO_PIN_9
#define LED0_GPIO_Port GPIOF
#define LED1_Pin GPIO_PIN_10
#define LED1_GPIO_Port GPIOF
#define STEP_IN1_Pin GPIO_PIN_0
#define STEP_IN1_GPIO_Port GPIOC
#define STEP_IN2_Pin GPIO_PIN_1
#define STEP_IN2_GPIO_Port GPIOC
#define STEP_IN3_Pin GPIO_PIN_2
#define STEP_IN3_GPIO_Port GPIOC
#define STEP_IN4_Pin GPIO_PIN_3
#define STEP_IN4_GPIO_Port GPIOC
#define KEY3_Pin GPIO_PIN_0
#define KEY3_GPIO_Port GPIOA
#define KEYPAD_COL2_Pin GPIO_PIN_4
#define KEYPAD_COL2_GPIO_Port GPIOC
#define KEYPAD_COL3_Pin GPIO_PIN_5
#define KEYPAD_COL3_GPIO_Port GPIOC
#define KEYPAD_COL4_Pin GPIO_PIN_6
#define KEYPAD_COL4_GPIO_Port GPIOC
#define KEYPAD_ROW3_Pin GPIO_PIN_5
#define KEYPAD_ROW3_GPIO_Port GPIOB
#define KEYPAD_ROW4_Pin GPIO_PIN_8
#define KEYPAD_ROW4_GPIO_Port GPIOB
#define KEYPAD_COL1_Pin GPIO_PIN_9
#define KEYPAD_COL1_GPIO_Port GPIOB
#define KEYPAD_ROW1_Pin GPIO_PIN_0
#define KEYPAD_ROW1_GPIO_Port GPIOE
#define KEYPAD_ROW2_Pin GPIO_PIN_1
#define KEYPAD_ROW2_GPIO_Port GPIOE
/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
