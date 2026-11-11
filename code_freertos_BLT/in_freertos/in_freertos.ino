#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ====== PIN MAP (ESP32 IN) ======
#define PIN_S1          32
#define PIN_S2          33
#define PIN_S3          34   // input-only
#define PIN_S4          35   // input-only

#define PIN_VATCAN      39   // VN, input-only (IR barrier)
#define PIN_BTN_MANUAL  16   // RX2
#define PIN_RELAY       17   // TX2

#define PIN_BUZZER      14
#define PIN_SERVO       13

#define PIN_FIRE        36   // VP, input-only
#define PIN_LIGHT       26

// RFID RC522 (VSPI)
#define SS_PIN          5
#define RST_PIN         27
#define SPI_SCK         18
#define SPI_MISO        19
#define SPI_MOSI        23

// I2C LCD
#define I2C_SDA         21
#define I2C_SCL         22

LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo cua_vao;

// ===== RFID =====
int readsuccess;
byte readcard[4];
char str[32] = "";
String StrUID;

// ===== Scan mode =====
volatile bool scanMode = false;

// ===== Anti-spam UID print =====
String lastUIDPrinted = "";
unsigned long lastUIDTime = 0;

// ===== Sensor states =====
volatile int anhsang = HIGH, lua = HIGH;
volatile int s1 = HIGH, s2 = HIGH, s3 = HIGH, s4 = HIGH;

// ===== OCR plate from Python =====
String bienSoOCR = "";

// ===== Gate states =====
TaskHandle_t gateTaskHandle = NULL;
volatile bool gateCycleRequest = false;   // mở theo chu trình bình thường
volatile bool gateHoldOpen = false;       // OPEN_GATE từ Python
volatile bool gateHoldOpenBeeped = false;
volatile bool gateForceClose = false;     // CLOSE_GATE từ Python
volatile bool gateBusy = false;           // đang chạy chu trình mở/đóng
volatile unsigned long metric_t0 = 0;     // UID -> OPEN_GATE

bool fireWasActive = false;

volatile bool buzzerOwned = false;
// ===== LCD state =====
enum LCDMode {
  LCD_MODE_WELCOME = 0,
  LCD_MODE_TRUE_MAP,
  LCD_MODE_MESSAGE
};

volatile LCDMode currentLCDMode = LCD_MODE_WELCOME;
volatile unsigned long lcdMessageUntil = 0;

// ===== Card list =====
struct TheXe {
  String uid;
  String hoten;
  String bienso;
  bool laResident;
  bool daGanBienSo;
  bool status;
};

const int MAX_THE = 15;
TheXe dsThe[MAX_THE];
int soThe = 0;

// =====================================================
// ================= LCD HELPERS ========================
// =====================================================

String lcdLine0 = "";
String lcdLine1 = "";
int lastShownSlots = -1;
int lastMapS1 = -1, lastMapS2 = -1, lastMapS3 = -1, lastMapS4 = -1;

