# 发布整理验证 — 2026-09-27

本次验证针对独立仓库发布整理后的源码。当前仓库仅初始化，尚无提交 SHA；
首次提交前若继续修改代码，应按修改范围重新验证。没有创建 GitHub 远端或发布 Release。

## 已完成

| 验证 | 结果 |
| --- | --- |
| 新目录默认 Release 构建，警告视为错误 | GCC 15.2，10/10 CTest 通过 |
| 完整构建矩阵 | 四种后端、13 个独立组件、自定义 PAL；18 组 CTest 合计 80 项通过 |
| 安装包消费者 | 完整矩阵的公共 target、独立 C/C++ 头文件、内部 target 拒绝和自定义 PAL 接入检查通过 |
| 干净源码副本 | 仅复制 Git 忽略规则允许的文件；默认 preset、安装和 `examples/minimal` 独立构建运行通过 |
| ASan / UBSan | 干净源码副本，`asan` preset、`ASAN_OPTIONS=detect_leaks=1`，10/10 CTest 通过 |
| 播放器 | 无下载素材的源码副本，生成测试画面；Chrome 自动播放、跨 10 秒循环、重连通过，均为 1280×720 |
| ISP HTTP / 协议单元测试 | 4/4 通过；未连接真实板卡 |
| 文档与脚本 | Markdown 文件链接、Python 语法、架构边界、板卡 shell 语法检查通过 |
| 发布文件 | 构建目录、下载媒体、IDE 缓存和 `.local/` 配置被 Git 忽略；候选文件约 1.1 MB |

本机依赖：OpenSSL 3.5.5、Mbed TLS 3.6.5、libSRTP 2.7.0、Opus 1.6.1。
完整矩阵使用固定提交的本地 libjuice checkout，未验证从远端首次下载源码的网络过程。
播放测试帧数：首次播放 35，循环检查 395，重连 37。

主要命令（构建目录可自行选择）：

```bash
cmake --preset default -DEWRTC_WARNINGS_AS_ERRORS=ON
cmake --build --preset default --parallel
ctest --preset default
python3 tests/verify_builds.py --output build-release-matrix --juice-source /path/to/libjuice
cmake --preset asan
cmake --build --preset asan --parallel
ASAN_OPTIONS=detect_leaks=1 ctest --preset asan
python3 -m unittest discover -s examples/luckfox -p test_isp.py -v
python3 tests/check_docs.py
```

浏览器测试使用独立 HTTP / WebSocket / CDP 端口，启动 `examples/run_player.py`
后执行 `tests/test_browser_player.py --url <播放器地址> --cdp-port <调试端口>`，
结束后清理测试进程。

## 未完成的环境验证

- Ubuntu 24.04 容器：Docker Hub 和镜像入口均在拉取镜像时返回网络 EOF，未能执行该环境下的 GCC / Clang 测试。
- GitHub Actions：工作流已加入，YAML 可解析，尚未在远端实际运行；不能视为已跑绿。
- 真实板卡部署、重启、弱网与公网互通：本次未重跑。板卡地址和 USB 序列号改为使用者显式配置；原本机配置保留在被忽略的 `.local/` 中。
