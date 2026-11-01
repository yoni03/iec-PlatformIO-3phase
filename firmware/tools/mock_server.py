"""Mock of the device's web API, for developing the UI on a PC.

    make -C test/host sim-data          # once: simulated history (C++ firmware code)
    python3 tools/mock_server.py         # then open http://localhost:8080  (admin / password123)
    python3 tools/mock_server.py 8081 --setup     # first-time setup screen
    python3 tools/mock_server.py 8082 --account   # Wi-Fi preset in secrets.h, no user yet

Serves web/index.html and answers the same API as the ESP32 (src/web.cpp),
with history from tools/sim-data/*.bin.
"""
import http.server
import json
import math
import os
import random
import secrets
import struct
import sys
import time
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tools", "sim-data")
REC = struct.Struct("<6I13H2B")
assert REC.size == 52
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
BOOT = time.time()


def load(name):
    path = os.path.join(DATA, name)
    if not os.path.exists(path):
        return []
    raw = open(path, "rb").read()
    return [raw[i:i + 52] for i in range(0, len(raw) - 51, 52)]


TIERS = {"m": load("minute.bin"), "h": load("hour.bin"), "d": load("day.bin")}
SESSIONS = set()
SETTINGS = {
    "price": 635200, "fixed": 0, "bill_day": 1, "breaker": 25, "demo": True,
    "host": "electricity-meter", "ssid": "HomeWiFi", "user": "admin", "ap_ssid": "HElectricity",
    "ver": "1.0.0",
    "rules": [{"en": 1, "days": 127, "s": 23, "e": 7, "pct": 200}] + [{"en": 0, "days": 127, "s": 0, "e": 24, "pct": 0}] * 3,
    "fs": {"m": 5, "h": 13, "d": 2},
}
CONFIGURED = "--setup" not in sys.argv and "--account" not in sys.argv
HAS_WIFI = "--setup" not in sys.argv  # --account: Wi-Fi preset in secrets.h, no user yet


def ts_of(r):
    return struct.unpack_from("<I", r)[0]


def today_summary():
    now = time.time()
    lt = time.localtime(now)
    day = int(time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1)))
    recs = [REC.unpack(r) for r in TIERS["m"] if ts_of(r) >= day]
    if not recs:
        return {"ok": True, "day": day, "imp": 0, "exp": 0, "cost": 0, "peak": 0, "peak_ts": 0, "base": 0}
    peak = max(recs, key=lambda r: r[7])
    return {"ok": True, "day": day, "imp": sum(r[3] for r in recs), "exp": sum(r[4] for r in recs),
            "cost": sum(r[5] for r in recs), "peak": peak[7], "peak_ts": peak[0], "base": min(r[6] for r in recs)}


