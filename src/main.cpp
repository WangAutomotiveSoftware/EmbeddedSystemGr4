#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP32Servo.h>


#define DHTPIN 15
#define DHTTYPE DHT22
#define POT_PIN 34       
#define RAIN_PIN 35      
#define LED_PIN 5
#define BUZZER_PIN 18
#define TRIG_PIN 12      
#define ECHO_PIN 14
#define SERVO_PIN 19     

// Chân 4 nút nhấn
#define BTN_TEMP 4
#define BTN_PARK 16
#define BTN_RAIN 17
#define BTN_THROT 25

DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Servo wiperServo;

// --- BIẾN TOÀN CỤC & MUTEX ---
SemaphoreHandle_t dataMutex;
float shared_temp = 0.0;
int shared_distance = 0;
int shared_rain = 0;
int shared_throttle = 0;
int display_mode = 0; 

// --- KHAI BÁO CÁC TASK ---
void TaskSensorsAndSafety(void *pvParameters);
void TaskWiperControl(void *pvParameters);
void TaskDisplayUpdate(void *pvParameters);
void TaskButtonRead(void *pvParameters);

void setup() {
  Serial.begin(115200);
  
  pinMode(BTN_TEMP, INPUT_PULLUP);
  pinMode(BTN_PARK, INPUT_PULLUP);
  pinMode(BTN_RAIN, INPUT_PULLUP);
  pinMode(BTN_THROT, INPUT_PULLUP);
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  dht.begin();
  
  ESP32PWM::allocateTimer(0);
  wiperServo.setPeriodHertz(50);
  wiperServo.attach(SERVO_PIN, 500, 2400);
  wiperServo.write(0);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED init failed"));
    for(;;);
  }
  
  dataMutex = xSemaphoreCreateMutex();

  // Khởi tạo các Task (FreeRTOS)
  xTaskCreate(TaskSensorsAndSafety, "Sensors", 4096, NULL, 2, NULL);
  xTaskCreate(TaskWiperControl, "Wiper", 2048, NULL, 2, NULL);
  xTaskCreate(TaskDisplayUpdate, "Display", 4096, NULL, 1, NULL);
  xTaskCreate(TaskButtonRead, "Buttons", 2048, NULL, 1, NULL);
}

void loop() {} // Để trống do dùng FreeRTOS

// --- TASK ĐỌC NÚT NHẤN CHUYỂN MÀN HÌNH ---
void TaskButtonRead(void *pvParameters) {
  for(;;) {
    if(digitalRead(BTN_TEMP) == LOW) { display_mode = 0; }
    if(digitalRead(BTN_PARK) == LOW) { display_mode = 1; }
    if(digitalRead(BTN_RAIN) == LOW) { display_mode = 2; }
    if(digitalRead(BTN_THROT) == LOW) { display_mode = 3; }
    vTaskDelay(pdMS_TO_TICKS(100)); // Chống dội phím (Debounce)
  }
}

// --- TASK ĐỌC CẢM BIẾN & LOG & CẢNH BÁO ---
void TaskSensorsAndSafety(void *pvParameters) {
  char logBuffer[100];
  static bool throttleBeep = false; // Biến trạng thái tạo tiếng bíp ngắt quãng
  
  for(;;) {
    float t = dht.readTemperature();
    
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    int d = (duration == 0) ? 999 : (duration * 0.034 / 2);

    int r = analogRead(RAIN_PIN);
    int th = map(analogRead(POT_PIN), 0, 4095, 0, 100);

    if(xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      if(!isnan(t)) shared_temp = t;
      shared_distance = d;
      shared_rain = r;
      shared_throttle = th;
      xSemaphoreGive(dataMutex);
    }

    snprintf(logBuffer, sizeof(logBuffer), "Temp: %.1fC | Park: %dcm | Rain: %d | Throt: %d%%", shared_temp, shared_distance, shared_rain, shared_throttle);
    Serial.println(logBuffer);

    // Logic an toàn nhiều cấp độ
    if(shared_temp >= 40.0 || (shared_distance > 0 && shared_distance <= 20)) {
      // Mức 1: Nguy hiểm (Nhiệt cao hoặc sắp đụng tường) -> Còi kêu chói, liên tục
      digitalWrite(LED_PIN, HIGH);
      tone(BUZZER_PIN, 1000); 
    } 
    else if (shared_throttle > 80) {
      // Mức 2: Cảnh báo (Chân ga > 80%) -> Còi bíp ngắt quãng, âm trầm
      digitalWrite(LED_PIN, LOW);
      throttleBeep = !throttleBeep; // Đảo trạng thái còi mỗi 500ms
      if (throttleBeep) tone(BUZZER_PIN, 500);
      else noTone(BUZZER_PIN);
    } 
    else {
      // Mức 0: An toàn -> Tắt toàn bộ cảnh báo
      digitalWrite(LED_PIN, LOW);
      noTone(BUZZER_PIN);
      throttleBeep = false;
    }

    vTaskDelay(pdMS_TO_TICKS(500)); 
  }
}

// --- TASK ĐIỀU KHIỂN GẠT MƯA ---
void TaskWiperControl(void *pvParameters) {
  int pos = 0;
  bool sweepUp = true;
  for(;;) {
    int currentRain = 0;
    if(xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      currentRain = shared_rain;
      xSemaphoreGive(dataMutex);
    }

    if(currentRain < 500) {
      wiperServo.write(0);
      pos = 0;
      vTaskDelay(pdMS_TO_TICKS(500));
    } else {
      if(sweepUp) pos += 10; else pos -= 10;
      if(pos >= 180) sweepUp = false;
      if(pos <= 0) sweepUp = true;
      wiperServo.write(pos);
      int speedDelay = (currentRain > 2000) ? 15 : 40; 
      vTaskDelay(pdMS_TO_TICKS(speedDelay));
    }
  }
}

// --- TASK HIỂN THỊ MÀN HÌNH THEO CHẾ ĐỘ ---
void TaskDisplayUpdate(void *pvParameters) {
  for(;;) {
    float t = 0; int d = 0, r = 0, th = 0;
    if(xSemaphoreTake(dataMutex, portMAX_DELAY)) {
      t = shared_temp;
      d = shared_distance;
      r = shared_rain;
      th = shared_throttle;
      xSemaphoreGive(dataMutex);
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    
    // Cảnh báo đè lên mọi chế độ hiển thị nếu có nguy hiểm
    if(t >= 40.0 || (d > 0 && d <= 20)) {
       display.setTextSize(1);
       display.println("! DANGER !");
    }

    display.setTextSize(2); // Cỡ chữ lớn
    display.setCursor(0, 20);

    switch(display_mode) {
      case 0: // Nút 1: Nhiệt độ
        display.println("ENGINE");
        display.setTextSize(1);
        display.print("Temp: "); display.print(t, 1); display.println(" C");
        break;
      case 1: // Nút 2: Lùi xe
        display.println("PARKING");
        display.setTextSize(1);
        if(d > 400) display.println("Clear");
        else { display.print("Dist: "); display.print(d); display.println(" cm"); }
        break;
      case 2: // Nút 3: Gạt mưa
        display.println("WIPER");
        display.setTextSize(1);
        if(r < 500) display.println("Off");
        else if(r < 2000) display.println("Speed: Low");
        else display.println("Speed: High");
        break;
      case 3: // Nút 4: Bướm ga
        display.println("THROTTLE");
        display.setTextSize(1);
        display.print("Power: "); display.print(th); display.println(" %");
        break;
    }

    display.display();
    vTaskDelay(pdMS_TO_TICKS(200)); 
  }
}
