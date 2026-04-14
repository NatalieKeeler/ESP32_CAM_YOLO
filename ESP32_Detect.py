"""
ESP32-CAM MJPEG Stream Viewer + YOLOv8 Object Detection
--------------------------------------------------------
Requirements:
    pip install requests opencv-python numpy ultralytics

On first run, ultralytics will auto-download yolov8n.pt (~6 MB).
"""

import cv2
import numpy as np
import requests
import time
from ultralytics import YOLO

# ── Config ────────────────────────────────────────────────────────────────────
ESP32_IP   = "10.0.0.2"   # ← change to your ESP32-CAM IP
STREAM_URL = f"http://{ESP32_IP}/stream"
TIMEOUT    = 10

# YOLO model: n=nano (fastest), s=small, m=medium, l=large, x=extra-large
MODEL_NAME  = "yolov8n.pt"

# Only show detections above this confidence (0.0 – 1.0)
CONF_THRESH = 0.45

# Set to a list of class names to filter, e.g. ["person", "cat"], or None for all
FILTER_CLASSES = None

# Run detection every N frames (1 = every frame, 2 = every other, etc.)
# Increase if FPS is too low on a slow machine.
DETECT_EVERY = 1
# ─────────────────────────────────────────────────────────────────────────────

# Colour palette — one colour per class ID, generated deterministically
def class_colour(class_id: int) -> tuple:
    np.random.seed(class_id + 42)
    return tuple(int(c) for c in np.random.randint(80, 255, 3))


def read_mjpeg_stream(url: str, timeout: int = 10):
    """Yields raw decoded frames from a multipart MJPEG stream."""
    with requests.get(url, stream=True, timeout=timeout) as r:
        r.raise_for_status()
        buf = b""
        for chunk in r.iter_content(chunk_size=4096):
            if not chunk:
                continue
            buf += chunk
            while True:
                start = buf.find(b"\xff\xd8")
                end   = buf.find(b"\xff\xd9", start)
                if start == -1 or end == -1:
                    break
                jpg = buf[start : end + 2]
                buf = buf[end + 2:]
                arr   = np.frombuffer(jpg, dtype=np.uint8)
                frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                if frame is not None:
                    yield frame


def draw_detections(frame: np.ndarray, results, names: dict,
                    filter_classes=None) -> int:
    """
    Draw bounding boxes + labels on frame in-place.
    Returns the number of objects drawn.
    """
    count = 0
    boxes = results[0].boxes

    for box in boxes:
        conf     = float(box.conf[0])
        class_id = int(box.cls[0])
        label    = names[class_id]

        if filter_classes and label not in filter_classes:
            continue
        if conf < CONF_THRESH:
            continue

        x1, y1, x2, y2 = map(int, box.xyxy[0])
        colour = class_colour(class_id)

        # Bounding box
        cv2.rectangle(frame, (x1, y1), (x2, y2), colour, 2)

        # Label background
        text    = f"{label}  {conf:.0%}"
        (tw, th), baseline = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.55, 1)
        cv2.rectangle(frame, (x1, y1 - th - baseline - 6), (x1 + tw + 4, y1), colour, -1)

        # Label text
        cv2.putText(
            frame, text,
            (x1 + 2, y1 - baseline - 2),
            cv2.FONT_HERSHEY_SIMPLEX, 0.55,
            (0, 0, 0), 1, cv2.LINE_AA,
        )
        count += 1

    return count


