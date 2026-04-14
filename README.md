# x264 H.264/AVC 编码器

静态链接的 x264 编码器，集成多种功能支持。

## 特性

- **静态链接** - 无外部依赖，开箱即用
- **多 CPU 优化** - 支持 x86-64、Haswell、Skylake、Alderlake、Raptorlake、Arrow Lake、Zen 2/3/4/5 等多款 CPU
- **高比特深度** - 支持 8-bit、10-bit 和全深度模式
- **PGO 优化** - Profile-Guided Optimization 提升性能
- **LTO 优化** - Link-Time Optimization 减少体积、提升速度
- **mimalloc** - 微软高性能内存分配器

## 依赖库

| 组件 | 版本 | 说明 |
|------|------|------|
| FFmpeg | n8.1 | 输入/输出支持 |
| mimalloc | v3.2.8 | 高性能内存分配器 |
| L-SMASH | `04e39f1` (2026-04-05) | MP4/MOV 输出支持 |
| obuparse | `c2156b4` (2026-02-22) | AV1 解析支持 |
| ffms2 | `0fa01d0` (2026-04-09) | 视频输入支持 |

## 编译环境

- **操作系统**: Windows
- **工具链**: MSYS2 CLANG64
- **编译器**: Clang/LLVM
- **汇编器**: NASM

## 下载

前往 [Releases](https://github.com/neil1123-cc/x264/releases) 页面下载最新版本。

## 使用方法

```bash
# 基本编码
x264 input.y4m -o output.mkv

# 使用预设
x264 --preset slower --tune film input.y4m -o output.mkv

# 指定码率
x264 --bitrate 5000 input.y4m -o output.mkv
```

## 许可证

x264 采用 [GNU GPL](https://www.gnu.org/licenses/gpl-2.0.html) 许可证。

## 致谢

- [x264](https://www.videolan.org/developers/x264.html) - VideoLAN
- [FFmpeg](https://ffmpeg.org/) - FFmpeg 项目
- [mimalloc](https://github.com/microsoft/mimalloc) - Microsoft
- [L-SMASH](https://github.com/vimeo/l-smash) - Vimeo
