# Signs firmware.bin with the OTA signing key and uploads it to the board
# over WiFi. Called by `pio run -e ota -t upload`, see platformio.ini.
# DCF77_HOST overrides the board's address, e.g. with its IP.
import base64
import os
import socket
import struct
import subprocess
import sys

firmware, host = sys.argv[1], os.environ.get("DCF77_HOST", sys.argv[2])
project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
key = os.environ.get("OTA_SIGNING_KEY", os.path.join(project_dir, "ota_signing.pem"))

signature = subprocess.run(
    ["openssl", "dgst", "-sha256", "-sign", key, firmware], check=True, capture_output=True
).stdout

def mdns_lookup(name, attempts=5, timeout=1.0):
    """Asks for name's IPv4 address over mDNS directly. The system resolver
    caches a negative answer for a minute if the ESP32 is slow to reply,
    so retrying through it doesn't help."""
    labels = b"".join(bytes([len(p)]) + p.encode() for p in name.rstrip(".").split("."))
    query = b"\0\0\0\0\0\1\0\0\0\0\0\0" + labels + b"\0\0\1\0\1"  # one A/IN question
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        for _ in range(attempts):
            # From a port other than 5353, the responder answers us by
            # unicast (RFC 6762, 6.7).
            sock.sendto(query, ("224.0.0.251", 5353))
            try:
                while True:
                    ip = parse_a_record(sock.recv(1500))
                    if ip:
                        return ip
            except socket.timeout:
                pass
    finally:
        sock.close()
    return None


def parse_a_record(msg):
    def skip_name(pos):
        while msg[pos]:
            if msg[pos] >= 0xC0:  # compression pointer ends the name
                return pos + 2
            pos += msg[pos] + 1
        return pos + 1

    questions, answers = struct.unpack(">HH", msg[4:8])
    pos = 12
    for _ in range(questions):
        pos = skip_name(pos) + 4
    for _ in range(answers):
        pos = skip_name(pos)
        rtype, _, _, length = struct.unpack(">HHIH", msg[pos:pos + 10])
        pos += 10
        if rtype == 1 and length == 4:
            return socket.inet_ntoa(msg[pos:pos + 4])
        pos += length
    return None


ip = mdns_lookup(host) if host.endswith(".local") else None
if not ip:
    try:
        ip = socket.getaddrinfo(host, 80, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
    except socket.gaierror:
        sys.exit(f"Could not resolve {host}. Is the board on WiFi? "
                 "Try its IP from the router: DCF77_HOST=<ip> pio run -e ota -t upload")

print(f"Uploading signed {firmware} to http://{host}/update ({ip})", flush=True)
result = subprocess.run([
    "curl", "--fail-with-body", "--show-error", "--silent",
    "-H", "Content-Type: application/octet-stream",
    "-H", "X-Signature: " + base64.b64encode(signature).decode(),
    "--data-binary", "@" + firmware,
    f"http://{ip}/update",
])
sys.exit(result.returncode)
