#include <SPI.h> // khai báo thư viện
#include <MFRC522.h>
#define RST_PIN         9           // Định nghĩa chân
#define SS_PIN          10   // SDA
#define SENSOR_VATCAN 2
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
LiquidCrystal_I2C lcd(0x27,16,2); // khai báo thư viện I2C địa chỉ và màn hình 16x2

MFRC522 mfrc522(SS_PIN, RST_PIN);   // Hàm rfid
int readsuccess;
byte readcard[4];
char str[32] = "";
String StrUID, user;

Servo cua_vao;

int anhsang,lua=HIGH;
int s1;
int s2;
int s3;
int s4,doorstate;

// Biển số nhận từ Python (camera IN)
String bienSoOCR = "";

// ===== CẤU TRÚC QUẢN LÝ THẺ =====
struct TheXe {
  String uid;
  String hoten;
  String bienso;
  bool laResident;     // true = cư dân, false = khách
  bool daGanBienSo;    // với guest: đã gán biển số hay chưa
};

// Ví dụ 3 thẻ resident + 6   thẻ guest trống
TheXe dsThe[] = {
  {"D02F965F", "Dang Quang Ninh", "85A45632", true, true},   // Resident
  {"294FB298", "Vu Thi Hoai Thu", "17A85126", true, true},    // Resident
  {"53372E56", "Dinh Kiet Vy", "30G49344", true, true},    // Resident
  {"56D64906", "", "", false, false}, // Guest
  {"D1764A06", "", "", false, false},  // Guest
  {"3A4D3216", "", "", false, false}  // Guest
};
int soThe = 6;

// Hàm kiểm tra bãi đầy
bool baiDay() {
  return (s1==LOW && s2==LOW && s3==LOW && s4==LOW);
}

// Hàm còi cảnh báo chung
void coiCanhBao(int soLan, int delayTime = 200) {
  for (int i = 0; i < soLan; i++) {
    digitalWrite(5, HIGH);
    delay(delayTime);
    digitalWrite(5, LOW);
    delay(delayTime);
  }
}

// ==== Các hàm LCD báo lỗi chi tiết ====
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
    lcd.print("Guest chua co");
    lcd.setCursor(0,1);
    lcd.print("Bien so");
}


// Xử lý khi quét thẻ thành công
void senddata() {
  readsuccess = getid();
  if (!readsuccess) return;

  bool found = false;
  for (int i = 0; i < soThe; i++) {
    if (dsThe[i].uid == StrUID) {
      found = true;

      if (dsThe[i].laResident) {
        // Cư dân
        bienSoOCR.trim();
        dsThe[i].bienso.trim();
        

        if (bienSoOCR.equals(dsThe[i].bienso)) {
            if (baiDay()) {
                hienParkingFull();
                return;
            }
            Serial.println("Resident OK -> Mo cua");
            Serial.print("DATA,DATE,TIME,");
            Serial.print(StrUID);
            Serial.print(",");
            Serial.print(dsThe[i].hoten);
            Serial.print(",");
            Serial.print(bienSoOCR);
            Serial.println(",In");

            mo_cua();
            bienSoOCR = "";   // reset sau khi mở cửa
        } else {
            Serial.println("Sai biển số resident!");
            LCD_SaiBienSo();
            coiCanhBao(2);
        }
    } else {
        // Guest
        if (bienSoOCR.length() == 0) {
            Serial.println("Guest chưa có biển số OCR, không mở!");
            LCD_GuestChuaGan();
            coiCanhBao(2);
            return;
        }

        if (!dsThe[i].daGanBienSo) {
            // Lần đầu vào
            dsThe[i].bienso = bienSoOCR;
            dsThe[i].daGanBienSo = true;
            Serial.println("Guest vao, gan bien so: " + bienSoOCR);
            Serial.println("REG:" + StrUID + "," + bienSoOCR);

            if (baiDay()) {
                hienParkingFull();
                return;
            }

            Serial.print("DATA,DATE,TIME,");
            Serial.print(StrUID);
            Serial.print(",Guest,");
            Serial.print(bienSoOCR);
            Serial.println(",In");

            mo_cua();
            bienSoOCR = "";   // reset sau khi mở cửa
        } else {
            // Đã có biển số -> so sánh
            bienSoOCR.trim();
            dsThe[i].bienso.trim();
            

            if (bienSoOCR.equals(dsThe[i].bienso)) {
                if (baiDay()) {
                    hienParkingFull();
                    return;
                }

                Serial.print("DATA,DATE,TIME,");
                Serial.print(StrUID);
                Serial.print(",Guest,");
                Serial.print(bienSoOCR);
                Serial.println(",In");

                mo_cua();
                bienSoOCR = "";   // reset sau khi mở cửa
            } else {
                Serial.println("Sai biển số guest!");
                LCD_SaiBienSo();
                coiCanhBao(2);
            }
        }
      }


    }
  }

  // Nếu UID không nằm trong danh sách
  if (!found) {
    Serial.println("Thẻ không hợp lệ!");
    LCD_SaiThe();
    coiCanhBao(4, 500);
  }
}


