#ifndef __BOOT_SCREEN_H
#define __BOOT_SCREEN_H

#include "stm32f10x.h"

#define BOOT_STEP_RTC        0
#define BOOT_STEP_ESP_AT     1
#define BOOT_STEP_ESP_RESET  2
#define BOOT_STEP_ESP_MODE   3
#define BOOT_STEP_ESP_JOIN   4
#define BOOT_STEP_ESP_LINK   5
#define BOOT_STEP_NTP        6
#define BOOT_STEP_WEATHER    7
#define BOOT_STEP_DHT11      8
#define BOOT_STEP_UI         9
#define BOOT_STEP_NUM        10

void Boot_Screen_Init(void);
void Boot_Screen_Step(u8 idx);
void Boot_Screen_Done(void);
void Boot_Screen_Error(u8 code);

#endif
