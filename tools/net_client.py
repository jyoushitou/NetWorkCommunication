#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NetWorkCommunication 的 Python 客户端 / 服务端（与 C++ 版对端协议一致）

线上格式（全部大端 / network byte order）：

    +-----------------+-----------------+-------------------+
    | msg_id (8 字节) | len (4 字节)     | body (len 字节)    |
    +-----------------+-----------------+-------------------+

约束（与 connon/source/NetConnection.cpp 的解析逻辑一致）：
  * len 必须满足 0 < len <= 1 MiB(1048576)，否则判为非法消息并断开连接；
  * msg_id 0 / 1 / 2 是保留 ID（连接测试 / 空闲超时提醒 / 忙超时提醒），
    业务消息建议从 1000 起；
  * 消息体是原始字节，默认按 UTF-8 编解码，加 --hex 可原样查看。

两种角色：

  客户端（默认）：连到某个服务端
      python tools/net_client.py --host 127.0.0.1 --port 26990
      python tools/net_client.py --host 10.200.36.112 -p 26990 -m "hello"

  服务端：默认绑定 0.0.0.0（内网任意网卡都能连），收到什么就原样回什么
      python tools/net_client.py --listen -p 26990
      python tools/net_client.py --listen --bind 192.168.1.10 -p 26990   # 只绑某张网卡

  注意：服务端模式要能被别的机器访问，Windows 防火墙得放行该端口（管理员执行一次）：
      New-NetFirewallRule -DisplayName "CommonNet 26990" -Direction Inbound `
          -Protocol TCP -LocalPort 26990 -Action Allow
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
from typing import Iterator, List, Optional, Tuple

# ── 协议常量（与 connon/include/NetConnection.h 对齐）────────────────────────
HEAD_ID_LENGTH = 8                 # msg_id 字段长度
HEAD_LEN_LENGTH = 4                # len 字段长度
HEAD_LENGTH = HEAD_ID_LENGTH + HEAD_LEN_LENGTH  # 12
MAX_LENGTH = 1024 * 1024           # 单条消息体上限 1 MiB
CHECK_CONNECTION_MSG_ID = 0        # 连接测试（客户端连上后先发这条）
SERVER_TIMEOUT_MSG_ID = 1          # 服务端：空闲超时提醒
SERVER_BUSY_TIMEOUT_MSG_ID = 2     # 服务端：忙超时提醒
DEFAULT_MSG_ID = 1000              # 业务消息起始 ID（README 约定从 1000 起）
CLOSE_COMMAND = "EXIT"             # 收到内容恰为 EXIT 的消息即关闭会话
REPLY_TEXT = "服务器已经收到消息"   # 与 examples/Server/main.cpp 的回包保持一致

_RESERVED = {
    CHECK_CONNECTION_MSG_ID: "check-connection",
    SERVER_TIMEOUT_MSG_ID: "server-timeout-reminder",
    SERVER_BUSY_TIMEOUT_MSG_ID: "server-busy-reminder",
}

_print_lock = threading.Lock()


def log(text: str) -> None:
    with _print_lock:
        print(text, flush=True)


def pack_frame(msg_id: int, payload: bytes) -> bytes:
    """把 msg_id + 消息体封装成一帧。非法长度直接抛 ValueError，不发脏数据。"""
    if not 0 < len(payload) <= MAX_LENGTH:
        raise ValueError(
            f"消息体长度必须在 1..{MAX_LENGTH} 字节之间（当前 {len(payload)}）；"
            "长度为 0 会被判定为非法消息并断开连接"
        )
    # "!QI" = 大端 uint64 + 大端 uint32，正好对应 C++ 侧的 htonll(8B) + htonl(4B)
    return struct.pack("!QI", msg_id & 0xFFFFFFFFFFFFFFFF, len(payload)) + payload


def recv_exact(sock: socket.socket, n: int) -> Optional[bytes]:
    """读满 n 字节；对端正常关闭返回 None，连接异常抛 OSError。"""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def iter_frames(sock: socket.socket) -> Iterator[Tuple[int, bytes]]:
    """按帧解析对端数据，yield (msg_id, body)；对端关闭即结束。"""
    while True:
        head = recv_exact(sock, HEAD_LENGTH)
        if head is None:
            return
        msg_id, msg_len = struct.unpack("!QI", head)
        if not 0 < msg_len <= MAX_LENGTH:
            raise ValueError(f"收到非法长度 {msg_len}，按协议视为连接已损坏")
        body = recv_exact(sock, msg_len)
        if body is None:
            return
        yield msg_id, body


def local_ipv4_addresses() -> List[str]:
    """列出本机可用于内网访问的 IPv4（用于服务端启动时提示对方该连哪个地址）。"""
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    # 兜底：靠一次不会真正发包的 UDP connect 拿到默认出口地址
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 53))
        ips.add(probe.getsockname()[0])
        probe.close()
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith("127."))


def describe(payload: bytes, show_hex: bool) -> str:
    if show_hex:
        return f"{payload.hex(' ')}  ({len(payload)}B)"
    return payload.decode("utf-8", errors="replace")


# ══════════════════════════════════════════════════════════════════════════
#  客户端
# ══════════════════════════════════════════════════════════════════════════
class Client:
    """极简客户端：一个接收线程负责打印，主线程负责输入与发送。"""

    def __init__(self, host: str, port: int, timeout: float, show_hex: bool) -> None:
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(None)  # 接收线程用阻塞读，超时只用于连接阶段
        self._show_hex = show_hex
        self._closed = False
        self._rx_thread = threading.Thread(target=self._recv_loop, name="rx", daemon=True)

    def _recv_loop(self) -> None:
        try:
            for msg_id, body in iter_frames(self._sock):
                tag = _RESERVED.get(msg_id, "")
                label = f"[RX] id={msg_id}" + (f" ({tag})" if tag else "")
                log(f"{label} {describe(body, self._show_hex)!r}")
        except (OSError, ValueError) as exc:
            if not self._closed:
                log(f"[RX] 连接异常结束：{exc}")
        finally:
            if not self._closed:
                log("[RX] 服务端已关闭连接")
            self.close()

    def send(self, msg_id: int, text: str) -> bool:
        payload = text.encode("utf-8")
        try:
            frame = pack_frame(msg_id, payload)
        except ValueError as exc:
            log(f"[TX] 未发送：{exc}")
            return False
        try:
            self._sock.sendall(frame)
        except OSError as exc:
            log(f"[TX] 发送失败：{exc}")
            return False
        log(f"[TX] id={msg_id} {describe(payload, self._show_hex)!r}")
        return True

    def start(self) -> None:
        self._rx_thread.start()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._sock.shutdown(socket.SHUT_RDWR)  # 唤醒阻塞在 recv 上的接收线程
        except OSError:
            pass
        self._sock.close()

    @property
    def closed(self) -> bool:
        return self._closed


def run_client(args: argparse.Namespace) -> int:
    try:
        client = Client(args.host, args.port, args.timeout, args.hex)
    except OSError as exc:
        print(f"连接 {args.host}:{args.port} 失败：{exc}", file=sys.stderr)
        return 1

    client.start()
    log(f"已连接 {args.host}:{args.port}（Ctrl+C 或输入 quit 退出）")

    # 与 C++ 客户端一致：连上先发一条连接测试
    if not args.no_check:
        client.send(CHECK_CONNECTION_MSG_ID, "check connection")

    next_id = args.id_base
    exit_code = 0
    try:
        if args.message is not None:
            msg_id = args.id if args.id is not None else next_id
            exit_code = 0 if client.send(msg_id, args.message) else 2
            threading.Event().wait(0.3)  # 稍等回包
        else:
            while not client.closed:
                try:
                    line = input()
                except EOFError:
                    break
                if not line:
                    continue  # 空行不发（协议不允许 0 长度）
                if line in ("quit", "exit"):
                    break
                msg_id = args.id if args.id is not None else next_id
                if args.id is None:
                    next_id += 1
                client.send(msg_id, line)
    except KeyboardInterrupt:
        print("\n收到 Ctrl+C，正在退出…", flush=True)
    finally:
        if args.send_exit and not client.closed:
            client.send(args.id if args.id is not None else next_id, CLOSE_COMMAND)
        client.close()
    return exit_code


# ══════════════════════════════════════════════════════════════════════════
#  服务端
# ══════════════════════════════════════════════════════════════════════════
def handle_connection(conn: socket.socket, addr: Tuple[str, int], show_hex: bool) -> None:
    peer = f"{addr[0]}:{addr[1]}"
    log(f"[+] {peer} 已连接")
    try:
        for msg_id, body in iter_frames(conn):
            tag = _RESERVED.get(msg_id, "")
            label = f"[RX] <{peer}> id={msg_id}" + (f" ({tag})" if tag else "")
            log(f"{label} {describe(body, show_hex)!r}")

            # 协议约定：内容恰为 EXIT 即关闭会话，且不回包
            if body == CLOSE_COMMAND.encode():
                log(f"[-] {peer} 发送 {CLOSE_COMMAND}，关闭会话")
                return

            conn.sendall(pack_frame(msg_id, REPLY_TEXT.encode("utf-8")))
    except (OSError, ValueError) as exc:
        log(f"[!] {peer} 断开：{exc}")
    finally:
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        conn.close()
        log(f"[-] {peer} 已断开")


def run_server(args: argparse.Namespace) -> int:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind((args.bind, args.port))
    except OSError as exc:
        print(f"绑定 {args.bind}:{args.port} 失败：{exc}", file=sys.stderr)
        return 1
    srv.listen(64)

    log(f"服务端已启动，绑定 {args.bind}:{args.port}")
    if args.bind in ("0.0.0.0", ""):
        lan = local_ipv4_addresses()
        if lan:
            log("内网可用的连接地址：" + "  ".join(f"{ip}:{args.port}" for ip in lan))
        else:
            log("没有探测到内网 IPv4，别的机器请用本机实际网卡地址连接")
        log("若别的机器连不上，检查 Windows 防火墙是否放行了该端口（见文件头注释）")
    else:
        log(f"仅监听 {args.bind}，其它网卡上的连接会被拒绝")

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(
                target=handle_connection, args=(conn, addr, args.hex), daemon=True
            ).start()
    except KeyboardInterrupt:
        log("\n收到 Ctrl+C，服务端退出")
    finally:
        srv.close()
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="NetWorkCommunication 的 Python 客户端 / 服务端（协议与 C++ 版一致）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--listen", action="store_true",
                        help="以服务端模式运行（默认绑定 0.0.0.0，内网可访问）")
    parser.add_argument("--bind", default="0.0.0.0",
                        help="[服务端] 监听地址；0.0.0.0 表示所有网卡")
    parser.add_argument("-H", "--host", default="127.0.0.1",
                        help="[客户端] 要连接的服务端地址（内网请填对方 IP，不能填 0.0.0.0）")
    parser.add_argument("-p", "--port", type=int, default=26990, help="端口")
    parser.add_argument("-m", "--message", help="[客户端] 发送这一条消息后退出")
    parser.add_argument("--id", type=int, help="固定使用该 msg_id；不给则从 --id-base 自增")
    parser.add_argument("--id-base", type=int, default=DEFAULT_MSG_ID,
                        help="[客户端] 业务消息起始 msg_id（每条 +1）")
    parser.add_argument("--timeout", type=float, default=5.0, help="[客户端] 连接超时（秒）")
    parser.add_argument("--hex", action="store_true", help="以十六进制显示消息体")
    parser.add_argument("--no-check", action="store_true",
                        help="[客户端] 连接后不自动发送 msg_id=0 的 check connection")
    parser.add_argument("--send-exit", action="store_true",
                        help="[客户端] 退出前发送一条内容为 EXIT 的消息，让对端优雅关闭会话")
    args = parser.parse_args(argv)

    # Windows 控制台默认 GBK，中文回显会乱码，这里强制 UTF-8
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    if args.listen:
        return run_server(args)

    if args.host in ("0.0.0.0", "::", ""):
        print("0.0.0.0 是「监听所有网卡」的绑定地址，不能作为连接目标；"
              "连内网请填对方具体 IP，例如 --host 10.200.36.112", file=sys.stderr)
        return 2
    return run_client(args)


if __name__ == "__main__":
    sys.exit(main())
