# 贡献指南

感谢参与 ewrtc。项目处于 0.x 阶段，变更应明确描述对公共 API、线程约定和支持范围的影响。

## 本地开发

先按 [中文 README](README.zh-CN.md) 安装依赖，再运行：

```bash
cmake --preset default -DEWRTC_WARNINGS_AS_ERRORS=ON
cmake --build --preset default --parallel
ctest --preset default
python3 tests/check_docs.py
```

后端或 CMake 变更需要运行 [完整构建矩阵](docs/building.md)。安装边界变更需要验证 `tests/consumer/`；媒体和协商行为变更按需运行浏览器测试，并注明未覆盖的环境。使用新的构建目录，避免缓存掩盖缺失依赖。

## 模块边界

- 公共 API 在 `include/`；内部协议接口在 `src/<module>/`，不安装。
- 每个模块只依赖约定的下层接口，不跨模块包含 `private.h`。
- OS 能力经 PAL 注入；平台实现放 `src/platform/`，板卡采集放示例层。
- 新功能优先扩展职责明确的模块；修改模块图时同时更新架构说明和边界检查。
- 保持回调生命周期、队列上限和销毁顺序的文档与实现一致。

C 使用 4 空格；CMake、JSON、YAML 使用 2 空格。`.editorconfig` 和 `.clang-format` 提供基础配置。仅格式化本次修改范围，避免混入整库格式重写。

## 提交修改

Issue 请提供操作系统、编译器、依赖版本、后端组合、最小复现和期望/实际行为。PR 说明具体问题、最终行为及验证结果；敏感凭据和设备个人配置应去除。安全问题遵循 [安全政策](SECURITY.md)。

自有代码按 [MIT](LICENSE) 贡献。引入第三方代码时保留其原始许可及来源，更新 [第三方说明](THIRD_PARTY_NOTICES.md)。
