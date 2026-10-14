#!/usr/bin/env python3
"""板端一键演示：真实 HTTP 下载 -> SIGINT 暂停 -> 续传 -> 两个文件 SHA-256 对比。

运行：python3 tests/demo_resume.py
源文件、目标文件保留在本项目 demo 下的新目录，便于手动 sha256sum 复核。
"""
import hashlib
import fcntl
from http.server import ThreadingHTTPServer
import os
from pathlib import Path
import selectors
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from demo_server import Handler


class DemoHandler(Handler):
    def log_message(self,*_):
        pass


def sha256(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda:f.read(65536),b""):
            h.update(block)
    return h.hexdigest()


def pause_download(args):
    """交互演示用 PTY 保留真实显示；保存日志时使用普通管道。"""
    pretty=sys.stdout.isatty() and os.environ.get("TERM","") not in ("","dumb")
    master=slave=None
    process=None
    sel=selectors.DefaultSelector()
    try:
        if pretty:
            master,slave=os.openpty()
            size=shutil.get_terminal_size()
            fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack("HHHH",size.lines,size.columns,0,0))
            process=subprocess.Popen(args+["--delay-ms","500"],stdout=slave,stderr=slave)
            os.close(slave)
            slave=None
            stream=master
            marker=b"\x1b[2K"
        else:
            process=subprocess.Popen(args+["--delay-ms","500"],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            stream=process.stdout.fileno()
            marker=b"progress="
        sel.register(stream,selectors.EVENT_READ)
        received=b""
        sent=False
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            if not sel.select(0.5):
                continue
            try:
                block=os.read(stream,65536)
            except OSError:
                break
            if not block:
                break
            sys.stdout.buffer.write(block)
            sys.stdout.buffer.flush()
            received+=block
            if not sent and marker in received:
                process.send_signal(signal.SIGINT)
                sent=True
        process.wait(timeout=5)
        if not sent or process.returncode!=130:
            raise RuntimeError("暂停演示失败，请检查以上运行输出。")
    finally:
        sel.close()
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        if master is not None:
            os.close(master)
        if slave is not None:
            os.close(slave)
        if process is not None and process.stdout is not None:
            process.stdout.close()


def main():
    root=Path(__file__).resolve().parent.parent
    binary=root/"sparseget"
    if not binary.is_file():
        raise SystemExit("Run make first.")
    (root/"demo").mkdir(exist_ok=True)
    folder=Path(tempfile.mkdtemp(prefix="resume-",dir=root/"demo"))
    source=folder/"source.bin"
    target=folder/"download.bin"
    source.write_bytes(bytes(range(256))*(4*1024*1024//256))
    server=ThreadingHTTPServer(("127.0.0.1",0),DemoHandler)
    server.source=source
    thread=threading.Thread(target=server.serve_forever,daemon=True)
    thread.start()
    url="http://127.0.0.1:%d/file.bin" % server.server_port
    args=[str(binary),"download",url,str(target)]
    print("演示文件目录：",folder,flush=True)
    print("\n步骤 1 / 4：开始 HTTP 下载，出现进度后自动发送 SIGINT 暂停。",flush=True)
    try:
        pause_download(args)
        print("\n步骤 2 / 4：查看暂停文件的空洞与磁盘占用。",flush=True)
        subprocess.run([str(binary),"status",str(target)],check=True)
        print("\n步骤 3 / 4：使用相同 URL 和目标文件继续下载。",flush=True)
        subprocess.run(args,check=True)
        print("\n步骤 4 / 4：独立计算源文件与下载文件的 SHA-256。",flush=True)
        src_hash,dst_hash=sha256(source),sha256(target)
        print("源文件 source.bin：\n  "+src_hash)
        print("下载文件 download.bin：\n  "+dst_hash)
        if src_hash!=dst_hash:
            raise RuntimeError("SHA-256 MISMATCH")
        print("\n[校验通过] 两个文件的 SHA-256 一致。\nETag 仅用于远端版本识别；以上结果来自对两个实际文件的独立计算。")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__=="__main__":
    main()
