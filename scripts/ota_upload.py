# Signs firmware.bin with the OTA signing key and uploads it to the board
# over WiFi. Called by `pio run -e ota -t upload`, see platformio.ini.
import base64
import os
import subprocess
import sys

firmware, host = sys.argv[1], sys.argv[2]
project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
key = os.environ.get("OTA_SIGNING_KEY", os.path.join(project_dir, "ota_signing.pem"))

signature = subprocess.run(
    ["openssl", "dgst", "-sha256", "-sign", key, firmware], check=True, capture_output=True
).stdout

print(f"Uploading signed {firmware} to http://{host}/update", flush=True)
result = subprocess.run([
    "curl", "--fail-with-body", "--show-error", "--silent",
    "-H", "Content-Type: application/octet-stream",
    "-H", "X-Signature: " + base64.b64encode(signature).decode(),
    "--data-binary", "@" + firmware,
    f"http://{host}/update",
])
sys.exit(result.returncode)
