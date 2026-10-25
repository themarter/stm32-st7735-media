/* ST73-Lite 接收器自测：不依赖 STM32 硬件，用 gcc 跑。
 *
 *   gcc -std=c99 -Wall -Wextra -Werror -pedantic firmware/st73_receiver.c firmware/test_receiver.c -o st73_test
 *   ./st73_test
 */

#include <stdio.h>
#include <string.h>

#include "st73_receiver.h"

#define BUF_MAX 8192u

static uint8_t  g_pixels[BUF_MAX];
static uint32_t g_pixel_len;
static uint16_t g_begin_w;
static uint16_t g_begin_h;
static uint8_t  g_begin_ori;
static uint8_t  g_accept = 1u;
static int      g_begin_calls;
static int      g_end_calls;
static int      g_reply_calls;
static uint8_t  g_last_status;
static int      g_failures;

static uint8_t on_begin(void *user, uint16_t w, uint16_t h, uint8_t orientation)
{
    (void)user;
    ++g_begin_calls;
    g_begin_w = w;
    g_begin_h = h;
    g_begin_ori = orientation;
    return g_accept;
}

static void on_write(void *user, const uint8_t *data, uint16_t length)
{
    (void)user;
    if (g_pixel_len + length <= BUF_MAX) {
        memcpy(g_pixels + g_pixel_len, data, length);
        g_pixel_len += length;
    }
    if ((length & 1u) != 0u) {
        printf("  !! write 长度不是偶数：%u\n", (unsigned)length);
        ++g_failures;
    }
}

static void on_end(void *user)
{
    (void)user;
    ++g_end_calls;
}

static void on_reply(void *user, uint8_t status)
{
    (void)user;
    ++g_reply_calls;
    g_last_status = status;
}

static void reset_state(void)
{
    g_pixel_len   = 0u;
    g_begin_w     = 0u;
    g_begin_h     = 0u;
    g_begin_ori   = 0u;
    g_accept      = 1u;
    g_begin_calls = 0;
    g_end_calls   = 0;
    g_reply_calls = 0;
    g_last_status = 0xFFu;
}

static size_t build_frame(uint8_t *out, uint16_t w, uint16_t h, uint8_t ori, uint8_t sum_delta)
{
    size_t  i;
    uint8_t sum = 0u;
    size_t  total = (size_t)ST73_HEADER_SIZE + (size_t)w * h * 2u;

    out[0] = ST73_SYNC0;
    out[1] = ST73_SYNC1;
    out[2] = ST73_CMD_IMAGE;
    out[3] = (uint8_t)(w & 0xFFu);
    out[4] = (uint8_t)(w >> 8);
    out[5] = (uint8_t)(h & 0xFFu);
    out[6] = (uint8_t)(h >> 8);
    out[7] = ori;

    for (i = 0u; i < ST73_HEADER_SIZE - 1u; ++i) {
        sum = (uint8_t)(sum + out[i]);
    }
    out[8] = (uint8_t)(sum + sum_delta);

    /* 让 0xA5 0x5A 周期性地出现在像素流里，验证不会被误当成帧头 */
    for (i = 0u; i < (size_t)w * h; ++i) {
        uint16_t v = (uint16_t)(i * 37u);
        if ((i % 97u) == 0u) {
            v = 0xA55Au;
        }
        out[ST73_HEADER_SIZE + i * 2u]      = (uint8_t)(v >> 8);
        out[ST73_HEADER_SIZE + i * 2u + 1u] = (uint8_t)(v & 0xFFu);
    }
    return total;
}

