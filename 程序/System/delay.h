#ifndef __DELAY_H
#define __DELAY_H
#include "stm32f10x.h"

void delay_init(u8 SYSCLK);
void delay_ms(u16 nms);
void delay_us(u32 nus);
void delay_dwt_init(void);
u32  delay_millis(void);
u32  delay_uptime(void);

#endif
