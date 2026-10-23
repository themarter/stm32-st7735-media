#ifndef OLED_STREAM_RX_H
#define OLED_STREAM_RX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_STREAM_VERSION 1u
#define OLED_STREAM_WIDTH 128u
#define OLED_STREAM_HEIGHT 64u
#define OLED_STREAM_FRAME_BYTES 1024u
#define OLED_STREAM_MAX_DATA_BYTES 256u
#define OLED_STREAM_MAX_ENCODED 300u

typedef void (*oled_stream_refresh_fn)(void *user);
typedef void (*oled_stream_uart_write_fn)(void *user, const uint8_t *bytes, size_t length);

typedef struct {
    uint8_t rx_encoded[OLED_STREAM_MAX_ENCODED];
    size_t rx_length;
    uint8_t *framebuffer;
    size_t framebuffer_capacity;
    size_t frame_bytes;
    uint16_t width;
    uint16_t height;
    uint16_t frame_id;
    uint16_t frame_delay_ms;
    uint16_t next_offset;
    uint8_t frame_active;
    uint8_t session_active;
    oled_stream_refresh_fn refresh;
    oled_stream_uart_write_fn uart_write;
    void *user;
} OLED_StreamReceiver;

/* framebuffer should be the existing OLED_TypeDef.pBuffer (1,024 bytes). */
void oled_stream_rx_init(OLED_StreamReceiver *receiver,
                         uint8_t *framebuffer,
                         size_t framebuffer_capacity,
                         uint16_t width,
                         uint16_t height,
                         oled_stream_refresh_fn refresh,
                         oled_stream_uart_write_fn uart_write,
                         void *user);

/* Feed arbitrary fragments from USART RX/DMA. Call from the main loop. */
void oled_stream_rx_feed(OLED_StreamReceiver *receiver,
                         const uint8_t *bytes,
                         size_t length);

#ifdef __cplusplus
}
#endif

#endif
