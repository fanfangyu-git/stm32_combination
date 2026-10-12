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

/* 音量/模式帧的发送超时：4~6 字节 @9600 约 6.3ms，100ms 余量充足 */
/* ---------------- 播放模式（手册 8.3「播放模式定义」/ 8.4「设置循环模式(18)」）----------------
   模块会把播放模式**存在自己内部**：如果它是「全盘循环(00)」或「目录循环(04)」，
   发一条「指定曲目(07)」播完当前这句后，它会**自己接着往下播下一个文件** ——
   表现就是「对应操作能播对，但播完不受控地继续播」。
   本驱动的对策：每次播报前显式把模式按回「单曲停止」，并先发一条停止清干净残留状态。 */
#define VOICE_MODE_ALL_LOOP      0x00   /* 全盘循环：按顺序播全盘，播完循环 */
#define VOICE_MODE_SINGLE_LOOP   0x01   /* 单曲循环：一直循环当前曲目 */
#define VOICE_MODE_SINGLE_STOP   0x02   /* 单曲停止：播完当前曲目一次就停 ← 我们要的 */
#define VOICE_MODE_ALL_RANDOM    0x03   /* 全盘随机 */
#define VOICE_MODE_DIR_LOOP      0x04   /* 目录循环：播完当前文件夹循环 */
#define VOICE_MODE_DIR_RANDOM    0x05   /* 目录随机 */
#define VOICE_MODE_DIR_SEQ_STOP  0x06   /* 目录顺序播放：播完当前文件夹停止 */
#define VOICE_MODE_ALL_SEQ_STOP  0x07   /* 顺序播放：按顺序播全盘，播完停止 */

/* 每次播报前要锁成的模式。
   ⚠ 手册自相矛盾：§8.3 的模式表写「单曲停止 = 02」，但 §8.4 的例子却写
     `AA 18 01 03 C6` 为「设置为单曲停止」。故这里做成可切换的常量：
     若实测播完**仍继续往下播**，把它改成 0x03 再试（也可用串口指令 VLOOP;3 在线试，
     不用重编）。VLOOP;<0..7> 可逐个试出你手上这颗模块的真值。 */
#define VOICE_ONESHOT_MODE       VOICE_MODE_SINGLE_STOP

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

/* 播放模式：上电锁成单曲停止，防止模块按自己存的循环模式"播完接着往下播" */
void VOICE_SetPlayMode(uint8_t mode);        /* VOICE_MODE_xxx */

#endif /* __VOICE_H */