// Hàm mở barrier
void mo_cua()
{   
    cua_vao.write(90); // mở barrier
    LCD_TRUE();

    // Còi kêu 2 lần "bíp bíp"
    for(int i=0;i<2;i++)
    {
      digitalWrite(5,HIGH);   // bật còi
      delay(100); 
      digitalWrite(5,LOW);  // tắt còi
      delay(100);  
    } 

    // Giữ barrier mở khi còn tín hiệu cảm biến
    unsigned long start = millis();
    while (true) {
      if (digitalRead(SENSOR_VATCAN) == LOW) {
        start = millis(); // reset thời gian khi còn xe
      }
      if (millis() - start > 3000) { // 3 giây không còn xe
        break;
      }
    }
    cua_vao.write(0); // Đóng barrier
}

// Hàm hiển thị Parking Full
void hienParkingFull() {
  lcd.clear();
  lcd.setCursor(4,0);
  lcd.print("Welcome");
  lcd.setCursor(2,1);
  lcd.print("Parking Full");

  for(int i=0;i<3;i++) {
    digitalWrite(5,HIGH);
    delay(200);
    digitalWrite(5,LOW);
    delay(200);
  }
  delay(2000);
}

// Hàm đọc UID từ thẻ RFID
int getid(){  
  if(!mfrc522.PICC_IsNewCardPresent()){
    return 0;
  }
  if(!mfrc522.PICC_ReadCardSerial()){
    return 0;
  }
  for(int i=0;i<4;i++) {
    readcard[i]=mfrc522.uid.uidByte[i];
    array_to_string(readcard, 4, str);
    StrUID = str;
  }
  mfrc522.PICC_HaltA();
  return 1;
}

// Hàm chuyển mảng byte UID thành chuỗi
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

// Hàm đọc cảm biến ánh sáng, lửa, và hiển thị
void sensor()
{
      s1= digitalRead(A0);
      s2= digitalRead(A1);
      s3= digitalRead(A2);
      s4= digitalRead(A3);
      anhsang= digitalRead(8);
      lua= digitalRead(7);

      if(anhsang==HIGH) digitalWrite(4,HIGH); // bật đèn
      else digitalWrite(4,LOW);
 
      if(lua==LOW) 
       {
           LCD_BAO_CHAY();
           cua_vao.write(90); 
           digitalWrite(5,HIGH);// loa kêu 
       }
       else 
       {
            digitalWrite(5,LOW);
            LCD();               
       }  
}

// Hàm đếm chỗ trống
int demChoTrong() {
  int count = 0;
  if (s1 != LOW) count++;
  if (s2 != LOW) count++;
  if (s3 != LOW) count++;
  if (s4 != LOW) count++;
  return count;
}

