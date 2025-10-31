
# 🚗 Smart Parking System — RFID + AI License Plate Recognition + Flask Dashboard

Hệ thống bãi đỗ xe thông minh kết hợp **Arduino**, **AI OCR (EasyOCR)**, **Camera kép (IN/OUT)** và **Web Dashboard (Flask)** để quản lý xe ra vào tự động bằng **thẻ RFID** và **nhận diện biển số**.

---

## 🧠 Tổng quan hệ thống

Hệ thống gồm 3 thành phần chính:

### 1️⃣ **Arduino VÀO (code_vao_AI.ino)**
- Đọc thẻ **RFID** qua module **MFRC522**.
- Kết nối **camera IN** qua Python để nhận diện biển số.
- Hiển thị LCD 16x2, cảnh báo còi & cảm biến.
- Gán biển số cho khách (guest) khi vào bãi.
- Gửi dữ liệu vào SQLite qua Python:
```

DATA,DATE,TIME,UID,Name,Plate,In

```

### 2️⃣ **Arduino RA (code_ra_AI.ino)**
- Xử lý khi xe **ra khỏi bãi**.
- Kiểm tra thẻ và biển số tương ứng (cư dân hoặc khách).
- Mở/đóng barie bằng servo.
- Reset lại biển số cho thẻ khách khi ra.
- Điều khiển thêm **đèn LED RGB** bằng **nút nhấn hoặc qua web Flask**.

### 3️⃣ **Python (smart_parking.py + app.py)**
- Nhận luồng video từ **2 camera** (IN và OUT).
- Nhận diện biển số bằng **EasyOCR** (ngôn ngữ `['en', 'vi']`).
- Giao tiếp serial với 2 Arduino qua `COM4` (IN) và `COM15` (OUT).
- Ghi log vào **SQLite database (`parking.db`)**.
- Đồng bộ thông tin khách từ Arduino IN sang Arduino OUT:
```

REG:UID,Plate

````
- Cung cấp **Web Dashboard** hiển thị:
- Hai camera IN/OUT (tự refresh mỗi 1 giây)
- Danh sách xe gần đây (cập nhật realtime)
- Nút điều khiển mở/đóng barie & đổi màu LED RGB

---

## 💡 Tính năng nổi bật

✅ **Nhận diện biển số tự động** bằng EasyOCR  
✅ **Mở barie tự động** khi thẻ và biển khớp  
✅ **Phân biệt cư dân & khách (resident/guest)**  
✅ **Ghi log ra/vào vào cơ sở dữ liệu SQLite**  
✅ **Web Dashboard điều khiển trực tiếp**:
 - Xem 2 camera (IN/OUT)
 - Theo dõi log realtime
 - Mở/đóng barie từ xa
 - Đổi màu LED RGB trang trí bãi đỗ
✅ **Cảm biến vật cản, còi cảnh báo, cảm biến cháy**
✅ **LCD 16x2 hiển thị trạng thái & chỗ trống**

---

## ⚙️ Cấu hình phần cứng

| Thành phần | Vai trò | Ghi chú |
|-------------|----------|--------|
| 2× Arduino UNO | Xử lý vào/ra riêng biệt | Arduino IN + Arduino OUT |
| 2× Camera USB | Nhận dạng biển số | Camera IN (vào), Camera OUT (ra) |
| MFRC522 RFID | Đọc thẻ RFID | Giao tiếp SPI |
| Servo MG90S | Đóng/mở barie | Điều khiển qua PWM |
| Cảm biến vật cản (IR) | Phát hiện xe tại cổng | Giữ barie mở khi có xe |
| LCD 16x2 I2C | Hiển thị trạng thái | Địa chỉ 0x27 |
| Buzzer | Cảnh báo | Pin D5 |
| LED RGB | Đèn hiệu | A1 (R), A2 (G), A3 (B) |
| Nút nhấn | Đổi màu LED | D3 (kích hoạt qua INPUT_PULLUP) |

---

## 💾 Cấu trúc thư mục

```
📂 smart_parking/
│
├── app.py                # Flask web server
├── smart_parking.py      # Main AI + OCR + Serial + DB logic
├── parking.db            # SQLite log database (auto-created)
│
├── code_vao_AI.ino       # Arduino VÀO
├── code_ra_AI.ino        # Arduino RA
│
├── templates/
│   └── index.html        # Giao diện web dashboard
└── static/
    ├── cam_in.jpg        # Frame camera IN (auto cập nhật)
    └── cam_out.jpg       # Frame camera OUT
