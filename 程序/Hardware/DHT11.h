#ifndef __DHT11_H
#define __DHT11_H

#include "stm32f10x.h"

#define DHT11_PORT  GPIOA
#define DHT11_RCC   RCC_APB2Periph_GPIOA
#define DHT11_IO    GPIO_Pin_6

void DHT11_IO_OUT(void);
void DHT11_IO_IN(void);
void DHT11_RST(void);
u8 DHT11_Check(void);
u8 DHT11_Init(void);
u8 DHT11_Read_Bit(void);
u8 DHT11_Read_Byte(void);

typedef struct {
    u8 temp_i;
    u8 temp_d;
    u8 humi_i;
    u8 humi_d;
} DHT11_TH;

u8 DHT11_Read_Data(DHT11_TH *out);

#define DHT11_BUSY   0
#define DHT11_OK     1
#define DHT11_NORES  2
#define DHT11_CRC    3

void DHT11_Start(void);
u8   DHT11_Poll(DHT11_TH *out);


#endif
