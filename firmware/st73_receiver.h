#ifndef ST73_RECEIVER_H
#define ST73_RECEIVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST73_PROTOCOL_VERSION 1u
#define ST73_MAX_TILE_SIZE 16u
#define ST73_MAX_TILE_BYTES (ST73_MAX_TILE_SIZE * ST73_MAX_TILE_SIZE * 2u)
#define ST73_MAX_ENCODED_PACKET 1900u

typedef void (*st73_draw_tile_fn)(
    void *user,
    uint16_t x, uint16_t y,
    uint16_t width, uint16_t height,
    const uint8_t *rgb565_be);

typedef void (*st73_set_orientation_fn)(void *user, uint8_t orientation);
typedef void (*st73_uart_write_fn)(void *user, const uint8_t *data, size_t len);

typedef struct {
    /* Scratch buffers live in receiver state to keep embedded stack use small. */
    uint8_t rx_encoded[ST73_MAX_ENCODED_PACKET];
    uint8_t tile_pixels[ST73_MAX_TILE_BYTES];
    size_t rx_len;

    uint16_t panel_width;
    uint16_t panel_height;

    uint8_t frame_active;
    uint8_t orientation;
    uint16_t frame_id;
    uint16_t frame_width;
    uint16_t frame_height;
    uint16_t frame_delay_ms;
    uint16_t expected_tiles;
    uint16_t received_tiles;
    uint16_t expected_chunk;

    st73_draw_tile_fn draw_tile;
    st73_set_orientation_fn set_orientation;
    st73_uart_write_fn uart_write;
    void *user;
} ST73_Receiver;

/* panel_width/height are the native logical dimensions at orientation 0. */
void st73_receiver_init(
    ST73_Receiver *receiver,
    uint16_t panel_width,
    uint16_t panel_height,
    st73_draw_tile_fn draw_tile,
    st73_set_orientation_fn set_orientation,
    st73_uart_write_fn uart_write,
    void *user);

/* Feed arbitrary UART byte fragments; zero-byte COBS delimiters are parsed internally. */
void st73_receiver_feed(ST73_Receiver *receiver, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
