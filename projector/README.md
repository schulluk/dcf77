# Projection clock (ESP32)

Status: planning, no code yet.

The idea: take the projection unit of a Technoline projection alarm clock
(a bright LED, a small transparent segment LCD and a lens) and drive it
directly from an ESP32 that gets the time from NTP over WiFi. No DCF77
reception, so antenna, distance and interference stop mattering, and the
clock's own electronics aren't needed.

## Plan

1. **Find out how the LCD is connected.** Open the projector arm and look
   at the connection to the main board: ribbon cable, single wires or a
   conductive rubber strip. The number of lines tells the drive scheme:
   about 30 means one line per segment plus one common line (static drive),
   12 to 17 means a few common lines multiplexed with the segment lines.
2. **Map lines to segments.** LCD segments need AC, DC slowly destroys
   them. Probe with a low AC signal, e.g. two ESP32 pins toggling in
   opposite phase at about 50 Hz, between a common line and one other line,
   and note which segment darkens.
3. **Drive the LCD.** Static drive works from GPIOs (or shift registers if
   there are more segments than free pins). A multiplexed LCD needs
   intermediate voltage levels; the easy way is an LCD driver chip such as
   the HT1621 (3-wire interface, up to 32 x 4 segments).
4. **Drive the LED** through a transistor with PWM, so it can be dimmed at
   night.
5. **Time** from NTP, CET/CEST. WiFi handling, NTP, the status page, the
   event log and signed updates over WiFi can come from
   [`../transmitter`](../transmitter/).
