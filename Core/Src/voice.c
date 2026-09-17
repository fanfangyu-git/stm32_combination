/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    voice.c
  * @brief   JQ8900-16P 语音播报模块驱动（USART3，9600 8N1）
  *          帧：0xAA + 指令类 + 数据长度 + 数据… + 校验和
  *          校验和 = 前面所有字节之和的低 8 位（已用手册三个例子核对：
  *                   AA+07+02+00+08=BB  AA+13+01+14=D2  AA+16+03+00+00+09=CC）
  ******************************************************************************
  */
/* USER CODE END Header */
#include "voice.h"
#include "usart.h"

#define VOICE_START        0xAA    /* 起始码 */
#define VOICE_TX_TIMEOUT   100     /* 6 字节 @9600 约 6.3ms，100ms 余量充足 */

/* 指令类（手册 8.4 通信指令） */
#define VC_QUERY_STATE     0x01    /* 查询播放状态 -> 返回 AA 01 01 <状态> SM */
#define VC_PLAY            0x02    /* 播放当前曲目(从头) */
#define VC_PAUSE           0x03    /* 暂停 */
#define VC_STOP            0x04    /* 停止 */
#define VC_PREV            0x05    /* 上一曲 */
#define VC_NEXT            0x06    /* 下一曲 */
#define VC_PLAY_TRACK      0x07    /* 指定曲目：AA 07 02 <高> <低> SM */
#define VC_SET_VOLUME      0x13    /* 音量设置：AA 13 01 <VOL> SM */
#define VC_VOL_UP          0x14    /* 音量加 */
#define VC_VOL_DOWN        0x15    /* 音量减 */
#define VC_INSERT_TRACK    0x16    /* 指定曲目插播：AA 16 03 <盘符> <高> <低> SM */
#define VC_SWITCH_DISC     0x0B    /* 切换到指定盘符：AA 0B 01 <盘符> SM（0=U盘 1=TF卡 2=FLASH） */

/* 组帧并发送：payload 是「起始码 … 最后一个数据字节」，函数自动追加校验和。
   整帧一次发完，避免分两次 Transmit 造成字节间空隙。 */
static void voice_send(const uint8_t *payload, uint8_t len)
{
  uint8_t frame[8];
  uint8_t sum = 0;
  uint8_t i;

  if ((size_t)len + 1u > sizeof(frame)) return;    /* 本驱动的帧最长 7 字节 */

  for (i = 0; i < len; i++)
  {
    frame[i] = payload[i];
    sum += payload[i];
  }
  frame[len] = sum;                                 /* 校验和：前面所有字节之和的低 8 位 */

  HAL_UART_Transmit(&huart3, frame, (uint16_t)(len + 1), VOICE_TX_TIMEOUT);
}

/* 无参指令：AA <cmd> 00 SM */
static void voice_cmd0(uint8_t cmd)
{
  uint8_t p[3] = { VOICE_START, cmd, 0x00 };
  voice_send(p, 3);
}

/* ---------------- 对外 API ---------------- */

void VOICE_Init(void)
{
  /* 模块自己会记住音量，这里只是把它拉回一个合适的默认值。
     没接模块时也只是把几个字节发进空气，不会阻塞或报错。 */
  VOICE_SetVolume(VOICE_DEFAULT_VOL);
}

void VOICE_Play(uint16_t track)
{
  uint8_t p[5] = { VOICE_START, VC_PLAY_TRACK, 0x02,
                   (uint8_t)(track >> 8), (uint8_t)(track & 0xFF) };
  voice_send(p, 5);
}

void VOICE_InsertPlay(uint16_t track, uint8_t disc)
{
  uint8_t p[6] = { VOICE_START, VC_INSERT_TRACK, 0x03, disc,
                   (uint8_t)(track >> 8), (uint8_t)(track & 0xFF) };
  voice_send(p, 6);
}

/* 切换到指定盘符：0=U盘 1=TF卡 2=FLASH。
   手册说明：切换到目标盘符后等待播放，曲目定位到第 1 首、且处于停止态；
   建议切换前先查询当前盘符（查询要读模块回复，本驱动暂未做接收）。
   用途：07 指定曲目播的是**当前盘符**，拷了文件却"对不上号"时先切到正确盘符。 */
void VOICE_SwitchDisc(uint8_t disc)
{
  uint8_t p[4];

  if (disc > VOICE_DISC_FLASH) disc = VOICE_DISC_FLASH;

  p[0] = VOICE_START;
  p[1] = VC_SWITCH_DISC;
  p[2] = 0x01;                 /* 数据长度 = 1 */
  p[3] = disc;
  voice_send(p, 4);
}

void VOICE_Stop(void)        { voice_cmd0(VC_STOP); }
void VOICE_Pause(void)       { voice_cmd0(VC_PAUSE); }
void VOICE_PlayCurrent(void) { voice_cmd0(VC_PLAY); }
void VOICE_Prev(void)        { voice_cmd0(VC_PREV); }
void VOICE_Next(void)        { voice_cmd0(VC_NEXT); }
void VOICE_VolumeUp(void)    { voice_cmd0(VC_VOL_UP); }
void VOICE_VolumeDown(void)  { voice_cmd0(VC_VOL_DOWN); }

void VOICE_SetVolume(uint8_t vol)
{
  uint8_t p[4];

  if (vol > VOICE_VOL_MAX) vol = VOICE_VOL_MAX;

  p[0] = VOICE_START;
  p[1] = VC_SET_VOLUME;
  p[2] = 0x01;                 /* 数据长度 = 1 */
  p[3] = vol;
  voice_send(p, 4);
}
