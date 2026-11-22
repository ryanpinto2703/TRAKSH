"""YOLO detection, HC-SR04 proximity, person-review queue, and control page.

Setup:  pip install -r requirements.txt
Run:    python yolo_camera_dashboard.py
Open:   http://127.0.0.1:5001/control       (control page)
        http://127.0.0.1:5001/dashboard     (TRAKSH dashboard, traksh_local.html one folder up)

Env vars (all optional):
  CAMERA_STREAM_URL  default http://192.168.4.1:81/stream
  SENSOR_URL         default http://192.168.4.50/distance   (ESP32-S3 with HC-SR04)
  YOLO_MODEL         default yolov8n.pt
  YOLO_CONFIDENCE    default 0.45
  ALERT_CLASS        default person
  PROXIMITY_CM       default 30
  EVENT_COOLDOWN_S   default 5     (min seconds between person events)
  CLIP_SECONDS       default 5     (length of the short clip saved per event)
  HOST               default 127.0.0.1
"""
import json
import os
import queue
import threading
import time
import urllib.request
from collections import deque
from datetime import datetime

import cv2
from flask import Flask, Response, jsonify, redirect, request, send_file, send_from_directory
from ultralytics import YOLO

CAMERA_STREAM_URL = os.getenv("CAMERA_STREAM_URL", "http://192.168.4.1:81/stream")
SENSOR_URL = os.getenv("SENSOR_URL", "http://192.168.4.50/distance")
ROVER_URL = os.getenv("ROVER_URL", "http://192.168.4.50")
MODEL_PATH = os.getenv("YOLO_MODEL", "yolov8n.pt")
HOST = os.getenv("HOST", "127.0.0.1")
EVENT_COOLDOWN_S = float(os.getenv("EVENT_COOLDOWN_S", "5"))
CLIP_SECONDS = float(os.getenv("CLIP_SECONDS", "5"))
CLIP_FPS = 5
HERE = os.path.dirname(os.path.abspath(__file__))
CAPTURE_DIR = os.path.join(HERE, "captures")
EVENTS_FILE = os.path.join(CAPTURE_DIR, "events.json")
os.makedirs(CAPTURE_DIR, exist_ok=True)

app = Flask(__name__)
model = YOLO(MODEL_PATH)
CLASS_NAMES = sorted(set(model.names.values()))

state_lock = threading.Lock()
config = {
    "detection_enabled": True,
    "alerts_enabled": True,
    "confidence": float(os.getenv("YOLO_CONFIDENCE", "0.45")),
    "alert_class": os.getenv("ALERT_CLASS", "person").lower(),
    "proximity_cm": float(os.getenv("PROXIMITY_CM", "30")),
}
status = {
    "camera": "connecting",
    "sensor": "connecting",
    "distance_cm": None,
    "fps": 0.0,
    "detections": [],
}
alert_log = deque(maxlen=50)
alerts = queue.Queue(maxsize=20)  # SSE banner feed

events_lock = threading.Lock()
events = []
next_event_id = 1

frame_cond = threading.Condition()
latest_jpeg = None
frame_seq = 0


# ---------- persistent person-event queue ----------

def load_events():
    global next_event_id
    if os.path.isfile(EVENTS_FILE):
        try:
            with open(EVENTS_FILE, encoding="utf-8") as f:
                events.extend(json.load(f))
        except (OSError, ValueError):
            pass
    next_event_id = max([e["id"] for e in events], default=0) + 1


def save_events():
    """Call with events_lock held."""
    tmp = EVENTS_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(events, f, indent=2)
    os.replace(tmp, EVENTS_FILE)


def create_event(snapshot_frame, label, confidence, when):
    """Save a snapshot and register a pending review event. Returns (event, clip_path)."""
    global next_event_id
    stamp = when.strftime("%Y%m%d_%H%M%S")
    snap_name = f"{label}_{stamp}.jpg"
    clip_name = f"{label}_{stamp}.mp4"
    cv2.imwrite(os.path.join(CAPTURE_DIR, snap_name), snapshot_frame)
    with events_lock:
        ev = {
            "id": next_event_id,
            "time": when.strftime("%Y-%m-%d %H:%M:%S"),
            "label": label,
            "confidence": round(float(confidence), 3),
            "snapshot": f"/captures/{snap_name}",
            "clip": f"/captures/{clip_name}",
            "status": "pending",
        }
        next_event_id += 1
        events.append(ev)
        save_events()
    return ev, os.path.join(CAPTURE_DIR, clip_name)


