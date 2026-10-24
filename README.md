# ST73 // LCD — STM32 + ST7735

面向 STM32F103 + ST7735 的浏览器媒体转换和串口投屏工具。此仓库只包含这个独立工具，不复制原私有仓库中的 ESP32 工程。

## 在线体验

部署后地址：<https://themarter.github.io/stm32-st7735/>

## 功能

- 本地转换 PNG、JPEG、WebP、GIF 与短视频为 RGB565。
- 支持 128×160、160×128、128×128、160×80 画幅以及四种显示方向。
- 16×16 tile 差分、RLE 压缩、离线导出 `.st73` 媒体包。
- Web Serial 实时传输，默认 921600 baud、8-N-1。
- 纯 C 协议接收端：COBS、CRC-32、握手/ACK、RGB565 tile 解码。无需固定 HAL 或 LL。

所有媒体转换均在浏览器本地进行。实时显示还要求 STM32 烧录并集成本仓库的接收模块，以及你现有的 ST7735 绘图回调。

## 本地运行网页

在仓库根目录执行：

    py -m http.server 8000

然后用最新版 Chrome/Edge 打开 <http://localhost:8000>。Web Serial 要求 HTTPS 或 localhost；浏览器会要求你主动选择串口。

## 接线

请使用 3.3 V 逻辑电平 USB-TTL，并共地：

| USB-TTL | STM32F103C8 |
| --- | --- |
| TX | PA10 / USART1_RX |
| RX | PA9 / USART1_TX |
| GND | GND |

UART 两侧均设为 921600、8-N-1。若模块无法稳定使用此波特率，请同时修改网页的 `state.baud` 与 MCU UART 配置。

LCD 接线仍与你之前确认的一致：SCL=PB10、SDA=PB11、DC=PB0。这些 LCD 引脚与 USB-TTL 的串口引脚用途不同；以你真实模组和驱动为准，勿将它们当作 UART 接线。

## 集成接收端

- `firmware/st73_receiver.h` / `.c`：可移植协议接收实现。
- `firmware/PROTOCOL.md`：串口 packet、控制命令、tile 格式。
- `firmware/test_receiver.c`：主机侧协议测试。

将两个 receiver 文件加入 Keil 工程，创建静态 `ST73_Receiver` 实例，调用 `st73_receiver_init()`，并实现 `draw_tile`、可选的 `set_orientation` 和 `uart_write` 回调。示例里的 `ST7735_DrawRGB565` 与 `UART1_Write` 都是占位函数名，必须换成你的真实驱动 API。UART 接收中断/DMA 只负责收集字节，在主循环调用 `st73_receiver_feed()`，不要在中断里运行阻塞式 LCD 绘图。

接收端不缓存整幅屏幕。128×160 的 RGB565 framebuffer 需要约 40 KiB，已超过 STM32F103C8 的 20 KiB SRAM；模块会解压一个 tile 并立即交给绘图回调。

## 协议自测

电脑装有 GCC 时，在仓库根目录运行：

    gcc -std=c99 -Wall -Wextra -Werror -pedantic firmware/st73_receiver.c firmware/test_receiver.c -o st73_test.exe
    .\st73_test.exe

测试覆盖 COBS/CRC、HELLO、分片 UART 输入、RGB565 tile、ACK 和 FRAME_END。主机测试通过不代表已在实体屏幕上验证；实际使用前仍需将回调接入你自己的 LCD 驱动并联调。
