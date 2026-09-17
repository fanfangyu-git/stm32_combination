/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    rc522.c
  * @brief   RC522 RFID 驱动 (硬件SPI1: PB3/PB4/PB5, 软件CS=PG7, RST=PB6)
  *          精简寄存器级实现：寻卡 + 防冲撞读UID
  ******************************************************************************
  */
/* USER CODE END Header */
#include "rc522.h"
#include "spi.h"
#include "gpio.h"

/* ---------- 引脚控制宏 ---------- */
#define RC522_CS_LOW()   HAL_GPIO_WritePin(RC522_CS_PORT, RC522_CS_PIN, GPIO_PIN_RESET)
#define RC522_CS_HIGH()  HAL_GPIO_WritePin(RC522_CS_PORT, RC522_CS_PIN, GPIO_PIN_SET)
#define RC522_RST_LOW()  HAL_GPIO_WritePin(RC522_RST_PORT, RC522_RST_PIN, GPIO_PIN_RESET)
#define RC522_RST_HIGH() HAL_GPIO_WritePin(RC522_RST_PORT, RC522_RST_PIN, GPIO_PIN_SET)

/* ---------- RC522 寄存器地址 ---------- */
#define CommandReg       0x01
#define ComIEnReg        0x02
#define ComIrqReg        0x04
#define ErrorReg         0x06
#define FIFODataReg      0x09
#define FIFOLevelReg     0x0A
#define ControlReg       0x0C
#define BitFramingReg    0x0D
#define ModeReg          0x11
#define TxControlReg     0x14
#define TxASKReg         0x15
#define TModeReg         0x2A
#define TPrescalerReg    0x2B
#define TReloadRegL      0x2D
#define TReloadRegH      0x2C
#define DivIrqReg        0x05
#define CRCResultRegL    0x22
#define CRCResultRegH    0x21

/* ---------- 命令字 ---------- */
#define PCD_IDLE         0x00
#define PCD_CALC_CRC     0x03
#define PCD_TRANSMIT     0x04
#define PCD_RECEIVE      0x08
#define PCD_TRANSCEIVE   0x0C
#define PCD_AUTHENT      0x0E   /* MFAuthent：Mifare 认证（RC522_ToCard 的 irq/wait 分支要用） */
#define PCD_RESETPHASE   0x0F

/* ---------- 底层位操作 ---------- */
static void RC522_SetBitMask(uint8_t reg, uint8_t mask)
{
  uint8_t tmp = RC522_ReadReg(reg);
  RC522_WriteReg(reg, tmp | mask);
}
static void RC522_ClearBitMask(uint8_t reg, uint8_t mask)
{
  uint8_t tmp = RC522_ReadReg(reg);
  RC522_WriteReg(reg, tmp & ~mask);
}

/* ---------- SPI 读写（地址格式: 写=(addr<<1)&0x7E, 读=(addr<<1)&0x7E|0x80） ---------- */
void RC522_WriteReg(uint8_t reg, uint8_t val)
{
  uint8_t addr = (reg << 1) & 0x7E;
  RC522_CS_LOW();
  HAL_SPI_Transmit(&hspi1, &addr, 1, 10);
  HAL_SPI_Transmit(&hspi1, &val, 1, 10);
  RC522_CS_HIGH();
}

uint8_t RC522_ReadReg(uint8_t reg)
{
  uint8_t addr = ((reg << 1) & 0x7E) | 0x80;
  uint8_t val = 0;
  RC522_CS_LOW();
  HAL_SPI_Transmit(&hspi1, &addr, 1, 10);
  HAL_SPI_Receive(&hspi1, &val, 1, 10);
  RC522_CS_HIGH();
  return val;
}

void RC522_AntennaOn(void)
{
  uint8_t tmp = RC522_ReadReg(TxControlReg);
  if (!(tmp & 0x03))
    RC522_SetBitMask(TxControlReg, 0x03);
}

/* ---------- 初始化 ---------- */
void RC522_Init(void)
{
  GPIO_InitTypeDef g = {0};

  /* 配置 CS(PG7) 与 RST(PB6) 为推挽输出 */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  g.Pin = RC522_CS_PIN;
  HAL_GPIO_Init(RC522_CS_PORT, &g);
  g.Pin = RC522_RST_PIN;
  HAL_GPIO_Init(RC522_RST_PORT, &g);
  RC522_CS_HIGH();

  /* RC522 最高支持 10MHz；SPI1 挂 APB2(84MHz)，16 分频 = 5.25MHz 更稳 */
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  HAL_SPI_Init(&hspi1);

  /* 硬件复位脉冲 */
  RC522_RST_HIGH();
  HAL_Delay(10);
  RC522_RST_LOW();
  HAL_Delay(10);
  RC522_RST_HIGH();
  HAL_Delay(10);

  /* 复位并配置定时器/天线 */
  RC522_WriteReg(CommandReg, PCD_RESETPHASE);
  HAL_Delay(5);
  RC522_WriteReg(TModeReg, 0x8D);
  RC522_WriteReg(TPrescalerReg, 0x3E);
  RC522_WriteReg(TReloadRegL, 0x1E);
  RC522_WriteReg(TReloadRegH, 0x00);
  RC522_WriteReg(TxASKReg, 0x40);
  RC522_WriteReg(ModeReg, 0x3D);
  RC522_AntennaOn();
}