def open_clip_writer(path, frame):
    h, w = frame.shape[:2]
    # avc1 (H.264) plays in browsers; fall back to mp4v (plays in QuickTime/VLC).
    for codec in ("avc1", "mp4v"):
        writer = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*codec), CLIP_FPS, (w, h))
        if writer.isOpened():
            return writer
    return None


# ---------- helpers ----------

def set_status(**kwargs):
    with state_lock:
        status.update(kwargs)


def publish_alert(message):
    """Never call while holding state_lock."""
    entry = f"[{datetime.now():%H:%M:%S}] {message}"
    with state_lock:
        alert_log.appendleft(entry)
    try:
        alerts.put_nowait(entry)
    except queue.Full:
        try:
            alerts.get_nowait()
            alerts.put_nowait(entry)
        except queue.Empty:
            pass


# ---------- background loops ----------

def detect_loop():
    global latest_jpeg, frame_seq
    cap = None
    last_event = 0.0
    clip_writer = None
    clip_end = 0.0
    frames_done, fps_start = 0, time.monotonic()
    while True:
        if cap is None or not cap.isOpened():
            if cap:
                cap.release()
            set_status(camera="connecting")
            cap = cv2.VideoCapture(CAMERA_STREAM_URL)
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            if not cap.isOpened():
                set_status(camera="offline")
                cap = None
                time.sleep(2)
                continue
            set_status(camera="online")

        ok, frame = cap.read()
        if not ok:
            set_status(camera="offline")
            cap.release()
            cap = None
            time.sleep(0.5)
            continue

        with state_lock:
            conf = config["confidence"]
            alert_class = config["alert_class"]
            detecting = config["detection_enabled"]
            alerts_on = config["alerts_enabled"]

        annotated, found, dets = frame, [], []
        if detecting:
            result = model.predict(frame, conf=conf, verbose=False)[0]
            annotated = result.plot()
            if result.boxes is not None:
                for box in result.boxes:
                    cid = int(box.cls[0].item())
                    label = str(result.names[cid]).lower()
                    score = float(box.conf[0].item())
                    dets.append({"label": label, "conf": round(score, 2)})
                    if label == alert_class:
                        found.append(score)
        set_status(detections=dets)

        now = time.monotonic()
        if found and alerts_on and now - last_event >= EVENT_COOLDOWN_S:
            last_event = now
            ev, clip_path = create_event(annotated, alert_class, max(found), datetime.now())
            publish_alert(f"{alert_class} detected x{len(found)}, confidence {max(found):.0%} (event {ev['id']})")
            if clip_writer is not None:
                clip_writer.release()
            clip_writer = open_clip_writer(clip_path, annotated)
            clip_end = now + CLIP_SECONDS

        if clip_writer is not None:
            if now < clip_end:
                clip_writer.write(annotated)
            else:
                clip_writer.release()
                clip_writer = None

        ok, jpg = cv2.imencode(".jpg", annotated, [cv2.IMWRITE_JPEG_QUALITY, 85])
        if ok:
            with frame_cond:
                latest_jpeg = jpg.tobytes()
                frame_seq += 1
                frame_cond.notify_all()

        frames_done += 1
        if now - fps_start >= 1.0:
            set_status(fps=round(frames_done / (now - fps_start), 1))
            frames_done, fps_start = 0, now


def sensor_loop():
    last_prox_alert = 0.0
    while True:
        try:
            with urllib.request.urlopen(SENSOR_URL, timeout=1.0) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            dist = data.get("distance_cm")
            set_status(sensor="ok" if dist is not None else "no echo", distance_cm=dist)
        except Exception:
            set_status(sensor="offline", distance_cm=None)
            time.sleep(1.0)
            continue

        with state_lock:
            limit = config["proximity_cm"]
            alerts_on = config["alerts_enabled"]
        now = time.monotonic()
        if dist is not None and dist < limit and alerts_on and now - last_prox_alert >= 3.0:
            last_prox_alert = now
            publish_alert(f"Obstacle at {dist:.0f} cm (limit {limit:.0f} cm)")
        time.sleep(0.2)


def video_frames():
    last = -1
    while True:
        with frame_cond:
            frame_cond.wait_for(lambda: frame_seq != last, timeout=1.0)
            if frame_seq == last:
                continue
            last = frame_seq
            data = latest_jpeg
        if data:
            yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + data + b"\r\n"


