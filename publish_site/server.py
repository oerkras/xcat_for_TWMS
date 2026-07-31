from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import argparse
import os


class PublishHandler(SimpleHTTPRequestHandler):
    server_version = "XCatTwmsPublish/1.0"

    def do_GET(self):
        if self.path.split("?", 1)[0] in ("/health", "/health/"):
            body = b"ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return super().do_GET()

    def end_headers(self):
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(403, "Directory listing is disabled")
        return None

    def log_message(self, fmt, *args):
        client = self.client_address[0] if self.client_address else "-"
        print(f"{self.log_date_time_string()} {client} {fmt % args}", flush=True)


def main():
    parser = argparse.ArgumentParser(description="XCat TWMS temporary publish server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=52080)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent))
    args = parser.parse_args()

    root = Path(args.root).resolve()
    os.chdir(root)

    httpd = ThreadingHTTPServer((args.host, args.port), PublishHandler)
    print(f"Serving {root} on http://{args.host}:{args.port}/", flush=True)
    print("Local URL: http://127.0.0.1:52080/", flush=True)
    print("Press Ctrl+C to stop.", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
