# ST73 // LCD

把一张图片送进 **STM32F103C8 + ST7735** 的彩屏。

只有这一件事。不做 GIF，不做视频，不做实时动画。

在线：<https://themarter.github.io/stm32-st7735-media/>

---

## 它怎么工作

```
图片 ──▶ 浏览器转 RGB565 ──▶ 串口(USB-TTL) ──▶ STM32 ──▶ DIO/CLK ──▶ ST7735
        (canvas, 本地完成)      921600 8-N-1     收字节    软件 SPI     亮屏
```

网页不碰引脚，它只把像素倒进串口。真正打时序的是 MCU：`PB10` 出时钟、`PB11` 出数据，一个字节 8 个时钟推上去。

## 接线

屏幕和串口是两组线，别混。

| ST7735 | STM32F103C8 | 作用 |
| --- | --- | --- |
| SCL / CLK | **PB10** | 时钟，GPIO 模拟 |
| SDA / DIO | **PB11** | 数据，GPIO 模拟 |
| DC | PB0 | 命令 / 数据 |
| RES | PB1 | 复位 |
| CS | PA7 | 片选 |
| BL / LED | PA6 | 背光 |

| USB-TTL | STM32F103C8 |
| --- | --- |
| TX | PA10（USART1_RX） |
| RX | PA9（USART1_TX） |
| GND | GND（必须共地） |

屏幕侧是 **SPI，不是 I²C**，尽管丝印写着 SCL/SDA。串口 921600、8-N-1。

## 网页怎么用

1. 拖一张图进去 → 自动裁成屏幕画幅、转 RGB565。
2. 选模组尺寸和方向。
3. 点「连接串口」，选你的 CH340 → 点「发送到屏幕」。

大约 0.5 秒一帧。发完会等 MCU 回 3 字节确认；收不到只是提示，不影响发送。

需要 Chrome 或 Edge，且页面跑在 HTTPS 或 localhost 下。

## 固件怎么用

`firmware/` 里是完整的四个文件：

| 文件 | 作用 |
| --- | --- |
| `st7735.c/.h` | ST7735 驱动，软件 SPI，流式写屏 |
| `st73_receiver.c/.h` | 串口字节 → 帧头解析 → 像素流，不缓存整帧 |
| `main_example.c` | USART1 + 环形队列 + 主循环，可直接替换工程里的 `main.c` |
| `test_receiver.c` | 主机侧自测，不需要板子 |

Keil 工程里放法：驱动和接收器进 `my_lib/`，`main_example.c` 当作 `user/main.c`。文件名保持 `st73_receiver.c` / `st7735.c`，工程的 `.uvprojx` 不用动。

改波特率就改 `main_example.c` 顶部的 `LINK_BAUD`，网页那边也要选一样的。

## 出问题按这个顺序查

**第一步：上电有没有图案**

上电后屏幕应该出现：四角四个彩色方块（红、绿、蓝、黄）+ 中间白色十字。

- 有 → DIO/CLK/DC/RES/CS 接线和初始化都是对的，问题在后面。
- 没有 → 先别管串口。查背光(BL)、共地、RES，或者模组是不是 128×160 之外的其他尺寸（那样初始化参数要改）。

**第二步：串口通不通**

网页能连上串口，但发送后屏幕没反应：

- 看网页日志里有没有「设备回执 OK」。有回执说明双向都通。
- 没回执但字节都发出去了 → 大概率是 STM32 的 TX（PA9）没接到 USB-TTL 的 RX。回执是单向诊断，不接也能显示。
- 检查波特率和固件里的 `LINK_BAUD` 是否一致。

**第三步：图像对不对**

- 颜色是反的 → 模组是 RGB 屏不是 BGR，改 `st7735.c` 里 `madctl[]` 的 `0x08` 位。
- 画面偏移几个像素 → 模组有 offset，需要在 `LCD_SetWindow` 里加偏移量。
- 方向不对 → 网页切方向就行，不用重烧。

## 协议

见 [`firmware/PROTOCOL.md`](firmware/PROTOCOL.md)。一句话：`A5 5A 01 W_LO W_HI H_LO H_HI ORI SUM` + `W*H*2` 字节 RGB565（高字节在前）。

## 自测

装了 gcc 就跑：

```
gcc -std=c99 -Wall -Wextra -Werror -pedantic firmware/st73_receiver.c firmware/test_receiver.c -o st73_test
./st73_test
```

32 项断言，覆盖整帧/逐字节/噪声同步/坏校验/尺寸拒收/连发两帧/半帧复位/像素逐字节一致。主机测试通过不等于屏幕能亮，显示还取决于模组版本和接线。

## 和上一版的关系

上一版想做 GIF 和视频实时投屏，于是堆了 COBS、CRC32、16×16 tile 差分、RLE、双向 ACK、帧序列号——功能没验证通，复杂度先起来了。

这一版全部删掉，回到「发一张图、点亮一块屏」。等这张图稳定显示了，再谈动画。