def event_stream():
    while True:
        try:
            yield f"data: {alerts.get(timeout=1)}\n\n"
        except queue.Empty:
            yield ": keep-alive\n\n"


# ---------- routes ----------

@app.route("/")
def root():
    return redirect("/control")


@app.route("/control")
def control_page():
    return send_file(os.path.join(HERE, "control.html"), mimetype="text/html")


@app.route("/dashboard")
def traksh_dashboard():
    path = os.path.abspath(os.path.join(HERE, "..", "traksh_local.html"))
    if not os.path.isfile(path):
        return "Dashboard file not found: expected traksh_local.html one folder above laptop_yolo", 404
    return send_file(path, mimetype="text/html")


@app.route("/video_feed")
def video_feed():
    return Response(video_frames(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/events")
def sse_events():
    return Response(event_stream(), mimetype="text/event-stream", headers={"Cache-Control": "no-cache"})


@app.route("/captures/<path:name>")
def captures(name):
    return send_from_directory(CAPTURE_DIR, name)


@app.route("/api/state")
def api_state():
    with state_lock:
        payload = {"status": dict(status), "config": dict(config), "log": list(alert_log)}
    payload["classes"] = CLASS_NAMES
    return jsonify(payload)


@app.route("/api/config", methods=["POST"])
def api_config():
    data = request.get_json(silent=True) or {}
    updates = {}
    try:
        if "detection_enabled" in data:
            updates["detection_enabled"] = bool(data["detection_enabled"])
        if "alerts_enabled" in data:
            updates["alerts_enabled"] = bool(data["alerts_enabled"])
        if "confidence" in data:
            c = float(data["confidence"])
            if not 0.05 <= c <= 0.95:
                raise ValueError("confidence must be between 0.05 and 0.95")
            updates["confidence"] = c
        if "proximity_cm" in data:
            p = float(data["proximity_cm"])
            if not 5 <= p <= 200:
                raise ValueError("proximity_cm must be between 5 and 200")
            updates["proximity_cm"] = p
        if "alert_class" in data:
            name = str(data["alert_class"]).lower()
            if name not in CLASS_NAMES:
                raise ValueError(f"unknown class: {name}")
            updates["alert_class"] = name
    except (TypeError, ValueError) as exc:
        return jsonify({"error": str(exc)}), 400
    with state_lock:
        config.update(updates)
        snapshot = dict(config)
    return jsonify({"config": snapshot})


@app.route("/api/events")
def api_events():
    with events_lock:
        pending = [dict(e) for e in events if e["status"] == "pending"]
    return jsonify({"events": pending})


@app.route("/api/events/<int:eid>/review", methods=["POST"])
def review_event(eid):
    with events_lock:
        for e in events:
            if e["id"] == eid:
                if e["status"] != "reviewed":
                    e["status"] = "reviewed"
                    e["reviewed_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    save_events()
                return jsonify(e)
    return jsonify({"error": "event not found"}), 404


@app.route("/api/drive", methods=["POST"])
def api_drive():
    data = request.get_json(silent=True) or {}
    cmd = str(data.get("cmd", "S")).upper()
    if cmd not in ("F", "B", "L", "R", "S"):
        return jsonify({"error": "cmd must be F, B, L, R or S"}), 400
    try:
        speed = max(0, min(200, int(data.get("speed", 150))))
    except (TypeError, ValueError):
        return jsonify({"error": "speed must be a number"}), 400
    try:
        with urllib.request.urlopen(f"{ROVER_URL}/drive?cmd={cmd}&speed={speed}", timeout=0.5) as resp:
            return jsonify({"ok": True, "rover": json.loads(resp.read().decode("utf-8"))})
    except Exception:
        return jsonify({"ok": False, "error": "rover unreachable"}), 502


if __name__ == "__main__":
    load_events()
    threading.Thread(target=detect_loop, daemon=True).start()
    threading.Thread(target=sensor_loop, daemon=True).start()
    print("Control page: http://127.0.0.1:5001/control")
    print(f"Camera: {CAMERA_STREAM_URL}")
    print(f"Sensor: {SENSOR_URL}")
    print(f"Model: {MODEL_PATH}")
    print(f"Captures: {CAPTURE_DIR}")
    app.run(host=HOST, port=5001, threaded=True, debug=False)