def draw_hud(frame: np.ndarray, fps: float, obj_count: int,
             detection_on: bool, model_name: str) -> None:
    """Draws the HUD bar at the top of the frame."""
    h, w = frame.shape[:2]

    # Semi-transparent dark bar
    overlay = frame.copy()
    cv2.rectangle(overlay, (0, 0), (w, 44), (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.6, frame, 0.4, 0, frame)

    status_col = (0, 220, 80) if detection_on else (60, 60, 200)
    status_txt = "DETECT ON" if detection_on else "DETECT OFF"

    cv2.putText(frame, f"FPS: {fps:5.1f}", (8, 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 220, 80), 1, cv2.LINE_AA)

    cv2.putText(frame, f"Objects: {obj_count}", (80, 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 180, 255), 1, cv2.LINE_AA)

    cv2.putText(frame, status_txt, (w - 90, 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.35, status_col, 1, cv2.LINE_AA)

    cv2.putText(frame, model_name, (w // 2 - 10 , 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, (180, 180, 180), 1, cv2.LINE_AA)


def print_help():
    print("""
╔══════════════════════════════════════════╗
║   ESP32-CAM  Object Detection Viewer m   ║
╠══════════════════════════════════════════╣
║  D  – toggle detection on/off            ║
║  S  – save current frame as PNG          ║
║  +  – increase confidence threshold      ║
║  -  – decrease confidence threshold      ║
║  Q / ESC – quit                          ║
╚══════════════════════════════════════════╝
""")


def main():
    print_help()
    print(f"Loading model: {MODEL_NAME} ...")
    model = YOLO(MODEL_NAME)
    names = model.names
    print(f"✅ Model ready  –  {len(names)} classes")
    print(f"Connecting to {STREAM_URL} ...\n")

    detection_on  = True
    conf_thresh   = CONF_THRESH
    fps_start     = time.time()
    frame_count   = 0
    fps_display   = 0.0
    obj_count     = 0
    last_results  = None
    frame_idx     = 0
    retry         = 0
    max_retries   = 5
    save_counter  = 0

    while retry < max_retries:
        try:
            for frame in read_mjpeg_stream(STREAM_URL, timeout=TIMEOUT):
                frame_count += 1
                frame_idx   += 1
                now     = time.time()
                elapsed = now - fps_start

                if elapsed >= 1.0:
                    fps_display = frame_count / elapsed
                    fps_start   = now
                    frame_count = 0

                # ── Run detection ─────────────────────────────────────────
                if detection_on and frame_idx % DETECT_EVERY == 0:
                    last_results = model(
                        frame,
                        conf=conf_thresh,
                        verbose=False,
                        stream=False,
                    )

                # ── Draw detections on frame ──────────────────────────────
                if detection_on and last_results is not None:
                    obj_count = draw_detections(
                        frame, last_results, names, FILTER_CLASSES
                    )

                # ── HUD ───────────────────────────────────────────────────
                draw_hud(frame, fps_display, obj_count, detection_on, MODEL_NAME)

                cv2.imshow("ESP32-CAM  |  Object Detection  |  D=toggle  Q=quit", frame)

                # ── Keyboard ─────────────────────────────────────────────
                key = cv2.waitKey(1) & 0xFF

                if key in (ord("q"), 27):                  # Q / ESC – quit
                    print("Quitting.")
                    cv2.destroyAllWindows()
                    return

                elif key == ord("d"):                       # D – toggle detection
                    detection_on = not detection_on
                    last_results = None
                    obj_count    = 0
                    print(f"Detection {'ON' if detection_on else 'OFF'}")

                elif key == ord("s"):                       # S – save frame
                    fname = f"capture_{save_counter:04d}.png"
                    cv2.imwrite(fname, frame)
                    save_counter += 1
                    print(f"Saved {fname}")

                elif key == ord("+"):                       # + – raise threshold
                    conf_thresh = min(0.95, conf_thresh + 0.05)
                    print(f"Confidence threshold: {conf_thresh:.2f}")

                elif key == ord("-"):                       # - – lower threshold
                    conf_thresh = max(0.05, conf_thresh - 0.05)
                    print(f"Confidence threshold: {conf_thresh:.2f}")

            print("Stream ended. Reconnecting in 2 s...")
            retry = 0
            time.sleep(2)

        except requests.exceptions.ConnectionError as e:
            retry += 1
            print(f"Connection error ({retry}/{max_retries}): {e}")
            time.sleep(2)

        except requests.exceptions.Timeout:
            retry += 1
            print(f"Timeout ({retry}/{max_retries}). Retrying...")
            time.sleep(2)

        except KeyboardInterrupt:
            print("\nStopped by user.")
            break

    cv2.destroyAllWindows()
    print("Done.")


if __name__ == "__main__":
    main()
