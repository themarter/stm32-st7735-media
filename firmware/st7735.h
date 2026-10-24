#ifndef __ST7735_H
#define __ST7735_H

#include "stm32f10x.h"

/* STM32F103C8 + ST7735（1.8" 128×160，软件 SPI）
 * CLK/SCL = PB10   DIO/SDA = PB11   DC = PB0   RES = PB1
 * CS      = PA7    BL      = PA6
 */

#define ST7735_WIDTH   128
#define ST7735_HEIGHT  160

#define ST7735_BLACK   0x0000
#define ST7735_WHITE   0xFFFF
#define ST7735_RED     0xF800
#define ST7735_GREEN   0x07E0
#define ST7735_BLUE    0x001F
#define ST7735_YELLOW  0xFFE0
#define ST7735_CYAN    0x07FF
#define ST7735_MAGENTA 0xF81F

void ST7735_Init(void);
void ST7735_SetRotation(uint8_t rotation);
uint16_t ST7735_GetWidth(void);
uint16_t ST7735_GetHeight(void);

void ST7735_FillScreen(uint16_t color);
void ST7735_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void ST7735_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/* 流式写屏：一次设窗口，然后连续灌像素。
 * 像素格式 RGB565，高字节在前，与串口协议一致。
 *
 *   if (ST7735_BeginFrame(0, 0, w, h)) {
 *       ... ST7735_WriteBytes(buf, len) 若干次 ...
 *       ST7735_EndFrame();
 *   }
 */
uint8_t ST7735_BeginFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void ST7735_WriteBytes(const uint8_t *data, uint16_t count);
void ST7735_WritePixel(uint16_t color);
void ST7735_EndFrame(void);

#endif
