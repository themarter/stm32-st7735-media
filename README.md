# OLED // STREAM — STM32 + SSD1306 I²C

这是按本机 Keil 工程现有驱动重新适配的版本，不是 ST7735 彩屏驱动。工程中的 my_lib/oled.c 明确使用 SSD1306 控制命令、128×64 单色缓冲和 I²C 写回调；每帧正好 1,024 字节。

## 网页能做什么

- 在浏览器本地导入 PNG、JPEG、WebP、GIF 和短视频。
- 转成 128×64 单色位图，支持阈值、反色、铺满裁切/完整适配。
- GIF/视频最高 120 帧，按所选帧率采样。
- 经 Web Serial 把帧数据送给 STM32；导出 .oled 离线媒体包。
- 接收端直接使用用户现有 OLED 驱动的 pBuffer 和 OLED_SendBuffer()，不使用 RGB565，也不假设 ST7735。

## 数据路径

网页本身不能直接操纵电脑以外的 STM32 GPIO/I²C 引脚。这里分成两段：

1. 浏览器通过 USB-TTL 的 UART 与 STM32 通信，协议波特率 115200、8-N-1。
2. STM32 将网页发来的 1,024 字节单色页缓冲交给现有 OLED 驱动，由 OLED 驱动经 I²C 刷屏。

## 接线与引脚

电脑到 STM32 用 3.3 V USB-TTL，双方共地：

| USB-TTL | STM32F103 |
| --- | --- |
| TX | PA10 / USART1_RX |
| RX | PA9 / USART1_TX |
| GND | GND |

USART1 两侧设为 115200、8-N-1；如果你的程序仍是 USART1_Init(9600)，网页会握手失败，需改为 115200。投屏时不要继续向同一 UART 输出 printf 调试信息，否则调试文本会混入二进制数据流。

OLED 的 I²C 侧按你之前描述的 CLK/SCL=PB10、DIO/SDA=PB11 来写集成示例：STM32F103 上这对应 I²C2。PB0/DC 不属于 I²C OLED 的信号线，不能再当作 OLED 的 DC 用。若你实际已经接在 I²C1 的 PB6/PB7，则应保留 I²C1，把下面 OLED 回调改回 I2C1；不要在软件中选 I²C2、却把屏幕接到 I²C1 引脚。

你当前 main.c 的 OLED_I2C_WriteCB 使用 I2C1，而 I2C1_Hardware_Init 把 PB6/PB7 配成 SCL/SDA；MPU6050 也使用 I2C1。若 OLED 接线要用 PB10/PB11，应初始化 I²C2，并让 OLED 的回调调用 My_I2C_SendBytes(I2C2, addr, pdata, size)，MPU6050 可以继续留在 I²C1。

## 接收端集成

文件：

- firmware/oled_stream_rx.h
- firmware/oled_stream_rx.c
- firmware/test_oled_stream_rx.c
- firmware/PROTOCOL.md

这是一层接收协议，不是整套 Keil 工程。将 C/H 加入现有 Keil 项目，给它传入 OLED_Init() 创建的 oled.pBuffer；刷新回调调用 OLED_SendBuffer(&oled)，UART 回调通过 USART1 回发 ACK。

初始化示意：

    static OLED_StreamReceiver stream_rx;

    static void OLED_StreamRefresh(void *user)
    {
        OLED_TypeDef *oled = (OLED_TypeDef *)user;
        (void)OLED_SendBuffer(oled);
    }

    static void OLED_StreamUartWrite(void *user, const uint8_t *data, size_t len)
    {
        size_t i;
        (void)user;
        for (i = 0; i < len; ++i) {
            while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {}
            USART_SendData(USART1, data[i]);
        }
    }

    /* oled_init.i2c_write_cb 与 OLED_Init() 成功后执行 */
    oled_stream_rx_init(&stream_rx, oled.pBuffer, 1024u, 128u, 64u,
                        OLED_StreamRefresh, OLED_StreamUartWrite, &oled);

    /* 从 USART1 接收到一个或多个字节时，把它们传给接收器 */
    oled_stream_rx_feed(&stream_rx, rx_bytes, rx_count);

实际集成还必须及时调用 feed：你现有主程序在 MPU6050 采样循环里有 Delay(200)，不能只在每 200 ms 才轮询 UART；应使用 USART1 RX 中断/DMA 环形缓冲区，并在主循环及时喂给解析器。OLED 接收模式期间，也要暂停原来每 200 ms 重绘角度/温度的代码，否则原界面会覆盖投屏帧。退出投屏模式命令会清除接收器的 session_active 标记，主程序再恢复原来的 OLED 页面。

若 OLED 实际使用 PB10/PB11，请初始化 I²C2。初始化示意（STM32 标准外设库）：

    static void I2C2_OLED_Init(void)
    {
        GPIO_InitTypeDef gpio;
        I2C_InitTypeDef i2c;
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
        RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);

        gpio.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_11;
        gpio.GPIO_Mode = GPIO_Mode_AF_OD;
        gpio.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(GPIOB, &gpio);

        I2C_DeInit(I2C2);
        i2c.I2C_Mode = I2C_Mode_I2C;
        i2c.I2C_DutyCycle = I2C_DutyCycle_2;
        i2c.I2C_OwnAddress1 = 0;
        i2c.I2C_Ack = I2C_Ack_Enable;
        i2c.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
        i2c.I2C_ClockSpeed = 100000;
        I2C_Init(I2C2, &i2c);
        I2C_Cmd(I2C2, ENABLE);
    }

    static int OLED_I2C_WriteCB(uint8_t addr, const uint8_t *pdata, uint16_t size)
    {
        return My_I2C_SendBytes(I2C2, addr, pdata, size);
    }

这里 OLED_SLAVE_ADDR=0x78 与现有驱动的“左对齐地址”约定一致，不要未经核对就改成 0x3C。

## 协议自测

电脑有 GCC 时，在仓库根目录运行：

    gcc -std=c99 -Wall -Wextra -Werror -pedantic firmware/oled_stream_rx.c firmware/test_oled_stream_rx.c -o oled_test.exe
    oled_test.exe

主机测试覆盖 COBS/CRC、握手、拆分 UART 输入、1024 字节页缓冲接收、帧刷新回调、ACK 与退出投屏。通过主机测试不代表已经在实物 OLED 上联调。