void lcdPrintFixed16(const String &line0, const String &line1) {
  String a = line0;
  String b = line1;
  while (a.length() < 16) a += " ";
  while (b.length() < 16) b += " ";
  lcd.setCursor(0, 0);
  lcd.print(a.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(b.substring(0, 16));
}

void showMessageLCD(const String &line0, const String &line1, unsigned long holdMs) {
  currentLCDMode = LCD_MODE_MESSAGE;
  lcdMessageUntil = millis() + holdMs;
  lcd.clear();
  lcdPrintFixed16(line0, line1);
}

void initWelcomeLCD() {
  currentLCDMode = LCD_MODE_WELCOME;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("     Welcome    ");
  lcd.setCursor(0, 1);
  lcd.print("Slots:   /4     ");
  lastShownSlots = -1;
}

void updateWelcomeSlotsOnly() {
  if (currentLCDMode != LCD_MODE_WELCOME) return;

  int slots = 0;
  if (s1 != LOW) slots++;
  if (s2 != LOW) slots++;
  if (s3 != LOW) slots++;
  if (s4 != LOW) slots++;

  if (slots != lastShownSlots) {
    lastShownSlots = slots;
    lcd.setCursor(7, 1);     // ngay sau "Slots: "
    lcd.print(slots);
  }
}

void initTrueMapLCD() {
  currentLCDMode = LCD_MODE_TRUE_MAP;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("1    2    3    4");
  lastMapS1 = lastMapS2 = lastMapS3 = lastMapS4 = -1;
}

void updateTrueMapOnly() {
  if (currentLCDMode != LCD_MODE_TRUE_MAP) return;

  if (s1 != lastMapS1) {
    lcd.setCursor(0, 1);
    lcd.print((s1 == LOW) ? "X" : "V");
    lastMapS1 = s1;
  }
  if (s2 != lastMapS2) {
    lcd.setCursor(5, 1);
    lcd.print((s2 == LOW) ? "X" : "V");
    lastMapS2 = s2;
  }
  if (s3 != lastMapS3) {
    lcd.setCursor(10, 1);
    lcd.print((s3 == LOW) ? "X" : "V");
    lastMapS3 = s3;
  }
  if (s4 != lastMapS4) {
    lcd.setCursor(15, 1);
    lcd.print((s4 == LOW) ? "X" : "V");
    lastMapS4 = s4;
  }
}

// =====================================================
// ================= COMMON HELPERS =====================
// =====================================================

bool baiDay() {
  return (s1 == LOW && s2 == LOW && s3 == LOW && s4 == LOW);
}

void beepShort(int soLan, int delayTime = 100) {
  buzzerOwned = true;
  for (int i = 0; i < soLan; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    vTaskDelay(pdMS_TO_TICKS(delayTime));
    digitalWrite(PIN_BUZZER, LOW);
    vTaskDelay(pdMS_TO_TICKS(delayTime));
  }
  buzzerOwned = false;
}

void LCD_SaiBienSo() {
  showMessageLCD("Sai bien so!    ", "Check again     ", 1500);
}

void LCD_SaiThe() {
  showMessageLCD("The khong hop   ", "Check again     ", 2200);
}

void LCD_GuestChuaGan() {
  showMessageLCD("Guest chua co   ", "Bien so         ", 1500);
}

void LCD_TheBiKhoa() {
  showMessageLCD("The bi khoa!    ", "Lien he quan ly ", 1800);
}

void LCD_BAO_CHAY() {
  showMessageLCD("****Warning*****", "Canh Bao Co Chay", 300);
}

void hienParkingFull() {
  showMessageLCD("    Welcome     ", " Parking Full   ", 2000);
}

int findIndexByUID(const String &uid) {
  for (int i = 0; i < soThe; i++) {
    if (dsThe[i].uid == uid) return i;
  }
  return -1;
}

void removeAt(int idx) {
  if (idx < 0 || idx >= soThe) return;
  for (int i = idx; i < soThe - 1; i++) {
    dsThe[i] = dsThe[i + 1];
  }
  soThe--;
}

bool parseActive(String s) {
  s.trim();
  s.toLowerCase();
  return (s == "active" || s == "1" || s == "true" || s == "on");
}

bool parseResident(String s) {
  s.trim();
  s.toLowerCase();
  return (s == "resident" || s == "res" || s == "1" || s == "true");
}

String getToken(const String &s, int index, char delim = '|') {
  int start = 0;
  for (int i = 0; i < index; i++) {
    int p = s.indexOf(delim, start);
    if (p == -1) return "";
    start = p + 1;
  }
  int end = s.indexOf(delim, start);
  if (end == -1) end = s.length();
  return s.substring(start, end);
}

void upsertCard(const String &uid, const String &hoten, const String &plate, bool laResident, bool active) {
  int idx = findIndexByUID(uid);
  if (idx == -1) {
    if (soThe >= MAX_THE) {
      Serial.println("ERR|FULL");
      return;
    }
    idx = soThe;
    soThe++;
  }

  dsThe[idx].uid = uid;
  dsThe[idx].hoten = hoten;
  dsThe[idx].bienso = plate;
  dsThe[idx].laResident = laResident;
  dsThe[idx].daGanBienSo = (plate.length() > 0);
  dsThe[idx].status = active;
}

// =====================================================
// ================= RFID ==============================
// =====================================================

void array_to_string(byte array[], unsigned int len, char buffer[]) {
  for (unsigned int i = 0; i < len; i++) {
    byte nib1 = (array[i] >> 4) & 0x0F;
    byte nib2 = (array[i] >> 0) & 0x0F;
    buffer[i * 2 + 0] = nib1 < 0xA ? '0' + nib1 : 'A' + nib1 - 0xA;
    buffer[i * 2 + 1] = nib2 < 0xA ? '0' + nib2 : 'A' + nib2 - 0xA;
  }
  buffer[len * 2] = '\0';
}

int getid() {
  if (!mfrc522.PICC_IsNewCardPresent()) return 0;
  if (!mfrc522.PICC_ReadCardSerial())   return 0;

  for (int i = 0; i < 4; i++) {
    readcard[i] = mfrc522.uid.uidByte[i];
    array_to_string(readcard, 4, str);
    StrUID = str;
  }
  mfrc522.PICC_HaltA();
  return 1;
}

// =====================================================
// ================= ISR ===============================
// =====================================================

void IRAM_ATTR vatCanISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (gateTaskHandle != NULL) {
    vTaskNotifyGiveFromISR(gateTaskHandle, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
      portYIELD_FROM_ISR();
    }
  }
}

