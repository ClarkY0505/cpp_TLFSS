"""Exercise the real CLI through a terminal and a loopback TCP peer. / 通过终端和本机回环 TCP 对端测试真实的命令行客户端。"""
import os
import pty
import select
import socket
import subprocess
import sys
import time
import unittest

CLIENT = sys.argv.pop(1)


class FtpClientTest(unittest.TestCase):
    def test_connects_and_receives_without_terminal_input(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(2)
            master, slave = pty.openpty()
            try:
                process = subprocess.Popen(
                    [CLIENT, '127.0.0.1', str(listener.getsockname()[1])],
                    stdin=slave, stdout=slave, stderr=slave)
            except BaseException:
                os.close(master)
                raise
            finally:
                os.close(slave)
            try:
                # No terminal input: startup must not depend on a key press / 无终端输入：启动不得依赖按键
                # or the EventLoop's ten-second poll timeout. / 或 EventLoop 的十秒轮询超时。
                self.read_until(master, b'Connecting...')
                try:
                    peer, _ = listener.accept()
                except socket.timeout:
                    self.fail('CLI did not connect without terminal input within 2 seconds')
                with peer:
                    peer.settimeout(2)
                    peer.sendall(b'Welcome to TLSS.\r\ntlss>')
                    self.read_until(master, b'tlss>')
                    os.write(master, b'help\n')
                    self.read_until(master, b'Commands:')
                    os.write(master, b'pwd\n')
                    self.assertEqual(self.read_line(peer), b'pwd\n',
                                     'help must be handled locally, not sent to the peer')
                    os.write(master, b'quit\n')
                    self.assertEqual(self.read_line(peer), b'quit\n')
                    self.assertEqual(peer.recv(1), b'')
                    peer.sendall(b'bye!\r\n')
                self.read_until(master, b'bye!')
                self.assertEqual(process.wait(timeout=3), 0)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                os.close(master)

    def read_until(self, fd, expected):
        data = b''
        deadline = time.monotonic() + 2
        while expected not in data:
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, repr(data))
            self.assertTrue(select.select([fd], [], [], remaining)[0],
                            f'Missing {expected!r}; output: {data!r}')
            chunk = os.read(fd, 65536)
            self.assertTrue(chunk, repr(data))
            data += chunk
        return data

    def read_line(self, peer):
        data = b''
        while not data.endswith(b'\n'):
            byte = peer.recv(1)
            self.assertTrue(byte, repr(data))
            data += byte
        return data


if __name__ == '__main__':
    unittest.main()
