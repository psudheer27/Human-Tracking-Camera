import socket
import struct
import numpy as np
import cv2
from ultralytics import YOLO
from sort import Sort
import time

# ----------------------------
# Settings
# ----------------------------
HOST = "0.0.0.0"
PORT = 5000

SECOND_BOARD_IP = "192.168.10.50"
SECOND_BOARD_PORT = 6000

WIDTH = 640
HEIGHT = 480
FRAME_BYTES = WIDTH * HEIGHT * 2
TARGET_CLASS_ID = 0

FRAME_CENTER_X = WIDTH // 2

# ----------------------------
# YOLO + SORT
# ----------------------------
model = YOLO("yolov8n.pt")
tracker = Sort(max_age=10, min_hits=2, iou_threshold=0.3)
current_target_id = None

# ----------------------------
# TCP helpers
# ----------------------------
def recv_exact(sock, size):
    buf = b''
    try:
        while len(buf) < size:
            chunk = sock.recv(size - len(buf))
            if not chunk:
                return None
            buf += chunk
    except socket.timeout:
        return None
    except Exception:
        return None
    return buf

# ----------------------------
# Video socket (Zybo #1)
# ----------------------------
video_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
video_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
video_sock.bind((HOST, PORT))
video_sock.listen(1)

print(f"[INFO] Listening for video on port {PORT}")
conn, addr = video_sock.accept()
print("[INFO] Video connected from", addr)

conn.settimeout(0.2)  # short timeout to avoid blocking

# ----------------------------
# Control socket (Zybo #2)
# ----------------------------
ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    ctrl_sock.connect((SECOND_BOARD_IP, SECOND_BOARD_PORT))
    ctrl_sock.settimeout(0.2)
    print("[INFO] Connected to second board")
except Exception as e:
    print("[WARN] Control socket failed:", e)
    ctrl_sock = None

# ----------------------------
# Frame buffer
# ----------------------------
last_good_frame = None
last_frame_time = 0

# ----------------------------
# Main loop
# ----------------------------
while True:
    new_frame_received = False

    # ---- Try to receive a new frame ----
    size_data = recv_exact(conn, 4)

    if size_data:
        frame_size = struct.unpack("!I", size_data)[0]

        if frame_size == FRAME_BYTES:
            payload = recv_exact(conn, frame_size)

            if payload:
                try:
                    yuyv = np.frombuffer(payload, dtype=np.uint8).reshape((HEIGHT, WIDTH, 2))
                    bgr = cv2.cvtColor(yuyv, cv2.COLOR_YUV2BGR_YUYV)

                    last_good_frame = bgr
                    last_frame_time = time.time()
                    new_frame_received = True

                except Exception:
                    pass
        else:
            # flush bad payload
            recv_exact(conn, frame_size)

    # ---- If no new frame, reuse last ----
    if last_good_frame is None:
        continue  # nothing to show yet

    frame = last_good_frame.copy()

    # ---- YOLO detection ----
    try:
        results = model(frame, verbose=False)[0]
    except Exception:
        continue

    detections = []
    for box in results.boxes:
        if int(box.cls[0]) != TARGET_CLASS_ID:
            continue
        x1, y1, x2, y2 = box.xyxy[0]
        conf = float(box.conf[0])
        detections.append([x1, y1, x2, y2, conf])

    det_array = np.array(detections) if detections else np.empty((0, 5))
    tracks = tracker.update(det_array)

    move = "none"
    target_visible = False

    for t in tracks:
        x1, y1, x2, y2, track_id = t
        track_id = int(track_id)

        if current_target_id is None:
            current_target_id = track_id

        if track_id == current_target_id:
            cx = int((x1 + x2) / 2)

            if cx < FRAME_CENTER_X - 20:
                move = "left"
            elif cx > FRAME_CENTER_X + 20:
                move = "right"

            target_visible = True

            cv2.rectangle(frame, (int(x1), int(y1)), (int(x2), int(y2)), (0,255,0), 2)
            cv2.circle(frame, (cx, int((y1+y2)/2)), 5, (0,0,255), -1)
            cv2.putText(frame, f"ID {track_id}", (int(x1), int(y1)-10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0,255,0), 2)
            break

    if not target_visible:
        current_target_id = None

    # ---- Send control command ----
    msg = (move + "\n").encode("utf-8")

    if ctrl_sock:
        try:
            ctrl_sock.sendall(msg)
        except Exception:
            pass

    # ---- Display ----
    cv2.imshow("YOLO + SORT Tracking", frame)
    if cv2.waitKey(1) == 27:
        break

# ----------------------------
# Cleanup
# ----------------------------
conn.close()
video_sock.close()
if ctrl_sock:
    ctrl_sock.close()
cv2.destroyAllWindows()

