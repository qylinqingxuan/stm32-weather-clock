#ifndef __GUI_H
#define __GUI_H

u16 Gui_BGR2RGB(u16 c);
void Gui_Circle(u16 X,u16 Y,u16 R,u16 fc);
void Gui_DrawLine(u16 x0, u16 y0,u16 x1, u16 y1,u16 Color);
void Gui_box(u16 x, u16 y, u16 w, u16 h,u16 bc);
void Gui_box2(u16 x,u16 y,u16 w,u16 h, u8 mode);
void DisplayButtonDown(u16 x1,u16 y1,u16 x2,u16 y2);
void DisplayButtonUp(u16 x1,u16 y1,u16 x2,u16 y2);
void Gui_DrawFont_GBK16(u16 x, u16 y, u16 fc, u16 bc, u8 *s);
void Gui_DrawFont_GBK24(u16 x, u16 y, u16 fc, u16 bc, u8 *s);
void Gui_DrawFont_Num32(u16 x, u16 y, u16 fc, u16 bc, u16 num);
void Gui_DrawFont_Num24(u16 x, u16 y, u16 fc, u16 bc, u16 num);
void Gui_DrawNum24String(u16 x, u16 y, u16 fc, u16 bc, const char *s);

void Gui_FillRegion(u16 x, u16 y, u16 w, u16 h);
void Gui_PushPixel(u16 c);
const unsigned char *Gui_Glyph16(const unsigned char *s, u8 *w, u8 *stride);

#endif
