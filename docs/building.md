# 构建与依赖

命令从仓库根目录运行。SDK 自身需要 CMake 3.20+；从 submodule 构建 libSRTP 2.7.0 需要 CMake 3.21+。Linux 默认使用 C11 编译器、Make、pkg-config。非 Linux 平台需要自行实现 PAL，目前没有对应平台的完整验证声明。

## 依赖策略

OpenSSL 固定为 1.1.1w，包括安装包消费者；其他系统库默认不强制精确版本。构建要求与历史实测版本分别列出，最低要求不表示该范围的所有版本都经过验证。

| 依赖 | 使用场景 | 构建最低要求 | 历史实测版本 |
| --- | --- | --- | --- |
| OpenSSL | 默认 Crypto / DTLS | 精确 1.1.1w | 1.1.1w；降级前为 3.5.5 |
| libSRTP | Session 的 SRTP | 2.5 | 2.7.0 |
| Mbed TLS | 可选 Crypto / DTLS | 3.6 | 3.6.5 |
| libjuice | 可选 ICE | 固定提交 | v1.7.3 对应 `6d0356d092701dcce1f731559d38dc94f52eeb14` |
| Opus | 媒体演示程序 | 提供 pkg-config 的 Opus | 1.6.1 |
| Python | 开发脚本 | 3.10 | 历史环境见各验证报告 |

Mbed TLS 需启用 DTLS-SRTP。通过 pkg-config 提供 `mbedcrypto`、`mbedtls`、`mbedx509`；某些发行版的旧版包不满足要求。libSRTP 可从固定源码构建或使用包管理器安装。示例的 Python 依赖保存在 `examples/requirements.txt`，不会成为 C SDK 的运行依赖。

`EWRTC_ENFORCE_DEPENDENCY_LOCK=ON` 额外锁定 Mbed TLS 和 libSRTP 为表中的实测版本。OpenSSL 始终精确锁定 1.1.1w；默认 libjuice 源码由 gitlink 固定，严格模式还校验所选 checkout 的 HEAD（包括本地 override）。

### 从 submodule 构建依赖

源码位于 `third_party/`，版本由主仓库的 gitlink 固定。构建脚本不联网，也不自动更新子模块。默认仅准备 OpenSSL 和 libSRTP：

```bash
git submodule update --init third_party/openssl third_party/libsrtp
bash tools/build_dependencies.sh
source .local/deps/env.sh
```

需要 Git、Perl、Python 3、tar、CMake 3.21+、make 和 C 编译器。默认静态安装到 `.local/deps`，构建产物位于 `build-deps/native`，不修改子模块源码。libSRTP 使用内置加密实现，避免引入另一套 TLS 依赖。

完整后端和示例构建：

```bash
git submodule update --init third_party/openssl third_party/libsrtp third_party/libjuice third_party/opus
git submodule update --init --recursive third_party/mbedtls
bash tools/build_dependencies.sh "$PWD/.local/deps" openssl libsrtp mbedtls opus
source .local/deps/env.sh
```

Mbed TLS 需要嵌套的 `framework` 子模块；OpenSSL 的嵌套测试仓库不参与本项目构建，无需初始化。Mbed TLS 的 DTLS-SRTP 配置只应用于构建目录内的源码副本，安装的配置头与库保持一致。Opus 仅供示例使用，关闭可选神经网络功能。libjuice 由启用它的 SDK 构建直接编译，无需单独安装。

`env.sh` 设置 `OPENSSL_ROOT_DIR`、`PKG_CONFIG_PATH` 和 `CMAKE_PREFIX_PATH`。运行安装包消费者时也要保留该环境。切换编译器、目标平台或静态/动态配置时，使用独立的构建目录和安装目录。

### 使用外部安装的依赖

