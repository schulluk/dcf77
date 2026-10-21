# Signs firmware.bin with the OTA signing key and uploads it to the board
# over WiFi. Called by `pio run -e ota -t upload`, see platformio.ini.
# DCF77_HOST overrides the board's address, e.g. with its IP.
import base64
import os
import socket
import subprocess
import sys
import time

firmware, host = sys.argv[1], os.environ.get("DCF77_HOST", sys.argv[2])
project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
key = os.environ.get("OTA_SIGNING_KEY", os.path.join(project_dir, "ota_signing.pem"))

signature = subprocess.run(
    ["openssl", "dgst", "-sha256", "-sign", key, firmware], check=True, capture_output=True
).stdout

# .local names go over mDNS, which an ESP32 doesn't always answer on the
# first try. Resolve with retries, then talk to the IP.
for attempt in range(10):
    try:
        ip = socket.getaddrinfo(host, 80, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
        break
    except socket.gaierror:
        time.sleep(1)
else:
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