// Các hàm LCD
void LCD(){
    lcd.clear();
    lcd.setCursor(0,0);   
    lcd.print("     Welcome    ");
    
    int slots = demChoTrong();
    lcd.setCursor(0,1);
    lcd.print("Slots: ");
    lcd.print(slots);
    lcd.print("/4  ");
}
void LCD_FALSE(){
    lcd.clear();
    lcd.setCursor(3,0);
    lcd.print("Wrong card");
    lcd.setCursor(0,1);
    lcd.print("Check Again");
}
void LCD_TRUE()
{
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("1    2    3    4"); 
    if(s1==LOW) { lcd.setCursor(0,1); lcd.print("X    "); }
    else        { lcd.setCursor(0,1); lcd.print("V    "); }
    if(s2==LOW) { lcd.setCursor(5,1); lcd.print("X    "); }
    else        { lcd.setCursor(5,1); lcd.print("V    "); }
    if(s3==LOW) { lcd.setCursor(10,1); lcd.print("X    "); }
    else        { lcd.setCursor(10,1); lcd.print("V    "); }
    if(s4==LOW) { lcd.setCursor(15,1); lcd.print("X"); }
    else        { lcd.setCursor(15,1); lcd.print("V"); }  
}
void LCD_BAO_CHAY(){
    lcd.setCursor(0,0);
    lcd.print("****Warning*****");
    lcd.setCursor(0,1);
    lcd.print("Canh Bao Co Chay");
}

//đọc dữ liệu biển số từ Python
void docBienSoTuPython() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    static String buffer = "";

    if (c == '\n') {
      buffer.trim();
      Serial.print("Raw from Python: '"); // debug
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
        Serial.println("Nhận yêu cầu xóa UID: " + uid);
        for (int i = 0; i < soThe; i++) {
          if (dsThe[i].uid == uid) {
            dsThe[i].bienso = "";
            dsThe[i].daGanBienSo = false;
            Serial.println("Đã reset thẻ khách " + uid);
          }
        }
      }

      // ===== Lệnh điều khiển thủ công từ Python / Web =====
      else if (buffer.equals("OPEN_GATE")) {
        Serial.println("Nhận lệnh mở barie thủ công từ Python!");
        cua_vao.write(100);     // mở barie
        digitalWrite(5, HIGH); // còi bíp nhẹ báo
        delay(100);
        digitalWrite(5, LOW);
      }

      else if (buffer.equals("CLOSE_GATE")) {
        Serial.println("Nhận lệnh đóng barie thủ công từ Python!");
        cua_vao.write(0);      // đóng barie
        digitalWrite(5, HIGH); // bíp nhẹ báo đóng
        delay(100);
        digitalWrite(5, LOW);
      }


      buffer = ""; // reset bộ đệm
    } else {
      buffer += c; // ghép ký tự vào chuỗi
    }
  }
}


// Setup
void setup() {  
    Serial.begin(9600);
    cua_vao.attach(6);
    cua_vao.write(0);
    pinMode(SENSOR_VATCAN, INPUT);
    pinMode(3, INPUT); // nút mở thủ công lối vào
    pinMode(4,OUTPUT);
    pinMode(5,OUTPUT);
    pinMode(7,INPUT);
    pinMode(8,INPUT);

    digitalWrite(5, LOW);   // đảm bảo tắt còi khi khởi động
    
    //lcd.init(); 
    lcd.init(); // initialize the lcd 
    lcd.backlight();

    SPI.begin();      // Init SPI bus
    mfrc522.PCD_Init(); // Init MFRC522 card
  
    Serial.println("CLEARDATA");
    Serial.println("LABEL,Date,Time,RFID UID,USER,Plate,IN/OUT");
    delay(1000);
    Serial.println("Scan PICC to see UID...");
    Serial.println("");
    delay(250);
    LCD(); 
    delay(250);
}

// Loop
void loop() {
  docBienSoTuPython();  // đọc dữ liệu IN:/OUT: từ Python

  sensor();
  senddata();

  if (digitalRead(3) == HIGH) {   // nếu nút bấm thủ công
    mo_cua();   // mở barrier như quẹt thẻ
  }

}
