"""Small, allowlisted client for the firmware's existing rkipc UNIX socket.

Wire ABI: little-endian int32; initial status, NUL-terminated function name
prefixed by its length, camera ID, optional value, result, dispatcher status.
Reference: LuckfoxTECH/luckfox-pico, common/socket_server/server.c.
No raw function names or camera IDs are accepted from the web client.
"""
import socket
import struct
import threading

FIELDS = ("brightness", "contrast", "saturation", "sharpness")
INT = struct.Struct("<i")


def validate_values(values):
    if not isinstance(values, dict) or not values or values.keys() - set(FIELDS):
        raise ValueError("只支持亮度、对比度、饱和度和锐度")
    if any(type(v) is not int or not 0 <= v <= 100 for v in values.values()):
        raise ValueError("参数必须是 0–100 的整数")
    return values


class IspClient:
    def __init__(self, path, timeout=3):
        self.path, self.timeout = path, timeout
        self.sock = None
        self.lock = threading.Lock()

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _int(self):
        data = bytearray()
        while len(data) < 4:
            chunk = self.sock.recv(4 - len(data))
            if not chunk:
                raise ConnectionError("ISP 控制连接已关闭")
            data.extend(chunk)
        return INT.unpack(data)[0]

    def _connect(self):
        if self.sock is None:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect(self.path)
            if self._int() != 0:
                raise ConnectionError("ISP 控制握手失败")

    def _call(self, field, value=None):
        self._connect()
        name = f"rk_isp_{'get' if value is None else 'set'}_{field}".encode() + b"\0"
        packet = INT.pack(len(name)) + name + INT.pack(0)
        if value is not None:
            packet += INT.pack(value)
        self.sock.sendall(packet)
        result = self._int() if value is None else None
        status, dispatch = self._int(), self._int()
        if status or dispatch:
            raise RuntimeError(f"ISP {field} 返回错误：{status}/{dispatch}")
        return result

    def exchange(self, values=None):
        if values is not None:
            validate_values(values)
        with self.lock:
            try:
                if values is not None:
                    # Sequential, acknowledged writes. Never replay uncertain writes.
                    for field, value in values.items():
                        self._call(field, value)
                current = {field: self._call(field) for field in FIELDS}
                if any(not 0 <= value <= 100 for value in current.values()):
                    raise RuntimeError("板子返回了无效的 ISP 参数")
                if values and any(current[k] != v for k, v in values.items()):
                    raise RuntimeError("ISP 参数回读不一致，请重新读取")
                return current
            except Exception:
                self.close()
                raise
