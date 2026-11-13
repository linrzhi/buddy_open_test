# Buddy

[English](README.md) | 简体中文

> AI 编程助手：请先阅读 [AGENTS.md](AGENTS.md)。

本仓库是 Buddy 的开源可定制版本。Buddy 是一个 USB 桌面设备（ESP32-S3），带一颗 **RGB 灯**，内置**电磁阀**可以把顶部的蘑菇或其他 3D 打印件顶起再落回（弹跳）。
本仓库用开源固件替换官方固件（预编译在 `firmware/bin`，源码在 `firmware/`），通过 USB 由 `buddy/` 中的
`buddy` 命令行和 Python 库控制。自定义功能写成 `apps/` 中的主机脚本；脚本做不到时再修改固件。

## 使用

需要 Python 3.9+。

```bash
pip install -e .              # 在仓库目录执行
buddy setup                   # 首次使用：安装开源固件并检查设备
buddy led breathe green
buddy jump 2
buddy notify meeting          # 蓝灯闪 + 弹跳三下
buddy remind 25m 休息一下
buddy led off
```

```python
from buddy import Buddy

with Buddy() as b:
    b.notify("meeting")
    b.wait(60)
    b.led_off()
```

## 文档

- [AGENTS.md](AGENTS.md)：命令、Python API 与规则（英文）
- [docs/recipes.md](docs/recipes.md)：应用食谱（英文）
- [docs/protocol.md](docs/protocol.md)：串口协议（英文）
- [docs/firmware.md](docs/firmware.md)：固件源码与编译（英文，ESP-IDF v6.0）
- [docs/official-firmware.md](docs/official-firmware.md)：官方固件与开源固件互相切换（英文）

## 许可证

固件与软件：Apache-2.0；`firmware/official/` 中的官方固件镜像除外。
