# IMGSHOW

ESP32 + ST7735 (128x128) 桌面信息屏。固件包含时钟、天气、股票、图片、视频、计时器和表情等页面，使用四个物理按钮操作。

## 编译

安装 PlatformIO 和项目 `platformio.ini` 中列出的库。在 `imgshow/` 下复制 `config.example.h` 为 `config.h`，修改自己的 Wi-Fi、热点密码和 Web 管理密码，然后运行 `pio run`。也可以在 Arduino IDE 中打开 `imgshow/imgshow.ino`，使用 ESP32 2.0.x 板包及 Adafruit GFX、Adafruit ST7735、ArduinoJson 7 库。

`config.h`、生成的媒体素材和本机参考文件均不会进入仓库。不提供受版权或商标保护的品牌图片；没有自备的 `imgshow/smoke_imgs.h` 时，图片页显示占位提示。需要图库时可自行生成该文件，数组格式见 `imgshow/app.h` 中的使用方式。

## 引脚

| 设备 | GPIO |
| --- | --- |
| ST7735 SCK / MOSI / RST / DC / CS | 18 / 23 / 4 / 2 / 5 |
| UP / DOWN / OK / BACK | 32 / 33 / 25 / 26 |

默认配网热点和 Web 管理凭据仅为示例值，上电前必须修改。硬件接线、供电与固件分区需要按自己的模块校对。

## 许可

本仓库的原创源码使用 MIT 许可证，见 `LICENSE`。