// =====================================================
// ================= SENSOR ============================
// =====================================================

void sensor() {
  s1 = digitalRead(PIN_S1);
  s2 = digitalRead(PIN_S2);
  s3 = digitalRead(PIN_S3);
  s4 = digitalRead(PIN_S4);

  anhsang = digitalRead(PIN_LIGHT);
  lua     = digitalRead(PIN_FIRE);

  // Relay theo ánh sáng: luôn cập nhật nhanh
  if (anhsang == HIGH) digitalWrite(PIN_RELAY, HIGH);
  else                 digitalWrite(PIN_RELAY, LOW);

  // Fire mode
  if (lua == LOW) {
    fireWasActive = true;
    LCD_BAO_CHAY();
    cua_vao.write(90);
    buzzerOwned = true;
    digitalWrite(PIN_BUZZER, HIGH);
    return;
  } else {
    if (fireWasActive) {
      fireWasActive = false;
      buzzerOwned = false;
      digitalWrite(PIN_BUZZER, LOW);
    } else {
      if (!buzzerOwned) {
        digitalWrite(PIN_BUZZER, LOW);
      }
    }
  }

  // Nếu đang ở message mode mà hết thời gian giữ, quay về welcome/map
  if (currentLCDMode == LCD_MODE_MESSAGE && millis() > lcdMessageUntil) {
    if (gateBusy) {
      initTrueMapLCD();
    } else {
      initWelcomeLCD();
    }
  }

  // Update LCD nhẹ, không clear liên tục
  if (currentLCDMode == LCD_MODE_WELCOME && !gateBusy && !scanMode && !gateHoldOpen) {
    updateWelcomeSlotsOnly();
  } else if (currentLCDMode == LCD_MODE_TRUE_MAP && gateBusy) {
    updateTrueMapOnly();
  }
}

// =====================================================
// ================= GATE TASK =========================
// =====================================================