依赖发现仍使用 `find_package(OpenSSL)` 和 `pkg-config`，不强制使用 `third_party` 编译的库。可以自行设置 `OPENSSL_ROOT_DIR`、`PKG_CONFIG_PATH`、`CMAKE_PREFIX_PATH`，无需初始化不使用的子模块。版本要求及 `EWRTC_ENFORCE_DEPENDENCY_LOCK` 行为保持不变。

启用 libjuice 时默认使用 `third_party/libjuice`，可通过 `-DEWRTC_LIBJUICE_SOURCE_DIR=/path/to/libjuice` 使用其他本地 checkout；旧的 `FETCHCONTENT_SOURCE_DIR_LIBJUICE` 参数仍兼容。严格模式校验 checkout 的 HEAD 与主仓库 gitlink 一致，需保留 Git 元数据；非严格模式也支持本地源码副本。

旧 OpenSSL 构建入口继续可用，默认安装路径保持不变，但现在使用 submodule 源码：

```bash
git submodule update --init third_party/openssl
bash tools/build_openssl.sh
export OPENSSL_ROOT_DIR="$PWD/.local/openssl-1.1.1w"
```

源码升级、离线准备和目录职责见 [第三方源码管理](../third_party/README.md)。

1.1.1w 是 1.1.1 的最后公开版本，已结束公共支持，参见 [官方发布记录](https://mta.openssl.org/pipermail/openssl-announce/2023-September/000274.html)。本次降级的验证范围见 [验证记录](openssl-1.1.1w-validation.md)；旧浏览器和长测记录仍属于 3.5.5 环境。

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
| `juice-openssl` | libjuice + OpenSSL，需预先初始化 libjuice 子模块或提供本地源码 |
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
| `EWRTC_ENFORCE_DEPENDENCY_LOCK` | OFF | 额外锁定 Mbed TLS / libSRTP 并校验 libjuice 提交 |
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
python3 tests/check_dependencies.py  # 需初始化 libjuice；验证缺失源码、版本校验和选项隔离
# 四种后端、每个组件、安装包消费者和自定义 PAL；需要全部依赖
python3 tests/verify_builds.py
# 系统依赖版本的同一矩阵
python3 tests/verify_builds.py --no-dependency-lock
```

有本地 libjuice checkout 时添加 `--juice-source /path/to/libjuice`。浏览器互通、弱网及长测需要 Chrome、FFmpeg、coturn 和 Python 示例依赖，详见 [接入指南](usage.md)。这些测试不包含在基础 GitHub Actions 构建中。

## 交叉编译

Luckfox 工具链与依赖准备见 [板卡示例](../examples/luckfox/README.md)。通用依赖构建脚本接受 `EWRTC_DEPS_TOOLCHAIN_FILE`、`EWRTC_DEPS_BUILD_DIR`、`EWRTC_DEPS_BUILD_TYPE` 和 `JOBS`。例如：

```bash
EWRTC_DEPS_TOOLCHAIN_FILE=/absolute/path/toolchain.cmake \
EWRTC_DEPS_BUILD_DIR="$PWD/build-deps/target" \
bash tools/build_dependencies.sh "$PWD/.local/target" mbedtls libsrtp
```

交叉构建生成的 `env.sh` 使用 `PKG_CONFIG_LIBDIR` 并清空 `PKG_CONFIG_PATH`，避免找到主机库；SDK 配置仍需传入相同的 CMake 工具链与目标库搜索路径。OpenSSL 使用自己的 Configure 系统，交叉构建还需显式指定 `OPENSSL_CONFIGURE_TARGET` 与 `CROSS_COMPILE`。

通用自定义平台设置 `EWRTC_WITH_LINUX_PAL=OFF` 并注入自己的 PAL。第三方后端自身的平台依赖不由 PAL 接管。

CMake 分为选项、组件图、依赖发现、安装、测试和示例六个模块；顶层只负责项目初始化及目标组装。每个第三方库的接入逻辑独立放在 `cmake/dependencies/`，源码准备脚本放在 `tools/dependencies/`。
