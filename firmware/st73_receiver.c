#include "st73_receiver.h"

#include <string.h>

#define ST73_CHANNEL_CONTROL 0u
#define ST73_CHANNEL_DATA    1u
#define ST73_CHANNEL_EVENT   2u

#define ST73_CMD_HELLO       0x01u
#define ST73_CMD_FRAME_BEGIN 0x10u
#define ST73_CMD_FRAME_END   0x11u
#define ST73_CMD_SESSION_STOP 0x12u

#define ST73_EVT_ACK         0x80u
#define ST73_EVT_HELLO       0x81u

#define ST73_STATUS_OK          0u
#define ST73_STATUS_BAD_COMMAND 1u
#define ST73_STATUS_BAD_STATE   2u
#define ST73_STATUS_BAD_SIZE    3u
#define ST73_STATUS_BAD_DATA    4u
#define ST73_STATUS_TOO_MANY    5u
#define ST73_STATUS_TILE_COUNT  6u

static uint16_t st73_get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void st73_put_u16le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)(value >> 8);
}

static uint32_t st73_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    uint8_t bit;

    for (i = 0; i < len; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return crc ^ 0xffffffffu;
}

static size_t st73_cobs_decode(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap)
{
    size_t in_pos = 0u;
    size_t out_pos = 0u;

    while (in_pos < src_len) {
        uint8_t code = src[in_pos++];
        size_t copy_len;

        if (code == 0u) {
            return 0u;
        }
        copy_len = (size_t)code - 1u;
        if (in_pos + copy_len > src_len || out_pos + copy_len > dst_cap) {
            return 0u;
        }
        if (copy_len != 0u) {
            memmove(dst + out_pos, src + in_pos, copy_len);
            in_pos += copy_len;
            out_pos += copy_len;
        }
        if (code != 0xffu && in_pos < src_len) {
            if (out_pos >= dst_cap) {
                return 0u;
            }
            dst[out_pos++] = 0u;
        }
    }
    return out_pos;
}

static size_t st73_cobs_encode(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap)
{
    size_t read_pos = 0u;
    size_t write_pos = 1u;
    size_t code_pos = 0u;
    uint8_t code = 1u;

    if (dst_cap == 0u) {
        return 0u;
    }

    while (read_pos < src_len) {
        if (src[read_pos] == 0u) {
            dst[code_pos] = code;
            code = 1u;
            code_pos = write_pos;
            if (write_pos >= dst_cap) {
                return 0u;
            }
            ++write_pos;
            ++read_pos;
        } else {
            if (write_pos >= dst_cap) {
                return 0u;
            }
            dst[write_pos++] = src[read_pos++];
            ++code;
            if (code == 0xffu) {
                dst[code_pos] = code;
                code = 1u;
                code_pos = write_pos;
                if (write_pos >= dst_cap) {
                    return 0u;
                }
                ++write_pos;
            }
        }
    }

    dst[code_pos] = code;
    return write_pos;
}

static void st73_send_packet(ST73_Receiver *r, uint8_t channel,
                             uint16_t sequence,
                             const uint8_t *payload, size_t payload_len)
{
    uint8_t raw[16];
    uint8_t encoded[20];
    size_t raw_len = 3u + payload_len + 4u;
    size_t encoded_len;
    uint32_t crc;

    if (r == NULL || r->uart_write == NULL || raw_len > sizeof(raw)) {
        return;
    }

    raw[0] = channel;
    st73_put_u16le(raw + 1, sequence);
    if (payload_len != 0u) {
        memcpy(raw + 3, payload, payload_len);
    }
    crc = st73_crc32(raw, 3u + payload_len);
    raw[3u + payload_len] = (uint8_t)(crc & 0xffu);
    raw[4u + payload_len] = (uint8_t)((crc >> 8) & 0xffu);
    raw[5u + payload_len] = (uint8_t)((crc >> 16) & 0xffu);
    raw[6u + payload_len] = (uint8_t)((crc >> 24) & 0xffu);

    encoded_len = st73_cobs_encode(raw, raw_len, encoded + 1u,
                                   sizeof(encoded) - 2u);
    if (encoded_len == 0u || encoded_len + 2u > sizeof(encoded)) {
        return;
    }
    encoded[0] = 0u;
    encoded[encoded_len + 1u] = 0u;
    r->uart_write(r->user, encoded, encoded_len + 2u);
}

static void st73_send_ack(ST73_Receiver *r, uint16_t seq, uint8_t status)
{
    uint8_t payload[2];
    payload[0] = ST73_EVT_ACK;
    payload[1] = status;
    st73_send_packet(r, ST73_CHANNEL_EVENT, seq, payload, sizeof(payload));
}

