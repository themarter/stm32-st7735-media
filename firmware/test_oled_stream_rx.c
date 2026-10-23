#include "oled_stream_rx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t rx_frame[OLED_STREAM_FRAME_BYTES];
static uint8_t response_wire[64];
static size_t response_length;
static unsigned refresh_count;

static uint32_t test_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    unsigned bit;
    for (i = 0u; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return crc ^ 0xffffffffu;
}

static size_t encode_cobs(const uint8_t *source, size_t source_length,
                          uint8_t *destination, size_t capacity)
{
    size_t read_pos = 0u, write_pos = 1u, code_pos = 0u;
    uint8_t code = 1u;
    assert(capacity > 1u);
    while (read_pos < source_length) {
        if (source[read_pos] == 0u) {
            destination[code_pos] = code;
            code = 1u; code_pos = write_pos++;
            assert(write_pos <= capacity);
            ++read_pos;
        } else {
            assert(write_pos < capacity);
            destination[write_pos++] = source[read_pos++];
            ++code;
            if (code == 0xffu) {
                destination[code_pos] = code;
                code = 1u; code_pos = write_pos++;
                assert(write_pos <= capacity);
            }
        }
    }
    destination[code_pos] = code;
    return write_pos;
}

static size_t decode_cobs(const uint8_t *source, size_t source_length,
                          uint8_t *destination, size_t capacity)
{
    size_t read_pos = 0u, write_pos = 0u;
    while (read_pos < source_length) {
        uint8_t code = source[read_pos++];
        size_t i;
        assert(code != 0u);
        for (i = 1u; i < code; ++i) {
            assert(read_pos < source_length && write_pos < capacity);
            destination[write_pos++] = source[read_pos++];
        }
        if (code != 0xffu && read_pos < source_length) {
            assert(write_pos < capacity);
            destination[write_pos++] = 0u;
        }
    }
    return write_pos;
}

static void uart_write(void *user, const uint8_t *bytes, size_t length)
{
    (void)user;
    assert(length <= sizeof(response_wire));
    memcpy(response_wire, bytes, length);
    response_length = length;
}

static void refresh(void *user)
{
    (void)user;
    ++refresh_count;
}

static size_t build_request(uint8_t channel, uint16_t sequence,
                            const uint8_t *payload, size_t payload_length,
                            uint8_t *wire, size_t wire_capacity)
{
    uint8_t raw[300], encoded[320];
    size_t raw_length = 3u + payload_length + 4u;
    size_t encoded_length;
    uint32_t crc;
    assert(raw_length <= sizeof(raw));
    raw[0] = channel;
    raw[1] = (uint8_t)sequence;
    raw[2] = (uint8_t)(sequence >> 8);
    memcpy(raw + 3u, payload, payload_length);
    crc = test_crc32(raw, 3u + payload_length);
    raw[3u + payload_length] = (uint8_t)crc;
    raw[4u + payload_length] = (uint8_t)(crc >> 8);
    raw[5u + payload_length] = (uint8_t)(crc >> 16);
    raw[6u + payload_length] = (uint8_t)(crc >> 24);
    encoded_length = encode_cobs(raw, raw_length, encoded, sizeof(encoded));
    assert(encoded_length + 2u <= wire_capacity);
    wire[0] = 0u;
    memcpy(wire + 1u, encoded, encoded_length);
    wire[encoded_length + 1u] = 0u;
    return encoded_length + 2u;
}

