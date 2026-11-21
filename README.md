# Rover Surveillance Prototype

A semi-autonomous rover prototype for security surveillance. A camera on the rover streams to a laptop. The laptop runs person detection with YOLO, saves a snapshot and a short clip for each detection, and queues them for human review. A control page on the laptop shows the live feed and drives the rover through an ESP32-S3, which also reads an ultrasonic distance sensor.

> **Prototype status.** This is a demo build. The laptop server and the dashboard were tested with a stubbed detector and a headless browser. The firmware and rover motion have not been tested end to end on hardware. Parts of the TRAKSH dashboard show simulated data. Read [Limitations](#limitations) before relying on it for anything real.

---

## Contents

- [What it does](#what-it-does)
- [Architecture](#architecture)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [Repository layout](#repository-layout)
- [Setup](#setup)
- [Using the dashboard](#using-the-dashboard)
- [Configuration](#configuration)
- [API reference](#api-reference)
- [Event format](#event-format)
- [Safety](#safety)
- [Testing](#testing)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)
- [License](#license)

---

## What it does

- **Live camera feed.** The ESP32-CAM streams video to the laptop. The laptop draws YOLO bounding boxes and re-serves the feed to the browser.
- **Person detection and review events.** When YOLO detects the configured class (default `person`), the laptop saves an annotated snapshot and a 5-second clip, and creates a pending review event.
- **Review queue.** The dashboard's AI Detection & Review page lists pending events with the snapshot and a clip link. **Mark reviewed** removes an event from the queue. Reviewed events stay out of the queue across page reloads and server restarts.
- **Obstacle distance.** An HC-SR04 ultrasonic sensor on the ESP32-S3 reports distance. The laptop polls it, shows it live, and raises an alert when something is closer than the proximity limit.
- **Rover drive control.** Hold a button, or use the arrow keys, on either the control page or the dashboard's Live Monitoring page. Releasing stops the rover.
- **Rover safety.** The S3 stops the motors if commands stop arriving, and it blocks forward movement near obstacles. See [Safety](#safety).

---

## Architecture

```
                    Wi-Fi: ESP32-CAM-Local (no internet)
  ┌────────────┐   ┌──────────────────────────────────────┐   ┌──────────────┐
  │ ESP32-CAM  │──►│ Laptop: Flask + YOLO (port 5001)     │◄──│ ESP32-S3     │
  │ camera     │   │  • reads camera stream               │   │ HC-SR04      │
  │ :81/stream │   │  • polls /distance on the S3         │   │ motor driver │
  └────────────┘   │  • sends /drive commands to the S3   │──►│ watchdog     │
                   │  • saves events, snapshots, clips    │   └──────────────┘
                   └───────────────┬──────────────────────┘
                                   │ HTTP / SSE (localhost)
                             ┌─────▼──────┐
                             │  Browser   │  /control, /dashboard
                             └────────────┘
```

- The ESP32-CAM and the ESP32-S3 both join the camera's Wi-Fi network, `ESP32-CAM-Local`, and get addresses on it. The camera is `192.168.4.1`, and the S3 is set to `192.168.4.50`. The laptop joins the same network.
- The laptop is the only intermediary. The S3 never talks to the camera directly, and the camera never talks to the S3.
- `ESP32-CAM-Local` has no internet connection. Install Python packages before joining it.

---

## Hardware

| Part | Role |
|---|---|
| AI Thinker ESP32-CAM | Camera, Wi-Fi access point `ESP32-CAM-Local`, stream on port 81 |
| ESP32-S3 (DevKitC-1 or similar) | HC-SR04 reading, motor control, watchdog and obstacle guard |
| HC-SR04 ultrasonic sensor | Obstacle distance, 2 cm to 400 cm |
| Dual H-bridge motor driver (L298N or TB6612 style) | Drives the left and right motor pairs |
| Two motors and a wheeled chassis | The rover |
| Separate battery for the motors | Motor power, kept apart from the S3 and camera |
| Power supply or power bank for the S3 | 5V, USB or regulated |
| Power supply for the camera | 5V, at least 1 A. Undervoltage causes resets. |
| Laptop (Mac, Windows, or Linux) | Runs YOLO, the server, and the browser pages |

An FM receiver (TEA5767) is on the parts list. It is not wired into the firmware yet.

---

## Wiring

### ESP32-S3

| Signal | GPIO | Notes |
|---|---|---|
| HC-SR04 TRIG | 4 | Output |
| HC-SR04 ECHO | 5 | Input. Use a voltage divider: 1 kΩ from ECHO to GPIO 5, and 2 kΩ from GPIO 5 to GND. The S3 is not 5 V tolerant. |
| Left motor IN1 | 10 | Direction |
| Left motor IN2 | 11 | Direction |
| Left motor EN | 14 | PWM speed |
| Right motor IN3 | 12 | Direction |
| Right motor IN4 | 13 | Direction |
| Right motor EN | 15 | PWM speed |

These are the pins assumed by `firmware/esp32s3_rover/src/main.cpp`. Change them there if your wiring differs. If you use PWM on the EN pins, remove the EN jumpers on an L298N.

**Power and ground:**

- HC-SR04 VCC goes to the 5 V pin. Its GND goes to ground.
- The motor driver's logic supply and the S3 must share ground with the motor battery.
- The motors run from their own battery. Do not power them from the S3's 5 V pin.

### ESP32-CAM

The camera pin map is in `firmware/esp32cam/src/main.cpp`. It uses the AI Thinker board's built-in camera wiring, so no changes are needed for that board.

---

## Repository layout

```
rover_prototype/
├── README.md
├── traksh_local.html               dashboard (keep ONE folder above laptop_yolo)
├── .gitignore
├── .vscode/extensions.json         recommends the PlatformIO extension
├── laptop_yolo/                    laptop side (Python)
│   ├── yolo_camera_dashboard.py    Flask server: YOLO, events, distance polling, drive API
│   ├── control.html                control page (served at /control)
│   ├── requirements.txt            flask, opencv-python, ultralytics
│   ├── yolov8n.pt                  YOLO weights
│   └── captures/                   created at runtime: snapshots, clips, events.json
└── firmware/                       PlatformIO projects: open each folder in VS Code
    ├── esp32cam/                   ESP32-CAM: camera and hotspot
    │   ├── platformio.ini
    │   └── src/main.cpp
    └── esp32s3_rover/              ESP32-S3: HC-SR04, motors, watchdog
        ├── platformio.ini
        └── src/main.cpp
```

Keep the file names exactly as shown. The dashboard must sit one folder above `laptop_yolo`, because the server looks for it at `../traksh_local.html`.

---

## Setup

### 1. Install VS Code and PlatformIO (once)

1. Install VS Code.
2. Install the **PlatformIO IDE** extension. The `.vscode/extensions.json` file recommends it, so VS Code may offer to install it.
3. Let PlatformIO finish its first-time setup. It downloads toolchains and frameworks, which needs internet.

### 2. Flash the ESP32-CAM

1. In VS Code, choose **File → Open Folder** and select `firmware/esp32cam`. Open this folder, not the top-level repo folder, so PlatformIO finds `platformio.ini`.
2. Hold the IO0 pin to GND and press RESET to enter flashing mode.
3. Click **Upload** in the PlatformIO toolbar.
4. When it finishes, release IO0 and press RESET.
5. Open the serial monitor at 115200 baud. It should print `Wi-Fi: ESP32-CAM-Local  Password: camera123` and `Open http://192.168.4.1/`.

### 3. Flash the ESP32-S3

1. In VS Code, choose **File → Open Folder** and select `firmware/esp32s3_rover`.
2. Connect the S3 by USB and click **Upload**.
3. Open the serial monitor at 115200 baud. It should print a line like `Connected: http://192.168.4.50/distance and /drive`.
4. If the serial monitor is blank, check the USB port. The `ARDUINO_USB_CDC_ON_BOOT` flag sends serial output over native USB. Boards with a separate USB-UART chip need that flag removed from `platformio.ini`.
5. If your S3 is not a DevKitC-1, change the `board` line in `firmware/esp32s3_rover/platformio.ini`.

### 4. Install the laptop packages (needs internet)

Do this while your laptop is on a normal network.

**Mac / Linux**

```bash
cd laptop_yolo
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

**Windows (PowerShell)**

```powershell
cd laptop_yolo
py -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
```

Check the install:

```bash
python3 -c "import flask, cv2, ultralytics; print('ok')"
```

### 5. Join the camera network and run the server

1. Join the laptop to `ESP32-CAM-Local` with password `camera123`.
2. Confirm the camera answers. Open `http://192.168.4.1/` in a browser, or run:

   ```bash
   python3 -c "import cv2; c=cv2.VideoCapture('http://192.168.4.1:81/stream'); print(c.isOpened(), c.read()[0])"
   ```

   It should print `True True`.
3. Start the server from `laptop_yolo`:

   ```bash
   python3 yolo_camera_dashboard.py
   ```

   It should print `Control page: http://127.0.0.1:5001/control`.

   The first run may take a little while while YOLO loads.

### 6. Open the pages

- **Control page:** http://127.0.0.1:5001/control
- **Dashboard:** http://127.0.0.1:5001/dashboard

Both run on the laptop at port 5001. The camera's own address (192.168.4.1) does not serve them.

---

## Using the dashboard

The dashboard is the TRAKSH single-page app. The parts connected to real hardware are:

- **Live Monitoring:** the camera feed, the live obstacle distance card, and the rover drive card. Scroll below the camera feed to find the distance and drive cards.
- **AI Detection & Review:** pending person events, with the snapshot, the clip link, and the Mark reviewed and Acknowledge buttons.
- **E-stop:** the software E-stop on the dashboard stops the rover and refuses drive commands until it is released.

The control page at `/control` shows the same live feed, status, and drive controls, plus settings for detection, alerts, confidence, alert class, and the proximity threshold.

**Driving:** hold a drive button, or hold an arrow key. Releasing stops the rover. Speed is set by the slider.

---

## Configuration

Set these environment variables before running the server. All are optional.

| Variable | Default | Purpose |
|---|---|---|
| `CAMERA_STREAM_URL` | `http://192.168.4.1:81/stream` | Camera stream the laptop reads |
| `SENSOR_URL` | `http://192.168.4.50/distance` | S3 distance endpoint |
| `ROVER_URL` | `http://192.168.4.50` | S3 base URL for drive commands |
| `YOLO_MODEL` | `yolov8n.pt` | Model file |
| `YOLO_CONFIDENCE` | `0.45` | Starting confidence threshold |
| `ALERT_CLASS` | `person` | Class that creates review events |
| `PROXIMITY_CM` | `30` | Starting obstacle alert distance |
| `EVENT_COOLDOWN_S` | `5` | Minimum seconds between review events |
| `CLIP_SECONDS` | `5` | Length of each saved clip |
| `HOST` | `127.0.0.1` | Server address. `0.0.0.0` makes it reachable from the network. |

Example, on Mac or Linux:

```bash
SENSOR_URL=http://192.168.4.50/distance PROXIMITY_CM=40 python3 yolo_camera_dashboard.py
```

On Windows PowerShell:

```powershell
$env:PROXIMITY_CM = "40"; python yolo_camera_dashboard.py
```

---

## API reference

All endpoints below are served by the laptop server on port 5001, except where marked as the S3 or the camera.

### Laptop server

| Method | Path | Description |
|---|---|---|
| GET | `/` | Redirects to `/control` |
| GET | `/control` | Control page |
| GET | `/dashboard` | TRAKSH dashboard (needs `traksh_local.html` one folder up) |
| GET | `/video_feed` | Annotated MJPEG stream |
| GET | `/events` | Server-sent events. Each message is one text line, such as a person detection or an obstacle warning. |
| GET | `/captures/<file>` | Saved snapshot or clip |
| GET | `/api/state` | Current status, config, and recent log lines |
| POST | `/api/config` | Update settings. Body is JSON with any of the keys below. |
| GET | `/api/events` | Pending person events |
| POST | `/api/events/<id>/review` | Mark an event reviewed. Returns 404 for an unknown ID. |
| POST | `/api/drive` | Send a drive command to the S3. Body: `{"cmd": "F"\|"B"\|"L"\|"R"\|"S", "speed": 0-200}` |

**`POST /api/config` keys:**

| Key | Type | Allowed values |
|---|---|---|
| `detection_enabled` | boolean | Turns YOLO on or off. When off, the feed still streams. |
| `alerts_enabled` | boolean | Turns alerts and events on or off |
| `confidence` | number | 0.05 to 0.95 |
| `alert_class` | string | Any class the model knows, such as `person` |
| `proximity_cm` | number | 5 to 200 |

Invalid values return HTTP 400 with an `error` message.

**`POST /api/drive` responses:**

- `200` with `{"ok": true, "rover": {...}}` when the S3 accepted the command.
- `400` for an invalid command or speed.
- `502` with `{"ok": false, "error": "rover unreachable"}` when the S3 cannot be reached.

### ESP32-S3

| Method | Path | Response |
|---|---|---|
| GET | `/distance` | `{"distance_cm": 42.3}`, or `{"distance_cm": null}` when there is no valid echo |
| GET | `/drive?cmd=F&speed=150` | `{"cmd": "F", "left": 150, "right": 150, "blocked": ""}`. `blocked` is `"obstacle"` when forward is blocked. |

### ESP32-CAM

| Path | Description |
|---|---|
| `http://192.168.4.1/` | Simple camera page |
| `http://192.168.4.1/capture` | Single JPEG snapshot |
| `http://192.168.4.1:81/stream` | MJPEG stream the laptop reads |

---

## Event format

Each person detection creates an entry in `laptop_yolo/captures/events.json`:

```json
{
  "id": 3,
  "time": "2026-11-22 04:58:07",
  "label": "person",
  "confidence": 0.873,
  "snapshot": "/captures/person_20261122_045807.jpg",
  "clip": "/captures/person_20261122_045807.mp4",
  "status": "pending",
  "reviewed_at": null
}
```

- `status` is `pending` until someone marks it reviewed. Then it becomes `reviewed` and `reviewed_at` is filled in.
- `captures/` is ignored by git. Keep it out of any shared copy of the repository.

---

## Safety

The rover has several layers of protection, but none of them replace a physical emergency stop.

- **Watchdog.** The S3 stops the motors if it receives no drive command for 500 ms. The laptop sends a command every 200 ms while a button is held. A crashed laptop, a dropped Wi-Fi link, or a closed browser therefore stops the rover.
- **Obstacle guard.** Forward is blocked when the HC-SR04 reads below 30 cm. If an obstacle appears while the rover is moving forward, it stops.
- **Speed cap.** The S3 caps speed at 200 out of 255, whatever the laptop sends.
- **Software E-stop.** The dashboard's E-stop stops the rover and refuses drive commands until released. It runs in the browser and on the laptop, so it fails if the laptop is unreachable. The watchdog then stops the rover.
- **Physical stop (required).** Wire a switch that cuts motor power. Test it before every run.

**Before the first run:**

1. Lift the wheels off the ground.
2. Check that the drive buttons move the wheels in the right directions.
3. Check that releasing a button stops the wheels within a moment.
4. Disconnect the laptop from Wi-Fi while the wheels are turning, and confirm they stop.
5. Put an object in front of the sensor, and confirm forward is blocked below 30 cm.
6. Only then put the rover on the ground.

---

## Testing

### What has been checked

- **Laptop server routes:** tested with a stubbed YOLO model. Returns the correct status codes for the control page, the dashboard, the state API, config validation, and drive validation.
- **Event queue:** tested by creating an event, saving its snapshot and clip, listing it as pending, marking it reviewed, and confirming it leaves the pending list.
- **Dashboard live page:** loaded in headless Chromium. The distance and drive cards render, and the page has no JavaScript errors apart from the offline font request.
- **JavaScript syntax:** the inline scripts in `traksh_local.html` and `control.html` pass Node's syntax check.

### What still needs a hardware test

- Camera stream read by the laptop under real Wi-Fi conditions.
- YOLO detections on live video, and the speed of detection on your laptop.
- HC-SR04 distance readings, and the S3's reply time.
- Motor directions, the watchdog, and the obstacle guard, with the wheels off the ground.
- The physical emergency stop.

### Suggested checks

```bash
# Camera stream
python3 -c "import cv2; c=cv2.VideoCapture('http://192.168.4.1:81/stream'); print(c.isOpened())"

# S3 distance
curl http://192.168.4.50/distance

# S3 drive, wheels off the ground
curl "http://192.168.4.50/drive?cmd=F&speed=100"
curl "http://192.168.4.50/drive?cmd=S"

# Laptop state
curl http://127.0.0.1:5001/api/state
```

---

## Limitations

- **Detection is a prompt, not a verdict.** YOLO detections are for human review. The system does not confirm threats.
- **No authentication.** The camera stream, the S3 endpoints, and the laptop server have no login. The laptop server binds to `127.0.0.1` by default. Do not expose it to a shared or public network, and do not put the camera's Wi-Fi password in any public place.
- **Clips start after detection.** There is no lead-in from before the person appeared. Playback speed is approximate.
- **Clip playback in browsers.** Depending on the OpenCV build, a clip may not play in a browser. QuickTime and VLC play MP4V clips.
- **Simulated dashboard panels.** The map, GPS, sensor health table, mission history, rover mode, and battery panels show simulated data. The camera feed, the review queue, and the live distance and drive controls are connected.
- **FM receiver not integrated.** The TEA5767 is not used by the firmware yet.
- **Assumed motor pins.** The S3 firmware assumes a generic dual H-bridge on the pins listed in [Wiring](#wiring).
- **Software E-stop only.** It is not a substitute for a hardware cut-off.
- **Single rover.** The server handles one camera and one S3.
- **Low-light performance.** YOLOv8n is a small, general-purpose model. Detection is weaker in low light.
- **Event retention.** Events and clips accumulate in `captures/`. Nothing deletes them automatically.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| 404 on `/control` | Old server script running, or `control.html` is in the wrong folder | Use the current script, and keep `control.html` in `laptop_yolo` |
| 404 on `/dashboard`, "Dashboard file not found" | `traksh_local.html` is not one folder above `laptop_yolo` | Move it up, and name it exactly `traksh_local.html` |
| Dashboard stuck on loading | The browser is waiting on an external font without internet | Use the current `traksh_local.html`, which loads fonts in the background |
| Camera "offline" on the control page | Laptop not on `ESP32-CAM-Local`, or the camera has no power | Join the Wi-Fi, and check the camera's supply |
| `No route to host` when testing the camera | Laptop not on the camera's Wi-Fi | Join `ESP32-CAM-Local` |
| Camera resets repeatedly | Weak power supply | Use a 5 V supply rated at 1 A or more |
| Distance "offline" | S3 not on Wi-Fi, or wrong IP | Check the S3 serial monitor, and set `SENSOR_URL` if its IP differs |
| Distance "no reading" | Sensor sees nothing in range, or the wiring is wrong | Check the TRIG, ECHO, and divider wiring |
| Drive says "rover not reachable" | Same as above, or `ROVER_URL` is wrong | Check that the S3 is at `192.168.4.50` |
| `pip` cannot reach pypi.org | Laptop is on the camera's Wi-Fi, which has no internet | Install requirements on a normal network first |
| ESP32-CAM upload fails | IO0 not held low | Hold IO0 to GND, press RESET, then retry |
| VS Code: `cannot open source file "WebServer.h"` | The wrong folder is open, or the first build hasn't finished | Open the `firmware/...` folder, build once, then run **PlatformIO: Rebuild IntelliSense Index** |
| Motors don't stop on release | Laptop closed, or Wi-Fi dropped | The watchdog should stop them within 500 ms. Check the wiring, and use the physical stop. |
| Review events don't appear | Detection or alerts are off, or the model isn't seeing the class | Turn detection and alerts on in the control page, and check the alert class |

---

## FAQ

**Why does the laptop sit in the middle?**
The laptop has the compute for YOLO, the browser, and the review storage. The camera and the S3 only need Wi-Fi. Keeping the laptop as the single point of contact makes the system easy to reason about, and it leaves room to move YOLO onto a Raspberry Pi later.

**Why does the laptop lose internet access?**
The camera creates its own Wi-Fi network without a router, so it has no upstream connection. You can install packages before joining it, and you can run the server while connected. For a longer demo, put the camera and S3 on a router that has internet access.

**Why port 5001?**
macOS uses port 5000 for AirPlay Receiver, so the server uses 5001 by default.

**Can I drive the rover from a phone?**
Not yet. The pages bind to `127.0.0.1` by default. Set `HOST=0.0.0.0` only on a network you trust, and understand that there is no login.

**Why does the S3 block forward movement?**
A 30 cm stop distance is a conservative default for a slow prototype. Change the `OBSTACLE_CM` constant in the firmware, and retest.

**Is the rover autonomous?**
No. It is semi-autonomous at most. Detections create review prompts, and a person decides what happens next. The rover does not navigate on its own.

**Can I change the detection model?**
Yes. Set `YOLO_MODEL` to another Ultralytics model file, such as `yolov8s.pt`. Larger models are more accurate and slower on a laptop.

---

## License

No license has been chosen yet. Add one before making the repository public. MIT is a common choice for prototypes.
