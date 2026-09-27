# ISP 实时调节

Chrome 访问 <http://127.0.0.1:8082/>。页面同时显示摄像头实时画面和四项
0–100 调节：亮度、对比度、饱和度、锐度。滑块和数值输入在线生效，
“恢复默认 · 50”将四项恢复到原配置默认值；“重新读取”获取板子当前值。
这些参数作用于同一个摄像头，会影响所有观看者。

## 模块与运行

- `isp_protocol.py`：调用原厂 rkipc 已有的 `/var/tmp/rkipc` UNIX socket。
  参数白名单、范围校验、串行调用、状态码检查、参数回读和断线处理独立于 UI。
- `isp_server.py`：仅在主机 `127.0.0.1:8082` 提供静态页面和 JSON API；
  通过独立 SSH UNIX socket 隧道访问板子，不在板子部署新程序。
  同一个连接重复使用，不为每次拖动创建 SSH 进程；隧道断开时退出，由服务重启。
- `isp_ui/`：独立 HTML/CSS/JS，复用 `examples/player/client.js` 连接现有
  8766 信令服务。100 ms 合并滑动事件，单个写请求在途；收到成功结果并回读
  一致后显示“已生效”。失败时提示重新读取，避免把未确认的参数显示为成功。

无需修改或重启现有的 `rkipc`、WebRTC 程序或信令服务。每个预览页面占用一个
现有 WebRTC 会话。多窗口修改同一参数时，以最后被板子接受的写入为准；
其他窗口可点“重新读取”同步滑块。

参数通过厂商接口更新运行态。当前固件没有在每次调整后立即写 INI 文件，
正常退出 rkipc 时由原程序保存；本工具不直接覆盖 INI，不承诺异常掉电后保留。

手动运行（使用实际 SSH 主机名替换占位符，确保 8082 端口空闲）：

```bash
python3 examples/luckfox/isp_server.py --host root@YOUR_DEVICE_IP
# 可选 --host root@YOUR_DEVICE_IP --key ~/.ssh/ewrtc_luckfox_ed25519 --port 8082
```

如需后台运行，可自行配置用户服务；本示例不会安装服务。HTTP 接口验证：

```bash
python3 -m unittest discover -s examples/luckfox -p test_isp.py -v
```

实现所依据的厂商协议：
[socket_server/server.c](https://github.com/LuckfoxTECH/luckfox-pico/blob/main/project/app/rkipc/rkipc/common/socket_server/server.c)，
[rv1106/isp.c](https://github.com/LuckfoxTECH/luckfox-pico/blob/main/project/app/rkipc/rkipc/common/isp/rv1106/isp.c)。
