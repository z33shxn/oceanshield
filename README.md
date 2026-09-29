# OceanShield

A wearable rescue-assistance band for seafarers. An ESP32 reads an AD8232 ECG, a MEMS motion sensor, a water-contact probe and an EM-18 RFID crew tag. It uploads to ThingSpeak every 30 seconds, and a MATLAB dashboard turns the readings into Safe / Moderate Risk / High Risk.

Built for the Embedded Programming (CS4502) project-based learning course at Chennai Institute of Technology.

## The interesting part: months of "no data"

For months the dashboard showed nothing useful, and the obvious suspects were the WiFi and the cloud. So before touching the hardware I pulled the channel's whole history (991 records) and analysed it offline with `analysis/oceanshield_log_analyzer.py`.

What the logs showed:

- **The pipeline was fine.** Uploads arrived on a steady ~30 second cadence. The board was online and posting, but the sensors were reading 0 V.
- **Two "independent" sensors were one signal.** The motion and ECG inputs correlated at +0.9992 in some sessions, which pointed at a shorted or moved wire rather than code.
- **The dashboard could not have raised an alarm anyway.** Replaying its rules over the full history showed that two of its three rules could never fire:
  - `mean(MEMS) > 2000` on a signal the firmware capped at 1023
  - `any(water > 1)` on a digital 0/1 input
- **The risk label was inverted.** A score of 0 (nothing wrong) fell through to "High Risk".

The firmware had its own problems:

- **Wasted ADC resolution.** It divided a 12-bit ADC reading by 4, throwing away resolution and flooring small signals to exactly 0.
- **RFID shared the USB console's UART.** That kept the crew-ID field stuck at 0.
- **Broken WiFi retries.** It restarted the WiFi association every 5 seconds inside the retry loop.
- **No real heart rate.** It sent one random ECG sample every 30 seconds instead of a heart rate.

Each corrected file lists every fix in its header comment.

## What's in the repo

| Path | What it is |
|---|---|
| `firmware/oceanshield_esp32/` | Corrected ESP32 firmware (details below) |
| `firmware/original_sketch/` | The original sketch, kept for comparison |
| `dashboard/SeafarerSafetyApp_FIXED.m` | Corrected MATLAB dashboard (details below) |
| `dashboard/original/` | The original dashboard |
| `analysis/oceanshield_log_analyzer.py` | Offline analysis of the ThingSpeak feed (details below) |

The corrected firmware adds:
- R-peak heart-rate detection with an adaptive threshold
- AD8232 leads-off handling
- RFID on its own UART
- a boot-time sensor self test
- a remote diagnostics string you can read without a USB cable

The corrected dashboard has:
- the fixed risk mapping
- rules based on motion peak-to-peak, BPM and a calibrated water threshold
- one API call per refresh instead of three
- staleness and all-zero warnings

The analyser checks upload cadence, per-session sensor health and cross-sensor correlation. It also replays the old dashboard rules over the full history and runs a scenario test of the corrected risk engine.

## Running it

Firmware: open `firmware/oceanshield_esp32/oceanshield_esp32.ino` in the Arduino IDE (ESP32 board package + ThingSpeak library), then fill in your WiFi details, ThingSpeak channel ID, write key and RFID tag.

Analyser (Python 3, standard library only):

```
python analysis/oceanshield_log_analyzer.py --simulate          # scenario test, no data needed
python analysis/oceanshield_log_analyzer.py --file feeds.json   # a saved ThingSpeak export
```

To read a live channel instead, set `OCEANSHIELD_CHANNEL_ID` and `OCEANSHIELD_READ_KEY`.

The recorded data isn't included, because it contains ECG readings from real people.
