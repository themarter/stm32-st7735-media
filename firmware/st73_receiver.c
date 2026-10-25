#include "st73_receiver.h"

/* 一次喂给驱动的最大字节数。小一点能让主循环更快回到串口队列，
 * 避免写屏期间 4 KiB 环形缓冲被灌满。 */
#define ST73_WRITE_SLICE 1024u

/* 帧头校验没过时，这 9 个字节里可能还藏着真正的同步字。
 *
 * 典型情况：上一帧发到一半断开，串口里残留的像素字节恰好是 A5 5A
 * （RGB565 里 0x5AA5 是很常见的灰色），接上下一帧时，这个假同步字
 * 会把真帧的 A5 5A 一起圈进窗口 —— 直接清零就整帧丢了，用户看到的是
 * 「点一次没反应，得再点一次」。
 *
 * 所以从窗口第 2 个字节往回找下一个 A5 5A，把尾巴搬到开头继续接。
 * 找不到成对的，退一步只找落单的 A5（下一字节可能正好是 5A）。 */
static void resync_within_header(ST73_Stream *s)
{
    uint8_t i;
    uint8_t keep = 0u;
    uint8_t tail;

    for (i = 1u; (uint8_t)(i + 1u) < ST73_HEADER_SIZE; ++i) {
        if (s->header[i] == ST73_SYNC0 && s->header[i + 1u] == ST73_SYNC1) {
            keep = i;
            break;
        }
    }
    if (keep == 0u) {
        for (i = 1u; i < ST73_HEADER_SIZE; ++i) {
            if (s->header[i] == ST73_SYNC0) {
                keep = i;
                break;
            }
        }
    }

    if (keep == 0u) {
        s->header_len = 0u;
        return;
    }

    tail = (uint8_t)(ST73_HEADER_SIZE - keep);
    for (i = 0u; i < tail; ++i) {
        s->header[i] = s->header[i + keep];
    }
    s->header_len = tail;
}

static void begin_frame(ST73_Stream *s)
{
    const uint8_t *h = s->header;
    uint8_t  sum = 0u;
    uint8_t  k;
    uint16_t w;
    uint16_t hgt;
    uint8_t  ori;

    for (k = 0u; k < (uint8_t)(ST73_HEADER_SIZE - 1u); ++k) {
        sum = (uint8_t)(sum + h[k]);
    }

    w   = (uint16_t)((uint16_t)h[3] | ((uint16_t)h[4] << 8));
    hgt = (uint16_t)((uint16_t)h[5] | ((uint16_t)h[6] << 8));
    ori = (uint8_t)(h[7] & 3u);

    /* 命令或校验不对就当没看见，继续找下一个同步字。
     * 注意这里不能直接把 header_len 清零：窗口里可能还含着真帧的同步字。 */
    if (h[2] != ST73_CMD_IMAGE || sum != h[8] || w == 0u || hgt == 0u) {
        resync_within_header(s);
        return;
    }

    s->header_len = 0u;

    s->width       = w;
    s->height      = hgt;
    s->orientation = ori;
    s->remaining   = (uint32_t)w * (uint32_t)hgt * 2u;
    s->active      = 1u;

    /* 尺寸不合法也必须进入 active，把像素流吞干净。
     * 否则像素里偶然出现的 0xA5 0x5A 会被当成新帧头，越错越远。 */
    if (s->begin != 0 && s->begin(s->user, w, hgt, ori) != 0u) {
        s->discard = 0u;
    } else {
        s->discard = 1u;
        if (s->reply != 0) {
            s->reply(s->user, ST73_STATUS_BAD_SIZE);
        }
    }
}

