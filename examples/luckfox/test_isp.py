"""Protocol failures and HTTP boundary checks; no board required."""
import http.client
import json
import socket
import threading
import unittest
from unittest.mock import patch
from http.server import ThreadingHTTPServer

from isp_protocol import FIELDS, INT, IspClient
from isp_server import Handler


def receive(sock, size):
    result = b""
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError
        result += chunk
    return result


class ProtocolTests(unittest.TestCase):
    def test_fragmented_replies_and_multiple_requests(self):
        host, board = socket.socketpair()
        values = dict.fromkeys(FIELDS, 100)
        errors = []

        def firmware():
            try:
                board.sendall(INT.pack(0))
                for _ in range(5):  # one write, four reads, one shared connection
                    length = INT.unpack(receive(board, 4))[0]
                    name = receive(board, length).decode().rstrip("\0")
                    self.assertEqual(INT.unpack(receive(board, 4))[0], 0)
                    op, field = name.removeprefix("rk_isp_").split("_", 1)
                    if op == "set":
                        values[field] = INT.unpack(receive(board, 4))[0]
                        reply = INT.pack(0) * 2
                    else:
                        reply = INT.pack(values[field]) + INT.pack(0) * 2
                    for byte in reply:
                        board.sendall(bytes([byte]))
            except Exception as error:
                errors.append(error)
            finally:
                board.close()

        worker = threading.Thread(target=firmware)
        worker.start()
        client = IspClient("unused")
        client.sock = host
        self.assertEqual(client._int(), 0)
        self.assertEqual(client.exchange({"brightness": 55}), {**dict.fromkeys(FIELDS, 100), "brightness": 55})
        client.close()
        worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])

    def test_invalid_inputs_never_connect(self):
        client = IspClient("unused")
        with patch.object(client, "_connect") as connect:
            for values in ({}, [], {"hdr": 1}, {"brightness": -1},
                           {"brightness": 101}, {"brightness": True}, {"brightness": 1.5}):
                with self.assertRaises(ValueError):
                    client.exchange(values)
            connect.assert_not_called()

    def test_vendor_error_closes_connection(self):
        host, board = socket.socketpair()
        client = IspClient("unused")
        client.sock = host
        board.sendall(INT.pack(-1) + INT.pack(0))
        with self.assertRaises(RuntimeError):
            client.exchange({"contrast": 40})
        self.assertIsNone(client.sock)
        board.close()


class HttpTests(unittest.TestCase):
    def test_local_boundary_and_input_validation(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.isp = IspClient("/no-board-needed")
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            def request(body, extra=None):
                connection = http.client.HTTPConnection(*server.server_address, timeout=3)
                connection.request("POST", "/api/isp", json.dumps(body),
                                   {"Content-Type": "application/json", **(extra or {})})
                response = connection.getresponse()
                status = response.status
                response.read()
                connection.close()
                return status

            self.assertEqual(request({"brightness": 40}, {"Origin": "http://unrelated.example"}), 403)
            self.assertEqual(request({"brightness": 40}, {"Host": "unrelated.example"}), 403)
            self.assertEqual(request({"brightness": 200}), 400)
            self.assertEqual(request(None), 400)
            self.assertEqual(request({"brightness": 40}), 503)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()


if __name__ == "__main__":
    unittest.main()
