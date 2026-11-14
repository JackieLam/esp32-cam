# ESP32-S3 AI CAM (DFRobot DFR1154) Demos

硬件文档: https://wiki.dfrobot.com.cn/SKU_DFR1154_ESP32_S3_AI_CAM

| 目录 | 内容 |
|---|---|
| `01_blink/` | 板载 LED (GPIO3) 闪烁 + 串口打印，验证环境 |
| `02_camera_stream/` | 摄像头 MJPEG 实时画面网页，`/` 看视频，`/capture` 拍照 |
| `03_person_detect/` | 实时画面 + 识别有没有人（浏览器端 TensorFlow.js COCO-SSD，需联网、建议 Chrome；改页面里的 `TARGET` 可换成狗/猫/车等） |

## 环境

```bash
brew install arduino-cli
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32   # 已验证 3.3.12
```

板子设置（Arduino IDE 同名选项）：ESP32S3 Dev Module / USB CDC On Boot: Enabled / PSRAM: OPI PSRAM / Flash 16MB / 分区 16M Flash (3MB APP/9.9MB FATFS)

## 编译 & 烧录

```bash
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
PORT=$(ls /dev/cu.usbmodem* | head -1)

# 摄像头 demo 首次需要配置 WiFi（仅 2.4GHz）
cp 02_camera_stream/secrets.h.example 02_camera_stream/secrets.h   # 然后编辑（03_person_detect 同理）

arduino-cli compile -u -p "$PORT" -b "$FQBN" 02_camera_stream
arduino-cli monitor -p "$PORT" -c baudrate=115200   # 按 RST 看 IP
```

烧录失败 / 端口消失：按住 BOOT，按一下 RST，松开 BOOT，再烧录。

## 清晰度 / 帧率

GC2145 无硬件 JPEG，靠 CPU 软件编码，分辨率越高帧率越低。`03_person_detect` 网页上可以直接切换（切换时摄像头会重新初始化，画面停顿约 1 秒）；开机默认值在 `.ino` 里的 `DEFAULT_SIZE` / `DEFAULT_QUALITY`。`02_camera_stream` 改 `FRAME_SIZE` / `JPEG_QUALITY` 后重新烧录。

实测单路帧率（ESP32 core 3.3.12）：

| 分辨率 | 画质 75 | 画质 85 | 每帧大小(q75) |
|---|---|---|---|
| 320×240 QVGA | 9.5 fps（q60: 10 fps） | — | 7 KB |
| 480×320 HVGA（03 默认） | 4.9 fps | 4.2 fps | 11 KB |
| 640×480 VGA | 2.8 fps | 2.8 fps | 19 KB |
| 800×600 SVGA | 1.9 fps | 1.4 fps | 28 KB |