static void st73_send_hello(ST73_Receiver *r, uint16_t seq)
{
    uint8_t payload[6];
    payload[0] = ST73_EVT_HELLO;
    st73_put_u16le(payload + 1, r->panel_width);
    st73_put_u16le(payload + 3, r->panel_height);
    payload[5] = ST73_PROTOCOL_VERSION;
    st73_send_packet(r, ST73_CHANNEL_EVENT, seq, payload, sizeof(payload));
}

static uint8_t st73_decode_tile(ST73_Receiver *r, const uint8_t *rec,
                                size_t record_len)
{
    uint16_t x, y, w, h, data_len;
    uint8_t codec;
    const uint8_t *src;
    uint8_t *pixels = r->tile_pixels;
    size_t out_len = 0u;
    size_t pos;
    uint16_t expected_pixels;

    if (record_len < 7u) {
        return ST73_STATUS_BAD_DATA;
    }

    x = rec[0];
    y = rec[1];
    w = rec[2];
    h = rec[3];
    codec = rec[4];
    data_len = st73_get_u16le(rec + 5);
    if ((size_t)data_len + 7u != record_len || w == 0u || h == 0u ||
        w > ST73_MAX_TILE_SIZE || h > ST73_MAX_TILE_SIZE ||
        (uint32_t)x + w > r->frame_width ||
        (uint32_t)y + h > r->frame_height ||
        (x % ST73_MAX_TILE_SIZE) != 0u ||
        (y % ST73_MAX_TILE_SIZE) != 0u) {
        return ST73_STATUS_BAD_SIZE;
    }

    expected_pixels = (uint16_t)(w * h);
    src = rec + 7u;

    if (codec == 0u) {
        if (data_len != (uint16_t)(expected_pixels * 2u)) {
            return ST73_STATUS_BAD_DATA;
        }
        memcpy(pixels, src, data_len);
        out_len = data_len;
    } else if (codec == 1u) {
        if ((data_len % 3u) != 0u) {
            return ST73_STATUS_BAD_DATA;
        }
        for (pos = 0u; pos < data_len; pos += 3u) {
            size_t run_bytes = ((size_t)src[pos] + 1u) * 2u;
            if (out_len + run_bytes > (size_t)expected_pixels * 2u) {
                return ST73_STATUS_BAD_DATA;
            }
            while (run_bytes != 0u) {
                pixels[out_len++] = src[pos + 1u];
                pixels[out_len++] = src[pos + 2u];
                run_bytes -= 2u;
            }
        }
        if (out_len != (size_t)expected_pixels * 2u) {
            return ST73_STATUS_BAD_DATA;
        }
    } else {
        return ST73_STATUS_BAD_DATA;
    }

    if (r->draw_tile != NULL) {
        r->draw_tile(r->user, x, y, w, h, pixels);
    }
    return ST73_STATUS_OK;
}