void taskGate(void *pvParameters) {
  unsigned long start = 0;

  for (;;) {
    if (gateForceClose) {
      gateForceClose = false;
      gateHoldOpen = false;
      gateCycleRequest = false;
      gateBusy = false;
      gateBusy = true;   // giữ bận trong lúc còi kêu

      cua_vao.write(11);
      buzzerOwned = true;
      digitalWrite(PIN_BUZZER, HIGH);
      vTaskDelay(pdMS_TO_TICKS(60));
      digitalWrite(PIN_BUZZER, LOW);
      buzzerOwned = false;
      
      gateBusy = false;
      initWelcomeLCD();
      continue;
    }

    if (gateHoldOpen) {
      gateBusy = true;
      cua_vao.write(100);

      if (!gateHoldOpenBeeped) {
        buzzerOwned = true;
        digitalWrite(PIN_BUZZER, HIGH);
        vTaskDelay(pdMS_TO_TICKS(60));
        digitalWrite(PIN_BUZZER, LOW);
        buzzerOwned = false;
        gateHoldOpenBeeped = true;
      }

      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!gateCycleRequest) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    gateBusy = true;
    gateCycleRequest = false;

    if (metric_t0 > 0) {
      unsigned long dt = millis() - metric_t0;
      Serial.print("METRIC,UID2OPEN,ms=");
      Serial.println(dt);
      metric_t0 = 0;
    }

    cua_vao.write(90);
    initTrueMapLCD();
    updateTrueMapOnly();

    // Beep ngắn hơn để đỡ block
    buzzerOwned = true;
    for (int i = 0; i < 2; i++) {
      digitalWrite(PIN_BUZZER, HIGH);
      vTaskDelay(pdMS_TO_TICKS(50));
      digitalWrite(PIN_BUZZER, LOW);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    buzzerOwned = false;

    start = millis();

    while (true) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

      if (digitalRead(PIN_VATCAN) == LOW) {
        start = millis();
      }

      if (lua == LOW) {
        cua_vao.write(90);
        digitalWrite(PIN_BUZZER, HIGH);
        vTaskDelay(pdMS_TO_TICKS(30));
        continue;
      }

      updateTrueMapOnly();

      if (millis() - start > 3000) break;
    }

    cua_vao.write(11);
    gateBusy = false;
    initWelcomeLCD();
    updateWelcomeSlotsOnly();
  }
}

// =====================================================
// ================= RFID LOGIC ========================
// =====================================================

void senddata() {
  readsuccess = getid();
  if (!readsuccess) return;

  // ===== SCAN MODE =====
  if (scanMode) {
    if (StrUID != lastUIDPrinted || (millis() - lastUIDTime) > 1500) {
      Serial.println("UID:" + StrUID);
      lastUIDPrinted = StrUID;
      lastUIDTime = millis();
    }

    showMessageLCD("UID SENT        ", StrUID.substring(0, 16), 1200);
    scanMode = false;
    return;
  }

  bool found = false;

  for (int i = 0; i < soThe; i++) {
    if (dsThe[i].uid == StrUID) {
      found = true;

      if (!dsThe[i].status) {
        Serial.println("The bi khoa!");
        LCD_TheBiKhoa();
        beepShort(2, 80);
        return;
      }

      if (dsThe[i].laResident) {
        bienSoOCR.trim();
        dsThe[i].bienso.trim();

        if (bienSoOCR.equals(dsThe[i].bienso)) {
          if (baiDay()) {
            hienParkingFull();
            beepShort(2, 100);
            return;
          }

          Serial.println("Resident OK -> Mo cua");
          Serial.print("DATA,DATE,TIME,");
          Serial.print(StrUID); Serial.print(",");
          Serial.print(dsThe[i].hoten); Serial.print(",");
          Serial.print(bienSoOCR); Serial.println(",In");

          metric_t0 = millis();
          gateCycleRequest = true;
          bienSoOCR = "";
        } else {
          Serial.println("Sai bien so resident!");
          LCD_SaiBienSo();
          beepShort(2, 70);
        }
      } else {
        // Guest
        if (bienSoOCR.length() == 0) {
          Serial.println("Guest chua co bien so OCR, khong mo!");
          LCD_GuestChuaGan();
          beepShort(2, 70);
          return;
        }

        if (!dsThe[i].daGanBienSo) {
          dsThe[i].bienso = bienSoOCR;
          dsThe[i].daGanBienSo = true;

          Serial.println("Guest vao, gan bien so: " + bienSoOCR);
          Serial.println("REG:" + StrUID + "," + bienSoOCR);

          if (baiDay()) {
            hienParkingFull();
            beepShort(2, 100);
            return;
          }

          Serial.print("DATA,DATE,TIME,");
          Serial.print(StrUID);
          Serial.print(",Guest,");
          Serial.print(bienSoOCR);
          Serial.println(",In");

          metric_t0 = millis();
          gateCycleRequest = true;
          bienSoOCR = "";
        } else {
          bienSoOCR.trim();
          dsThe[i].bienso.trim();

          if (bienSoOCR.equals(dsThe[i].bienso)) {
            if (baiDay()) {
              hienParkingFull();
              beepShort(2, 100);
              return;
            }

            Serial.print("DATA,DATE,TIME,");
            Serial.print(StrUID);
            Serial.print(",Guest,");
            Serial.print(bienSoOCR);
            Serial.println(",In");

            metric_t0 = millis();
            gateCycleRequest = true;
            bienSoOCR = "";
          } else {
            Serial.println("Sai bien so guest!");
            LCD_SaiBienSo();
            beepShort(2, 70);
          }
        }
      }
      return;
    }
  }

  if (!found) {
    Serial.println("The khong hop le!");
    LCD_SaiThe();
    beepShort(3, 90);
  }
}

