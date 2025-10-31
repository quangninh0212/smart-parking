import cv2
import easyocr
import re
import time
import serial
import sqlite3
import threading
from datetime import datetime
from flask import Flask, render_template, Response, jsonify, request

import serial.tools.list_ports

def safe_serial(port):
    try:
        ser = serial.Serial(port, 9600, timeout=1)
        print(f"[OK] Connected to {port}")
        return ser
    except serial.SerialException as e:
        print(f"[ERROR] {port} busy or unavailable: {e}")
        return None

arduino_in = safe_serial("COM4")
arduino_out = safe_serial("COM14")


# ==== CẤU HÌNH CAMERA ====
cap_in = cv2.VideoCapture(1)
cap_out = cv2.VideoCapture(2)

reader = easyocr.Reader(['en', 'vi'])
pattern = re.compile(r"^\d{2}[A-Z]-?\d{4,5}$")
required_count = 3
timeout = 2

# ==== BIẾN TOÀN CỤC ====
last_plate_in, last_plate_out = "", ""
plate_counter_in, plate_counter_out = {}, {}
last_seen_time_in, last_seen_time_out = time.time(), time.time()

# ==== SQLITE ====
DB_FILE = "parking.db"
def init_db():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("""
        CREATE TABLE IF NOT EXISTS logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            time TEXT,
            uid TEXT,
            name TEXT,
            plate TEXT,
            direction TEXT
        )
    """)
    conn.commit()
    conn.close()

def save_log(uid, name, plate, direction):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("INSERT INTO logs (time, uid, name, plate, direction) VALUES (?, ?, ?, ?, ?)",
              (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), uid, name, plate, direction))
    conn.commit()
    conn.close()
    print(f"[DB] Saved: {uid}, {name}, {plate}, {direction}")

# ==== OCR & XỬ LÝ FRAME ====
def process_frame(frame, plate_counter, last_plate, last_seen_time, prefix, ser):
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    results = reader.readtext(gray)
    parts = []

    for (bbox, text, prob) in results:
        text = re.sub(r'[^A-Z0-9]', '', text.upper())
        if len(text) >= 2 and prob > 0.5:
            parts.append(text)
            (top_left, _, bottom_right, _) = bbox
            top_left = tuple(map(int, top_left))
            bottom_right = tuple(map(int, bottom_right))
            cv2.rectangle(frame, top_left, bottom_right, (0, 255, 0), 2)
            cv2.putText(frame, text, (top_left[0], top_left[1] - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)

    candidate = None
    if len(parts) == 1:
        candidate = parts[0]
    elif len(parts) >= 2:
        candidate = parts[0] + "" + "".join(parts[1:])

    if candidate and pattern.match(candidate):
        plate_counter[candidate] = plate_counter.get(candidate, 0) + 1
        if plate_counter[candidate] >= required_count and candidate != last_plate:
            print(f"{prefix} Biển số:", candidate)
            last_plate = candidate
            last_seen_time = time.time()
            plate_counter.clear()

            ser.write((prefix + candidate + "\n").encode())
            print("Sent to Arduino:", prefix + candidate)

    if time.time() - last_seen_time > timeout:
        last_plate = ""

    return frame, plate_counter, last_plate, last_seen_time


# ==== ĐỌC TỪ ARDUINO ====
def read_from_arduino(ser, ser_out=None):
    global current_slots
    if ser.in_waiting > 0:
        line = ser.readline().decode(errors="ignore").strip()
        if line.startswith("DATA"):
            print("[LOG]", line)
            try:
                parts = line.split(",")
                uid = parts[3] if len(parts) > 3 else ""
                name = parts[4] if len(parts) > 4 else ""
                plate = parts[5] if len(parts) > 5 else ""
                direction = parts[6] if len(parts) > 6 else ""
                save_log(uid, name, plate, direction)

                if name == "Guest" and direction == "In" and ser_out is not None:
                    reg_cmd = f"REG:{uid},{plate}\n"
                    ser_out.write(reg_cmd.encode())
                    print("[SYNC] Sent to Arduino OUT:", reg_cmd.strip())

                if name == "Guest" and direction == "Out" and arduino_in is not None:
                    del_cmd = f"DEL:{uid}\n"
                    arduino_in.write(del_cmd.encode())
                    print("[SYNC] Sent DEL to Arduino IN:", del_cmd.strip())

            except Exception as e:
                print("[ERROR] Parse log failed:", e)


# ==== HÀM MỞ / ĐÓNG BARRIER ====
def open_gate(gate='in'):
    ser = arduino_in if gate == 'in' else arduino_out
    ser.write(b'OPEN_GATE\n')
    print(f"[MANUAL] Sent OPEN_GATE to Arduino {gate.upper()}")

def close_gate(gate='in'):
    ser = arduino_in if gate == 'in' else arduino_out
    ser.write(b'CLOSE_GATE\n')
    print(f"[MANUAL] Sent CLOSE_GATE to Arduino {gate.upper()}")


# ==== LUỒNG NỀN: OCR + GIAO TIẾP ====
def camera_loop():
    global plate_counter_in, last_plate_in, last_seen_time_in
    global plate_counter_out, last_plate_out, last_seen_time_out

    while True:
        ret_in, frame_in = cap_in.read()
        ret_out, frame_out = cap_out.read()
        if not ret_in or not ret_out:
            continue

        frame_in, plate_counter_in, last_plate_in, last_seen_time_in = process_frame(
            frame_in, plate_counter_in, last_plate_in, last_seen_time_in, "IN:", arduino_in
        )
        frame_out, plate_counter_out, last_plate_out, last_seen_time_out = process_frame(
            frame_out, plate_counter_out, last_plate_out, last_seen_time_out, "OUT:", arduino_out
        )

        cv2.imwrite("static/cam_in.jpg", frame_in)
        cv2.imwrite("static/cam_out.jpg", frame_out)

        read_from_arduino(arduino_in, ser_out=arduino_out)
        read_from_arduino(arduino_out)

        time.sleep(0.1)


# ==== KHỞI TẠO FLASK ====
app = Flask(__name__)

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/logs")
def get_logs():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT time, uid, name, plate, direction FROM logs ORDER BY id DESC LIMIT 20")
    data = c.fetchall()
    conn.close()
    return jsonify(data)

@app.route("/gate/<direction>/<action>", methods=["POST"])
def control_gate(direction, action):
    if direction not in ["in", "out"] or action not in ["open", "close"]:
        return jsonify({"error": "Invalid command"}), 400
    if action == "open":
        open_gate(direction)
    else:
        close_gate(direction)
    return jsonify({"status": f"Gate {direction} {action}ed successfully"})

@app.route('/led/toggle', methods=['POST'])
def toggle_led():
    arduino_out.write(b'TOGGLE_LED\n')
    print("[WEB] Sent TOGGLE_LED to Arduino OUT")
    return jsonify({"status": "LED toggled"})


# ==== CHẠY TOÀN BỘ HỆ THỐNG ====
if __name__ == "__main__":
    init_db()
    threading.Thread(target=camera_loop, daemon=True).start()  # chạy OCR song song
    app.run(debug=False, host="0.0.0.0", port=5000)
