#include <SPI.h>
#include <MFRC522.h>

#define RST_PIN         9
#define SS_PIN          10
#define SENSOR_VATCAN A0

#define R_PIN A1
#define G_PIN A2
#define B_PIN A3
#define BUTTON_PIN 3

#include <Wire.h>
#include <Servo.h>
#include <LiquidCrystal_I2C.h>
LiquidCrystal_I2C lcd(0x27,16,2); 

MFRC522 mfrc522(SS_PIN, RST_PIN);

Servo cua_ra;

// ==== Biến RFID ====
int readsuccess;
byte readcard[4];
char str[32] = "";
String StrUID;

// ==== Biển số OCR từ Python (OUT:) ====
String bienSoOCR = "";

// ==== Cấu trúc quản lý thẻ ====
struct TheXe {
  String uid;
  String hoten;
  String bienso;
  bool laResident;
  bool daGanBienSo;
};

TheXe dsThe[] = {
  {"D02F965F", "Dang Quang Ninh", "85A45632", true, true},   // Resident
  {"294FB298", "Vu Thi Hoai Thu", "17A85126", true, true},    // Resident
  {"53372E56", "Dinh Kiet Vy", "30G49344", true, true},    // Resident
  {"56D64906", "", "", false, false}, // Guest
  {"D1764A06", "", "", false, false},  // Guest
  {"3A4D3216", "", "", false, false}  // Guest
};
int soThe = 6;

// --------------------------------------------------------------------
// Hàm còi cảnh báo
void coiCanhBao(int soLan, int delayTime = 200) {
  for (int i = 0; i < soLan; i++) {
    digitalWrite(5, HIGH);
    delay(delayTime);
    digitalWrite(5, LOW);
    delay(delayTime);
  }
}

// --------------------------------------------------------------------
// Hàm LCD hiển thị lỗi
void LCD_SaiBienSo() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Sai bien so!");
  lcd.setCursor(0,1);
  lcd.print("Check again");
}

void LCD_SaiThe() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("The khong hop");
  lcd.setCursor(0,1);
  lcd.print("Check again");
}

void LCD_GuestChuaGan() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Guest chua gan");
  lcd.setCursor(0,1);
  lcd.print("Bien so");
}

// --------------------------------------------------------------------
// Hàm mở cửa
void mo_cua()
{
    cua_ra.write(100); // mở barrier
    LCD_TRUE();

    // Còi kêu 2 lần "bíp bíp"
    coiCanhBao(2, 100);

    // Giữ barrier mở khi còn tín hiệu cảm biến
    unsigned long start = millis();
    while (true) {
      if (digitalRead(SENSOR_VATCAN) == LOW) {
        start = millis();
      }
      if (millis() - start > 3000) {
        break;
      }
    }

    // Đóng barrier
    cua_ra.write(10); 
}

// --------------------------------------------------------------------
// Hàm xử lý RA
void senddata()
{
   readsuccess = getid();
   if(readsuccess)
   {
      bool found = false;
      bienSoOCR.trim();  // loại bỏ khoảng trắng trước khi so sánh

      for (int i=0; i<soThe; i++) {
        if (dsThe[i].uid == StrUID) {
          found = true;
          if (dsThe[i].laResident) {
            bienSoOCR.trim();
            dsThe[i].bienso.trim();
            Serial.print("OCR: '"); Serial.print(bienSoOCR); Serial.print("' ");
            Serial.print("DS: '");  Serial.print(dsThe[i].bienso); Serial.println("'");

            if (bienSoOCR.equals(dsThe[i].bienso)) {
                Serial.println("Resident OK -> Mo cua");
                Serial.println("DATA,DATE,TIME," + StrUID + ',' + dsThe[i].hoten + ',' + bienSoOCR + ",Out");
                mo_cua();
                bienSoOCR = "";  // reset
            } else {
                Serial.println("Sai biển số resident khi RA!");
                LCD_SaiBienSo();
                coiCanhBao(2);
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
                    mo_cua();
                    dsThe[i].bienso = "";
                    dsThe[i].daGanBienSo = false;
                    bienSoOCR = ""; // reset
                } else {
                    Serial.println("Sai biển số guest khi RA!");
                    LCD_SaiBienSo();
                    coiCanhBao(2);
                }
            } else {
                Serial.println("Guest này chưa có biển số gán từ lúc vào!");
                LCD_GuestChuaGan();
                coiCanhBao(2);
            }
          }


        }
      }
      if (!found) {
        Serial.println("Thẻ không hợp lệ!");
        LCD_SaiThe();
        coiCanhBao(4, 500);
      }
   }
}

// --------------------------------------------------------------------
// Đọc UID RFID
int getid(){  
  if(!mfrc522.PICC_IsNewCardPresent()){
    return 0;
  }
  if(!mfrc522.PICC_ReadCardSerial()){
    return 0;
  }
  for(int i=0;i<4;i++)
  {
    readcard[i]=mfrc522.uid.uidByte[i];
    array_to_string(readcard, 4, str);
    StrUID = str;
  }
  mfrc522.PICC_HaltA();
  return 1;
}

// Hàm chuyển mảng byte thành chuỗi
void array_to_string(byte array[], unsigned int len, char buffer[])
{
    for (unsigned int i = 0; i < len; i++)
    {
        byte nib1 = (array[i] >> 4) & 0x0F;
        byte nib2 = (array[i] >> 0) & 0x0F;
        buffer[i*2+0] = nib1  < 0xA ? '0' + nib1  : 'A' + nib1  - 0xA;
        buffer[i*2+1] = nib2  < 0xA ? '0' + nib2  : 'A' + nib2  - 0xA;
    }
    buffer[len*2] = '\0';
}

