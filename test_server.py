#!/usr/bin/env python3
"""
Synthetic spectrum server — protocol reference + GUI dev harness (no board, no DSP needed).

Single port: HTTP GET serves ./www static files, GET /ws upgrades to a binary WebSocket
speaking PROTOCOL.md v1. Generates a synthetic 100 MHz-span scene (noise floor, FM-like
carriers, a drifting CW tone, occasional bursts) at a configurable row rate.

stdlib only. Usage:  python3 test_server.py [--port 7681] [--bins 4096] [--rows 960]
"""

import argparse
import base64
import hashlib
import math
import os
import random
import socket
import struct
import threading
import time

MAGIC = 0x50534652
VERSION = 1
MSG_SPECTRA, MSG_STATUS, MSG_CONFIG, MSG_ACK = 1, 2, 16, 17
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WWW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "www")
REC = None
MIME = {".html": "text/html", ".js": "application/javascript", ".css": "text/css"}


def hdr(mtype, seq, payload_bytes):
	return struct.pack("<IBBHII", MAGIC, VERSION, mtype, 0, seq, payload_bytes)


class Scene:
	"""Synthetic dB spectrum. Hot path is C-level byte ops (randbytes + translate + map(max))
	so a pure-Python harness can still saturate thousands of rows/s."""

	def __init__(self, bins, db_min=-110.0, db_max=-10.0):
		self.bins = bins
		self.t = 0.0
		self.burst_until = 0.0
		self.burst_env = None
		q = lambda db: max(0, min(255, int((db - db_min) * 255.0 / (db_max - db_min))))
		self.q = q
		env = bytearray(bins)
		for i in range(bins):
			x = i / bins
			db = -130.0
			for (pos, width, pk) in [(0.18, 0.012, -28), (0.36, 0.010, -35), (0.55, 0.014, -31), (0.71, 0.006, -44)]:
				d = (x - pos) / width
				if abs(d) < 6.0:
					db = max(db, pk - d * d * 3.0)
			env[i] = q(db)
		self.env = bytes(env)
		# random byte -> noise level around -100 dB with ~5 dB spread
		self.noise_tab = bytes(q(-103.0 + (b / 255.0) * 6.0) for b in range(256))

	def row(self, buf, db_min, db_max):
		bins, t, q = self.bins, self.t, self.q
		self.t += 1.0 / 960
		noise = random.randbytes(bins).translate(self.noise_tab)
		buf[:] = bytes(map(max, self.env, noise))
		# drifting CW tone (~30 bins painted per row)
		c = int((0.85 + 0.1 * math.sin(t * 0.21)) * bins)
		for k in range(-15, 16):
			i = c + k
			if 0 <= i < bins:
				v = q(-20.0 - (k * k) * 0.35)
				if v > buf[i]:
					buf[i] = v
		# occasional wideband burst
		if t > self.burst_until and random.random() < 0.0007:
			self.burst_until = t + random.uniform(0.05, 0.4)
			c = random.uniform(0.1, 0.9)
			self.burst_env = bytes(q(-30.0 - ((i / bins - c) / 0.03) ** 2 * 2.0) if abs(i / bins - c) < 0.15 else 0 for i in range(bins))
		if t < self.burst_until and self.burst_env:
			buf[:] = bytes(map(max, buf, self.burst_env))


