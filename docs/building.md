# 构建与依赖

命令从仓库根目录运行。CMake 3.20+；Linux 默认使用 C11 编译器、Make、pkg-config。非 Linux 平台需要自行实现 PAL，目前没有对应平台的完整验证声明。

## 依赖策略

默认使用已安装的系统库，不强制系统库精确版本；构建最低要求与历史实测版本分别列出。最低要求是 API 使用边界，不表示该范围的所有版本都经过验证。

| 依赖 | 使用场景 | 构建最低要求 | 历史实测版本 |
| --- | --- | --- | --- |
| OpenSSL | 默认 Crypto / DTLS | 3.0 | 3.5.5 |
| libSRTP | Session 的 SRTP | 2.5 | 2.7.0 |
| Mbed TLS | 可选 Crypto / DTLS | 3.6 | 3.6.5 |
| libjuice | 可选 ICE | 固定提交 | v1.7.3 对应 `6d0356d092701dcce1f731559d38dc94f52eeb14` |
| Opus | 媒体演示程序 | 提供 pkg-config 的 Opus | 1.6.1 |
| Python | 开发脚本 | 3.10 | 历史环境见各验证报告 |

Mbed TLS 需启用 DTLS-SRTP。通过 pkg-config 提供 `mbedcrypto`、`mbedtls`、`mbedx509`；某些发行版的旧版包不满足要求。OpenSSL / libSRTP 通常由包管理器提供。示例的 Python 依赖保存在 `examples/requirements.txt`，不会成为 C SDK 的运行依赖。

`EWRTC_ENFORCE_DEPENDENCY_LOCK=ON` 强制历史实测精确版本，适合复现旧矩阵。libjuice 无论此选项如何都会固定下载提交；严格模式还校验本地 override 的 HEAD。

## 构建预设

```bash
cmake --list-presets
cmake --preset native-openssl
cmake --build --preset native-openssl --parallel
ctest --preset native-openssl
```

| 预设 | 内容 |
| --- | --- |
| `default` | native ICE + OpenSSL，输出 `build-default/` |
| `native-openssl` | 默认后端，独立输出目录 |
| `native-mbedtls` | native ICE + Mbed TLS |
| `juice-openssl` | libjuice + OpenSSL，配置时需联网或提供本地源码 |
| `juice-mbedtls` | libjuice + Mbed TLS |
| `protocols` | 仅内部 SDP、RTP 及其依赖，不安装完整 SDK |
| `asan` | native + OpenSSL，Debug + ASan/UBSan，需 GCC 或 Clang |

每个预设均有同名 configure / build / test 入口。新增个人配置写入被忽略的 `CMakeUserPresets.json`。已有构建目录会保留原缓存；发布整理后的新默认值应使用新目录验证。

## 常用选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `EWRTC_COMPONENTS` | `session` | 自动补齐内部依赖 |
| `EWRTC_WITH_LINUX_PAL` | ON | 独立的 Linux 平台提供者 |
| `EWRTC_WITH_NATIVE_ICE` / `EWRTC_WITH_LIBJUICE` | ON / OFF | ICE 后端 |
| `EWRTC_WITH_OPENSSL` / `EWRTC_WITH_MBEDTLS` | ON / OFF | Crypto / DTLS 后端 |
| `EWRTC_BUILD_TESTS` | ON | 单元测试及架构检查 |
| `EWRTC_BUILD_EXAMPLES` | OFF | Opus 媒体演示程序 |
| `EWRTC_BUILD_INTERNAL_EXAMPLES` | OFF | 内部协议示例 |
| `EWRTC_ENFORCE_DEPENDENCY_LOCK` | OFF | 历史精确版本校验 |
| `EWRTC_WARNINGS_AS_ERRORS` | OFF | 开发 / CI 可开启 |

不使用预设时：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

所有命令独立于父目录名称。安装包通过 `ewrtc::ewrtc` / `ewrtc::session` 和 `ewrtc::pal_linux` 接入。内部协议库及头文件不导出。0.x 阶段 CMake 包版本匹配限制在同一次版本内，升级 0.2 → 0.3 需显式评估 API 变化。

## 验证

```bash
python3 tests/check_architecture.py
python3 tests/check_docs.py
# 四种后端、每个组件、安装包消费者和自定义 PAL；需要全部依赖
python3 tests/verify_builds.py
# 系统依赖版本的同一矩阵
python3 tests/verify_builds.py --no-dependency-lock
```

有本地 libjuice checkout 时添加 `--juice-source /path/to/libjuice`。浏览器互通、弱网及长测需要 Chrome、FFmpeg、coturn 和 Python 示例依赖，详见 [接入指南](usage.md)。这些测试不包含在基础 GitHub Actions 构建中。

## 交叉编译

Luckfox 工具链与依赖准备见 [板卡示例](../examples/luckfox/README.md)。通用自定义平台设置 `EWRTC_WITH_LINUX_PAL=OFF` 并注入自己的 PAL。第三方后端自身的平台依赖不由 PAL 接管。

CMake 分为选项、组件图、依赖发现、安装、测试和示例六个模块；顶层只负责项目初始化及目标组装。