// =====================================================
// ================= SERIAL PARSE =======================
// =====================================================

void docBienSoTuPython() {
  static String buffer = "";

  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      buffer.trim();

      Serial.print("Raw from Python: '");
      Serial.print(buffer);
      Serial.println("'");

      if (buffer.startsWith("IN:")) {
        bienSoOCR = buffer.substring(3);
        bienSoOCR.trim();
        Serial.println("Camera IN -> bienSoOCR = '" + bienSoOCR + "'");
      }
      else if (buffer.startsWith("DEL:")) {
        String uid = buffer.substring(4);
        uid.trim();
        Serial.println("Nhan yeu cau xoa UID: " + uid);
        for (int i = 0; i < soThe; i++) {
          if (dsThe[i].uid == uid) {
            dsThe[i].bienso = "";
            dsThe[i].daGanBienSo = false;
            Serial.println("Da reset the khach " + uid);
          }
        }
      }
      else if (buffer.equals("SCAN_ON")) {
        scanMode = true;
        showMessageLCD("Scan mode ON    ", "Tap RFID card   ", 1500);
        Serial.println("OK|SCAN_ON");
      }
      else if (buffer.equals("SCAN_OFF")) {
        scanMode = false;
        showMessageLCD("SCAN MODE OFF   ", "                ", 800);
        Serial.println("OK|SCAN_OFF");
      }
      else if (buffer.equals("RESET_CARDS")) {
        soThe = 0;
        Serial.println("OK|RESET_CARDS");
      }
      else if (buffer.startsWith("ADD|")) {
        String uid   = getToken(buffer, 1);
        String name  = getToken(buffer, 2);
        String plate = getToken(buffer, 3);
        String type  = getToken(buffer, 4);
        String st    = getToken(buffer, 5);

        uid.trim(); name.trim(); plate.trim(); type.trim(); st.trim();
        bool isResident = parseResident(type);
        bool active = parseActive(st);
        upsertCard(uid, name, plate, isResident, active);

        Serial.println("OK|ADD|" + uid);
      }
      else if (buffer.startsWith("UPDATE|")) {
        String uid   = getToken(buffer, 1);
        String name  = getToken(buffer, 2);
        String plate = getToken(buffer, 3);
        String type  = getToken(buffer, 4);

        uid.trim(); name.trim(); plate.trim(); type.trim();
        bool isResident = parseResident(type);

        int idx = findIndexByUID(uid);
        bool active = true;
        if (idx != -1) active = dsThe[idx].status;

        upsertCard(uid, name, plate, isResident, active);
        Serial.println("OK|UPDATE|" + uid);
      }
      else if (buffer.startsWith("DELETE|")) {
        String uid = getToken(buffer, 1);
        uid.trim();
        int idx = findIndexByUID(uid);
        if (idx != -1) {
          removeAt(idx);
          Serial.println("OK|DELETE|" + uid);
        } else {
          Serial.println("ERR|NOT_FOUND");
        }
      }
      else if (buffer.startsWith("DISABLE|")) {
        String uid = getToken(buffer, 1);
        uid.trim();
        int idx = findIndexByUID(uid);
        if (idx != -1) {
          dsThe[idx].status = false;
          Serial.println("OK|DISABLE|" + uid);
        } else {
          Serial.println("ERR|NOT_FOUND");
        }
      }
      else if (buffer.startsWith("ENABLE|")) {
        String uid = getToken(buffer, 1);
        uid.trim();
        int idx = findIndexByUID(uid);
        if (idx != -1) {
          dsThe[idx].status = true;
          Serial.println("OK|ENABLE|" + uid);
        } else {
          Serial.println("ERR|NOT_FOUND");
        }
      }
      else if (buffer.equals("OPEN_GATE")) {
        Serial.println("Nhan lenh mo barie thu cong tu Python!");
        gateHoldOpen = true;
        gateHoldOpenBeeped = false;
        gateForceClose = false;
      }
      else if (buffer.equals("CLOSE_GATE")) {
        Serial.println("Nhan lenh dong barie thu cong tu Python!");
        gateHoldOpen = false;
        gateHoldOpenBeeped = false;
        gateForceClose = true;
      }

      buffer = "";
    } else {
      buffer += c;
    }
  }
}