```

---

## 🔧 Cài đặt phần mềm

### 1️⃣ Yêu cầu môi trường

* Python ≥ 3.9
* Arduino IDE ≥ 2.2.1
* Các thư viện Python:

  ```bash
  pip install flask opencv-python easyocr pyserial sqlite3
  ```

### 2️⃣ Kết nối phần cứng

* Arduino IN: `COM4`
* Arduino OUT: `COM15`
* Camera IN: `cv2.VideoCapture(2)`
* Camera OUT: `cv2.VideoCapture(1)`

> ⚠️ Nếu cổng COM hoặc chỉ số camera khác, chỉnh lại trong `smart_parking.py`.

---

## 🚀 Cách chạy dự án

### 🔹 Bước 1: Nạp code Arduino

Nạp `code_vao_AI.ino` và `code_ra_AI.ino` tương ứng cho 2 board Arduino.

### 🔹 Bước 2: Chạy Python AI

```bash
python smart_parking.py
```

Hệ thống AI OCR bắt đầu nhận diện và ghi log.

### 🔹 Bước 3: Chạy Web Dashboard

```bash
python app.py
```

Truy cập:

```
http://127.0.0.1:5000
```

Xem camera, log xe, và điều khiển barie/LED.

---

## 📊 Cấu trúc dữ liệu (SQLite)

| Trường      | Kiểu         | Mô tả                    |
| ----------- | ------------ | ------------------------ |
| `id`        | INTEGER (PK) | Tự tăng                  |
| `time`      | TEXT         | Thời gian ghi log        |
| `uid`       | TEXT         | UID thẻ RFID             |
| `name`      | TEXT         | Tên người (hoặc “Guest”) |
| `plate`     | TEXT         | Biển số                  |
| `direction` | TEXT         | “In” / “Out”             |

---

## 🖥 Giao diện Web

* Hai luồng camera hiển thị realtime
* Bảng log cập nhật 2s/lần
* Nút điều khiển:

  * 🚪 Mở/Đóng Barrier IN
  * 🚪 Mở/Đóng Barrier OUT
  * 💡 Đổi màu LED RGB

---

## 🧩 Mở rộng & Nâng cấp

* 🔐 Thêm đăng nhập admin trên web
* ☁️ Chuyển database SQLite sang MySQL/PostgreSQL
* 🧭 Thêm bản đồ vị trí slot trong bãi
* 🔊 Ghi âm/loa phát thanh khi nhận diện xe cư dân
* 📸 Tự động chụp ảnh lưu log khi mở barie
* 📱 Xây dựng mobile app (React Native / Flutter) điều khiển từ xa

---

## 👨‍💻 Tác giả

**Đặng Quang Ninh**
📍 Học viện Kỹ thuật Mật mã — Khoa Công nghệ Thông tin
📧 Email: dangninh138@gmail.com
💬 Dự án thực hiện trong khuôn khổ đồ án “Hệ thống bãi đỗ xe thông minh ứng dụng RFID và AI nhận diện biển số”.

---

## 🏁 Giấy phép

Dự án phát hành theo giấy phép **MIT License**.
Bạn có thể tự do sử dụng, chỉnh sửa, và phân phối với mục đích học tập và nghiên cứu.

```
