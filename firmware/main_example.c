/* ST73-Lite 参考主程序：STM32F103C8 + ST7735
 *
 * LCD : CLK/SCL=PB10  DIO/SDA=PB11  DC=PB0  RES=PB1  CS=PA7  BL=PA6
 * 链路: USB-TTL CH340 -> USART1（TX=PA9, RX=PA10），921600 8-N-1
 *
 * 串口中断只负责把字节塞进环形队列；找帧头、写屏都在主循环里做，
 * 避免在中断里 bit-bang 几十毫秒。
 */

#include "stm32f10x.h"
#include "delay.h"
#include "st7735.h"
#include "st73_receiver.h"

#define LINK_BAUD      921600u
#define RX_SIZE        4096u
#define RX_MASK        (RX_SIZE - 1u)
#define RX_TIMEOUT_MS  3000u

static volatile uint8_t  g_rx[RX_SIZE];
static volatile uint16_t g_rx_head = 0u;
static volatile uint16_t g_rx_tail = 0u;

static ST73_Stream g_stream;
static uint32_t    g_last_rx_tick;

static void USART1_Init(uint32_t baudrate)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef uart;
    NVIC_InitTypeDef nvic;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_USART1, ENABLE);

    gpio.GPIO_Pin   = GPIO_Pin_9;                  /* TX */
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin  = GPIO_Pin_10;                  /* RX */
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&uart);
    uart.USART_BaudRate            = baudrate;
    uart.USART_WordLength          = USART_WordLength_8b;
    uart.USART_StopBits            = USART_StopBits_1;
    uart.USART_Parity              = USART_Parity_No;
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(USART1, &uart);

    nvic.NVIC_IRQChannel                   = USART1_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority        = 0;
    nvic.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic);

    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART1, ENABLE);
}

void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        uint8_t  byte = (uint8_t)USART_ReceiveData(USART1);
        uint16_t next = (uint16_t)((g_rx_head + 1u) & RX_MASK);

        if (next != g_rx_tail) {                   /* 满了就丢，不覆盖未读数据 */
            g_rx[g_rx_head] = byte;
            g_rx_head = next;
        }
    }
}

static uint8_t OnBegin(void *user, uint16_t w, uint16_t h, uint8_t orientation)
{
    (void)user;
    ST7735_SetRotation(orientation);
    return ST7735_BeginFrame(0u, 0u, w, h);
}

static void OnWrite(void *user, const uint8_t *data, uint16_t length)
{
    (void)user;
    ST7735_WriteBytes(data, length);
}

static void OnEnd(void *user)
{
    (void)user;
    ST7735_EndFrame();
}

static void OnReply(void *user, uint8_t status)
{
    uint8_t frame[3];

    (void)user;
    frame[0] = ST73_SYNC1;
    frame[1] = ST73_SYNC0;
    frame[2] = status;
    USART_SendData(USART1, frame[0]);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) { }
    USART_SendData(USART1, frame[1]);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) { }
    USART_SendData(USART1, frame[2]);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) { }
}

static void DrainUart(void)
{
    uint8_t  chunk[256];
    uint16_t count;

    while (g_rx_tail != g_rx_head) {
        count = 0u;
        while (count < sizeof(chunk) && g_rx_tail != g_rx_head) {
            chunk[count] = g_rx[g_rx_tail];
            ++count;
            g_rx_tail = (uint16_t)((g_rx_tail + 1u) & RX_MASK);
        }
        g_last_rx_tick = GetTick();
        st73_stream_feed(&g_stream, chunk, count);
    }
}

/* 上电图案：能在不看串口的情况下判断「驱动 + 接线」是否正常。
 * 看到这个图说明 DIO/CLK/DC/RES/CS 都对，接下来的问题就只可能在串口或协议。 */
static void StartupPattern(void)
{
    uint16_t w = ST7735_GetWidth();
    uint16_t h = ST7735_GetHeight();

    ST7735_FillScreen(ST7735_BLACK);
    ST7735_FillRect(0u,        0u,        16u, 16u, ST7735_RED);
    ST7735_FillRect((uint16_t)(w - 16u), 0u, 16u, 16u, ST7735_GREEN);
    ST7735_FillRect(0u, (uint16_t)(h - 16u), 16u, 16u, ST7735_BLUE);
    ST7735_FillRect((uint16_t)(w - 16u), (uint16_t)(h - 16u), 16u, 16u, ST7735_YELLOW);

    ST7735_FillRect((uint16_t)(w / 2u - 1u), 0u, 2u, h, ST7735_WHITE);
    ST7735_FillRect(0u, (uint16_t)(h / 2u - 1u), w, 2u, ST7735_WHITE);
}

int main(void)
{
    Delay_Init();
    ST7735_Init();
    USART1_Init(LINK_BAUD);

    st73_stream_init(&g_stream, OnBegin, OnWrite, OnEnd, OnReply, 0);
    StartupPattern();

    g_last_rx_tick = GetTick();

    while (1) {
        DrainUart();

        /* 网页发一半断开时，状态机会卡在等像素。
         * 静默超过 3 秒就丢掉这一帧，回到找同步字。 */
        if (st73_stream_busy(&g_stream) != 0u &&
            (uint32_t)(GetTick() - g_last_rx_tick) > RX_TIMEOUT_MS) {
            ST7735_EndFrame();
            st73_stream_reset(&g_stream);
        }
    }
}
