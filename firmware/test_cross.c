/* 交叉验证：把网页真实产出的帧喂给固件的接收状态机
 *
 * 网页端和固件端各自都自测过，但那是各测各的。这个程序拿网页真正写进串口的
 * 字节流（frame.bin），一个字节不改地送进 st73_stream_feed，确认：
 *   1. 帧头能被识别、宽高/方向解析正确
 *   2. 像素字节数与内容逐字节一致，没有错位
 *   3. 结束回调与回执状态正确
 *   4. 前面掺垃圾字节也能重新同步
 *   5. 校验和坏掉 / 屏幕拒收时，像素被吞干净而不是错位
 *
 * frame.bin 怎么来：在网页上选一张图，用串口工具（或 Chrome 里打的桩）把真正
 * 写进串口的字节原样存成文件。128×160 一帧正好 40969 字节 = 9 字节头 + 40960。
 * 用法：./test_cross frame.bin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "st73_receiver.h"

static uint8_t *g_expect = NULL;   /* 期望的像素字节（不含 9 字节头） */
static uint32_t g_expect_len = 0;

static uint32_t g_written = 0;
static uint32_t g_mismatch = 0;
static int g_begin_calls = 0;
static int g_end_calls = 0;
static int g_reply_calls = 0;
static int g_reply_status = -1;
static uint16_t g_w = 0;
static uint16_t g_h = 0;
static uint8_t g_rot = 0;
static int g_accept = 1;           /* 模拟 BeginFrame 成功/失败 */

static uint8_t on_begin(void *user, uint16_t w, uint16_t h, uint8_t orientation)
{
    (void)user;
    g_w = w;
    g_h = h;
    g_rot = orientation;
    ++g_begin_calls;
    return (uint8_t)g_accept;
}