// =====================================================
// ================= TASKS =============================
// =====================================================

void taskSerial(void *pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(5);

  for (;;) {
    docBienSoTuPython();
    vTaskDelayUntil(&lastWake, period);
  }
}

void taskSensor(void *pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(10);   // nhanh hơn để nút bấm/cảm biến nhạy

  static int lastBtnState = LOW;
  static unsigned long lastBtnDebounce = 0;

  for (;;) {
    sensor();

    int btnState = digitalRead(PIN_BTN_MANUAL);
    unsigned long now = millis();

    if (!gateHoldOpen && !gateBusy) {
      if (btnState == HIGH && lastBtnState == LOW && (now - lastBtnDebounce > 80)) {
        gateCycleRequest = true;
        metric_t0 = 0;
        lastBtnDebounce = now;
      }
    }

    lastBtnState = btnState;

    vTaskDelayUntil(&lastWake, period);
  }
}

void taskRFID(void *pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(20);

  for (;;) {
    if (!gateBusy && !gateHoldOpen) {
      senddata();
    }
    vTaskDelayUntil(&lastWake, period);
  }
}

void setup() {
  Serial.begin(9600);

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();

  cua_vao.setPeriodHertz(50);
  cua_vao.attach(PIN_SERVO, 500, 2400);
  cua_vao.write(11);

  pinMode(PIN_VATCAN, INPUT);
  pinMode(PIN_BTN_MANUAL, INPUT);

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  pinMode(PIN_FIRE, INPUT);
  pinMode(PIN_LIGHT, INPUT);

  pinMode(PIN_S1, INPUT);
  pinMode(PIN_S2, INPUT);
  pinMode(PIN_S3, INPUT);
  pinMode(PIN_S4, INPUT);

  digitalWrite(PIN_BUZZER, LOW);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);
  mfrc522.PCD_Init();

  attachInterrupt(digitalPinToInterrupt(PIN_VATCAN), vatCanISR, CHANGE);

  Serial.println("CLEARDATA");
  Serial.println("LABEL,Date,Time,RFID UID,USER,Plate,IN/OUT");
  delay(300);
  Serial.println("Scan PICC to see UID...");

  initWelcomeLCD();
  updateWelcomeSlotsOnly();

  xTaskCreatePinnedToCore(taskSerial, "taskSerial", 5000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskSensor, "taskSensor", 4000, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskRFID,   "taskRFID",   5000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskGate,   "taskGate",   4000, NULL, 4, &gateTaskHandle, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}