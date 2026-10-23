#include "oled_stream_rx.h"

#include <string.h>

#define CHANNEL_CONTROL 0u
#define CHANNEL_DATA    1u
#define CHANNEL_EVENT   2u

#define CMD_HELLO       0x01u
#define CMD_FRAME_BEGIN 0x10u
#define CMD_FRAME_END   0x11u
#define CMD_SESSION_STOP 0x12u

#define EVT_ACK         0x80u
#define EVT_HELLO       0x81u

#define STATUS_OK          0u
#define STATUS_BAD_COMMAND 1u
#define STATUS_BAD_STATE   2u
#define STATUS_BAD_SIZE    3u
#define STATUS_BAD_DATA    4u
#define STATUS_TOO_LARGE   5u

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u16le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)(value >> 8);
}

static uint32_t crc32_ieee(const uint8_t *data, size_t length)
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

/* In-place COBS decode; decoded output never exceeds encoded input length. */
static size_t cobs_decode_in_place(uint8_t *buffer, size_t length)
{
    size_t read_pos = 0u;
    size_t write_pos = 0u;

    while (read_pos < length) {
        uint8_t code = buffer[read_pos++];
        size_t copy_length;

        if (code == 0u) {
            return 0u;
        }
        copy_length = (size_t)code - 1u;
        if (read_pos + copy_length > length) {
            return 0u;
        }
        if (copy_length != 0u) {
            memmove(buffer + write_pos, buffer + read_pos, copy_length);
            read_pos += copy_length;
            write_pos += copy_length;
        }
        if (code != 0xffu && read_pos < length) {
            buffer[write_pos++] = 0u;
        }
    }
    return write_pos;
}

static size_t cobs_encode(const uint8_t *source, size_t source_length,
                          uint8_t *destination, size_t capacity)
{
    size_t read_pos = 0u;
    size_t write_pos = 1u;
    size_t code_pos = 0u;
    uint8_t code = 1u;

    if (capacity == 0u) {
        return 0u;
    }

    while (read_pos < source_length) {
        if (source[read_pos] == 0u) {
            destination[code_pos] = code;
            code = 1u;
            code_pos = write_pos;
            if (write_pos >= capacity) return 0u;
            ++write_pos;
            ++read_pos;
        } else {
            if (write_pos >= capacity) return 0u;
            destination[write_pos++] = source[read_pos++];
            ++code;
            if (code == 0xffu) {
                destination[code_pos] = code;
                code = 1u;
                code_pos = write_pos;
                if (write_pos >= capacity) return 0u;
                ++write_pos;
            }
        }
    }

    destination[code_pos] = code;
    return write_pos;
}

static void send_packet(OLED_StreamReceiver *receiver, uint8_t channel,
                        uint16_t sequence, const uint8_t *payload,
                        size_t payload_length)
{
    uint8_t raw[16];
    uint8_t encoded[20];
    size_t raw_length = 3u + payload_length + 4u;
    size_t encoded_length;
    uint32_t crc;

    if (receiver == NULL || receiver->uart_write == NULL ||
        payload_length > 6u || raw_length > sizeof(raw)) {
        return;
    }

    raw[0] = channel;
    put_u16le(raw + 1u, sequence);
    if (payload_length != 0u) {
        memcpy(raw + 3u, payload, payload_length);
    }
    crc = crc32_ieee(raw, 3u + payload_length);
    raw[3u + payload_length] = (uint8_t)crc;
    raw[4u + payload_length] = (uint8_t)(crc >> 8);
    raw[5u + payload_length] = (uint8_t)(crc >> 16);
    raw[6u + payload_length] = (uint8_t)(crc >> 24);

    encoded_length = cobs_encode(raw, raw_length, encoded + 1u, sizeof(encoded) - 2u);
    if (encoded_length == 0u || encoded_length + 2u > sizeof(encoded)) {
        return;
    }
    encoded[0] = 0u;
    encoded[encoded_length + 1u] = 0u;
    receiver->uart_write(receiver->user, encoded, encoded_length + 2u);
}

static void send_ack(OLED_StreamReceiver *receiver, uint16_t sequence, uint8_t status)
{
    const uint8_t payload[2] = {EVT_ACK, status};
    send_packet(receiver, CHANNEL_EVENT, sequence, payload, sizeof(payload));
}

static void send_hello(OLED_StreamReceiver *receiver, uint16_t sequence)
{
    uint8_t payload[6];
    payload[0] = EVT_HELLO;
    put_u16le(payload + 1u, receiver->width);
    put_u16le(payload + 3u, receiver->height);
    payload[5] = OLED_STREAM_VERSION;
    send_packet(receiver, CHANNEL_EVENT, sequence, payload, sizeof(payload));
}