static void on_write(void *user, const uint8_t *data, uint16_t length)
{
    uint16_t i;

    (void)user;
    for (i = 0; i < length; ++i) {
        if (g_written < g_expect_len) {
            if (data[i] != g_expect[g_written]) {
                ++g_mismatch;
            }
        } else {
            ++g_mismatch;          /* 多出来的字节也算错 */
        }
        ++g_written;
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
    g_reply_status = (int)status;
}

static void stats_reset(void)
{
    g_written = 0;
    g_mismatch = 0;
    g_begin_calls = 0;
    g_end_calls = 0;
    g_reply_calls = 0;
    g_reply_status = -1;
    g_w = 0;
    g_h = 0;
    g_rot = 0;
    g_accept = 1;
}

static int g_fail = 0;

static void check(const char *what, int ok, const char *detail)
{
    printf("  %s %-46s %s\n", ok ? "ok  " : "FAIL", what, detail ? detail : "");
    if (!ok) {
        ++g_fail;
    }
}

static uint8_t *read_file(const char *path, uint32_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long  size;
    uint8_t *buf;

    if (f == NULL) {
        printf("cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (uint32_t)size;
    return buf;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "frame.bin";
    ST73_Stream s;
    uint8_t *buf;
    uint32_t len;
    uint32_t i;
    char detail[160];
    uint32_t expect_pixels;

    buf = read_file(path, &len);
    if (buf == NULL) {
        return 1;
    }
    if (len < ST73_HEADER_SIZE) {
        printf("file too short: %u\n", len);
        free(buf);
        return 1;
    }

    g_expect     = buf + ST73_HEADER_SIZE;
    g_expect_len = len - ST73_HEADER_SIZE;
    expect_pixels = (uint32_t)g_expect_len / 2u;

    printf("frame.bin: %u bytes (header %u + pixels %u)\n\n",
           len, (unsigned)ST73_HEADER_SIZE, g_expect_len);

    /* ---------- 1. 整块一次喂进去 ---------- */
    stats_reset();
    st73_stream_init(&s, on_begin, on_write, on_end, on_reply, NULL);
    st73_stream_feed(&s, buf, len);

    printf("[1] whole buffer in one feed\n");
    check("begin called once", g_begin_calls == 1, NULL);
    snprintf(detail, sizeof(detail), "w=%u h=%u rot=%u", g_w, g_h, g_rot);
    check("width/height/rotation", g_w == 128 && g_h == 160 && g_rot == 0, detail);
    snprintf(detail, sizeof(detail), "%u / %u, mismatch=%u",
             g_written, (unsigned)g_expect_len, g_mismatch);
    check("pixel bytes match header size", g_written == g_expect_len && g_mismatch == 0, detail);
    check("end called once", g_end_calls == 1, NULL);
    snprintf(detail, sizeof(detail), "status=0x%02x calls=%d", g_reply_status, g_reply_calls);
    check("reply OK", g_reply_calls == 1 && g_reply_status == ST73_STATUS_OK, detail);
    check("stream idle afterwards", st73_stream_busy(&s) == 0, NULL);

    /* ---------- 2. 逐字节喂（最坏的分包情况） ---------- */
    stats_reset();
    st73_stream_init(&s, on_begin, on_write, on_end, on_reply, NULL);
    for (i = 0; i < len; ++i) {
        st73_stream_feed(&s, buf + i, 1u);
    }
    printf("\n[2] one byte per feed\n");
    snprintf(detail, sizeof(detail), "%u / %u, mismatch=%u",
             g_written, (unsigned)g_expect_len, g_mismatch);
    check("pixel bytes intact", g_written == g_expect_len && g_mismatch == 0, detail);
    check("end + reply once", g_end_calls == 1 && g_reply_calls == 1, NULL);

    /* ---------- 3. 前面掺垃圾，必须能重新同步 ---------- */
    stats_reset();
    st73_stream_init(&s, on_begin, on_write, on_end, on_reply, NULL);
    {
        static const uint8_t junk[] = {
            0x00, 0xA5, 0x11, 0x22, 0xA5, 0xA5, 0x5A, 0x99, 0x00,
            0xA5, 0x5A, 0x07, 0xFF            /* 一个假帧头 + 非法 cmd */
        };
        st73_stream_feed(&s, junk, (uint16_t)sizeof(junk));
    }
    st73_stream_feed(&s, buf, len);
    printf("\n[3] junk bytes before the real frame\n");
    snprintf(detail, sizeof(detail), "%u / %u, mismatch=%u, begin=%d",
             g_written, (unsigned)g_expect_len, g_mismatch, g_begin_calls);
    check("resynced and pixel bytes intact",
          g_written == g_expect_len && g_mismatch == 0, detail);

    /* ---------- 4. 校验和坏掉：应当拒收 ---------- */
    stats_reset();
    st73_stream_init(&s, on_begin, on_write, on_end, on_reply, NULL);
    {
        uint8_t *bad = (uint8_t *)malloc(len);
        if (bad != NULL) {
            memcpy(bad, buf, len);
            bad[8] = (uint8_t)(bad[8] ^ 0xFFu);
            st73_stream_feed(&s, bad, len);
            free(bad);
        }
    }
    printf("\n[4] corrupted checksum\n");
    snprintf(detail, sizeof(detail), "begin=%d written=%u status=%d",
             g_begin_calls, g_written, g_reply_status);
    check("frame rejected, no pixels written", g_begin_calls == 0 && g_written == 0, detail);

    /* ---------- 5. 屏幕拒收尺寸：像素要吞掉，不能错位 ---------- */
    stats_reset();
    g_accept = 0;                       /* 模拟 BeginFrame 返回失败 */
    st73_stream_init(&s, on_begin, on_write, on_end, on_reply, NULL);
    st73_stream_feed(&s, buf, len);
    printf("\n[5] display refuses the frame\n");
    snprintf(detail, sizeof(detail), "begin=%d written=%u status=%d",
             g_begin_calls, g_written, g_reply_status);
    check("pixels swallowed, nothing drawn", g_written == 0 && g_begin_calls == 1, detail);
    check("stream back to idle", st73_stream_busy(&s) == 0, NULL);

    printf("\n%u of %u bytes of pixel data verified byte-for-byte.\n",
           (unsigned)expect_pixels * 2u, (unsigned)g_expect_len);
    printf(g_fail == 0 ? "\nALL PASS\n" : "\n%d FAILURES\n", g_fail);

    free(buf);
    return g_fail == 0 ? 0 : 1;
}