static void st73_process_packet(ST73_Receiver *r, size_t encoded_len)
{
    /* COBS output never exceeds input length; decode in place to avoid a large stack buffer. */
    uint8_t *raw = r->rx_encoded;
    size_t raw_len;
    uint32_t actual_crc, expected_crc;
    uint8_t channel;
    uint16_t seq;
    const uint8_t *payload;
    size_t payload_len;

    raw_len = st73_cobs_decode(r->rx_encoded, encoded_len, raw, sizeof(r->rx_encoded));
    if (raw_len < 7u) {
        return;
    }
    actual_crc = st73_crc32(raw, raw_len - 4u);
    expected_crc = (uint32_t)raw[raw_len - 4u]
                 | ((uint32_t)raw[raw_len - 3u] << 8)
                 | ((uint32_t)raw[raw_len - 2u] << 16)
                 | ((uint32_t)raw[raw_len - 1u] << 24);
    if (actual_crc != expected_crc) {
        return;
    }

    channel = raw[0];
    seq = st73_get_u16le(raw + 1);
    payload = raw + 3;
    payload_len = raw_len - 7u;

    if (channel == ST73_CHANNEL_CONTROL) {
        uint8_t status = ST73_STATUS_OK;

        if (payload_len == 1u && payload[0] == ST73_CMD_HELLO) {
            st73_send_hello(r, seq);
            return;
        }

        if (payload_len == 12u && payload[0] == ST73_CMD_FRAME_BEGIN) {
            uint16_t id = st73_get_u16le(payload + 1);
            uint16_t width = st73_get_u16le(payload + 3);
            uint16_t height = st73_get_u16le(payload + 5);
            uint8_t orientation = payload[7];
            uint16_t tiles = st73_get_u16le(payload + 10);
            uint16_t expected_w = (orientation & 1u) ? r->panel_height : r->panel_width;
            uint16_t expected_h = (orientation & 1u) ? r->panel_width : r->panel_height;

            if (orientation > 3u || width != expected_w || height != expected_h ||
                width > 255u || height > 255u) {
                status = ST73_STATUS_BAD_SIZE;
            } else if (tiles > ((width + 15u) / 16u) * ((height + 15u) / 16u)) {
                status = ST73_STATUS_TOO_MANY;
            } else {
                r->frame_active = 1u;
                r->frame_id = id;
                r->frame_width = width;
                r->frame_height = height;
                r->orientation = orientation;
                r->frame_delay_ms = st73_get_u16le(payload + 8);
                r->expected_tiles = tiles;
                r->received_tiles = 0u;
                r->expected_chunk = 0u;
                if (r->set_orientation != NULL) {
                    r->set_orientation(r->user, orientation);
                }
            }
        } else if (payload_len == 3u && payload[0] == ST73_CMD_FRAME_END) {
            uint16_t id = st73_get_u16le(payload + 1);
            if (!r->frame_active || id != r->frame_id) {
                status = ST73_STATUS_BAD_STATE;
            } else if (r->received_tiles != r->expected_tiles) {
                status = ST73_STATUS_TILE_COUNT;
            } else {
                r->frame_active = 0u;
            }
        } else if (payload_len == 1u && payload[0] == ST73_CMD_SESSION_STOP) {
            r->frame_active = 0u;
        } else {
            status = ST73_STATUS_BAD_COMMAND;
        }
        st73_send_ack(r, seq, status);
        return;
    }

    if (channel == ST73_CHANNEL_DATA) {
        uint16_t id, chunk;
        size_t pos;
        uint16_t count_this_packet = 0u;
        uint8_t status = ST73_STATUS_OK;

        if (!r->frame_active || payload_len < 4u) {
            st73_send_ack(r, seq, ST73_STATUS_BAD_STATE);
            return;
        }
        id = st73_get_u16le(payload);
        chunk = st73_get_u16le(payload + 2);
        if (id != r->frame_id) {
            st73_send_ack(r, seq, ST73_STATUS_BAD_STATE);
            return;
        }
        if (chunk != r->expected_chunk) {
            st73_send_ack(r, seq, ST73_STATUS_BAD_DATA);
            return;
        }

        pos = 4u;
        while (pos < payload_len) {
            size_t rec_len;
            uint16_t data_len;
            if (payload_len - pos < 7u) {
                status = ST73_STATUS_BAD_DATA;
                break;
            }
            data_len = st73_get_u16le(payload + pos + 5u);
            rec_len = 7u + data_len;
            if (rec_len > payload_len - pos) {
                status = ST73_STATUS_BAD_DATA;
                break;
            }
            if ((uint32_t)r->received_tiles + count_this_packet >= r->expected_tiles) {
                status = ST73_STATUS_TOO_MANY;
                break;
            }
            status = st73_decode_tile(r, payload + pos, rec_len);
            if (status != ST73_STATUS_OK) {
                break;
            }
            ++count_this_packet;
            pos += rec_len;
        }

        if (status == ST73_STATUS_OK && pos == payload_len) {
            r->received_tiles = (uint16_t)(r->received_tiles + count_this_packet);
            ++r->expected_chunk;
        } else if (status == ST73_STATUS_OK) {
            status = ST73_STATUS_BAD_DATA;
        }
        st73_send_ack(r, seq, status);
        return;
    }

    st73_send_ack(r, seq, ST73_STATUS_BAD_COMMAND);
}

void st73_receiver_init(ST73_Receiver *r,
                        uint16_t panel_width,
                        uint16_t panel_height,
                        st73_draw_tile_fn draw_tile,
                        st73_set_orientation_fn set_orientation,
                        st73_uart_write_fn uart_write,
                        void *user)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->panel_width = panel_width;
    r->panel_height = panel_height;
    r->draw_tile = draw_tile;
    r->set_orientation = set_orientation;
    r->uart_write = uart_write;
    r->user = user;
}

void st73_receiver_feed(ST73_Receiver *r, const uint8_t *data, size_t len)
{
    size_t i;
    if (r == NULL || data == NULL) {
        return;
    }

    for (i = 0u; i < len; ++i) {
        uint8_t byte = data[i];
        if (byte == 0u) {
            if (r->rx_len != 0u) {
                st73_process_packet(r, r->rx_len);
                r->rx_len = 0u;
            }
        } else if (r->rx_len < sizeof(r->rx_encoded)) {
            r->rx_encoded[r->rx_len++] = byte;
        } else {
            r->rx_len = 0u;
        }
    }
}
