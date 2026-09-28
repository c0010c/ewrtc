# 诊断日志

SDK 复用 `ewrtc_pal.log.write(ctx, level, text)`，不新增公共结构字段，也不依赖外部日志库。
Linux PAL 将完整记录一次写入 stderr，并补上换行；自定义 PAL 可以写入应用已有的日志系统。

```text
2026-09-28T06:32:08.012Z INFO  SESSION c1/s7        created ice=native dtls=openssl worker=0
2026-09-28T06:32:08.087Z INFO  DTLS    c1/s7        handshake started role=server
2026-09-28T06:32:08.124Z INFO  DTLS    c1/s7        handshake completed elapsed=37ms srtp=ready
2026-09-28T06:32:08.125Z INFO  SESSION c1/s7        state connecting -> connected age=113ms
```

这是格式示例。字段顺序为 UTC 时间、等级、模块、作用域、消息。`c1/s7` 是进程内 context
编号和 context 内的 session token；组件直接输出的作用域为 `-`，不强行关联到某个会话。
会话编号不会因普通槽位复用而复用，context 编号使用无符号递增计数器，整数回绕除外。
`age` 从会话创建算起，握手 `elapsed` 从 DTLS 启动算起，均使用单调时钟。
独立组件未提供 UTC 时钟时使用 `+毫秒ms`，无时钟时显示 `time-unavailable`。
输出不包含 ANSI 颜色；ASCII 控制字符转换成 `\xNN`，超长消息以 `[truncated]` 标记。
单条记录最多 1023 字节（不含结尾 NUL），使用有界栈缓冲区，不为每条日志申请堆内存。

## 等级和关闭方式

统一构建选项 `EWRTC_LOG_MIN_LEVEL` 控制最低等级，默认 `1`：

| 值 | 等级 | 用途 |
| --- | --- | --- |
| 0 | DEBUG | 额外协议诊断，如 TURN 鉴权挑战 |
| 1 | INFO | 创建、状态迁移、选中路径、握手、关闭 |
| 2 | WARN | 路径中断及高频异常汇总 |
| 3 | ERROR | 具体失败阶段和错误原因 |
| 4 | OFF | 关闭全部 SDK 日志 |

例如，在已经配置好的构建目录中调整并重新构建：

```bash
cmake -S . -B build-default -DEWRTC_LOG_MIN_LEVEL=2
cmake --build build-default --parallel
```

也可以在创建 context 前设置 `config.pal.log.write = NULL`，关闭该 context 及其组件的日志。
PAL 表由构造函数复制；修改原始表不会动态改变已创建对象的行为。
过滤在格式化和诊断参数求值前完成。已有业务回调与统计仍然工作。

## 高频异常

队列拒绝区分 session 字节额度、context 字节额度、工作项数量、控制槽耗尽及控制项过大。
发送背压、无效发送、SRTP 解保护失败和媒体接收失败分别累计。

生产者只更新计数；包括 libjuice 在适配器锁内调用的分配器，均不调用日志输出端。
会话工作线程在安全位置取出计数，按会话最多每 5 秒输出一批非零项；第一次有计数时可立即输出，
关闭完成前强制输出剩余计数。窗口检查复用已有工作线程运行机会，不增加专用定时器，
因此空闲时可能晚于 5 秒输出。这里的 `count` 是上次汇总以来的次数，不是丢失数据包总数。

```text
2026-09-28T06:33:06.100Z WARN  QUEUE   c1/s7        summary reason=context_byte_limit count=128
```

目前 `unprotect_failed` 表示 SRTP 处理阶段失败，不区分底层鉴权和重放错误；不能从统一的
`EWRTC_SECURITY` 推断具体 SRTP 原因。native 与 libjuice 可见的内部诊断深度不同；native TURN
使用 Send/Data indications，没有 ChannelBind 日志。

## 回调与模块边界

`write` 是同步回调，可能来自 API 调用线程、准备线程和会话工作线程。输出端须线程安全、
快速返回，不重入 SDK；记录的字符串只在调用期间有效。SDK 不在持有自己的协议、适配器或
队列锁时调用它，但不保证应用提供的文件、串口或其他输出端不会阻塞。
如果需要异步落盘，接入应用已有的日志设施；SDK 不创建日志线程。

公共格式、过滤和转义位于 `src/pal/log.c`；会话标识和计数汇总位于 `src/session/log.c`；
各协议模块在自己的事件发生点提供诊断。独立组件只依赖 PAL，不依赖 Session。

日志不会输出密码、密钥、完整 SDP 或媒体内容。选中的 ICE 路径会包含候选地址，便于排查网络路径。
底层具体原因和上层会话失败阶段可能分别各有一条记录；不逐包重复打印可恢复的媒体异常。