class Client(threading.Thread):
	def __init__(self, sock, args):
		super().__init__(daemon=True)
		self.sock = sock
		self.args = args
		self.seq = 0
		self.max_rows = args.rows
		self.mode = 0
		self.center = 100e6
		self.span = 100e6
		self.gap_until = 0.0
		self.live_payload = bytearray(0)
		self.lock = threading.Lock()
		self.alive = True
		self.reported_backlog = 0

	def ws_send(self, payload):
		n = len(payload)
		if n < 126:
			head = struct.pack("!BB", 0x82, n)
		elif n < 65536:
			head = struct.pack("!BBH", 0x82, 126, n)
		else:
			head = struct.pack("!BBQ", 0x82, 127, n)
		with self.lock:
			self.sock.sendall(head + payload)

	def reader(self):
		try:
			while self.alive:
				h = self.sock.recv(2)
				if len(h) < 2:
					break
				ln = h[1] & 0x7F
				if ln == 126:
					ln = struct.unpack("!H", self.sock.recv(2))[0]
				elif ln == 127:
					ln = struct.unpack("!Q", self.sock.recv(8))[0]
				mask = self.sock.recv(4) if h[1] & 0x80 else b"\0\0\0\0"
				data = bytearray()
				while len(data) < ln:
					chunk = self.sock.recv(ln - len(data))
					if not chunk:
						return
					data += chunk
				for i in range(len(data)):
					data[i] ^= mask[i & 3]
				op = h[0] & 0x0F
				if op == 8:
					break
				if op == 9:  # ping -> pong
					with self.lock:
						self.sock.sendall(struct.pack("!BB", 0x8A, 0))
					continue
				if op != 2 or len(data) < 16:
					continue
				magic, ver, mtype = struct.unpack_from("<IBB", data, 0)
				if magic != MAGIC or ver != VERSION:
					continue
				if mtype == MSG_CONFIG:
					max_rows, mode = struct.unpack_from("<HH", data, 16)
					self.max_rows = max_rows or self.args.rows
					self.mode = mode
					print(f"[cfg] max_rows={self.max_rows} mode={mode}")
				elif mtype == 18:  # TUNE: f64 center freq
					freq = struct.unpack_from("<d", data, 16)[0]
					def retune(f=freq):
						time.sleep(0.18)
						self.center = f
						print(f"[tune] center={f/1e6:.3f}M")
					threading.Thread(target=retune, daemon=True).start()
				elif mtype == 20:  # RATE: u32 Hz on the 2x ladder; ~like the radio: quiesce gap + new span
					rate = struct.unpack_from("<I", data, 16)[0]
					if rate in (100000000, 50000000, 25000000, 12500000, 6250000, 3125000):
						def rechannel(rt=rate):
							self.gap_until = time.monotonic() + 0.30
							time.sleep(0.30)
							self.span = float(rt)
							print(f"[rate] span={rt/1e6:.3f}M")
						threading.Thread(target=rechannel, daemon=True).start()
				elif mtype == MSG_ACK:
					_, backlog = struct.unpack_from("<II", data, 16)
					self.reported_backlog = backlog
		except OSError:
			pass
		finally:
			self.alive = False

	def run(self):
		threading.Thread(target=self.reader, daemon=True).start()
		bins, batch = self.args.bins, self.args.batch
		db_min, db_max = -110.0, -10.0
		scene = Scene(bins)
		payload = bytearray(batch * bins)
		rows_sent = drop = 0
		rate = self.max_rows
		last_status = time.monotonic()
		try:
			while self.alive:
				rate = self.max_rows
				if self.reported_backlog > batch * bins * 4:
					rate = max(60, rate // 2)  # client is drowning: back off
				t0 = time.monotonic()
				for r in range(batch):
					scene.row(memoryview(payload)[r * bins:(r + 1) * bins], db_min, db_max)
				if time.monotonic() < self.gap_until:
					time.sleep(0.02)
					continue
				if self.mode == 3:
					# live framing: k contiguous lines at the live plane width (2048)
					lb = 2048
					if len(self.live_payload) != batch * lb:
						self.live_payload = bytearray(batch * lb)
					for r in range(batch):
						row = memoryview(payload)[r * bins:(r + 1) * bins]
						out = memoryview(self.live_payload)[r * lb:(r + 1) * lb]
						for i in range(lb):
							a = row[2 * i]
							b = row[2 * i + 1]
							out[i] = a if a > b else b
					sp = hdr(MSG_SPECTRA, self.seq, 48 + batch * lb) + struct.pack(
						"<IHHffddIIII", lb, batch, 3, db_min, db_max,
						self.center, self.span, bins, 1, rate, 0)
					self.ws_send(sp + self.live_payload)
					self.seq += 1
					rows_sent += batch
					if time.monotonic() - last_status >= 1.0:
						st = hdr(MSG_STATUS, self.seq, 32) + struct.pack(
							"<IIIfffII", rate * 4, rows_sent, drop, 92.0, 0.35, 0.4, 0, 0)
						self.ws_send(st)
						self.seq += 1
						rows_sent = drop = 0
						last_status = time.monotonic()
					dt = batch / max(rate, 1) - (time.monotonic() - t0)
					if dt > 0:
						time.sleep(dt)
					continue
				sp = hdr(MSG_SPECTRA, self.seq, 48 + len(payload)) + struct.pack(
					"<IHHffddIIII", bins, batch, self.mode, db_min, db_max,
					self.center, self.span, bins, 1 if self.mode == 0 else 32, rate, 0)
				self.ws_send(sp + payload)
				self.seq += 1
				rows_sent += batch
				if time.monotonic() - last_status >= 1.0:
					st = hdr(MSG_STATUS, self.seq, 32) + struct.pack(
						"<IIIfffII", rate * 4, rows_sent, drop, 92.0, 0.35, 0.4, 0, 0)
					self.ws_send(st)
					self.seq += 1
					rows_sent = drop = 0
					last_status = time.monotonic()
				dt = batch / max(rate, 1) - (time.monotonic() - t0)
				if dt > 0:
					time.sleep(dt)
		except OSError:
			pass
		finally:
			self.alive = False
			try:
				self.sock.close()
			except OSError:
				pass


def http_serve(sock, args):
	try:
		req = b""
		while b"\r\n\r\n" not in req:
			chunk = sock.recv(4096)
			if not chunk:
				return
			req += chunk
		head = req.decode("latin1")
		line = head.split("\r\n")[0]
		path = line.split(" ")[1].split("?")[0]
		hdrs = {}
		for l in head.split("\r\n")[1:]:
			if ":" in l:
				k, v = l.split(":", 1)
				hdrs[k.strip().lower()] = v.strip()
		if path.startswith("/api/app/vsg/"):
			import json as _j
			global REC
			if path == "/api/app/vsg/record":
				REC = {"state": 1, "got": 0, "t0": time.monotonic(), "name": "test.cs16"}
				body = _j.dumps({"ok": 1, "name": "test.cs16"}).encode()
			else:
				if REC and REC["state"] == 1:
					REC["got"] = int((time.monotonic() - REC["t0"]) * 10e6)
					if time.monotonic() - REC["t0"] > 2.5:
						REC["state"] = 2
				body = _j.dumps({"rec_state": REC["state"] if REC else 0, "rec_got": REC["got"] if REC else 0}).encode()
			sock.sendall((f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n"
			              "Connection: close\r\n\r\n").encode() + body)
			return
		if path == "/ws" and "websocket" in hdrs.get("upgrade", "").lower():
			accept = base64.b64encode(hashlib.sha1((hdrs["sec-websocket-key"] + WS_GUID).encode()).digest()).decode()
			sock.sendall((f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
			              f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n").encode())
			print(f"[ws] client {sock.getpeername()}")
			Client(sock, args).run()
			return
		fp = os.path.normpath(os.path.join(WWW, path.lstrip("/") or "index.html"))
		if fp == WWW:
			fp = os.path.join(WWW, "index.html")
		if not fp.startswith(WWW) or not os.path.isfile(fp):
			sock.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
			return
		body = open(fp, "rb").read()
		mime = MIME.get(os.path.splitext(fp)[1], "application/octet-stream")
		sock.sendall((f"HTTP/1.1 200 OK\r\nContent-Type: {mime}\r\nContent-Length: {len(body)}\r\n"
		              f"Cache-Control: no-cache\r\nConnection: close\r\n\r\n").encode() + body)
	except OSError:
		pass
	finally:
		if not threading.current_thread().name.startswith("ws"):
			try:
				sock.close()
			except OSError:
				pass


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--port", type=int, default=7681)
	ap.add_argument("--bins", type=int, default=4096)
	ap.add_argument("--rows", type=int, default=960, help="rows/s default (client CONFIG overrides)")
	ap.add_argument("--batch", type=int, default=16, help="rows per SPECTRA message")
	args = ap.parse_args()
	srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
	srv.bind(("0.0.0.0", args.port))
	srv.listen(8)
	print(f"http+ws on :{args.port}  (bins={args.bins} rows/s={args.rows} batch={args.batch})")
	while True:
		c, _ = srv.accept()
		c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
		threading.Thread(target=http_serve, args=(c, args), daemon=True).start()


if __name__ == "__main__":
	main()