static size_t request_and_check_response(OLED_StreamReceiver *receiver,
                                         uint8_t channel, uint16_t sequence,
                                         const uint8_t *payload, size_t payload_length,
                                         uint8_t *response_payload, size_t response_capacity)
{
    uint8_t wire[400], raw[64];
    size_t wire_length = build_request(channel, sequence, payload, payload_length, wire, sizeof(wire));
    size_t raw_length, payload_length_out;
    uint32_t expected_crc;

    response_length = 0u;
    /* The parser must accept a packet split at an arbitrary UART read boundary. */
    oled_stream_rx_feed(receiver, wire, wire_length / 2u);
    oled_stream_rx_feed(receiver, wire + wire_length / 2u, wire_length - wire_length / 2u);

    assert(response_length >= 9u);
    assert(response_wire[0] == 0u && response_wire[response_length-1u] == 0u);
    raw_length = decode_cobs(response_wire + 1u, response_length - 2u, raw, sizeof(raw));
    assert(raw_length >= 7u);
    expected_crc = (uint32_t)raw[raw_length - 4u]
                 | ((uint32_t)raw[raw_length - 3u] << 8)
                 | ((uint32_t)raw[raw_length - 2u] << 16)
                 | ((uint32_t)raw[raw_length - 1u] << 24);
    assert(test_crc32(raw, raw_length - 4u) == expected_crc);
    assert(raw[0] == 2u);
    assert(raw[1] == (uint8_t)sequence && raw[2] == (uint8_t)(sequence >> 8));
    payload_length_out = raw_length - 7u;
    assert(payload_length_out <= response_capacity);
    memcpy(response_payload, raw + 3u, payload_length_out);
    return payload_length_out;
}

static void expect_ack(OLED_StreamReceiver *receiver, uint8_t channel,
                       uint16_t sequence, const uint8_t *payload, size_t length)
{
    uint8_t reply[16];
    size_t reply_length = request_and_check_response(receiver, channel, sequence,
        payload, length, reply, sizeof(reply));
    assert(reply_length == 2u && reply[0] == 0x80u && reply[1] == 0u);
}

int main(void)
{
    OLED_StreamReceiver receiver;
    uint8_t reply[16];
    size_t reply_length;
    const uint8_t hello[] = {0x01u};
    uint8_t begin[9] = {0x10u, 0x2au, 0u, 128u, 0u, 64u, 0u, 250u, 0u};
    uint8_t data_payload[4u + OLED_STREAM_MAX_DATA_BYTES];
    uint8_t end[3] = {0x11u, 0x2au, 0u};
    const uint8_t stop[] = {0x12u};
    size_t offset;
    size_t i;

    for (i = 0u; i < sizeof(rx_frame); ++i) {
        rx_frame[i] = (uint8_t)((i * 7u + 3u) & 0xffu);
    }
    memset(rx_frame, 0, sizeof(rx_frame));
    refresh_count = 0u;
    oled_stream_rx_init(&receiver, rx_frame, sizeof(rx_frame), 128u, 64u,
                        refresh, uart_write, NULL);

    reply_length = request_and_check_response(&receiver, 0u, 7u, hello, sizeof(hello),
                                               reply, sizeof(reply));
    assert(reply_length == 6u);
    assert(reply[0] == 0x81u && reply[1] == 128u && reply[2] == 0u);
    assert(reply[3] == 64u && reply[4] == 0u && reply[5] == 1u);
    assert(receiver.session_active == 1u);

    expect_ack(&receiver, 0u, 8u, begin, sizeof(begin));
    for (offset = 0u; offset < OLED_STREAM_FRAME_BYTES; offset += OLED_STREAM_MAX_DATA_BYTES) {
        size_t n = OLED_STREAM_MAX_DATA_BYTES;
        uint16_t frame_id = 42u, off = (uint16_t)offset;
        data_payload[0] = (uint8_t)frame_id;
        data_payload[1] = (uint8_t)(frame_id >> 8);
        data_payload[2] = (uint8_t)off;
        data_payload[3] = (uint8_t)(off >> 8);
        for (i = 0u; i < n; ++i) data_payload[4u+i] = (uint8_t)(((offset+i) * 7u + 3u) & 0xffu);
        expect_ack(&receiver, 1u, (uint16_t)(9u + offset/OLED_STREAM_MAX_DATA_BYTES),
                   data_payload, 4u+n);
    }
    assert(receiver.next_offset == OLED_STREAM_FRAME_BYTES);

    expect_ack(&receiver, 0u, 13u, end, sizeof(end));
    assert(receiver.frame_active == 0u);
    assert(refresh_count == 1u);
    for (i = 0u; i < sizeof(rx_frame); ++i) {
        assert(rx_frame[i] == (uint8_t)((i * 7u + 3u) & 0xffu));
    }

    expect_ack(&receiver, 0u, 14u, stop, sizeof(stop));
    assert(receiver.session_active == 0u);

    puts("OLED STREAM tests: PASS (COBS/CRC, HELLO, fragmented UART, 1024-byte page frame, refresh, ACK, SESSION_STOP)");
    return 0;
}
