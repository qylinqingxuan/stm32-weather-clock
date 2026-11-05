#ifndef __QDTFT_H
#define __QDTFT_H

void showimage_auto(const unsigned char *p);
void showimage_xy(const unsigned char *p, u16 x, u16 y);
void Gui_DrawWeatherIcon(u16 x, u16 y, u8 code);
void WeatherClock_UI(void);
void WeatherClock_RefreshWeather(void);
void WeatherClock_SetIndoor(void);
void WeatherClock_Tick(u32 epoch);
void WeatherClock_RowScroll(const char *old_s, const char *new_s, u8 t);
void WeatherClock_RowFirst(void);
void WeatherClock_PollRow(u32 epoch);
void WeatherClock_RowRefresh(void);
u16 Gui_ScrollTextWidth(const unsigned char *s);
void Gui_ScrollText(u16 x, u16 y, u16 w, u16 fc, u16 bc,
                    const unsigned char *s, u16 offset);
void Pet_DrawPatch(const unsigned char *p);
void Pet_Blink(void);
void Pet_Bob(void);
void Pet_Lift(void);
void Pet_Sleep(void);
void WeatherClock_PetPlay(u8 act);
void WeatherClock_PollPet(u32 epoch);
#endif
