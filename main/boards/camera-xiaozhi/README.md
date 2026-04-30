# Camera Xiaozhi

## 简介

这是为当前 fork 项目新增的独立板型骨架，目标是承载“摄像头小智”硬件的后续移植工作。

当前版本基于以下已确认信息建立：

- MCU: `ESP32-S3`
- Flash / PSRAM: `16MB Flash + 8MB PSRAM`
- LCD 驱动: `ST7789`
- 摄像头模组: `GC0308`
- 音频链路: `ES7210 + ES8311 + NS4150B`
- 引脚基线：来源于供应商提供的 `config.h`（已与原理图及 `zhengchen-cam` 板型核对一致）

## 当前实现范围

- 独立板型目录，编译配置独立。
- 适配核心外设：
  - `PCA9557` 控制 LCD_CS / PA_EN / DVP_PWDN
  - `ST7789` SPI 屏，支持横/竖屏切换（NVS `lcd_display/lcd_mode`）
  - `BoxAudioCodec`（ES7210 + ES8311 + NS4150B 功放）
  - `GC0308` DVP 摄像头（FRAMESIZE_VGA）
  - `PowerManager`（GPIO47 充电检测，ADC_CHANNEL_9/GPIO10 电池电量）
  - 三按键：BOOT(GPIO0) / 音量+(GPIO3) / 音量-(GPIO46)
- 未接入触摸逻辑。

## 注意事项

- DVP 引脚与 I2S 引脚无重叠，音频和摄像头可独立工作。

## 编译

推荐使用发布脚本：

```bash
python ./scripts/release.py camera-xiaozhi
```

如需手动编译，请在 `menuconfig` 中选择：

```text
Xiaozhi Assistant -> Board Type -> Camera Xiaozhi
```
