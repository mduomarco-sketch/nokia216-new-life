"""Fake vivagym.myvitale.com for tools/host_vg.c (plain HTTP on 127.0.0.1:8765).

Usage: python tools/mock_server.py <expected password>
"""
import json
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

PASSWORD = sys.argv[1] if len(sys.argv) > 1 else "secret"
# any non-empty client id is accepted (the real one is in your secrets.h)
TEMP = "TEMP/tok+en="  # characters that must be url-encoded in the form body
counter = {"user": 0}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, chunked=False):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Connection", "close")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(data), 17):
                part = data[i:i + 17]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()

    def new_user_token(self):
        counter["user"] += 1
        n = counter["user"]
        return {"access_token": f"USER{n}", "refresh_token": f"REF{n}", "token_type": "Bearer", "expires_in": 3600}

    def do_POST(self):
        p = urllib.parse.urlparse(self.path).path
        b = self.body()
        if p == "/oauth/v2/token":
            j = json.loads(b)
            ok = j.get("grant_type") == "client_credentials" and bool(j.get("client_id")) and bool(j.get("client_secret"))
            if ok:
                return self.send(200, json.dumps({"access_token": TEMP, "expires_in": 3600}))
            return self.send(400, '{"error":"invalid_client"}')
        if p == "/api/v2.0/es/exerp/newAuth":
            f = dict(urllib.parse.parse_qsl(b))
            print("  server got newAuth: email=%r password_ok=%s temp_ok=%s app=%r" % (
                f.get("email"), f.get("password") == PASSWORD, f.get("access_token") == TEMP, f.get("appName")), flush=True)
            if f.get("access_token") != TEMP:
                return self.send(400, '{"code":1,"message":"bad temp token"}')
            if f.get("password") != PASSWORD:
                return self.send(401, '{"code":2,"message":"Usuario o contrase\\u00f1a incorrectos"}')
            return self.send(200, json.dumps(self.new_user_token()), chunked=True)
        self.send(404, "{}")

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == "/api/email/refresh":
            if q.get("refresh_token", "").startswith("REF"):
                return self.send(200, json.dumps(self.new_user_token()))
            return self.send(400, '{"error":"invalid_grant"}')
        if u.path == "/api/v2.0/exerp/qr":
            if not self.headers.get("Authorization", "").startswith("Bearer USER"):
                return self.send(401, '{"error":"invalid_grant","error_description":"The access token provided has expired."}')
            ts = int(time.time() * 1000)
            return self.send(200, json.dumps(f"exerp:checkin:123p000000-{ts}-0123456789abcdef0123456789abcdef"))
        self.send(404, "{}")


HTTPServer(("127.0.0.1", 8765), H).serve_forever()
