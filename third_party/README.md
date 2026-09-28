# 第三方源码管理

此目录的依赖均为 Git submodule，主仓库记录精确提交；`.gitmodules` 只记录路径和上游 URL。库版本保持迁移前的选择：

| 目录 | 版本 | 使用范围 |
| --- | --- | --- |
| `openssl` | 1.1.1w | 默认 Crypto / DTLS |
| `libsrtp` | 2.7.0 | SRTP |
| `mbedtls` | 3.6.5 | 可选 Crypto / DTLS，含嵌套 framework |
| `libjuice` | 1.7.3 | 可选 ICE |
| `opus` | 1.6.1 | 示例音频编解码 |

默认构建只初始化所需源码：

```bash
git submodule update --init third_party/openssl third_party/libsrtp
bash tools/build_dependencies.sh
source .local/deps/env.sh
```

其他依赖按需初始化：

```bash
git submodule update --init third_party/libjuice third_party/opus
git submodule update --init --recursive third_party/mbedtls
```

也可使用 `git clone --recurse-submodules` 获取全部源码，但它还会拉取 OpenSSL 的可选上游测试仓库，本项目构建不需要这些仓库。切换主仓库提交后，对所用子模块再次运行 `git submodule update --init`；Mbed TLS 加上 `--recursive`。

职责分工：

- `third_party/` 保存上游源码引用及原始许可证。
- `tools/build_dependencies.sh` 调度 `tools/dependencies/` 中的独立构建脚本，把选中的库安装到指定前缀；不联网、不更新源码。
- `cmake/dependencies/` 按启用组件发现依赖；libjuice 直接使用本地源码编译，其他库从安装前缀查找。
- `build-deps/` 和 `.local/` 保存被 Git 忽略的构建产物。Mbed TLS 的配置修改发生在构建目录副本内。

Python 包继续由 `examples/requirements.txt` 管理，工具链和板卡固件单独准备。使用系统库或自备安装目录时无需初始化相应源码。完整命令及交叉编译选项见 [构建说明](../docs/building.md)。

## 升级和补丁

显式选择并审查上游 tag/commit，在子模块内 checkout 后，在主仓库提交新的 gitlink。不要使用自动跟随分支的 `git submodule update --remote` 作为日常构建步骤。构建脚本检查所用 gitlink 与 checkout 一致；有意升级后先暂存新的 gitlink，再构建验证。

需要持久补丁时使用可访问的 fork，并提交补丁后更新主仓库引用及必要的 `.gitmodules` URL。未提交的子模块修改不会由主仓库提交保存；发布前检查 `git submodule foreach --recursive 'git status --short'`。同步更新版本检查、第三方声明，并执行完整后端及安装包消费者矩阵。

## 离线和源码发布

在联网阶段初始化所需源码和嵌套子模块，随后配置、编译不访问网络。离线移交需同时携带已初始化的工作树和 Git 元数据（严格校验使用它们）。普通主仓库 ZIP 或 `git archive` 不包含子模块内容，不能直接作为完整源码包；发布时需另行打包这些源码及其许可证。
