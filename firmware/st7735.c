#include "st7735.h"
#include "delay.h"

/* 软件 SPI mode 0：CLK 空闲低，上升沿采样。
 * 直接写 BSRR/BRR，避免 GPIO_SetBits 之类的函数调用开销，
 * 72 MHz 下一个字节约 1 µs。 */

#define LCD_BUS   GPIOB
#define LCD_BUS_A GPIOA

#define CLK_PIN   GPIO_Pin_10
#define DIN_PIN   GPIO_Pin_11
#define DC_PIN    GPIO_Pin_0
#define RST_PIN   GPIO_Pin_1
#define CS_PIN    GPIO_Pin_7
#define BL_PIN    GPIO_Pin_6

#define PIN_H(port, pin) ((port)->BSRR = (pin))
#define PIN_L(port, pin) ((port)->BRR  = (pin))

static uint16_t g_width  = ST7735_WIDTH;
static uint16_t g_height = ST7735_HEIGHT;
static uint8_t  g_rotation = 0;
static uint8_t  g_streaming = 0;

static void LCD_WriteByte(uint8_t value)
{
    uint8_t i;

    for (i = 0u; i < 8u; ++i) {
        PIN_L(LCD_BUS, CLK_PIN);                 /* CLK 低：准备数据 */
        if ((value & 0x80u) != 0u) {
            PIN_H(LCD_BUS, DIN_PIN);
        } else {
            PIN_L(LCD_BUS, DIN_PIN);
        }
        PIN_H(LCD_BUS, CLK_PIN);                 /* CLK 高：ST7735 在上升沿采样 */
        value = (uint8_t)(value << 1);
    }
    PIN_L(LCD_BUS, CLK_PIN);
}

static void LCD_Command(uint8_t cmd)
{
    PIN_L(LCD_BUS_A, CS_PIN);
    PIN_L(LCD_BUS, DC_PIN);
    LCD_WriteByte(cmd);
    PIN_H(LCD_BUS_A, CS_PIN);
}

static void LCD_Data8(uint8_t data)
{
    PIN_L(LCD_BUS_A, CS_PIN);
    PIN_H(LCD_BUS, DC_PIN);
    LCD_WriteByte(data);
    PIN_H(LCD_BUS_A, CS_PIN);
}

static void LCD_DataArray(const uint8_t *data, uint16_t count)
{
    uint16_t i;

    PIN_L(LCD_BUS_A, CS_PIN);
    PIN_H(LCD_BUS, DC_PIN);
    for (i = 0u; i < count; ++i) {
        LCD_WriteByte(data[i]);
    }
    PIN_H(LCD_BUS_A, CS_PIN);
}

static void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    LCD_Command(0x2A);
    PIN_L(LCD_BUS_A, CS_PIN);
    PIN_H(LCD_BUS, DC_PIN);
    LCD_WriteByte((uint8_t)(x0 >> 8));
    LCD_WriteByte((uint8_t)x0);
    LCD_WriteByte((uint8_t)(x1 >> 8));
    LCD_WriteByte((uint8_t)x1);
    PIN_H(LCD_BUS_A, CS_PIN);

    LCD_Command(0x2B);
    PIN_L(LCD_BUS_A, CS_PIN);
    PIN_H(LCD_BUS, DC_PIN);
    LCD_WriteByte((uint8_t)(y0 >> 8));
    LCD_WriteByte((uint8_t)y0);
    LCD_WriteByte((uint8_t)(y1 >> 8));
    LCD_WriteByte((uint8_t)y1);
    PIN_H(LCD_BUS_A, CS_PIN);

    LCD_Command(0x2C);                           /* RAMWR */
}

static void LCD_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Pin   = CLK_PIN | DIN_PIN | DC_PIN | RST_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_BUS, &gpio);

    gpio.GPIO_Pin   = CS_PIN | BL_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_BUS_A, &gpio);

    PIN_H(LCD_BUS_A, CS_PIN);
    PIN_L(LCD_BUS, CLK_PIN);
    PIN_H(LCD_BUS_A, BL_PIN);
    PIN_H(LCD_BUS, RST_PIN);
    PIN_L(LCD_BUS, DC_PIN);
}

void ST7735_SetRotation(uint8_t rotation)
{
    static const uint8_t madctl[4] = {0xC8, 0xA8, 0x08, 0x68};

    g_rotation = (uint8_t)(rotation & 3u);
    g_width    = (g_rotation & 1u) ? ST7735_HEIGHT : ST7735_WIDTH;
    g_height   = (g_rotation & 1u) ? ST7735_WIDTH  : ST7735_HEIGHT;

    if (g_streaming != 0u) {
        ST7735_EndFrame();
    }
    LCD_Command(0x36);
    LCD_Data8(madctl[g_rotation]);
}

uint16_t ST7735_GetWidth(void)  { return g_width;  }
uint16_t ST7735_GetHeight(void) { return g_height; }

