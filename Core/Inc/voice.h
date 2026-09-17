/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    voice.h
  * @brief   JQ8900-16P 语音播报模块驱动（两线串 / UART 模式）
  *          接口：USART3 (PB10=TX -> 模块 RX, PB11=RX <- 模块 TX)，9600 8N1
  *
  *  手册要点：
  *   - 模块是**从机**：上电默认等待态，所有播放全由主机发起，自己从不主动发数据。
  *   - 串口为 **3.3V TTL**，与 F407 直连即可（手册说主机若为 5V 需串 1K 电阻）。
  *   - 帧格式（8.1 通信格式）：
  *         起始码(0xAA) + 指令类 + 数据长度n + 数据1 … 数据n + 校验SM
  *     校验 SM = (起始码 … 最后一个数据字节 的累加和) & 0xFF
  *     16 位数据高字节在前、低字节在后
  *   - 音频文件必须命名为 **5 位数字**（00001.mp3 … 65535.mp3），
  *     这个五位数就是 VOICE_Play() 的曲目号；用别的命名无法按曲目号播放。
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __VOICE_H
#define __VOICE_H

#include "main.h"

/* 音量范围：手册为 30 级 */
#define VOICE_VOL_MAX        30
#define VOICE_DEFAULT_VOL    20      /* 上电默认音量 */

/* 盘符（手册 8.3：查询返回按位区分 USB=BIT0 / SD=BIT1 / FLASH=BIT2；
   插播指令里的盘符字节按 0/1/2 传，模块板载 SPI-Flash 用 VOICE_DISC_FLASH） */
#define VOICE_DISC_USB       0
#define VOICE_DISC_SD        1
#define VOICE_DISC_FLASH     2

void VOICE_Init(void);                       /* 上电设置默认音量（须在调度器启动前调用也没问题） */

/* 播报：track = 音频文件名里的 5 位数字，1~65535 */
void VOICE_Play(uint16_t track);             /* 指定曲目播放（当前盘符） */
void VOICE_InsertPlay(uint16_t track, uint8_t disc);  /* 插播：播完自动回到插播点继续 */

/* 传输控制 */
void VOICE_Stop(void);
void VOICE_Pause(void);
void VOICE_PlayCurrent(void);                /* 从当前曲目开头播放 */

/* 盘符：指定曲目播的是"当前盘符"，拷了文件却对不上号时先切到正确盘符 */
void VOICE_SwitchDisc(uint8_t disc);         /* VOICE_DISC_USB / SD / FLASH */

/* 音量 */
void VOICE_SetVolume(uint8_t vol);           /* 0 ~ VOICE_VOL_MAX */
void VOICE_VolumeUp(void);
void VOICE_VolumeDown(void);

#endif /* __VOICE_H */
