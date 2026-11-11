#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "BluetoothSerial.h"

// ================== PIN MAP (ESP32 OUT) ==================
#define PIN_VATCAN      34
#define PIN_BTN_MANUAL  16
#define PIN_BUZZER      14
#define PIN_FIRE        36
#define PIN_SERVO       13

#define R_PIN           25
#define G_PIN           33
#define B_PIN           26

#define BUTTON_PIN      17

#define SS_PIN          5
#define RST_PIN         27
#define SPI_SCK         18
#define SPI_MISO        19
#define SPI_MOSI        23

#define I2C_SDA         21
#define I2C_SCL         22

// ================== LCD / RFID / SERVO ==================
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo cua_ra;

// ==== Biến RFID ====
int readsuccess;
byte readcard[4];
char str[32] = "";
String StrUID;

// ===== Scan mode =====
volatile bool scanMode = false;

// ===== Anti-spam UID print =====
String lastUIDPrinted = "";
unsigned long lastUIDTime = 0;

// ==== Biển số OCR từ Python (OUT:) ====
String bienSoOCR = "";

// ===== Gate states =====
TaskHandle_t gateTaskHandle = NULL;
volatile bool gateCycleRequest = false;
volatile bool gateHoldOpen = false;
volatile bool gateForceClose = false;
volatile bool gateBusy = false;
volatile unsigned long metric_t0 = 0;
volatile bool gateHoldOpenBeeped = false;

volatile bool buzzerOwned = false;

bool fireWasActive = false;

BluetoothSerial SerialBT;
bool btConnected = false;
String btBuffer = "";

// ===== LCD state =====
enum LCDMode {
  LCD_MODE_IDLE = 0,
  LCD_MODE_GOODBYE,
  LCD_MODE_MESSAGE
};

volatile LCDMode currentLCDMode = LCD_MODE_IDLE;
volatile unsigned long lcdMessageUntil = 0;

// ==== Cấu trúc quản lý thẻ ====
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

// ================== PWM RGB ==================
const int PWM_FREQ = 5000;
const int PWM_RES  = 8;   // 0..255

int ledState = 0;         // 0=off, 1=red, 2=green, 3=blue, 4=yellow, 5=white
bool lastButton = HIGH;

// =====================================================
// ================= LCD HELPERS ========================
// =====================================================

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

void initIdleLCD() {
  currentLCDMode = LCD_MODE_IDLE;
  lcd.clear();
  lcdPrintFixed16("     Please     ", "Check your card ");
}

void initGoodbyeLCD() {
  currentLCDMode = LCD_MODE_GOODBYE;
  lcd.clear();
  lcdPrintFixed16("    Goodbye     ", "See you again   ");
}

void LCD_SaiBienSo() {
  showMessageLCD("Sai bien so!    ", "Check again     ", 1500);
}

void LCD_SaiThe() {
  showMessageLCD("The khong hop   ", "Check again     ", 2200);
}

void LCD_GuestChuaGan() {
  showMessageLCD("Guest chua gan  ", "Bien so         ", 1500);
}

void LCD_TheBiKhoa() {
  showMessageLCD("The bi khoa!    ", "Lien he quan ly ", 1800);
}

void LCD_BAO_CHAY() {
  showMessageLCD("****Warning**** ", "Canh Bao Co Chay", 300);
}

void refreshLCDState() {
  if (currentLCDMode == LCD_MODE_MESSAGE && millis() > lcdMessageUntil) {
    if (gateBusy) initGoodbyeLCD();
    else if (!scanMode && !gateHoldOpen) initIdleLCD();
  }
}

// =====================================================
// ================= COMMON HELPERS =====================
// =====================================================

void setColor(int r, int g, int b) {
  ledcWrite(R_PIN, 255 - r);
  ledcWrite(G_PIN, 255 - g);
  ledcWrite(B_PIN, 255 - b);
}

