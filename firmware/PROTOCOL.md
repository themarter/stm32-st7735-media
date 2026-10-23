# ST73 Protocol v1

本文对应本目录中的 app.js 和 st73_receiver.c。多字节数值均为 little-endian，除非标明 RGB565 像素数据为 big-endian。串口 8-N-1，默认 921600 baud。

## Wire packet

每个 packet 结构为：

- channel: uint8（0=控制，1=tile 数据，2=设备事件/响应）
- sequence: uint16 LE
- payload: 0 至 1800 字节（由 web sender 限制）
- crc32: uint32 LE；对 channel、sequence 和 payload 计算 CRC-32/ISO-HDLC（反射多项式 0xEDB88320，初值 0xFFFFFFFF，最终 XOR 0xFFFFFFFF）

raw packet 经 COBS 编码，前后各加 0x00 作为分隔符。接收端遇到 0x00 就结束当前编码片段；连续分隔符产生的空片段忽略。响应复用请求的 sequence，写入 channel 2，然后以同样格式返回。

## Payload

### HELLO — channel 0, command 0x01

Request payload: one byte 01.

Response payload: 6 bytes:

- 0: 0x81 (HELLO event)
- 1-2: panel width uint16 LE
- 3-4: panel height uint16 LE
- 5: protocol version (currently 1)

### FRAME_BEGIN — channel 0, command 0x10

Request payload is exactly 12 bytes:

- 0: 0x10
- 1-2: frame ID uint16 LE
- 3-4: logical frame width uint16 LE
- 5-6: logical frame height uint16 LE
- 7: orientation 0/1/2/3
- 8-9: frame delay in ms uint16 LE
- 10-11: expected changed tile-record count uint16 LE

On success, ACK payload is [0x80, 0]. Error ACK is [0x80, status].

### TILE_DATA — channel 1

Payload begins with frame ID uint16 LE and chunk index uint16 LE, followed by one or more complete tile records. Chunk index starts at 0 for every frame and must increase by one. Sender never splits a tile record across packets.

Each record:

- x, y, width, height: 1 byte each
- codec: 0=raw, 1=RLE
- data length: uint16 LE
- data bytes

Raw codec stores width × height RGB565 pixels, each pixel high byte then low byte. RLE codec stores triples: run_minus_1, pixel_hi, pixel_lo; run length is run_minus_1 + 1. Decompression must yield exactly width × height pixels. A tile is at most 16×16 pixels and tile origins are multiples of 16; final row/column tiles may be smaller.

Every accepted packet gets ACK [0x80, 0].

### FRAME_END — channel 0, command 0x11

Request payload is exactly 3 bytes:

- 0: 0x11
- 1-2: frame ID uint16 LE

Success requires that the number of accepted tile records match the count declared in FRAME_BEGIN. ACK payload is [0x80, 0].

## ACK status codes

- 0: success
- 1: unknown command or unsupported channel
- 2: no active frame / wrong frame ID / invalid state
- 3: invalid frame dimensions, orientation, or tile rectangle
- 4: malformed payload, chunk order, codec or RLE data
- 5: tile count exceeds the count declared in FRAME_BEGIN
- 6: FRAME_END tile count does not match declaration

A corrupted wire packet (including CRC mismatch) is dropped without ACK; the browser will time out and report a receive/connection problem.

## STM32 implementation notes

The receiver module expects integration callbacks and a display driver that can write rectangular windows. It does not manage SPI, ST7735 offsets, rotation registers, reset timing or backlight. The default hello dimensions are the configured panel dimensions, so the browser accepts those dimensions or their swapped orientation. Make sure the chosen module's visible area and orientation match the coordinate conventions of your ST7735 driver.

The browser renders the first frame as a full update; later frames only include changed 16×16 tiles. At the start of each animation loop the first frame is sent as a full update again, so the receiver doesn't need a framebuffer or tile cache.
