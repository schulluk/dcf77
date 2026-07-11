# DCF77 Mini-Transmitter (ESP32)

Re-broadcasts the correct time as a low-power DCF77 signal (77.5 kHz) for a
radio-controlled clock with poor reception. Time comes from NTP over WiFi
(PTB servers — the same institute that runs the real DCF77 transmitter).

## Hardware

Parts:
- Freenove ESP32-WROOM board
- Ferrite antenna salvaged from a DCF77 receiver module (SP6007)
- 1× resistor ~470 Ω … 1 kΩ (from the resistor kit)

### Antenna surgery

The ferrite rod antenna **together with its parallel tuning capacitor** forms
a resonant tank at 77.5 kHz. On the SP6007 module the capacitor sits on the
PCB near the antenna terminals. Either:
- cut the PCB so the antenna terminals + capacitor stay together, or
- desolder the capacitor and solder it directly across the coil wires.

Do **not** use the coil without the capacitor — off-resonance the field is
far weaker.

### Wiring

```
ESP32 GPIO 4 ──[ 470 Ω … 1 kΩ ]──┬── ferrite coil ──┐
                                 │                  │
                                 └── tuning cap ────┴── GND
```

(Coil and capacitor in parallel, driven through the series resistor.)

Onboard LED (GPIO 2) blinks with each modulation dip — a heartbeat that the
signal is being generated.

## Build & flash

```sh
cp include/config.example.h include/config.h   # then edit WiFi credentials
pio run -t upload
pio device monitor
```

## Usage

Place the ferrite antenna within a few centimeters of the clock's own
antenna, roughly parallel to it. Force the clock into manual sync mode (or
wait for its next scheduled sync, usually at night). A full DCF77 sync takes
2–5 minutes of clean minutes.

The transmit power through the series resistor is tiny — range is a few cm,
which is intentional (keeps it legal and avoids disturbing anything else).

## Signal details

- 77.5 kHz carrier from ESP32 LEDC PWM at 50 % duty.
- Amplitude dips at each second start: 100 ms = bit 0, 200 ms = bit 1,
  reduced to ~15 % amplitude (~5 % PWM duty), like the real transmitter.
- Second 59 has no dip (minute marker).
- Frame encodes the *following* minute: BCD time/date, CET/CEST flags,
  three even-parity bits — per the standard DCF77 layout.