static void process_packet(OLED_StreamReceiver *receiver, size_t encoded_length)
{
    uint8_t *raw = receiver->rx_encoded;
    size_t raw_length;
    uint32_t crc_actual, crc_expected;
    uint8_t channel;
    uint16_t sequence;
    const uint8_t *payload;
    size_t payload_length;
    uint8_t status = STATUS_OK;

    raw_length = cobs_decode_in_place(raw, encoded_length);
    if (raw_length < 7u) return;

    crc_actual = crc32_ieee(raw, raw_length - 4u);
    crc_expected = (uint32_t)raw[raw_length - 4u]
                 | ((uint32_t)raw[raw_length - 3u] << 8)
                 | ((uint32_t)raw[raw_length - 2u] << 16)
                 | ((uint32_t)raw[raw_length - 1u] << 24);
    if (crc_actual != crc_expected) return;

    channel = raw[0];
    sequence = get_u16le(raw + 1u);
    payload = raw + 3u;
    payload_length = raw_length - 7u;

    if (channel == CHANNEL_CONTROL) {
        if (payload_length == 1u && payload[0] == CMD_HELLO) {
            receiver->session_active = 1u;
            send_hello(receiver, sequence);
            return;
        }

        if (payload_length == 9u && payload[0] == CMD_FRAME_BEGIN) {
            uint16_t frame_id = get_u16le(payload + 1u);
            uint16_t width = get_u16le(payload + 3u);
            uint16_t height = get_u16le(payload + 5u);

            if (width != receiver->width || height != receiver->height ||
                width != OLED_STREAM_WIDTH || height != OLED_STREAM_HEIGHT ||
                receiver->frame_bytes != OLED_STREAM_FRAME_BYTES ||
                receiver->framebuffer == NULL ||
                receiver->framebuffer_capacity < receiver->frame_bytes) {
                status = STATUS_BAD_SIZE;
            } else {
                receiver->session_active = 1u;
                receiver->frame_active = 1u;
                receiver->frame_id = frame_id;
                receiver->frame_delay_ms = get_u16le(payload + 7u);
                receiver->next_offset = 0u;
            }
        } else if (payload_length == 3u && payload[0] == CMD_FRAME_END) {
            uint16_t frame_id = get_u16le(payload + 1u);
            if (!receiver->frame_active || frame_id != receiver->frame_id) {
                status = STATUS_BAD_STATE;
            } else if (receiver->next_offset != receiver->frame_bytes) {
                status = STATUS_BAD_DATA;
            } else {
                receiver->frame_active = 0u;
                if (receiver->refresh != NULL) {
                    receiver->refresh(receiver->user);
                }
            }
        } else if (payload_length == 1u && payload[0] == CMD_SESSION_STOP) {
            receiver->frame_active = 0u;
            receiver->session_active = 0u;
        } else {
            status = STATUS_BAD_COMMAND;
        }
        send_ack(receiver, sequence, status);
        return;
    }

    if (channel == CHANNEL_DATA) {
        uint16_t frame_id, offset;
        size_t chunk_length;

        if (!receiver->frame_active || payload_length < 5u) {
            send_ack(receiver, sequence, STATUS_BAD_STATE);
            return;
        }
        frame_id = get_u16le(payload);
        offset = get_u16le(payload + 2u);
        chunk_length = payload_length - 4u;
        if (frame_id != receiver->frame_id) {
            status = STATUS_BAD_STATE;
        } else if (chunk_length > OLED_STREAM_MAX_DATA_BYTES ||
                   offset != receiver->next_offset ||
                   chunk_length > receiver->frame_bytes - receiver->next_offset) {
            status = (chunk_length > OLED_STREAM_MAX_DATA_BYTES) ? STATUS_TOO_LARGE : STATUS_BAD_DATA;
        } else {
            memcpy(receiver->framebuffer + offset, payload + 4u, chunk_length);
            receiver->next_offset = (uint16_t)(receiver->next_offset + chunk_length);
        }
        send_ack(receiver, sequence, status);
        return;
    }

    send_ack(receiver, sequence, STATUS_BAD_COMMAND);
}

void oled_stream_rx_init(OLED_StreamReceiver *receiver,
                         uint8_t *framebuffer,
                         size_t framebuffer_capacity,
                         uint16_t width,
                         uint16_t height,
                         oled_stream_refresh_fn refresh,
                         oled_stream_uart_write_fn uart_write,
                         void *user)
{
    if (receiver == NULL) return;
    memset(receiver, 0, sizeof(*receiver));
    receiver->framebuffer = framebuffer;
    receiver->framebuffer_capacity = framebuffer_capacity;
    receiver->width = width;
    receiver->height = height;
    receiver->frame_bytes = ((size_t)width * (size_t)height) / 8u;
    receiver->refresh = refresh;
    receiver->uart_write = uart_write;
    receiver->user = user;
}

void oled_stream_rx_feed(OLED_StreamReceiver *receiver, const uint8_t *bytes, size_t length)
{
    size_t i;
    if (receiver == NULL || bytes == NULL) return;

    for (i = 0u; i < length; ++i) {
        uint8_t byte = bytes[i];
        if (byte == 0u) {
            if (receiver->rx_length != 0u) {
                process_packet(receiver, receiver->rx_length);
                receiver->rx_length = 0u;
            }
        } else if (receiver->rx_length < sizeof(receiver->rx_encoded)) {
            receiver->rx_encoded[receiver->rx_length++] = byte;
        } else {
            receiver->rx_length = 0u;
        }
    }
}
