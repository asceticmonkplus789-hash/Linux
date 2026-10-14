#!/usr/bin/env python3
"""在真实 Linux 文件系统上验证系统调用效果、续传和异常处理。

测试使用临时目录、回环 HTTP 服务器，不依赖公网，不覆盖用户文件。
"""
import fcntl
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import selectors
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest

BIN = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 else "./sparseget").resolve())
DATA = bytes(range(256)) * (4 * 1024 * 1024 // 256)


class FixtureHandler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_HEAD(self):
        self.respond(True)

    def do_GET(self):
        self.respond(False)

    def respond(self, head):
        data = self.server.data
        tag = '"' + hashlib.sha256(data).hexdigest() + '"'
        mode = self.path
        if mode == "/weak":
            tag = "W/" + tag
        if mode == "/long-etag":
            tag = '"' + 'e' * 253 + '"'
        if mode == "/opaque-etag":
            tag = '"revision-seven"'
        offset = 0 if head else int(self.headers.get("Range", "bytes=0-")[6:-1])
        self.server.requests.append(("HEAD" if head else "GET", offset, self.headers.get("If-Range")))
        if not head and mode == "/race":
            tag = '"changed-between-head-and-get"'
        self.send_response(200 if head or mode == "/ignore" else 206)
        if mode != "/no-length":
            self.send_header("Content-Length", str(len(data)-offset))
        if mode == "/duplicate-length":
            self.send_header("Content-Length", str(len(data)-offset))
        self.send_header("ETag", tag)
        if mode == "/chunked":
            self.send_header("Transfer-Encoding", "chunked")
        if not head:
            start = offset + 1 if mode == "/bad-range" else offset
            value = "bytes %d-%d/%d" % (start, len(data)-1, len(data))
            if mode == "/signed-range":
                value = "bytes +%d-%d/%d" % (start, len(data)-1, len(data))
            self.send_header("Content-Range", value)
        self.send_header("Connection", "close")
        self.end_headers()
        if not head:
            body = data[offset:]
            if mode == "/truncate" and self.server.truncate:
                body = body[:100000]
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fault_tmp = tempfile.TemporaryDirectory(prefix="sparseget-fault-lib-")
        cls.fault_lib = str(Path(cls.fault_tmp.name)/"fault_io.so")
        subprocess.run(["cc","-shared","-fPIC","-Wall","-Wextra","-Werror",
                        str(Path(__file__).with_name("fault_io.c")),"-o",cls.fault_lib,"-ldl"],
                       check=True, capture_output=True)
        cls.preload = cls.fault_lib
        # ASan 的 runtime 必须位于测试注入库之前。
        if os.environ.get("SPARSEGET_SANITIZED"):
            asan = subprocess.check_output(["cc","-print-file-name=libasan.so"],text=True).strip()
            cls.preload = asan + ":" + cls.fault_lib
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), FixtureHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = "http://127.0.0.1:%d" % cls.server.server_port

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.fault_tmp.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="sparseget-test-")
        self.d = Path(self.tmp.name)
        self.server.data = DATA
        self.server.requests = []
        self.server.truncate = True

    def tearDown(self):
        self.tmp.cleanup()

    def run_cmd(self, *args, code=0, env=None):
        p = subprocess.run([BIN, *map(str, args)], capture_output=True, text=True,
                           timeout=30, env=env)
        self.assertEqual(p.returncode, code, p.stdout + p.stderr)
        return p

    def target(self, name="out.bin"):
        return self.d / name

    def pause(self, path=None, route="/file"):
        path = path or self.target()
        p = subprocess.Popen([BIN,"download",self.base+route,str(path),"--delay-ms","80"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        selector = selectors.DefaultSelector()
        selector.register(p.stdout, selectors.EVENT_READ)
        output = b""
        deadline = time.monotonic()+15
        try:
            while b"progress=" not in output and time.monotonic()<deadline:
                if selector.select(1):
                    part = os.read(p.stdout.fileno(), 4096)
                    if not part:
                        break
                    output += part
            self.assertIn(b"progress=", output)
            p.send_signal(signal.SIGINT)
            tail, err = p.communicate(timeout=10)
            output += tail
            self.assertEqual(p.returncode, 130, repr((output,err)))
            self.assertIn(b"paused:", output)
        finally:
            selector.close()
            if p.poll() is None:
                p.kill()
                p.communicate()
        state = Path(str(path)+".resume").read_text().splitlines()
        offset = int(state[1].split()[3])
        self.assertGreater(offset,0)
        self.assertLess(offset,len(DATA))
        return offset

    def test_01_create_is_sparse_and_zero_filled(self):
        f=self.target()
        self.run_cmd("create",f,"128K")
        s=f.stat()
        self.assertEqual(s.st_size,131072)
        self.assertEqual(s.st_blocks,0)
        self.assertEqual(f.read_bytes(),b"\0"*131072)
        out=self.run_cmd("status",f).stdout
        self.assertIn("allocated_blocks_512=0",out)
        self.assertIn("hole_percent=100.00",out)

    def test_02_positional_write_preserves_other_bytes_and_holes(self):
        f=self.target()
        self.run_cmd("create",f,"1M")
        self.run_cmd("write",f,"4096","HELLO")
        self.run_cmd("write",f,"900000","END")
        expected=bytearray(1024*1024)
        expected[4096:4101]=b"HELLO"
        expected[900000:900003]=b"END"
        self.assertEqual(f.read_bytes(),expected)
        self.assertLess(f.stat().st_blocks*512,1024*1024//10)

    def test_03_reject_bad_sizes_and_bounds(self):
        f=self.target()
        for value in ["0","127K","-1","1Z","18446744073709551615G","+128K"]:
            self.run_cmd("create",f,value,code=1)
            self.assertFalse(f.exists())
        self.run_cmd("create",f,"128K")
        for offset in ["-1","131072","9223372036854775807"]:
            self.run_cmd("write",f,offset,"X",code=1)
        self.assertEqual(f.read_bytes(),b"\0"*131072)
        self.run_cmd("write",f,"131071","Z")
        self.assertEqual(f.read_bytes()[-1:],b"Z")

    def test_04_existing_file_is_never_truncated(self):
        f=self.target()
        f.write_bytes(b"KEEP THIS DATA")
        self.run_cmd("create",f,"128K",code=1)
        self.run_cmd("download",self.base+"/file",f,code=1)
        self.assertEqual(f.read_bytes(),b"KEEP THIS DATA")

    def test_05_full_download_is_byte_identical(self):
        f=self.target()
        self.run_cmd("download",self.base+"/file",f)
        self.assertEqual(f.read_bytes(),DATA)
        self.assertEqual(hashlib.sha256(f.read_bytes()).digest(),hashlib.sha256(DATA).digest())
        self.run_cmd("download",self.base+"/file",f)
        self.assertEqual(len([x for x in self.server.requests if x[0]=="GET"]),1)

    def test_06_ctrl_c_and_resume_from_saved_offset(self):
        f=self.target()
        offset=self.pause(f)
        self.assertEqual(f.stat().st_size,len(DATA))
        self.assertLess(f.stat().st_blocks*512,len(DATA))
        self.assertEqual(f.read_bytes()[:offset],DATA[:offset])
        self.assertEqual(f.read_bytes()[offset:],b"\0"*(len(DATA)-offset))
        self.run_cmd("download",self.base+"/file",f)
        self.assertEqual(f.read_bytes(),DATA)
        gets=[x for x in self.server.requests if x[0]=="GET"]
        self.assertEqual(gets[-1][1],offset)
        self.assertIsNotNone(gets[-1][2])

    def test_07_changed_remote_data_rejected(self):
        f=self.target()
        self.pause(f)
        before=f.read_bytes()
        self.server.data=b"Z"+DATA[1:]
        self.run_cmd("download",self.base+"/file",f,code=1)
        self.assertEqual(f.read_bytes(),before)

    def test_08_modified_local_prefix_rejected(self):
        f=self.target()
        self.pause(f)
        with f.open("r+b") as stream:
            stream.write(b"MODIFIED")
        p=self.run_cmd("download",self.base+"/file",f,code=1)
        self.assertIn("prefix was modified",p.stderr)

    def test_09_replaced_local_inode_rejected(self):
        f=self.target()
        self.pause(f)
        other=self.target("replacement")
        other.write_bytes(f.read_bytes())
        other.replace(f)
        self.run_cmd("download",self.base+"/file",f,code=1)

    def test_10_bad_http_responses_are_rejected(self):
        for route in ["/ignore","/bad-range","/weak","/no-length","/duplicate-length","/chunked","/race"]:
            with self.subTest(route=route):
                f=self.target(route[1:])
                self.run_cmd("download",self.base+route,f,code=1)
                if f.exists():
                    self.assertEqual(f.stat().st_blocks,0)

    def test_11_network_truncation_can_resume(self):
        f=self.target()
        self.run_cmd("download",self.base+"/truncate",f,code=1)
        state=Path(str(f)+".resume").read_text().splitlines()
        offset=int(state[1].split()[3])
        self.assertEqual(offset,100000)
        self.server.truncate=False
        self.run_cmd("download",self.base+"/truncate",f)
        self.assertEqual(f.read_bytes(),DATA)
        self.assertEqual([x for x in self.server.requests if x[0]=="GET"][-1][1],100000)

    def test_12_concurrent_writer_rejected(self):
        f=self.target()
        self.run_cmd("create",f,"128K")
        with f.open("r+b") as stream:
            fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
            p=self.run_cmd("write",f,"0","X",code=1)
            self.assertIn("busy",p.stderr)
        self.assertEqual(f.stat().st_blocks,0)

    def test_13_symlinks_and_non_regular_files_rejected(self):
        f=self.target()
        actual=self.target("actual")
        actual.write_bytes(b"KEEP")
        f.symlink_to(actual)
        self.run_cmd("write",f,"0","BAD",code=1)
        self.run_cmd("download",self.base+"/file",f,code=1)
        self.assertEqual(actual.read_bytes(),b"KEEP")
        fifo=self.target("fifo")
        os.mkfifo(fifo)
        self.run_cmd("write",fifo,"0","BAD",code=1)

    def test_14_manual_write_to_managed_download_rejected(self):
        f=self.target()
        self.pause(f)
        self.run_cmd("write",f,"0","BAD",code=1)

    def test_15_bad_state_and_changed_url_rejected(self):
        f=self.target()
        self.pause(f)
        self.run_cmd("download",self.base+"/other",f,code=1)
        state=Path(str(f)+".resume")
        state.write_text("CORRUPT\n")
        self.run_cmd("download",self.base+"/file",f,code=1)

    def test_16_invalid_url_and_usage(self):
        f=self.target()
        for url in ["https://localhost/file", "http://localhost:99999/f", "http://bad\r\nhost/f", "http://u:p@host/f"]:
            self.run_cmd("download",url,f,code=1)
        self.run_cmd("download",self.base+"/file",f,"--delay-ms","-1",code=2)
        self.run_cmd("unknown",code=2)
        self.run_cmd("--help")

    def test_17_empty_remote_file(self):
        self.server.data=b""
        f=self.target()
        self.run_cmd("download",self.base+"/file",f)
        self.assertEqual(f.read_bytes(),b"")
        self.assertEqual([x for x in self.server.requests if x[0]=="GET"],[])

    def test_18_uncheckpointed_suffix_is_overwritten_on_resume(self):
        f=self.target()
        offset=self.pause(f)
        with f.open("r+b") as stream:
            stream.seek(offset)
            stream.write(b"UNCOMMITTED DATA")
        self.run_cmd("download",self.base+"/file",f)
        self.assertEqual(f.read_bytes(),DATA)

    def test_19_maximum_accepted_url_round_trips_through_state(self):
        self.server.data=DATA[:131072]
        url=self.base+"/"+"a"*(2047-len(self.base)-1)
        f=self.target()
        self.run_cmd("download",url,f)
        self.run_cmd("download",url,f)
        self.assertEqual(f.read_bytes(),self.server.data)

    def test_20_maximum_accepted_etag_round_trips_through_state(self):
        self.server.data=DATA[:131072]
        f=self.target()
        self.run_cmd("download",self.base+"/long-etag",f)
        self.run_cmd("download",self.base+"/long-etag",f)
        self.assertEqual(f.read_bytes(),self.server.data)

    def test_21_signed_content_range_is_rejected(self):
        self.run_cmd("download",self.base+"/signed-range",self.target(),code=1)

    def fault_env(self, plan):
        env=os.environ.copy()
        env["LD_PRELOAD"]=self.preload
        env["SPARSEGET_FAULT"]=plan
        return env

    def saved_offset(self, f):
        return int(Path(str(f)+".resume").read_text().splitlines()[1].split()[3])

    def test_22_sigkill_discards_uncommitted_tail(self):
        for after_checkpoint in [False,True]:
            with self.subTest(after_checkpoint=after_checkpoint):
                f=self.target("kill-%s" % after_checkpoint)
                p=subprocess.Popen([BIN,"download",self.base+"/file",str(f),"--delay-ms","50"],
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                sel=selectors.DefaultSelector()
                sel.register(p.stdout,selectors.EVENT_READ)
                pending=b""
                found=False
                try:
                    end=time.monotonic()+15
                    while time.monotonic()<end and not found:
                        if not sel.select(1):
                            continue
                        part=os.read(p.stdout.fileno(),4096)
                        if not part:
                            break
                        pending+=part
                        while b"\n" in pending:
                            line,pending=pending.split(b"\n",1)
                            if not line.startswith(b"progress="):
                                continue
                            written=int(line.split(b"/",1)[0].split(b"=")[1])
                            saved=int(line.split(b"saved_offset=")[1])
                            if written>saved and bool(saved)==after_checkpoint:
                                found=True
                                break
                    self.assertTrue(found)
                    p.kill()
                    p.communicate(timeout=10)
                    self.assertEqual(p.returncode,-signal.SIGKILL)
                finally:
                    sel.close()
                    if p.poll() is None:
                        p.kill()
                        p.communicate()
                saved=self.saved_offset(f)
                self.assertEqual(bool(saved),after_checkpoint)
                self.run_cmd("download",self.base+"/file",f)
                self.assertEqual([x for x in self.server.requests if x[0]=="GET"][-1][1],saved)
                self.assertEqual(f.read_bytes(),DATA)

    def test_23_sigkill_during_each_commit_stage(self):
        plans=[("data:2:kill-after",False), ("state:2:kill-after",False),
               ("rename:2:kill-before",False), ("rename:2:kill-after",True),
               ("dir:2:kill-before",True), ("dir:2:kill-after",True)]
        for i,(plan,committed) in enumerate(plans):
            with self.subTest(plan=plan):
                f=self.target("stage-%d" % i)
                self.run_cmd("download",self.base+"/file",f,env=self.fault_env(plan),code=-signal.SIGKILL)
                saved=self.saved_offset(f)
                self.assertEqual(bool(saved),committed)
                self.assertEqual(f.read_bytes()[:saved],DATA[:saved])
                self.run_cmd("download",self.base+"/file",f)
                self.assertEqual([x for x in self.server.requests if x[0]=="GET"][-1][1],saved)
                self.assertEqual(f.read_bytes(),DATA)

    def test_24_commit_errors_never_report_success_or_saved_pause(self):
        plans=["data:2:error","state:2:error","rename:2:error","dir:2:error",
               "data:2:interrupt-error"]
        for i,plan in enumerate(plans):
            with self.subTest(plan=plan):
                f=self.target("error-%d" % i)
                p=self.run_cmd("download",self.base+"/file",f,env=self.fault_env(plan),code=1)
                self.assertNotIn("paused:",p.stdout)
                self.assertNotIn("complete:",p.stdout)
                self.assertIn("NOT confirmed saved",p.stderr)
                self.run_cmd("download",self.base+"/file",f)
                self.assertEqual(f.read_bytes(),DATA)

    def test_25_final_partial_batch_is_saved(self):
        self.server.data=DATA[:150123]
        f=self.target()
        self.run_cmd("download",self.base+"/file",f)
        self.assertEqual(self.saved_offset(f),150123)
        self.assertEqual(f.read_bytes(),self.server.data)

    def test_26_non_hash_etag_and_independent_sha256(self):
        f=self.target()
        self.run_cmd("download",self.base+"/opaque-etag",f)
        self.assertIn('"revision-seven"',Path(str(f)+".resume").read_text())
        self.assertEqual(hashlib.sha256(f.read_bytes()).hexdigest(),hashlib.sha256(DATA).hexdigest())

    def test_27_checkpoint_syscall_count(self):
        if not shutil.which("strace"):
            self.skipTest("strace is not installed; syscall measurement skipped")
        binaries=[("current",BIN)]
        if os.environ.get("SPARSEGET_BASELINE"):
            binaries.insert(0,("baseline",os.environ["SPARSEGET_BASELINE"]))
        for label,binary in binaries:
            f=self.target(label+".bin")
            trace=self.target(label+".trace")
            p=subprocess.run(["strace","-qq","-e","trace=fdatasync,fsync,rename,renameat,renameat2","-o",str(trace),
                              binary,"download",self.base+"/file",str(f)],
                             capture_output=True,text=True,timeout=30)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            lines=trace.read_text().splitlines()
            counts={name:sum(line.startswith(name+"(") for line in lines)
                    for name in ["fdatasync","fsync","rename"]}
            # aarch64 的 libc rename 使用 renameat 系统调用。
            counts["rename"]+=sum(line.startswith(("renameat(","renameat2(")) for line in lines)
            print("\n4 MiB %s syscall counts: %s" % (label,counts),flush=True)
            self.assertEqual(f.read_bytes(),DATA)
            if label=="current":
                self.assertEqual(counts,{"fdatasync":5,"fsync":10,"rename":5})

    def test_28_sync_interrupted_by_signal_retries_and_saves(self):
        for event in ["data","state","dir"]:
            with self.subTest(event=event):
                f=self.target(event+"-eintr")
                p=self.run_cmd("download",self.base+"/file",f,
                               env=self.fault_env(event+":2:interrupt-eintr"),code=130)
                self.assertIn("paused:",p.stdout)
                self.assertNotIn("NOT confirmed saved",p.stderr)
                self.assertGreaterEqual(self.saved_offset(f),1024*1024)
                self.run_cmd("download",self.base+"/file",f)
                self.assertEqual(f.read_bytes(),DATA)

    def test_29_initial_io_error_can_retry_without_manual_cleanup(self):
        plans=["truncate:1:error","data:1:error","temp:1:error","write:1:error",
               "state:1:error","rename:1:error","dir:1:error"]
        sentinel=self.target("unrelated.txt")
        sentinel.write_bytes(b"KEEP")
        for i,plan in enumerate(plans):
            with self.subTest(plan=plan):
                f=self.target("initial-%d" % i)
                p=self.run_cmd("download",self.base+"/file",f,env=self.fault_env(plan),code=1)
                self.assertNotIn("complete:",p.stdout)
                if plan.startswith("dir:"):
                    self.assertTrue(f.exists())
                    self.assertEqual(self.saved_offset(f),0)
                else:
                    self.assertFalse(f.exists())
                    self.assertFalse(Path(str(f)+".resume").exists())
                    self.assertIn("initialization rolled back",p.stderr)
                self.run_cmd("download",self.base+"/file",f)
                self.assertEqual(f.read_bytes(),DATA)
                self.assertEqual(sentinel.read_bytes(),b"KEEP")

    def test_30_hard_link_cannot_bypass_managed_write_protection(self):
        f=self.target()
        self.pause(f)
        alias=self.target("alias.bin")
        os.link(f,alias)
        before=f.read_bytes()
        for path in [f,alias]:
            p=self.run_cmd("write",path,"0","BAD",code=1)
            self.assertIn("hard-linked",p.stderr)
        self.run_cmd("download",self.base+"/file",f,code=1)
        self.assertEqual(f.read_bytes(),before)
        alias.unlink()
        self.run_cmd("download",self.base+"/file",f)
        self.assertEqual(f.read_bytes(),DATA)

    def test_31_complete_reports_committed_final_offset(self):
        self.server.data=DATA[:150123]
        f=self.target()
        for _ in range(2):
            p=self.run_cmd("download",self.base+"/file",f)
            self.assertIn("complete: saved_offset=150123 total_bytes=150123",p.stdout)
            self.assertEqual(self.saved_offset(f),150123)

    def run_pty(self, *args, pause=False, width=110, extra_env=None):
        master,slave=os.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack("HHHH",30,width,0,0))
        env=os.environ.copy()
        env["TERM"]="xterm-256color"
        env.pop("NO_COLOR",None)
        if extra_env:
            env.update(extra_env)
        p=subprocess.Popen([BIN,*map(str,args)],stdin=subprocess.DEVNULL,
                           stdout=slave,stderr=slave,env=env)
        os.close(slave)
        sel=selectors.DefaultSelector()
        sel.register(master,selectors.EVENT_READ)
        out=b""
        sent=False
        try:
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                if not sel.select(0.5):
                    continue
                try:
                    block=os.read(master,65536)
                except OSError:
                    break  # Linux PTY: slave close is reported as EIO.
                if not block:
                    break
                out+=block
                if pause and not sent and b"\x1b[2K" in out:
                    p.send_signal(signal.SIGINT)
                    sent=True
            p.wait(timeout=5)
            if pause:
                self.assertTrue(sent,"no terminal progress frame received")
            return p.returncode,out.decode("utf-8")
        finally:
            sel.close()
            os.close(master)
            if p.poll() is None:
                p.kill()
                p.wait()

    def test_32_terminal_pause_resume_and_completion(self):
        f=self.target()
        code,out=self.run_pty("download",self.base+"/file",f,"--delay-ms","200",pause=True)
        self.assertEqual(code,130,out)
        self.assertIn("[开始下载]",out)
        self.assertIn("[已暂停]",out)
        self.assertNotIn("[已完成]",out)
        saved=self.saved_offset(f)
        self.assertIn("保存偏移：%d 字节" % saved,out)
        self.assertIn("\x1b[2K",out)
        self.assertNotIn("progress=",out)
        code,out=self.run_pty("download",self.base+"/file",f)
        self.assertEqual(code,0,out)
        self.assertIn("[断点续传]",out)
        self.assertIn("起始偏移：%d 字节" % saved,out)
        self.assertIn("[已完成]",out)
        self.assertIn("保存偏移：%d 字节" % len(DATA),out)
        self.assertEqual(f.read_bytes(),DATA)

    def test_33_terminal_status_and_plain_logs(self):
        f=self.target()
        code,out=self.run_pty("create",f,"1M")
        self.assertEqual(code,0,out)
        self.assertIn("[已创建]",out)
        code,out=self.run_pty("write",f,"4096","HELLO")
        self.assertEqual(code,0,out)
        self.assertIn("[已写入]",out)
        code,out=self.run_pty("status",f)
        self.assertEqual(code,0,out)
        self.assertIn("逻辑大小：1.00 MiB",out)
        self.assertIn("实际占用：4.00 KiB",out)
        self.assertIn("空洞占比：99.61%",out)
        plain=self.run_cmd("status",f).stdout
        self.assertNotIn("\x1b",plain)
        self.assertNotIn("\r",plain)
        self.assertIn("logical_bytes=1048576",plain)
        code,out=self.run_pty("status",f,extra_env={"TERM":"dumb"})
        self.assertEqual(code,0,out)
        self.assertNotIn("\x1b",out)
        self.assertIn("logical_bytes=1048576",out)

    def test_34_narrow_terminal_no_color_and_error_output(self):
        f=self.target()
        code,out=self.run_pty("download",self.base+"/file",f,"--delay-ms","200",
                             pause=True,width=40,extra_env={"NO_COLOR":"1"})
        self.assertEqual(code,130,out)
        self.assertIn("进度",out)
        self.assertNotIn("\x1b[1;",out)
        self.assertNotIn("\x1b[0m",out)
        code,out=self.run_pty("write",f,"0","BAD")
        self.assertEqual(code,1,out)
        self.assertIn("[错误]",out)
        bad=self.target("short-response.bin")
        code,out=self.run_pty("download",self.base+"/truncate",bad)
        self.assertEqual(code,1,out)
        self.assertNotIn("[已完成]",out)
        self.assertIn("\r\n[错误]",out)
        self.assertEqual(self.saved_offset(bad),100000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
