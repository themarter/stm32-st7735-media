# OLED STREAM Protocol v1

本协议按 SSD1306 128×64、单色 1-bit 页缓冲设计。它不是 ST7735/RGB565 协议。

## 物理链路

- PC browser → USB-TTL → STM32 USART1：115200 baud，8-N-1。
- STM32 → SSD1306 OLED：I²C，由现有 OLED 驱动负责刷新。
- 一帧显示内容固定 1,024 字节；byte index 为 page × 128 + x；bit 0 对应该 page 顶部像素 y % 8 == 0。这与工程中 OLED->pBuffer[x + y/8*128] 的存储方式一致。

## Wire packet

raw packet：

- channel: uint8（0=控制，1=画面数据，2=响应）
- sequence: uint16 LE
- payload: 通道 1 的 payload 最多 260 字节（4 字节帧 ID/偏移头 + 256 字节画面数据），其他控制 payload 更短
- crc32: uint32 LE，CRC-32/ISO-HDLC，反射多项式 0xEDB88320、初值/final XOR 均为 0xFFFFFFFF；校验 channel、sequence、payload

raw packet 经 COBS 编码，前后各加 0x00 分隔。ACK 使用请求相同的 sequence，并从 channel 2 返回。

## Commands

### HELLO — channel 0

Request payload: [0x01].

Response payload, 6 bytes: [0x81, width_u16le, height_u16le, version]. 当前固定为 [0x81, 128, 0, 64, 0, 1]。

### FRAME_BEGIN — channel 0

Payload exactly 9 bytes:
- byte 0: 0x10
- bytes 1..2: frame ID uint16 LE
- bytes 3..4: width uint16 LE (128)
- bytes 5..6: height uint16 LE (64)
- bytes 7..8: frame delay milliseconds uint16 LE

成功开始新帧后应答 [0x80, 0]。如果已经有一帧正在接收，新 FRAME_BEGIN 会以新 frame ID 重新开始接收。

### FRAME_DATA — channel 1

Payload = frame ID uint16 LE + offset uint16 LE + 1..256 raw framebuffer bytes。偏移必须等于下一段尚未接收的位置，从 0 顺序递增；不可跳过、重叠或越过 1024。

正常应答 [0x80, 0]。

### FRAME_END — channel 0

Payload exactly [0x11, frame ID uint16 LE]。只在已经接收到连续完整的 1,024 字节时成功。接收器先调用 display refresh 回调，再回复 [0x80, 0]。

### SESSION_STOP — channel 0

Payload [0x12]。结束投屏会话，使主程序可以恢复其它 OLED 页面；回复 [0x80, 0]。

## ACK status

- 0: success
- 1: unknown command or unsupported channel
- 2: no active frame or frame ID mismatch
- 3: wrong geometry or malformed size
- 4: invalid offset, sequence of data, CRC payload, or incomplete frame
- 5: frame data exceeds the 1,024-byte page buffer

CRC 错误包不回复 ACK；浏览器会超时提示通信失败。

## Performance

默认 4 FPS。每帧经 UART 发送 1,024 字节，然后 STM32 调用现有 OLED_SendBuffer() 通过 I²C 刷新。I²C 刷新耗时、UART 波特率和 USB-TTL 延迟决定实测帧率；网页并不直接控制 I²C 引脚。