void ST7735_Init(void)
{
    static const uint8_t frm1[]   = {0x01, 0x2C, 0x2D};
    static const uint8_t frm2[]   = {0x01, 0x2C, 0x2D};
    static const uint8_t frm3[]   = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    static const uint8_t pwctr1[] = {0xA2, 0x02, 0x84};
    static const uint8_t pwctr3[] = {0x0A, 0x00};
    static const uint8_t pwctr4[] = {0x8A, 0x2A};
    static const uint8_t pwctr5[] = {0x8A, 0xEE};
    static const uint8_t gmctrp[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                                     0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    static const uint8_t gmctrn[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                                     0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};

    LCD_GPIO_Init();

    PIN_L(LCD_BUS, RST_PIN); Delay(20);
    PIN_H(LCD_BUS, RST_PIN); Delay(120);

    LCD_Command(0x01); Delay(150);                 /* SWRESET，模组差异大，给足时间 */
    LCD_Command(0x11); Delay(120);                 /* SLPOUT */

    LCD_Command(0xB1); LCD_DataArray(frm1, sizeof(frm1));
    LCD_Command(0xB2); LCD_DataArray(frm2, sizeof(frm2));
    LCD_Command(0xB3); LCD_DataArray(frm3, sizeof(frm3));
    LCD_Command(0xB4); LCD_Data8(0x07);            /* INVCTR */
    LCD_Command(0xC0); LCD_DataArray(pwctr1, sizeof(pwctr1));
    LCD_Command(0xC1); LCD_Data8(0xC5);
    LCD_Command(0xC2); LCD_DataArray(pwctr3, sizeof(pwctr3));
    LCD_Command(0xC3); LCD_DataArray(pwctr4, sizeof(pwctr4));
    LCD_Command(0xC4); LCD_DataArray(pwctr5, sizeof(pwctr5));
    LCD_Command(0xC5); LCD_Data8(0x0E);            /* VMCTR2，VCOM 电压 */
    LCD_Command(0x20);                             /* INVOFF */
    LCD_Command(0x3A); LCD_Data8(0x05);            /* COLMOD = 16 bit */
    LCD_Command(0xE0); LCD_DataArray(gmctrp, sizeof(gmctrp));
    LCD_Command(0xE1); LCD_DataArray(gmctrn, sizeof(gmctrn));

    ST7735_SetRotation(0);

    LCD_Command(0x13); Delay(10);                  /* NORON */
    LCD_Command(0x29); Delay(100);                 /* DISPON */

    ST7735_FillScreen(ST7735_BLACK);
}

uint8_t ST7735_BeginFrame(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (w == 0u || h == 0u) {
        return 0u;
    }
    if ((uint32_t)x + w > g_width || (uint32_t)y + h > g_height) {
        return 0u;
    }

    LCD_SetWindow(x, y, (uint16_t)(x + w - 1u), (uint16_t)(y + h - 1u));

    PIN_L(LCD_BUS_A, CS_PIN);
    PIN_H(LCD_BUS, DC_PIN);
    g_streaming = 1u;
    return 1u;
}

void ST7735_WriteBytes(const uint8_t *data, uint16_t count)
{
    uint16_t i;

    if (data == 0 || g_streaming == 0u) {
        return;
    }
    for (i = 0u; i < count; ++i) {
        LCD_WriteByte(data[i]);
    }
}

void ST7735_WritePixel(uint16_t color)
{
    if (g_streaming == 0u) {
        return;
    }
    LCD_WriteByte((uint8_t)(color >> 8));
    LCD_WriteByte((uint8_t)color);
}

void ST7735_EndFrame(void)
{
    if (g_streaming == 0u) {
        return;
    }
    PIN_H(LCD_BUS_A, CS_PIN);
    g_streaming = 0u;
}

void ST7735_FillScreen(uint16_t color)
{
    uint32_t n = (uint32_t)g_width * g_height;

    if (ST7735_BeginFrame(0, 0, g_width, g_height) == 0u) {
        return;
    }
    while (n--) {
        LCD_WriteByte((uint8_t)(color >> 8));
        LCD_WriteByte((uint8_t)color);
    }
    ST7735_EndFrame();
}

void ST7735_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t n;

    if (x >= g_width || y >= g_height || w == 0u || h == 0u) {
        return;
    }
    if ((uint32_t)x + w > g_width)  { w = (uint16_t)(g_width - x);  }
    if ((uint32_t)y + h > g_height) { h = (uint16_t)(g_height - y); }

    if (ST7735_BeginFrame(x, y, w, h) == 0u) {
        return;
    }
    n = (uint32_t)w * h;
    while (n--) {
        LCD_WriteByte((uint8_t)(color >> 8));
        LCD_WriteByte((uint8_t)color);
    }
    ST7735_EndFrame();
}

void ST7735_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= g_width || y >= g_height) {
        return;
    }
    if (ST7735_BeginFrame(x, y, 1u, 1u) == 0u) {
        return;
    }
    LCD_WriteByte((uint8_t)(color >> 8));
    LCD_WriteByte((uint8_t)color);
    ST7735_EndFrame();
}
