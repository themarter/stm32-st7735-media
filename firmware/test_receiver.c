#include "st73_receiver.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t tx[128];
static size_t tx_len;
static unsigned draw_count;
static uint8_t drawn_pixel[2];

static uint32_t crc32_test(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    unsigned bit;
    for (i = 0; i < len; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return crc ^ 0xffffffffu;
}

static size_t cobs_encode_test(const uint8_t *src, size_t len, uint8_t *dst)
{
    size_t in = 0u, out = 1u, code_pos = 0u;
    uint8_t code = 1u;
    while (in < len) {
        if (src[in] == 0u) {
            dst[code_pos] = code;
            code = 1u;
            code_pos = out++;
            ++in;
        } else {
            dst[out++] = src[in++];
            ++code;
            if (code == 0xffu) {
                dst[code_pos] = code;
                code = 1u;
                code_pos = out++;
            }
        }
    }
    dst[code_pos] = code;
    return out;
}

static size_t cobs_decode_test(const uint8_t *src, size_t len, uint8_t *dst)
{
    size_t in = 0u, out = 0u;
    while (in < len) {
        uint8_t code = src[in++];
        size_t i;
        assert(code != 0u);
        for (i = 1u; i < code; ++i) {
            assert(in < len);
            dst[out++] = src[in++];
        }
        if (code != 0xffu && in < len) {
            dst[out++] = 0u;
        }
    }
    return out;
}

static void uart_write(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    assert(len <= sizeof(tx));
    memcpy(tx, data, len);
    tx_len = len;
}

static void draw_tile(void *user, uint16_t x, uint16_t y,
                      uint16_t w, uint16_t h, const uint8_t *pixels)
{
    (void)user;
    assert(x == 0u && y == 0u && w == 1u && h == 1u);
    drawn_pixel[0] = pixels[0];
    drawn_pixel[1] = pixels[1];
    ++draw_count;
}

static size_t build_request(uint8_t channel, uint16_t seq,
                            const uint8_t *payload, size_t payload_len,
                            uint8_t *wire)
{
    uint8_t raw[128];
    size_t raw_len = 3u + payload_len + 4u;
    uint32_t crc;
    size_t enc_len;

    raw[0] = channel;
    raw[1] = (uint8_t)(seq & 0xffu);
    raw[2] = (uint8_t)(seq >> 8);
    memcpy(raw + 3, payload, payload_len);
    crc = crc32_test(raw, 3u + payload_len);
    raw[3u + payload_len] = (uint8_t)crc;
    raw[4u + payload_len] = (uint8_t)(crc >> 8);
    raw[5u + payload_len] = (uint8_t)(crc >> 16);
    raw[6u + payload_len] = (uint8_t)(crc >> 24);
    wire[0] = 0u;
    enc_len = cobs_encode_test(raw, raw_len, wire + 1u);
    wire[enc_len + 1u] = 0u;
    return enc_len + 2u;
}

static const uint8_t *request_and_get_response(
    ST73_Receiver *receiver, uint8_t channel, uint16_t seq,
    const uint8_t *payload, size_t payload_len,
    uint8_t *response_payload, size_t *response_payload_len)
{
    uint8_t wire[160];
    uint8_t raw[128];
    size_t wire_len = build_request(channel, seq, payload, payload_len, wire);
    size_t raw_len;
    uint32_t expected_crc;

    tx_len = 0u;
    /* Split the request to exercise incremental UART feeding. */
    st73_receiver_feed(receiver, wire, wire_len / 2u);
    st73_receiver_feed(receiver, wire + wire_len / 2u, wire_len - wire_len / 2u);

    assert(tx_len >= 9u);
    assert(tx[0] == 0u && tx[tx_len - 1u] == 0u);
    raw_len = cobs_decode_test(tx + 1u, tx_len - 2u, raw);
    assert(raw_len >= 7u);
    expected_crc = (uint32_t)raw[raw_len - 4u]
                 | ((uint32_t)raw[raw_len - 3u] << 8)
                 | ((uint32_t)raw[raw_len - 2u] << 16)
                 | ((uint32_t)raw[raw_len - 1u] << 24);
    assert(raw[0] == 2u);
    assert(raw[1] == (uint8_t)(seq & 0xffu));
    assert(raw[2] == (uint8_t)(seq >> 8));
    assert(crc32_test(raw, raw_len - 4u) == expected_crc);
    *response_payload_len = raw_len - 7u;
    memcpy(response_payload, raw + 3u, *response_payload_len);
    return response_payload;
}

static void assert_ack(ST73_Receiver *receiver, uint8_t channel,
                       uint16_t seq, const uint8_t *payload, size_t len)
{
    uint8_t reply[32];
    size_t reply_len = 0u;
    (void)request_and_get_response(receiver, channel, seq, payload, len,
                                   reply, &reply_len);
    assert(reply_len == 2u);
    assert(reply[0] == 0x80u && reply[1] == 0u);
}

int main(void)
{
    ST73_Receiver receiver;
    uint8_t reply[32];
    size_t reply_len = 0u;
    const uint8_t hello[] = { 0x01u };
    uint8_t begin[12] = {
        0x10u, 0x34u, 0x12u, 128u, 0u, 160u, 0u, 0u,
        100u, 0u, 1u, 0u
    };
    uint8_t tile_data[13] = {
        0x34u, 0x12u, 0u, 0u, /* frame ID, chunk index */
        0u, 0u, 1u, 1u, 0u, 2u, 0u, /* one raw 1x1 tile */
        0xf8u, 0x00u /* RGB565 red, big-endian */
    };
    const uint8_t end[] = { 0x11u, 0x34u, 0x12u };

    st73_receiver_init(&receiver, 128u, 160u, draw_tile, NULL, uart_write, NULL);

    (void)request_and_get_response(&receiver, 0u, 7u, hello, sizeof(hello),
                                   reply, &reply_len);
    assert(reply_len == 6u);
    assert(reply[0] == 0x81u && reply[1] == 128u && reply[2] == 0u);
    assert(reply[3] == 160u && reply[4] == 0u && reply[5] == 1u);

    assert_ack(&receiver, 0u, 8u, begin, sizeof(begin));
    assert_ack(&receiver, 1u, 9u, tile_data, sizeof(tile_data));
    assert(draw_count == 1u && drawn_pixel[0] == 0xf8u && drawn_pixel[1] == 0x00u);
    assert_ack(&receiver, 0u, 10u, end, sizeof(end));
    assert(receiver.frame_active == 0u);

    puts("ST73 receiver tests: PASS (COBS/CRC, HELLO, fragmented UART, RGB565 tile, ACK, frame-end)");
    return 0;
}