/* ---------- 与卡片收发 ---------- */
static uint8_t RC522_ToCard(uint8_t cmd, uint8_t *send, uint8_t send_len,
                             uint8_t *back, uint8_t *back_len)
{
  uint8_t irqEn = 0x00, waitIRq = 0x00;
  uint8_t lastBits = 0, n = 0, i = 0;
  uint16_t timeout = 0;                 /* 必须 16 位：2000 塞进 uint8_t 会截断成 208 */
  uint8_t status = MI_ERR;

  switch (cmd)
  {
    case PCD_AUTHENT:     irqEn = 0x12; waitIRq = 0x10; break;
    case PCD_TRANSCEIVE:  irqEn = 0x77; waitIRq = 0x30; break;
  }

  RC522_WriteReg(ComIEnReg, irqEn | 0x80);
  RC522_ClearBitMask(ComIrqReg, 0x80);
  RC522_SetBitMask(FIFOLevelReg, 0x80);
  RC522_WriteReg(CommandReg, PCD_IDLE);

  for (i = 0; i < send_len; i++)
    RC522_WriteReg(FIFODataReg, send[i]);

  RC522_WriteReg(CommandReg, cmd);
  if (cmd == PCD_TRANSCEIVE)
    RC522_SetBitMask(BitFramingReg, 0x80);

  timeout = 2000;
  do {
    n = RC522_ReadReg(ComIrqReg);
    timeout--;
  } while ((timeout != 0) && !(n & 0x01) && !(n & waitIRq));

  RC522_ClearBitMask(BitFramingReg, 0x80);

  if ((timeout != 0) && !(n & 0x01))
  {
    if ((RC522_ReadReg(ErrorReg) & 0x1B) == 0x00)
    {
      status = MI_OK;
      if (n & irqEn & 0x01)
        status = MI_NOTAGERR;
      if (cmd == PCD_TRANSCEIVE)
      {
        n = RC522_ReadReg(FIFOLevelReg);
        lastBits = RC522_ReadReg(ControlReg) & 0x07;
        if (lastBits)
          *back_len = (n - 1) * 8 + lastBits;
        else
          *back_len = n * 8;
        if (n == 0) n = 1;
        if (n > 16) n = 16;
        for (i = 0; i < n; i++)
          back[i] = RC522_ReadReg(FIFODataReg);
      }
    }
  }

  RC522_WriteReg(CommandReg, PCD_IDLE);
  return status;
}

uint8_t RC522_Request(uint8_t req_mode, uint8_t *tag_type)
{
  uint8_t status, back_bits;
  RC522_WriteReg(BitFramingReg, 0x07);  /* 仅发 7 bit */
  tag_type[0] = req_mode;
  status = RC522_ToCard(PCD_TRANSCEIVE, tag_type, 1, tag_type, &back_bits);
  if ((status != MI_OK) || (back_bits != 0x10))
    status = MI_ERR;
  return status;
}

uint8_t RC522_Anticoll(uint8_t *ser_num)
{
  uint8_t status, i, ser_num_check = 0, back_bits;
  uint8_t buf[2];

  RC522_WriteReg(BitFramingReg, 0x00);
  buf[0] = PICC_ANTICOLL;
  buf[1] = 0x20;
  status = RC522_ToCard(PCD_TRANSCEIVE, buf, 2, ser_num, &back_bits);
  if (status == MI_OK)
  {
    for (i = 0; i < 4; i++)
      ser_num_check ^= ser_num[i];
    if (ser_num_check != ser_num[4])
      status = MI_ERR;
  }
  return status;
}

void RC522_CalcCRC(uint8_t *pindata, uint8_t len, uint8_t *poutdata)
{
  RC522_ClearBitMask(DivIrqReg, 0x04);
  RC522_SetBitMask(FIFOLevelReg, 0x80);
  RC522_WriteReg(CommandReg, PCD_IDLE);
  for (uint8_t i = 0; i < len; i++)
    RC522_WriteReg(FIFODataReg, pindata[i]);
  RC522_WriteReg(CommandReg, PCD_CALC_CRC);
  uint8_t i = 0xFF, n;
  do {
    n = RC522_ReadReg(DivIrqReg);
    i--;
  } while ((i != 0) && !(n & 0x04));
  poutdata[0] = RC522_ReadReg(CRCResultRegL);
  poutdata[1] = RC522_ReadReg(CRCResultRegH);
}

void RC522_Halt(void)
{
  uint8_t buf[4], back_bits;
  buf[0] = PICC_HALT;
  buf[1] = 0;
  RC522_CalcCRC(buf, 2, &buf[2]);
  RC522_ToCard(PCD_TRANSCEIVE, buf, 4, buf, &back_bits);
}

/* ---------- 便捷接口 ---------- */
uint8_t RC522_Check(uint8_t *uid4, uint8_t *uid_len)
{
  uint8_t buf[5];

  if (RC522_Request(PICC_REQIDL, buf) != MI_OK)
    return 1;
  if (RC522_Anticoll(buf) != MI_OK)
    return 1;

  for (uint8_t i = 0; i < 4; i++)
    uid4[i] = buf[i];
  *uid_len = 4;
  RC522_Halt();
  return 0;
}