// --------------------------------------------------------------------
// Hàm LCD mặc định
void LCD(){
    lcd.setCursor(0,0);
    lcd.print("     Please     ");
    lcd.setCursor(0,1);
    lcd.print("Check your card ");
}
void LCD_TRUE()
{
    lcd.clear();
    lcd.setCursor(5,0);
    lcd.print("Goodbye");
    lcd.setCursor(0,1);
    lcd.print("See you again  ");
}
void LCD_BAO_CHAY(){
    lcd.setCursor(0,0);
    lcd.print("****Warning*** ");
    lcd.setCursor(0,1);
    lcd.print("Canh Bao Co Chay");
}

int ledState = 0; // trạng thái LED: 0=off, 1=red, 2=green, 3=blue, 4=yellow, 5=white
bool lastButton = HIGH;

void setColor(int r, int g, int b) {
  analogWrite(R_PIN, r);
  analogWrite(G_PIN, g);
  analogWrite(B_PIN, b);
}

void handleButton() {
  bool buttonState = digitalRead(BUTTON_PIN);
  if (lastButton == HIGH && buttonState == LOW) {
    ledState = (ledState + 1) % 6; // quay vòng 0→5
    switch (ledState) {
      case 0: setColor(0, 0, 0); break;           // Tắt
      case 1: setColor(255, 0, 0); break;         // Đỏ
      case 2: setColor(0, 255, 0); break;         // Xanh lá
      case 3: setColor(0, 0, 255); break;         // Xanh dương
      case 4: setColor(255, 255, 0); break;       // Vàng
      case 5: setColor(255, 255, 255); break;     // Trắng
    }

    // >>> Buzzer kêu "bíp" 1 lần
    digitalWrite(5, HIGH);
    delay(100);
    digitalWrite(5, LOW);

    delay(200); // chống dội nút
  }
  lastButton = buttonState;
}

// Đọc biển số ra
void docBienSoTuPython() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    static String buffer = "";

    if (c == '\n') {
      buffer.trim();
      Serial.print("Raw from Python: '");
      Serial.print(buffer);
      Serial.println("'");

      if (buffer.startsWith("OUT:")) {
        bienSoOCR = buffer.substring(4);
        bienSoOCR.trim();
        Serial.println("Camera OUT -> bienSoOCR = '" + bienSoOCR + "'");
      } else if (buffer.startsWith("REG:")) {
        int commaIndex = buffer.indexOf(',');
        if (commaIndex > 4) {
          String uid = buffer.substring(4, commaIndex);
          String plate = buffer.substring(commaIndex + 1);
          for (int i = 0; i < soThe; i++) {
            if (dsThe[i].uid == uid) {
              dsThe[i].bienso = plate;
              dsThe[i].daGanBienSo = true;
              Serial.println("Guest " + uid + " da duoc cap bien so " + plate);
            }
          }
        }
      }

      // ===== Lệnh điều khiển thủ công từ Python / Web =====
      else if (buffer.equals("OPEN_GATE")) {
        Serial.println("🔓 Nhận lệnh mở barie thủ công từ Python!");
        cua_ra.write(100);     // mở barie
        digitalWrite(5, HIGH); // còi bíp nhẹ báo
        delay(100);
        digitalWrite(5, LOW);
      }

      else if (buffer.equals("CLOSE_GATE")) {
        Serial.println("🔒 Nhận lệnh đóng barie thủ công từ Python!");
        cua_ra.write(10);      // đóng barie
        digitalWrite(5, HIGH); // bíp nhẹ báo đóng
        delay(100);
        digitalWrite(5, LOW);
      }

      else if (buffer.equals("TOGGLE_LED")) {
        Serial.println("💡 Nhận lệnh đổi màu LED từ Python!");
        ledState = (ledState + 1) % 6; // quay vòng như nút
        switch (ledState) {
          case 0: setColor(0, 0, 0); break;
          case 1: setColor(255, 0, 0); break;
          case 2: setColor(0, 255, 0); break;
          case 3: setColor(0, 0, 255); break;
          case 4: setColor(255, 255, 0); break;
          case 5: setColor(255, 255, 255); break;
        }

        digitalWrite(5, HIGH);
        delay(50);
        digitalWrite(5, LOW);
      }


      
      buffer = ""; // reset sau khi xử lý xong
    } else {
      buffer += c; // ghép ký tự
    }
  }
}





// --------------------------------------------------------------------
void setup() {
    Serial.begin(9600);
    pinMode(SENSOR_VATCAN, INPUT);
    cua_ra.attach(6);
    pinMode(2, INPUT); // nút mở thủ công lối ra
    pinMode(5,OUTPUT);
    pinMode(7,INPUT);
    cua_ra.write(10);

    digitalWrite(5, LOW);   
    
    lcd.init();                     
    lcd.backlight();
    lcd.clear();
    
    SPI.begin();      
    mfrc522.PCD_Init(); 
    delay(250);
    LCD(); 
    delay(250);

    // RGB setup
    pinMode(R_PIN, OUTPUT);
    pinMode(G_PIN, OUTPUT);
    pinMode(B_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    setColor(0,0,0); // tắt ban đầu
}

// --------------------------------------------------------------------
void loop() {
  docBienSoTuPython();   // Nhận dữ liệu OUT từ Python hoặc REG từ Arduino VÀO

  if(digitalRead(7)==LOW) {
    lcd.clear();
    LCD_BAO_CHAY();
    digitalWrite(5,HIGH);
    cua_ra.write(100);
  } else {
    LCD();
    digitalWrite(5,LOW);
  }

  if (digitalRead(2) == HIGH) {   // nếu nút bấm thủ công
    mo_cua();   
  }
  
  senddata(); // xử lý RFID + biển số OCR

  handleButton(); // xử lý nút đổi màu LED RGB
}
