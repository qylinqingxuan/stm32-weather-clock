#ifndef __RTC_H__
#define __RTC_H__

#include "stm32f10x.h"

typedef struct
{
    u16 year;
    u8  month;
    u8  day;
    u8  hour;
    u8  min;
    u8  sec;
    u8  week;
} DateTime;
u8   RTC_Config (void);
u8   RTC_IsValid (void);
void RTC_SetEpoch (u32 epoch);
u32  RTC_GetEpoch (void);
void RTC_DateTimeFromEpoch (u32 epoch, DateTime *dt);
void RTC_GetDateTime (DateTime *dt);

#endif
