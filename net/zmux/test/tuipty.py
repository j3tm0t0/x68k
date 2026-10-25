"""zmux (TUI) を pty で動かし、画面を pyte で読む (e2e_fake.py と e2e_tui.py で使う)"""
import fcntl, os, pty, select, signal, struct, termios, threading, time

import pyte


class Tui:
    def __init__(self, zbin, env, cols=80, rows=24):
        self.cols, self.rows = cols, rows
        pid, fd = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            os.execve(zbin, [zbin], env)
        self.pid, self.fd = pid, fd
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = bytearray()
        self.alive = True
        threading.Thread(target=self._rd, daemon=True).start()

    def _rd(self):
        while True:
            try:
                r, _, _ = select.select([self.fd], [], [], 0.2)
                if not r:
                    continue
                d = os.read(self.fd, 65536)
            except OSError:
                d = b""
            if not d:
                self.alive = False
                return
            self.raw += d
            self.stream.feed(d)

    def lines(self):
        return [self.screen.display[i] for i in range(self.rows)]

    def text(self):
        return "\n".join(self.lines())

    def send(self, b):
        os.write(self.fd, b if isinstance(b, bytes) else b.encode())

    def wait(self, pat, t=15):
        end = time.time() + t
        while time.time() < end:
            if pat in self.text():
                return True
            time.sleep(0.05)
        return False

    def wait_gone(self, pat, t=10):
        end = time.time() + t
        while time.time() < end:
            if pat not in self.text():
                return True
            time.sleep(0.05)
        return False

    def wait_exit(self, t=10):
        end = time.time() + t
        while time.time() < end:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return True
            time.sleep(0.05)
        return False

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


def wait_until(f, t=10):
    end = time.time() + t
    while time.time() < end:
        if f():
            return True
        time.sleep(0.05)
    return False
