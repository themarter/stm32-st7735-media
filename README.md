# ST73 // LCD — STM32F103 + ST7735

浏览器端图片、GIF、视频转 RGB565，并通过 USB-TTL 串口发送给 **STM32F103C8 + ST7735**。网页只负责媒体处理和串口传输，不直接控制 LCD 引脚。

## 在线体验

<https://themarter.github.io/stm32-st7735-media/>

## 当前功能

- 本地转换 PNG、JPEG、WebP、GIF 与短视频为 RGB565。
- 支持 128×160、160×128、128×128、160×80 画幅和四种方向。
- 16×16 tile 差分、RLE 压缩、离线导出 `.st73` 媒体包。
- Web Serial 实时传输，默认 921600 baud、8-N-1。
- C 接收协议：COBS、CRC-32、握手/ACK、RGB565 tile 解码；不缓存 40 KiB 的整屏帧缓冲，适配 STM32F103C8 的 20 KiB SRAM。

## 你的实际接线

LCD 与 USB-TTL 是两组不同的线路。

| ST7735 | STM32F103C8 |
| --- | --- |
| SCL / CLK | PB10 |
| SDA / DIO | PB11 |
| DC | PB0 |
| RES | PB1 |
| CS | PA7 |
| BL / LED | PA6 |

屏幕驱动使用 PB10/PB11 GPIO 模拟 SPI 时序；**不是 I²C，也不是 ESP32**。

USB-TTL（3.3 V 逻辑电平、必须共地）：

| USB-TTL | STM32F103C8 |
| --- | --- |
| TX | PA10 / USART1_RX |
| RX | PA9 / USART1_TX |
| GND | GND |

串口设置为 921600、8-N-1。网页的串口只传图像数据，STM32 再通过 PB10/PB11 驱动 ST7735。

## Keil 工程集成

本次已把以下代码加入本地 Keil 工程 `C:\Users\11695\Desktop\stm32project\模块`：

- `my_lib/st7735.c`、`my_lib/st7735.h`：ST7735 初始化、RGB565 矩形/像素绘制、方向切换。
- `my_lib/st73_receiver.c`、`my_lib/st73_receiver.h`：串口投屏协议接收、分块解码和 ACK。
- `user/main.c`：USART1 921600 RX 中断 + 4096 字节环形队列；中断只收字节，主循环解析协议并绘制，避免在中断里阻塞刷屏。

没有收到网页串口数据时，主程序运行彩色方块移动测试；收到串口数据后，移动测试停止，让媒体画面控制屏幕。断开投屏后会保留最后一帧。

## 构建与验证状态

当前已用 ARMCC 5.06 编译主程序、ST7735 驱动和接收器，并成功链接生成：

- `Objects/template.axf`
- `Objects/template.hex`

这是软件构建验证，不等于实机显示验证。当前电脑检测到 USB-SERIAL CH340K（COM6），没有检测到 ST-LINK；因此尚未把 HEX 烧录到芯片，也未声称屏幕已经显示成功。原始主程序和工程文件保存在 `backup_pre_st7735_20261024`，动画测试版保存在 `backup_pre_stream_20261024`。

## 本地运行网页

在仓库根目录执行：

    py -m http.server 8000

然后用最新版 Chrome/Edge 打开 <http://localhost:8000>。Web Serial 需要 HTTPS 或 localhost，点击“连接 STM32”后由你选择串口。

## 协议自测

电脑装有 GCC 时，在仓库根目录运行：

    gcc -std=c99 -Wall -Wextra -Werror -pedantic firmware/st73_receiver.c firmware/test_receiver.c -o st73_test.exe
    .\st73_test.exe

主机协议测试通过不代表已在实体屏幕上验证。显示效果还会受到 ST7735 模组版本、可视区域偏移和屏幕方向设置影响。