static void expect(int condition, const char *what)
{
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

static void case_basic(void)
{
    ST73_Stream stream;
    uint8_t frame[ST73_HEADER_SIZE + 8u * 6u * 2u];
    size_t len;

    printf("1. 整帧一次喂入（8x6）\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    len = build_frame(frame, 8u, 6u, 1u, 0u);
    st73_stream_feed(&stream, frame, len);

    expect(g_begin_calls == 1, "begin 调用一次");
    expect(g_begin_w == 8u && g_begin_h == 6u && g_begin_ori == 1u, "尺寸与方向正确");
    expect(g_pixel_len == 8u * 6u * 2u, "像素字节数 = W*H*2");
    expect(g_end_calls == 1, "end 调用一次");
    expect(g_reply_calls == 1 && g_last_status == ST73_STATUS_OK, "回执 OK");
    expect(st73_stream_busy(&stream) == 0u, "收完不再 busy");
}

static void case_byte_by_byte(void)
{
    ST73_Stream stream;
    uint8_t frame[ST73_HEADER_SIZE + 16u * 4u * 2u];
    size_t len;
    size_t i;

    printf("2. 逐字节喂入（16x4，模拟串口一块一块来）\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    len = build_frame(frame, 16u, 4u, 0u, 0u);
    for (i = 0u; i < len; ++i) {
        st73_stream_feed(&stream, &frame[i], 1u);
    }

    expect(g_begin_calls == 1, "begin 调用一次");
    expect(g_pixel_len == 16u * 4u * 2u, "像素字节数正确");
    expect(g_end_calls == 1, "end 调用一次");
    expect(g_last_status == ST73_STATUS_OK, "回执 OK");
    expect((size_t)g_begin_w * g_begin_h * 2u == g_pixel_len, "W*H*2 与实收一致");
}

static void case_noise(void)
{
    ST73_Stream stream;
    uint8_t buf[64 + ST73_HEADER_SIZE + 4u * 4u * 2u];
    size_t len;
    size_t noise = 0u;

    printf("3. 前置噪声里带孤立的 0xA5\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);

    buf[noise++] = 0x00u;
    buf[noise++] = ST73_SYNC0;      /* 假同步字，后面不是 0x5A */
    buf[noise++] = 0x11u;
    buf[noise++] = ST73_SYNC0;
    buf[noise++] = ST73_SYNC0;
    buf[noise++] = 0xFFu;
    buf[noise++] = ST73_SYNC0;      /* 真同步字前再撒一个 0xA5 */
    buf[noise++] = ST73_SYNC0;

    len = build_frame(buf + noise, 4u, 4u, 2u, 0u);
    st73_stream_feed(&stream, buf, noise + len);

    expect(g_begin_calls == 1, "只识别出一帧");
    expect(g_pixel_len == 4u * 4u * 2u, "像素字节数正确");
    expect(g_begin_ori == 2u, "方向 = 2");
    expect(g_last_status == ST73_STATUS_OK, "回执 OK");
}

static void case_bad_sum(void)
{
    ST73_Stream stream;
    uint8_t frame[ST73_HEADER_SIZE + 4u * 4u * 2u];
    size_t len;

    printf("4. 校验和错误\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    len = build_frame(frame, 4u, 4u, 0u, 0x5Au);
    st73_stream_feed(&stream, frame, len);

    expect(g_begin_calls == 0, "不开始帧");
    expect(g_reply_calls == 0, "不回执");
    expect(st73_stream_busy(&stream) == 0u, "仍在找同步字");
}

static void case_rejected_size(void)
{
    ST73_Stream stream;
    uint8_t frame[ST73_HEADER_SIZE + 32u * 32u * 2u];
    size_t len;

    printf("5. 尺寸超出面板，驱动拒收\n");
    reset_state();
    g_accept = 0u;
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    len = build_frame(frame, 32u, 32u, 0u, 0u);
    st73_stream_feed(&stream, frame, len);

    expect(g_begin_calls == 1, "begin 被调用");
    expect(g_pixel_len == 0u, "像素被丢弃，没有画");
    expect(g_end_calls == 0, "不调 end");
    expect(g_last_status == ST73_STATUS_BAD_SIZE, "回执 BAD_SIZE");
    expect(st73_stream_busy(&stream) == 0u, "整帧吞完，回到空闲");
}

static void case_two_frames(void)
{
    ST73_Stream stream;
    uint8_t frame[ST73_HEADER_SIZE + 4u * 4u * 2u];
    size_t len;

    printf("6. 连续两帧\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    len = build_frame(frame, 4u, 4u, 0u, 0u);
    st73_stream_feed(&stream, frame, len);
    st73_stream_feed(&stream, frame, len);

    expect(g_begin_calls == 2, "begin 两次");
    expect(g_end_calls == 2, "end 两次");
    expect(g_pixel_len == 2u * 4u * 4u * 2u, "像素累计正确");
    expect(g_reply_calls == 2, "回执两次");
}

static void case_abandon_frame(void)
{
    ST73_Stream stream;
    uint8_t frame[ST73_HEADER_SIZE + 16u * 16u * 2u];
    size_t len;

    printf("7. 半帧后 reset，再收新帧\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    len = build_frame(frame, 16u, 16u, 0u, 0u);
    st73_stream_feed(&stream, frame, len / 2u);
    expect(st73_stream_busy(&stream) == 1u, "半帧时 busy");

    st73_stream_reset(&stream);
    expect(st73_stream_busy(&stream) == 0u, "reset 后空闲");

    g_pixel_len   = 0u;
    g_begin_calls = 0;
    len = build_frame(frame, 4u, 4u, 0u, 0u);
    st73_stream_feed(&stream, frame, len);
    expect(g_begin_calls == 1, "新帧被识别");
    expect(g_pixel_len == 4u * 4u * 2u, "新帧像素完整");
}

static void case_pixel_content(void)
{
    ST73_Stream stream;
    uint8_t frame[ST73_HEADER_SIZE + 8u * 8u * 2u];
    size_t len;
    size_t i;
    int mismatch = 0;

    printf("8. 像素内容逐字节一致\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    len = build_frame(frame, 8u, 8u, 0u, 0u);
    st73_stream_feed(&stream, frame, len);

    for (i = 0u; i < (size_t)8u * 8u * 2u; ++i) {
        if (g_pixels[i] != frame[ST73_HEADER_SIZE + i]) {
            ++mismatch;
        }
    }
    expect(mismatch == 0, "64 个像素无一字节错位");
}

static void case_ping(void)
{
    ST73_Stream stream;
    uint8_t ping[4] = { ST73_SYNC0, ST73_SYNC1, ST73_CMD_PING, ST73_PING_MAGIC };
    uint8_t frame[ST73_HEADER_SIZE + 8u * 8u * 2u];
    size_t  len;
    size_t  i;

    printf("9. PING 握手\n");
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    st73_stream_feed(&stream, ping, 4u);

    expect(g_begin_calls == 0, "不开始帧");
    expect(g_reply_calls == 1, "回一次");
    expect(g_last_status == ST73_STATUS_INFO, "回执是设备信息");
    expect(st73_stream_busy(&stream) == 0u, "握手后仍在找同步字");

    /* 握完手紧接着发图，不能因为 PING 而错位 */
    len = build_frame(frame, 8u, 8u, 0u, 0u);
    st73_stream_feed(&stream, ping, 4u);
    st73_stream_feed(&stream, frame, len);

    expect(g_reply_calls == 3, "PING + 图像各回一次");
    expect(g_begin_calls == 1, "图像正常开始");
    expect(g_pixel_len == 8u * 8u * 2u, "像素长度正确");

    for (i = 0u; i < (size_t)8u * 8u * 2u; ++i) {
        if (g_pixels[i] != frame[ST73_HEADER_SIZE + i]) {
            break;
        }
    }
    expect(i == (size_t)8u * 8u * 2u, "PING 没有吞掉图像的头");

    /* 3 字节的 A5 5A 00 不算 PING，像素里这种情况很常见 */
    reset_state();
    st73_stream_init(&stream, on_begin, on_write, on_end, on_reply, 0);
    {
        uint8_t not_ping[4] = { ST73_SYNC0, ST73_SYNC1, ST73_CMD_PING, 0x25u };
        st73_stream_feed(&stream, not_ping, 4u);
    }
    expect(g_reply_calls == 0, "A5 5A 00 25 不当成 PING");
    expect(g_begin_calls == 0, "也不当成图像");
}

int main(void)
{
    printf("ST73-Lite receiver test\n\n");

    case_basic();
    case_byte_by_byte();
    case_noise();
    case_bad_sum();
    case_rejected_size();
    case_two_frames();
    case_abandon_frame();
    case_pixel_content();
    case_ping();

    printf("\n%s（%d 个失败）\n", g_failures == 0 ? "全部通过" : "有失败用例", g_failures);
    return g_failures == 0 ? 0 : 1;
}
