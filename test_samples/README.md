# H.264 / Opus 测试样本

本目录保留历史来源、校验和与探测记录。媒体二进制不随 Git 仓库发布，
也不适用项目的 MIT 许可证；若自行下载，请核对来源许可。
默认播放示例使用 FFmpeg 自动生成的测试画面，不依赖这些文件。

下载日期：2026-09-25。原始文件保持不变；H.264 裸流由 MP4 无损提取，没有重新编码。

## H.264

| 文件（h264/） | 分辨率 | 视频时长 | 原始帧率 | 编码 |
| --- | --- | --- | --- | --- |
| road_360p.mp4 / road_360p.h264 | 640×360 | 5.70 秒 | 30 fps | H.264 High |
| buses_720p.mp4 / buses_720p.h264 | 1280×720 | 10.11 秒 | 30000/1001 fps | H.264 High |
| traffic_1080p.mp4 / traffic_1080p.h264 | 1920×1080 | 15.48 秒 | 30000/1001 fps | H.264 High |

来源：[Samplelib](https://samplelib.com/sample-mp4.html)。MP4 同时含 AAC 音轨；`.h264` 仅含视频，是带起始码的 Annex B 裸流。

裸流没有 MP4 的逐帧时间戳，ffprobe 的裸流 r_frame_rate 可能显示为原始帧率的两倍。发送或封装时请使用上表原始帧率。这三份均为 High profile，不适合用来验证仅支持 Baseline 的解码器。

提取命令：

```bash
ffmpeg -i input.mp4 -map 0:v:0 -c:v copy -bsf:v h264_mp4toannexb -f h264 output.h264
```

## Opus

| 文件（opus/） | 时长 | 解码采样率 | 声道 | 内容 |
| --- | --- | --- | --- | --- |
| ehren-paper_lights-96.opus | 228.11 秒 | 48000 Hz | 2 | 官方音乐演示，标称 96 kb/s |
| testvector01.ogg | 29.48 秒 | 48000 Hz | 2 | FFmpeg 托管的 Opus 测试向量 |
| testvector02.ogg | 25.03 秒 | 48000 Hz | 2 | FFmpeg 托管的 Opus 测试向量 |

来源：[Opus 官方演示](https://opus-codec.org/examples/) 和 [FFmpeg Opus 样本库](https://samples.ffmpeg.org/A-codecs/opus/)。

这三份音频都是 Ogg 容器内的 Opus；`.ogg` 文件已用 ffprobe 确认编码为 Opus。用于 RTP 时需要先解封装取得 Opus 包，不能直接把完整 Ogg 文件作为 RTP payload。

## 验证与记录

- 全部 9 个媒体文件已用 ffprobe 检查编码，并用 FFmpeg 完整解码检查。
- `testvector02.ogg` 解码到 null 输出时出现非单调 DTS 警告；按样本数重建输出时间戳（`asetpts=N/SR/TB`）后，完整解码为原始 PCM 无报错。保留原始样本，适合检查时间戳边界；普通播放测试优先选音乐样本或 testvector01。
- `sources.json`：原始下载链接。
- `manifest.json`：来源、大小、SHA-256、完整探测信息与验证日志。
- `SHA256SUMS`：媒体文件校验和，在本目录执行 `sha256sum -c SHA256SUMS` 可检查。
