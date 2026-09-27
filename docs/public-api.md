# SDK 公开 API 边界

应用通过 `ewrtc.h` 操作不透明的 `ewrtc_context` 和 `ewrtc_session`，使用配置、回调和统计结构体完成接入。STUN/TURN 地址与凭据、SDP 和 candidate 仍通过 Session 配置及函数传递。业务 API 的函数签名、字段、后端枚举名称和值保持不变。

## 公开内容

- `ewrtc.h`：Context/Session 生命周期、信令输入输出、音视频收发、统计。
- `ewrtc/common.h`：错误码、状态、方向、输入限制和平台地址类型及转换。
- `ewrtc/backends.h`：配置和统计使用的 Crypto、ICE、DTLS 后端枚举。
- `ewrtc/pal.h`、`ewrtc/pal/*.h`：按实例注入的平台服务表和能力检查。
- `ewrtc/platform/linux.h`：可选 Linux PAL 提供者；关闭该提供者时不安装此头。

对象句柄保持 opaque；配置、回调、统计和平台服务表保留可直接读写的字段。

## 内部内容

Crypto、SDP、STUN、TURN、ICE、DTLS、SRTP、RTP/RTCP、Media 的接口移动到 `src/<模块>/<模块>.h`。Common 的字节序、ASCII、UTC 辅助，以及 PAL 的分配、时钟、随机数包装函数也归入内部头文件。模块实现私有布局仍由各自的 `private.h` 或 `.c` 管理。

内部代码和测试显式获得 `src` 搜索路径，公开 target 不传播该路径。模块仍可通过 `EWRTC_COMPONENTS` 单独构建和测试；此选项用于 SDK 开发和裁剪，不代表可对外安装的组件列表。内部 RTP/TURN 示例由 `EWRTC_BUILD_INTERNAL_EXAMPLES=ON` 启用，只在源码树构建。

## 安装与链接

安装包只公开 `ewrtc::ewrtc`、`ewrtc::session` 和可选的 `ewrtc::pal_linux`。其中 Session target 转发到聚合库，协议组件库不再安装、导出。Linux PAL 静态库包含自身所需的 Common/PAL 实现，可独立链接。libjuice 后端的包还包含链接所需的私有依赖 target `ewrtc::_libjuice`，不属于应用 API，也不能作为公开 package component 请求。

```cmake
find_package(ewrtc CONFIG REQUIRED COMPONENTS session pal_linux)
target_link_libraries(my_app PRIVATE ewrtc::session ewrtc::pal_linux)
```

静态库仍包含协议实现的链接符号；本次收窄的是支持的头文件与 CMake 接口，没有引入动态库符号导出机制。旧的协议头文件和 `ewrtc::stun`、`ewrtc::srtp` 等 target 不再支持。升级时使用新的安装目录，避免旧安装遗留已移除的头文件和库。

## 验证

`tests/check_architecture.py` 检查公开头文件白名单、公开头文件依赖闭包、内部模块依赖、实现私有头隔离及平台边界。

`tests/verify_builds.py` 验证四种 ICE/TLS 后端组合、各内部模块独立构建、干净目录安装、公开 target 的外部链接和运行、所有安装头文件的 C/C++ 独立编译、拒绝请求内部协议组件，以及应用自定义 PAL 的安装后接入。安装检查同时拒绝多余头文件和协议组件库。

2026-09-25 本地验证通过：

| 范围 | 结果 |
| --- | --- |
| native/OpenSSL、native/Mbed TLS | 每组 10 项 CTest 通过 |
| libjuice/OpenSSL、libjuice/Mbed TLS | 每组 8 项 CTest 通过 |
| 13 个组件独立构建 | 构建及各自 CTest 全部通过 |
| 四种完整 SDK 安装包 | 公开 Session、聚合库、Linux PAL 的外部编译、链接、运行通过 |
| 公开头文件 | 每个安装头文件分别以 C11、C++11 编译通过 |
| 公开边界 | 安装头文件/库检查通过；内部 target 不存在；STUN/SRTP 组件请求按预期不可用 |
| 关闭 Linux PAL | 6 项 CTest、安装后自定义 C11 PAL 接入通过；native 聚合库不引用 pthread 或 Linux 提供者 |

本次构建与日志位于 `build-api-boundary/`，日志为 `verification.log` 和 `verification-continued.log`。首轮在 libjuice 安装检查处发现验证脚本使用了错误的依赖库文件名白名单；修正为实际的 `libjuice-static.a` 后，从该安装检查继续完成剩余验证。协议实现没有因此调整。
