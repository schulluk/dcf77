# DCF77

ESP32 projects that keep a projection alarm clock on the right time.

| Folder | What it is | Status |
|---|---|---|
| [`transmitter/`](transmitter/) | DCF77 mini-transmitter: feeds a radio-controlled clock a clean 77.5 kHz time signal from a few centimeters away, time from NTP | built, in use |
| [`projector/`](projector/) | Drives the projection unit of a projection clock directly from an ESP32, time from NTP, no radio at all | planning |

Each folder is its own PlatformIO project: run `pio` inside it, e.g.
`cd transmitter && pio run -t upload`.

Repo-wide: `.gitignore` keeps WiFi credentials (`include/config.h` in every
project) and any key file out of git, and `.githooks/pre-commit` rejects
commits that add a private key. Enable the hook once per clone:

```sh
git config core.hooksPath .githooks
```

MIT licensed, see [LICENSE](LICENSE).