void applyLedState() {
  switch (ledState) {
    case 0: setColor(0,   0,   0);   break;
    case 1: setColor(255, 0,   0);   break;
    case 2: setColor(0,   255, 0);   break;
    case 3: setColor(0,   0,   255); break;
    case 4: setColor(255, 255, 0);   break;
    case 5: setColor(255, 255, 255); break;
  }
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

// ===== Quản lý mảng thẻ =====
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
// ================= GATE TASK =========================
// =====================================================

void taskGate(void *pvParameters) {
  unsigned long start = 0;

  for (;;) {
    if (gateForceClose) {
      gateForceClose = false;
      gateHoldOpen = false;
      gateHoldOpenBeeped = false;
      gateCycleRequest = false;
      gateBusy = true;

      cua_ra.write(20);

      buzzerOwned = true;
      digitalWrite(PIN_BUZZER, HIGH);
      vTaskDelay(pdMS_TO_TICKS(60));
      digitalWrite(PIN_BUZZER, LOW);
      buzzerOwned = false;

      gateBusy = false;
      initIdleLCD();
      continue;
    }

    if (gateHoldOpen) {
      gateBusy = true;
      cua_ra.write(100);

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

    cua_ra.write(100);
    initGoodbyeLCD();

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

      if (digitalRead(PIN_FIRE) == LOW) {
        cua_ra.write(100);
        digitalWrite(PIN_BUZZER, HIGH);
        vTaskDelay(pdMS_TO_TICKS(30));
        continue;
      }

      if (millis() - start > 3000) break;
    }

    cua_ra.write(20);
    gateBusy = false;
    initIdleLCD();
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
  bienSoOCR.trim();

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

        Serial.print("OCR: '"); Serial.print(bienSoOCR); Serial.print("' ");
        Serial.print("DS: '");  Serial.print(dsThe[i].bienso); Serial.println("'");

        if (bienSoOCR.equals(dsThe[i].bienso)) {
          Serial.println("Resident OK -> Mo cua");
          Serial.println("DATA,DATE,TIME," + StrUID + ',' + dsThe[i].hoten + ',' + bienSoOCR + ",Out");
          metric_t0 = millis();
          gateCycleRequest = true;
          bienSoOCR = "";
        } else {
          Serial.println("Sai bien so resident khi RA!");
          LCD_SaiBienSo();
          beepShort(2, 70);
        }
      } else {
        if (dsThe[i].daGanBienSo) {
          bienSoOCR.trim();
          dsThe[i].bienso.trim();

          Serial.print("OCR: '"); Serial.print(bienSoOCR); Serial.print("' ");
          Serial.print("DS: '");  Serial.print(dsThe[i].bienso); Serial.println("'");

          if (bienSoOCR.equals(dsThe[i].bienso)) {
            Serial.println("Guest OK -> Mo cua");
            Serial.println("DATA,DATE,TIME," + StrUID + ",Guest," + bienSoOCR + ",Out");

            metric_t0 = millis();
            gateCycleRequest = true;

            dsThe[i].bienso = "";
            dsThe[i].daGanBienSo = false;
            bienSoOCR = "";
          } else {
            Serial.println("Sai bien so guest khi RA!");
            LCD_SaiBienSo();
            beepShort(2, 70);
          }
        } else {
          Serial.println("Guest chua co bien so gan tu luc vao!");
          LCD_GuestChuaGan();
          beepShort(2, 70);
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
// ================= RGB BUTTON ========================
// =====================================================

void handleButton() {
  bool buttonState = digitalRead(BUTTON_PIN);

  if (lastButton == HIGH && buttonState == LOW) {
    ledState = (ledState + 1) % 6;
    applyLedState();

    digitalWrite(PIN_BUZZER, HIGH);
    vTaskDelay(pdMS_TO_TICKS(150));
    digitalWrite(PIN_BUZZER, LOW);
  }

  lastButton = buttonState;
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

      if (buffer.startsWith("OUT:")) {
        bienSoOCR = buffer.substring(4);
        bienSoOCR.trim();
        Serial.println("Camera OUT -> bienSoOCR = '" + bienSoOCR + "'");
      }
      else if (buffer.startsWith("REG:")) {
        int commaIndex = buffer.indexOf(',');
        if (commaIndex > 4) {
          String uid = buffer.substring(4, commaIndex);
          String plate = buffer.substring(commaIndex + 1);
          uid.trim(); plate.trim();

          for (int i = 0; i < soThe; i++) {
            if (dsThe[i].uid == uid) {
              dsThe[i].bienso = plate;
              dsThe[i].daGanBienSo = true;
              Serial.println("Guest " + uid + " da duoc cap bien so " + plate);
            }
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
        Serial.println("Nhan lenh mo barie thu cong!");
        gateHoldOpen = true;
        gateHoldOpenBeeped = false;
        gateForceClose = false;
      }
      else if (buffer.equals("CLOSE_GATE")) {
        Serial.println("Nhan lenh dong barie thu cong!");
        gateHoldOpen = false;
        gateHoldOpenBeeped = false;
        gateForceClose = true;
      }
      else if (buffer.equals("TOGGLE_LED")) {
        Serial.println("Nhan lenh doi mau LED!");
        ledState = (ledState + 1) % 6;
        applyLedState();

        buzzerOwned = true;
        digitalWrite(PIN_BUZZER, HIGH);
        vTaskDelay(pdMS_TO_TICKS(80));
        digitalWrite(PIN_BUZZER, LOW);
        buzzerOwned = false;
      }

      buffer = "";
    } else {
      buffer += c;
    }
  }
}

void beepConfirmBT(int ms = 80) {
  buzzerOwned = true;
  digitalWrite(PIN_BUZZER, HIGH);
  vTaskDelay(pdMS_TO_TICKS(ms));
  digitalWrite(PIN_BUZZER, LOW);
  buzzerOwned = false;
}

void handleBluetoothCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "OFF") {
    ledState = 0;
  }
  else if (cmd == "RED") {
    ledState = 1;
  }
  else if (cmd == "GREEN") {
    ledState = 2;
  }
  else if (cmd == "BLUE") {
    ledState = 3;
  }
  else if (cmd == "YELLOW") {
    ledState = 4;
  }
  else if (cmd == "WHITE") {
    ledState = 5;
  }
  else if (cmd == "NEXT") {
    ledState = (ledState + 1) % 6;
  }
  else {
    SerialBT.println("ERR");
    return;
  }

  applyLedState();
  SerialBT.print("OK:");
  SerialBT.println(cmd);
  beepConfirmBT(80);
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
  const TickType_t period = pdMS_TO_TICKS(10);

  static int lastManualBtn = LOW;
  static unsigned long lastManualDebounce = 0;
  static int lastRgbBtn = HIGH;
  static unsigned long lastRgbDebounce = 0;

  for (;;) {
    // Fire sensor
    if (digitalRead(PIN_FIRE) == LOW) {
      fireWasActive = true;
      LCD_BAO_CHAY();
      buzzerOwned = true;
      digitalWrite(PIN_BUZZER, HIGH);
      cua_ra.write(100);
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

    refreshLCDState();

    // Manual barrier button
    int manualState = digitalRead(PIN_BTN_MANUAL);
    unsigned long now = millis();

    if (!gateHoldOpen && !gateBusy) {
      if (manualState == HIGH && lastManualBtn == LOW && (now - lastManualDebounce > 80)) {
        gateCycleRequest = true;
        metric_t0 = 0;
        lastManualDebounce = now;
      }
    }
    lastManualBtn = manualState;

    // RGB button
    bool rgbState = digitalRead(BUTTON_PIN);
    if (lastRgbBtn == HIGH && rgbState == LOW && (now - lastRgbDebounce > 120)) {
      ledState = (ledState + 1) % 6;
      applyLedState();

      buzzerOwned = true;
      digitalWrite(PIN_BUZZER, HIGH);
      vTaskDelay(pdMS_TO_TICKS(60));
      digitalWrite(PIN_BUZZER, LOW);
      buzzerOwned = false;

      lastRgbDebounce = now;
    }
    lastRgbBtn = rgbState;

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


void taskBluetooth(void *pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(20);

  for (;;) {
    while (SerialBT.available()) {
      char c = (char)SerialBT.read();

      if (c == '\n' || c == '\r') {
        btBuffer.trim();
        if (btBuffer.length() > 0) {
          handleBluetoothCommand(btBuffer);
          btBuffer = "";
        }
      } else {
        btBuffer += c;
      }
    }

    vTaskDelayUntil(&lastWake, period);
  }
}

// =====================================================
// ================= SETUP / LOOP =======================
// =====================================================

void setup() {
  Serial.begin(9600);

  SerialBT.begin("ESP32_RGB_OUT", true);   // true = có thể đặt pin

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  cua_ra.setPeriodHertz(50);
  cua_ra.attach(PIN_SERVO, 500, 2400);
  cua_ra.write(20);

  pinMode(PIN_VATCAN, INPUT);
  pinMode(PIN_BTN_MANUAL, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FIRE, INPUT);

  digitalWrite(PIN_BUZZER, LOW);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SS_PIN);
  mfrc522.PCD_Init();
  delay(250);

  ledcAttach(R_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(G_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(B_PIN, PWM_FREQ, PWM_RES);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setColor(0, 0, 0);

  attachInterrupt(digitalPinToInterrupt(PIN_VATCAN), vatCanISR, CHANGE);

  initIdleLCD();

  xTaskCreatePinnedToCore(taskSerial, "taskSerial", 5000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskSensor, "taskSensor", 4000, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(taskRFID,   "taskRFID",   5000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskGate,   "taskGate",   4000, NULL, 4, &gateTaskHandle, 1);
  xTaskCreatePinnedToCore(taskBluetooth, "taskBluetooth", 5000, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}