# 开源库迁移计划 - 文档导航

本目录记录将 robot-esp32 工程中 Espressif 闭源预编译静态库替换为开源 C/C++ 实现的完整计划。
目标：第一步在 ESP32-S3 上原地替换验证，第二步实现跨平台（Linux/ARM）移植。

## 文档列表

| 文件 | 内容 |
|------|------|
| [01-lib-analysis.md](./01-lib-analysis.md) | 当前工程所有预编译 `.a` 库清单、调用位置、接口层分析 |
| [02-oss-selection.md](./02-oss-selection.md) | 各模块开源替代库评估：许可证、资源占用、可行性 |
| [03-migration-roadmap.md](./03-migration-roadmap.md) | 分阶段实施计划：受影响文件、实施步骤、验证方法 |
| [04-api-mapping.md](./04-api-mapping.md) | 闭源 API 与开源 API 逐函数对照表（编码阶段直接参考） |
| [05-runtime-audio-stability.md](./05-runtime-audio-stability.md) | 开源音频链路运行问题、实时性修复与测试结果 |

## 背景与动机

当前工程音频处理、语音唤醒、Opus 编解码、JPEG 图像处理均依赖 Espressif 闭源预编译 `.a` 静态库，
这些库仅支持 ESP32 系列芯片（Xtensa LX7 / RISC-V 指令集），无法在 Linux/ARM/x86 等平台编译运行。

受影响的预编译库共 8 个，分布在 3 个 managed component 中：

- `espressif/esp-sr` — AFE/AEC/NS/VAD/WakeNet（语音算法核心）
- `espressif/esp_audio_codec` — Opus 编解码器
- `espressif/esp_audio_effects` — 采样率转换
- `espressif/esp_new_jpeg` — JPEG 编解码

## 迁移策略

```
阶段一（ESP32-S3 原地替换）
  ├─ 用开源 C/C++ 库替换闭源 .a 库
  ├─ 新建 open_* 适配类，通过 Kconfig 开关与原实现并存
  └─ 逐 Phase 验证功能等价性（通话质量、画质、延迟）

阶段二（跨平台移植）
  ├─ 抽取 platform.h 替换 FreeRTOS/ESP-IDF 专属 API
  ├─ 支持 Linux CMake 独立构建
  └─ 单元测试验证算法输出一致性
```

## 执行优先级

最小路径（优先保证跨平台可编译）：

```
Phase 1（Opus）→ Phase 2（重采样）→ Phase 3（JPEG）→ Phase 7（AFE 流水线）→ Phase 9（跨平台）
```

Phase 8（唤醒词 TFLite Micro）高风险，不阻塞主线，可独立并行推进。