static size_t consume_header(ST73_Stream *s, const uint8_t *p, size_t n)
{
    size_t i = 0u;

    while (i < n) {
        uint8_t b = p[i];

        ++i;

        if (s->header_len == 0u) {
            if (b == ST73_SYNC0) {
                s->header[0] = b;
                s->header_len = 1u;
            }
        } else if (s->header_len == 1u) {
            if (b == ST73_SYNC1) {
                s->header[1] = b;
                s->header_len = 2u;
            } else if (b != ST73_SYNC0) {
                s->header_len = 0u;
            }
        } else {
            s->header[s->header_len] = b;
            ++s->header_len;
            if (s->header_len == ST73_HEADER_SIZE) {
                begin_frame(s);
                return i;
            }
        }
    }

    return i;
}

static void finish_frame(ST73_Stream *s)
{
    if (s->discard == 0u) {
        if (s->end != 0) {
            s->end(s->user);
        }
        if (s->reply != 0) {
            s->reply(s->user, ST73_STATUS_OK);
        }
    }
    s->active     = 0u;
    s->discard    = 0u;
    s->header_len = 0u;
    s->half_valid = 0u;
}

static size_t consume_pixels(ST73_Stream *s, const uint8_t *p, size_t n)
{
    size_t used = 0u;
    size_t take;

    /* 拒收的帧只按字节吞掉，不需要像素对齐。 */
    if (s->discard != 0u) {
        take = (n < (size_t)s->remaining) ? n : (size_t)s->remaining;
        s->remaining -= (uint32_t)take;
        if (s->remaining == 0u) {
            finish_frame(s);
        }
        return take;
    }

    /* 上一轮末尾只剩一个字节，先把它和新字节拼成一个像素。 */
    if (s->half_valid != 0u) {
        uint8_t pair[2];

        pair[0] = s->half_byte;
        pair[1] = p[0];
        used = 1u;
        s->half_valid = 0u;
        s->write(s->user, pair, 2u);
        s->remaining -= 2u;
        if (s->remaining == 0u) {
            finish_frame(s);
            return used;
        }
    }

    take = n - used;
    if (take > (size_t)s->remaining) {
        take = (size_t)s->remaining;
    }
    if (take > ST73_WRITE_SLICE) {
        take = ST73_WRITE_SLICE;
    }
    take &= ~(size_t)1u;            /* 只成对消费，保证是完整像素 */

    if (take != 0u) {
        s->write(s->user, p + used, (uint16_t)take);
        used += take;
        s->remaining -= (uint32_t)take;
        if (s->remaining == 0u) {
            finish_frame(s);
        }
        return used;
    }

    /* 只剩一个字节，挂起等下一批，绝不能丢。 */
    if ((n - used) == 1u) {
        s->half_byte  = p[used];
        s->half_valid = 1u;
        ++used;
    }
    return used;
}

void st73_stream_reset(ST73_Stream *stream)
{
    uint8_t k;

    if (stream == 0) {
        return;
    }
    for (k = 0u; k < ST73_HEADER_SIZE; ++k) {
        stream->header[k] = 0u;
    }
    stream->header_len  = 0u;
    stream->active      = 0u;
    stream->discard     = 0u;
    stream->half_valid  = 0u;
    stream->half_byte   = 0u;
    stream->width       = 0u;
    stream->height      = 0u;
    stream->orientation = 0u;
    stream->remaining   = 0u;
}

void st73_stream_init(
    ST73_Stream *stream,
    st73_begin_fn begin,
    st73_write_fn write,
    st73_end_fn end,
    st73_reply_fn reply,
    void *user)
{
    if (stream == 0) {
        return;
    }
    stream->begin = begin;
    stream->write = write;
    stream->end   = end;
    stream->reply = reply;
    stream->user  = user;
    st73_stream_reset(stream);
}

void st73_stream_feed(ST73_Stream *stream, const uint8_t *data, size_t length)
{
    size_t i = 0u;

    if (stream == 0 || data == 0 || length == 0u) {
        return;
    }

    while (i < length) {
        if (stream->active != 0u) {
            i += consume_pixels(stream, data + i, length - i);
        } else {
            i += consume_header(stream, data + i, length - i);
        }
    }
}

uint8_t st73_stream_busy(const ST73_Stream *stream)
{
    return (stream != 0) ? stream->active : 0u;
}
