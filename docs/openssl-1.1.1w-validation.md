# OpenSSL 1.1.1w 降级验证

日期：2026-09-27。环境：Linux x86_64、GCC 15.2、OpenSSL 1.1.1w、libSRTP 2.7.0、Mbed TLS 3.6.5；libjuice 固定提交与 [构建说明](building.md) 一致。

## 变更范围

- 项目和安装包消费者均精确要求 OpenSSL 1.1.1w，拒绝系统 OpenSSL 3.5.5；不受 `EWRTC_ENFORCE_DEPENDENCY_LOCK` 开关影响。
- OpenSSL 在独立目录静态安装。源码包 SHA-256 为 `cf3098950cb4d853ad95c0841f1f9c6d3dc102dccfcacd521d93925208b76ac8`。
- 使用独立、基于 PAL 分配器的内存数据报 BIO 替代新版 `BIO_s_dgram_mem()`。每次读取保留单包边界，队列数据上限为 64 KiB；统一队首移除和资源释放逻辑。
- 证书获取和错误队列测试改用 1.1.1 API。公开 SDK 接口不变；许可证说明、CI 和依赖安装指引同步更新。

## 验证结果

| 验证 | 结果 |
| --- | --- |
| native + OpenSSL | 12/12 CTest 通过 |
| libjuice + OpenSSL | 10/10 CTest 通过 |
| native + Mbed TLS | 10/10 CTest 通过 |
| libjuice + Mbed TLS | 8/8 CTest 通过 |
| 13 个独立组件构建与测试 | 全部通过 |
| 自定义 PAL 构建与测试 | 7/7 CTest 通过 |
| 安装包、公开头文件 C/C++ 消费者、内部 API 不导出 | 全部通过 |
| native + OpenSSL，ASan / UBSan / 泄漏检查 | 12/12 CTest 通过 |
| 配置时误选系统 OpenSSL 3.5.5 | 按预期拒绝 |

新增 BIO 测试覆盖空队列重试、数据报不合并、短读丢弃余量、分配失败、队列上限、重置与销毁无泄漏，同时检查头文件与运行时均为 1.1.1w。DTLS 传输测试覆盖双端丢包、`EWRTC_AGAIN` / `EWRTC_BACKPRESSURE` 后原包重试、MTU、重传握手和一致的 SRTP 密钥导出。

复现完整矩阵：

```bash
bash tools/build_openssl.sh
export OPENSSL_ROOT_DIR="$PWD/.local/openssl-1.1.1w"
python3 tests/verify_builds.py --output build-openssl111-matrix
```

可使用 `--juice-source /path/to/libjuice` 指定已有 checkout。本次未重跑浏览器互通、长时间持续传输及板卡交叉编译；旧验证报告中的 OpenSSL 3.5.5 记录保留为历史结果。
