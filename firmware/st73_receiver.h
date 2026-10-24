#ifndef ST73_RECEIVER_H
#define ST73_RECEIVER_H

/* ST73-Lite v2 —— 极简单图流式接收器。
 *
 * 线上只有一种东西：9 字节帧头 + W*H*2 字节 RGB565。
 * 接收器不做整帧缓冲，像素字节攒够一批就直接交给 write 回调，
 * 由驱动按 DIO/CLK 时序推给 ST7735。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST73_SYNC0           0xA5u
#define ST73_SYNC1           0x5Au
#define ST73_CMD_IMAGE       0x01u
#define ST73_HEADER_SIZE     9u

/* 回执状态 */
#define ST73_STATUS_OK       0x00u
#define ST73_STATUS_BAD_SIZE 0x01u
#define ST73_STATUS_ABORTED  0x02u

/* 开始一帧：驱动在这里设方向、设窗口，并保持 CS 低、DC 高。
 * 返回 1 表示接受这一帧；返回 0 表示尺寸不合法，随后的像素会被丢弃。 */
typedef uint8_t (*st73_begin_fn)(void *user, uint16_t width, uint16_t height, uint8_t orientation);
/* 连续像素数据，length 一定是偶数，高字节在前。 */
typedef void (*st73_write_fn)(void *user, const uint8_t *rgb565_be, uint16_t length);
/* 结束一帧：拉高 CS。 */
typedef void (*st73_end_fn)(void *user);
/* 回 3 字节状态：0x5A 0xA5 status。允许传 NULL。 */
typedef void (*st73_reply_fn)(void *user, uint8_t status);

typedef struct {
    uint8_t  header[ST73_HEADER_SIZE];
    uint8_t  header_len;

    uint8_t  active;        /* 1 = 正在收像素 */
    uint8_t  discard;       /* 1 = 帧头不接受，像素只丢不画 */
    uint8_t  half_valid;    /* 1 = 存着上一批末尾落单的那个字节 */
    uint8_t  half_byte;

    uint16_t width;
    uint16_t height;
    uint8_t  orientation;
    uint32_t remaining;     /* 还差多少像素字节 */

    st73_begin_fn begin;
    st73_write_fn write;
    st73_end_fn   end;
    st73_reply_fn reply;
    void *user;
} ST73_Stream;

void st73_stream_init(
    ST73_Stream *stream,
    st73_begin_fn begin,
    st73_write_fn write,
    st73_end_fn end,
    st73_reply_fn reply,
    void *user);

/* 喂任意分片的串口字节；内部自行找同步字。 */
void st73_stream_feed(ST73_Stream *stream, const uint8_t *data, size_t length);

/* 上电或异常时清状态，避免半帧残留。 */
void st73_stream_reset(ST73_Stream *stream);

/* 是否正在接收一帧，可以用来点 LED。 */
uint8_t st73_stream_busy(const ST73_Stream *stream);

#ifdef __cplusplus
}
#endif

#endif