def live_power(t):
    base = 650 + 300 * math.sin(t / 90) + random.random() * 60
    return [int(base * 0.45), int(base * 0.35), int(base * 0.2)]


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def authed(self):
        c = self.headers.get("Cookie", "")
        return any(f"sid={s}" in c for s in SESSIONS)

    def send(self, code, body, ctype="application/json", headers=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == "/":
            return self.send(200, open(os.path.join(ROOT, "web", "index.html"), "rb").read(), "text/html; charset=utf-8")
        if u.path == "/api/status":
            return self.send(200, {"configured": CONFIGURED, "authed": self.authed(), "setup": not HAS_WIFI,
                                   "has_wifi": HAS_WIFI, "has_user": CONFIGURED, "ap": not HAS_WIFI,
                                   "host": "electricity-meter", "ver": "1.0.0", "ap_ssid": "HElectricity"})
        if u.path == "/api/scan":
            return self.send(200, {"list": [{"ssid": "HomeWiFi", "rssi": -52, "open": False},
                                            {"ssid": "Neighbor", "rssi": -80, "open": False}]})
        if not self.authed():
            return self.send(401, {"error": "auth"})
        if u.path == "/api/live":
            t = time.time()
            pl = live_power(t)
            up = int((t - BOOT) * 1000)
            return self.send(200, {
                "up": up, "now": int(t * 1000), "tsrc": "ntp", "ok": True, "age": 600,
                "m": {"p": sum(pl), "pe": 0, "ph": 3, "pp": True, "tariff": 1, "imp": 12345678, "exp": 0,
                      "t": [12345678, 0, 0], "l": [[pl[0], 2312, pl[0] * 100 // 231], [pl[1], 2298, pl[1] * 100 // 230],
                                                   [pl[2], 2321, pl[2] * 100 // 232]]},
                "today": today_summary(), "price": 508160 if time.localtime().tm_hour >= 23 or time.localtime().tm_hour < 7 else 635200,
                "disc": 200 if time.localtime().tm_hour >= 23 or time.localtime().tm_hour < 7 else 0,
                "st": {"ok": up // 1000, "crc": 2, "ovf": 0},
                "sys": {"heap": 180000, "rssi": -61, "wifi": True, "ap": False, "fs_used": 640000, "fs_total": 1441792,
                        "demo": SETTINGS["demo"]}})
        if u.path == "/api/recent":
            since = int(q.get("since", 0))
            up = int((time.time() - BOOT) * 1000)
            start = max(since + 1000, up - 900_000)
            s = []
            for t in range(start - start % 1000, up, 1000):
                if t <= since:
                    continue
                pl = live_power(BOOT + t / 1000)
                s.append([t, sum(pl), 0, *pl])
            return self.send(200, {"up": up, "now": int(time.time() * 1000), "s": s})
        if u.path == "/api/history":
            f, to = int(q.get("from", 0)), int(q.get("to", 2**32 - 1))
            recs = [r for r in TIERS.get(q.get("tier", "m"), []) if f <= ts_of(r) < to]
            return self.send(200, b"IECH" + struct.pack("<HH", 1, 52) + b"".join(recs), "application/octet-stream")
        if u.path == "/api/export.csv":
            f, to = int(q.get("from", 0)), int(q.get("to", 2**32 - 1))
            lines = ["﻿זמן,צריכה (kWh),עלות (₪)"]
            for r in TIERS.get(q.get("tier", "m"), []):
                v = REC.unpack(r)
                if f <= v[0] < to:
                    lines.append(f"{time.strftime('%Y-%m-%d %H:%M', time.localtime(v[0]))},{v[3] / 1000:.3f},{v[5] / 1e6:.4f}")
            return self.send(200, "\r\n".join(lines), "text/csv; charset=utf-8",
                             [("Content-Disposition", 'attachment; filename="electricity.csv"')])
        if u.path == "/api/settings":
            return self.send(200, SETTINGS)
        self.send(404, {"error": "not found"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        if self.headers.get("X-Requested-With") != "fetch":
            return self.send(403, {"error": "csrf"})
        q = dict(urllib.parse.parse_qsl(body.decode(errors="ignore")))
        if u.path == "/api/login":
            if q.get("user") == "admin" and q.get("pass") == "password123":
                s = secrets.token_hex(16)
                SESSIONS.add(s)
                return self.send(200, {"ok": True}, headers=[("Set-Cookie", f"sid={s}; Path=/; HttpOnly; SameSite=Strict")])
            return self.send(401, {"error": "bad"})
        if u.path == "/api/setup":
            return self.send(200, {"ok": True, "reboot": bool(q.get("ssid")) or not HAS_WIFI,
                                   "host": q.get("host") or "electricity-meter"})
        if not self.authed():
            return self.send(401, {"error": "auth"})
        if u.path == "/api/settings":
            for k in ("price", "fixed", "bill_day", "breaker"):
                if k in q:
                    SETTINGS[k] = int(q[k])
            if "demo" in q:
                SETTINGS["demo"] = q["demo"] == "1"
            for i in range(4):
                if f"r{i}_en" in q:
                    SETTINGS["rules"][i] = {"en": int(q[f"r{i}_en"]), "pct": int(q[f"r{i}_pct"]), "s": int(q[f"r{i}_s"]),
                                            "e": int(q[f"r{i}_e"]), "days": int(q[f"r{i}_days"])}
            return self.send(200, {"ok": True})
        if u.path == "/api/logout":
            return self.send(200, {"ok": True}, headers=[("Set-Cookie", "sid=; Path=/; Max-Age=0")])
        return self.send(200, {"ok": True})


if __name__ == "__main__":
    print(f"mock server on http://localhost:{PORT}  (admin / password123)")
    http.server.ThreadingHTTPServer(("", PORT), H).serve_forever()
