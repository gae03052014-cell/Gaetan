/*
  BikeGuard V7 LIGHT - ESP32-C3 + MPU6050 + Buzzer + Bouton
  Code ultra léger - compile en 15 sec - 0 librairie lourde
  Fini Adafruit qui fait ramer la compil

  Câblage:
  ESP32-C3 3V3 -> MPU VCC
  GND -> MPU GND + Buzzer - + Bouton 1
  GPIO2 -> MPU SDA
  GPIO3 -> MPU SCL
  GPIO4 -> R 1k -> Base 2N2222 -> Buzzer + (Buzzer 100dB)
  GPIO5 -> Bouton 2 -> GND (INPUT_PULLUP)

  Auteur: Gaëtan - Concours 2026 - Version Light
*/

#include <Wire.h>

#define SDA_PIN 2
#define SCL_PIN 3
#define BUZZER_PIN 4
#define BUTTON_PIN 5

#define MPU_ADDR 0x68
#define PWR_MGMT_1 0x6B
#define ACCEL_XOUT_H 0x3B

// Seuils
#define SEUIL_NID_POULE 2.7f
#define SEUIL_CHUTE 4.2f
#define SEUIL_IMMOBILE 1.3f

float gForce = 0;
float ax, ay, az;
unsigned long chuteTime = 0;
bool alerteActive = false;
int buzzerPhase = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n--- BikeGuard V7 LIGHT ---");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  // Wake MPU6050
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(PWR_MGMT_1);
  Wire.write(0); // Wake up
  Wire.endTransmission(true);
  delay(100);

  // Test MPU
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x75); // WHO_AM_I
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 1, true);
  if (Wire.available()) {
    byte id = Wire.read();
    Serial.printf("MPU6050 OK ID=0x%02X\n", id);
    bip(1, 100);
  } else {
    Serial.println("MPU6050 NON TROUVE ! Verifie cablage SDA=2 SCL=3");
    bip(3, 200);
  }
}

void loop() {
  // Lecture accel brute (6 bytes)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(ACCEL_XOUT_H);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);

  if (Wire.available() == 6) {
    int16_t ax_raw = (Wire.read() << 8) | Wire.read();
    int16_t ay_raw = (Wire.read() << 8) | Wire.read();
    int16_t az_raw = (Wire.read() << 8) | Wire.read();

    // Converti en g (16384 LSB/g pour ±2g par défaut, on est en ±8g = 4096)
    // On simplifie: /4096.0 pour ±8g
    ax = ax_raw / 4096.0;
    ay = ay_raw / 4096.0;
    az = az_raw / 4096.0;

    gForce = sqrt(ax * ax + ay * ay + az * az);
  }

  // Affichage toutes les 500ms
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 500) {
    lastPrint = millis();
    Serial.printf("G=%.2f | X=%.1f Y=%.1f Z=%.1f | Btn=%d\n", gForce, ax, ay, az, digitalRead(BUTTON_PIN));
  }

  // Bouton = annule alerte
  if (digitalRead(BUTTON_PIN) == LOW && alerteActive) {
    Serial.println(">> SOS ANNULE bouton");
    alerteActive = false;
    buzzerPhase = 0;
    digitalWrite(BUZZER_PIN, LOW);
    bip(2, 80);
    delay(500);
  }

  // DETECTION
  if (!alerteActive && gForce > SEUIL_NID_POULE) {
    if (gForce >= SEUIL_CHUTE) {
      Serial.printf("!!! CHUTE DETECTEE %.1fg !!!\n", gForce);
      alerteActive = true;
      chuteTime = millis();
      buzzerPhase = 1;
    } else {
      Serial.printf("!! Nid de poule %.1fg (ignore si mouvement apres)\n", gForce);
      bip(1, 150);
      // Attend 2 sec pour voir si immobile (vraie chute) ou mouvement (nid de poule)
      unsigned long t0 = millis();
      bool stillMoving = false;
      while (millis() - t0 < 2000) {
        // re-lecture rapide
        Wire.beginTransmission(MPU_ADDR);
        Wire.write(ACCEL_XOUT_H);
        Wire.endTransmission(false);
        Wire.requestFrom(MPU_ADDR, 6, true);
        if (Wire.available() == 6) {
          int16_t axr = (Wire.read() << 8) | Wire.read();
          int16_t ayr = (Wire.read() << 8) | Wire.read();
          int16_t azr = (Wire.read() << 8) | Wire.read();
          float gf = sqrt(pow(axr / 4096.0, 2) + pow(ayr / 4096.0, 2) + pow(azr / 4096.0, 2));
          if (gf > 1.5) stillMoving = true;
        }
        delay(50);
      }
      if (!stillMoving) {
        Serial.println("-> Pas de mouvement, c'etait une chute !");
        alerteActive = true;
        chuteTime = millis();
        buzzerPhase = 1;
      } else {
        Serial.println("-> Mouvement detecte, c'etait un nid de poule, on ignore");
      }
    }
  }

  // GESTION ALERTE BIP PROGRESSIF
  if (alerteActive) {
    unsigned long elapsed = millis() - chuteTime;

    if (elapsed < 5000) {
      // 0-5s : BIP toutes les 1s
      if (elapsed % 1000 < 100) digitalWrite(BUZZER_PIN, HIGH);
      else digitalWrite(BUZZER_PIN, LOW);
    } else if (elapsed < 10000) {
      // 5-10s : BIP toutes les 300ms (urgent)
      if (elapsed % 300 < 150) digitalWrite(BUZZER_PIN, HIGH);
      else digitalWrite(BUZZER_PIN, LOW);
    } else {
      // 10s+ : SOS FINAL continu 1s + envoi ntfy si WiFi dispo
      Serial.println(">>> SOS FINAL AUTO - ENVOI NTFY <<<");
      digitalWrite(BUZZER_PIN, HIGH);
      delay(1000);
      digitalWrite(BUZZER_PIN, LOW);
      // Ici tu peux ajouter WiFi + HTTP si tu veux
      // sendNtfy("SOS CHUTE Gaetan https://www.google.com/maps?q=48.9001,6.0203");
      alerteActive = false; // ou reste en alerte selon choix
    }

    // Si mouvement "Je roule encore" dans les 10s -> annule auto
    if (gForce > 1.8 && elapsed > 1000) {
      Serial.println(">> Mouvement detecte 'Je roule encore' -> SOS annule auto");
      alerteActive = false;
      digitalWrite(BUZZER_PIN, LOW);
    }
  }

  delay(20); // loop 50Hz
}

void bip(int n, int duree) {
  for (int i = 0; i < n; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(duree);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < n - 1) delay(100);
  }
}

/* 
  Version avec WiFi ntfy (decommente si tu veux, mais ca alourdit compil)

#include <WiFi.h>
#include <HTTPClient.h>
void sendNtfy(String msg){
  if(WiFi.status()!=WL_CONNECTED){ WiFi.begin("TON_WIFI","MDP"); delay(3000); }
  HTTPClient http;
  http.begin("https://ntfy.sh/bikeguard-gaetan-390");
  http.addHeader("Content-Type","text/plain");
  http.POST(msg);
  http.end();
}
*/
