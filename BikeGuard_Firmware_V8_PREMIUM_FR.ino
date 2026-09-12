/*
  BikeGuard V8 PRO PREMIUM FR - Firmware ESP32-C3 GRATUIT
  Version 1.3.0 - Compatible avec App V8 PRO PREMIUM FR 100% française
  
  Toutes fonctions premium débloquées sans payer :
  - Détection chute intelligente avec filtre SUSPECT
  - Anti-fausse alerte nid-de-poule
  - Buzzer SOS 120dB 10min si téléphone cassé
  - LED stroboscope
  - Blackbox mémoire crash + renvoi auto
  - WiFi SOS : scan FreeWiFi/SFR si BLE mort
  - Anti-vol 120dB si mouvement sans BLE
  - OTA BLE 512b + OTA WiFi
  - Deep Sleep + batterie réelle GPIO2
  - Multi-vélos, suivi 10s

  Câblage V8 PRO PREMIUM FR (ESP32-C3) :
  MPU6050 VCC->3.3V, GND->GND, SDA->GPIO4, SCL->GPIO5, INT->GPIO3
  Buzzer 85dB +->GPIO6, -->GND (pour version 120dB mettre transistor 2N2222)
  LED Rouge SOS ->GPIO7->GND (via résistance 220Ω)
  LED Verte STOP ->GPIO10->GND
  Batterie LiPo 3.7V-4.2V -> [100k] -> GPIO2 -> [100k] -> GND
  Bouton SOS ->GPIO8 + GND (INPUT_PULLUP)
  Bouton STOP ->GPIO9 + GND (INPUT_PULLUP)

  Librairies :
  - MPU6050 by Electronic Cats
  - NimBLE-Arduino by h2zero
  - WiFi (ESP32)
  - ArduinoOTA
  - HTTPClient
  - Preferences (NVS)

  Version : 1.3.0
  ID Boîtier par défaut : BG-V8-390
*/

#include <Wire.h>
#include <MPU6050.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <Update.h>

#define SDA_PIN 4
#define SCL_PIN 5
#define MPU_INT_PIN 3
#define BUZZER_PIN 6
#define LED_ROUGE_PIN 7
#define LED_VERTE_PIN 10
#define BAT_ADC_PIN 2
#define BTN_SOS_PIN 8
#define BTN_STOP_PIN 9

// BLE UUIDs - Compatibles V8
#define SERVICE_UUID "0000ff00-0000-1000-8000-00805f9b34fb"
#define ALERT_CHAR_UUID "0000ff01-0000-1000-8000-00805f9b34fb"
#define STATUS_CHAR_UUID "0000ff02-0000-1000-8000-00805f9b34fb"
#define CONFIG_CHAR_UUID "0000ff03-0000-1000-8000-00805f9b34fb"
#define OTA_CHAR_UUID "0000ff04-0000-1000-8000-00805f9b34fb" // Nouveau pour OTA BLE

// Config par défaut - comme sur captures BG-V8-390
float seuilG = 2.8; // G - Thr G 2,8
int seuilAngle = 60; // degrés - A 60°
String nomBoitier = "BG-V8-390";
String versionFirmware = "1.3.0";
String ntfyTopic = "bikeguard-gae-demo"; // Topic ntfy.sh pour WiFi SOS
bool modeAntivol = false;
bool wifiSOSActive = true;

// MPU
MPU6050 mpu;
bool mpuOK = false;
float baseGx=0, baseGy=0, baseGz=0;
float gFiltre = 1.0;

// État
enum Etat { REPOS=0, SUSPECT=1, COMPTEUR=2, ALERTE=3, ANTIVOL=4 };
Etat etatActuel = REPOS;
unsigned long entreeEtat = 0;
unsigned long dernierGrosG = 0;
unsigned long derniereAlerte = 0;
unsigned long debutSansBLE = 0;
int compteRebours = 30; // secondes avant envoi

// Batterie
int batteriePct = 100;
float batterieVolt = 4.0;
unsigned long dernierBat = 0;

// Blackbox
Preferences preferences;
struct CrashData {
  float g;
  float tilt;
  unsigned long timestamp;
  float lat; // 0 si pas de GPS (GPS vient du tel)
  float lng;
};
CrashData dernierCrash;

// BLE
NimBLEServer* pServeur;
NimBLECharacteristic* pAlerteChar;
NimBLECharacteristic* pStatusChar;
NimBLECharacteristic* pConfigChar;
NimBLECharacteristic* pOTAChar;
NimBLECharacteristic* pBatChar;
bool bleConnecte = false;
bool otaEnCours = false;
size_t otaTaille = 0;
size_t otaRecu = 0;

// WiFi SOS - Réseaux ouverts à scanner
const char* wifiSOS_SSID[] = {"FreeWiFi", "FreeWifi_secure", "SFR WiFi FON", "SFR WiFi", "Orange", "Bouygues Telecom Wi-Fi", "Freebox-"};
const int nbWifiSOS = 7;

void bip(int type){
  // 1=bip court, 2=2 bips OK MPU, 3=3 bips BLE connecté, 4=SOS ...---..., 5=compte rebours, 6=antivol
  if(type==1){ digitalWrite(BUZZER_PIN,HIGH); digitalWrite(LED_ROUGE_PIN,HIGH); delay(100); digitalWrite(BUZZER_PIN,LOW); digitalWrite(LED_ROUGE_PIN,LOW); }
  else if(type==2){ for(int i=0;i<2;i++){digitalWrite(BUZZER_PIN,HIGH);digitalWrite(LED_VERTE_PIN,HIGH);delay(120);digitalWrite(BUZZER_PIN,LOW);digitalWrite(LED_VERTE_PIN,LOW);delay(120);} }
  else if(type==3){ for(int i=0;i<3;i++){digitalWrite(BUZZER_PIN,HIGH);delay(100);digitalWrite(BUZZER_PIN,LOW);delay(100);} }
  else if(type==4){ for(int i=0;i<3;i++){digitalWrite(BUZZER_PIN,HIGH);digitalWrite(LED_ROUGE_PIN,HIGH);delay(150);digitalWrite(BUZZER_PIN,LOW);digitalWrite(LED_ROUGE_PIN,LOW);delay(150);} delay(200); for(int i=0;i<3;i++){digitalWrite(BUZZER_PIN,HIGH);digitalWrite(LED_ROUGE_PIN,HIGH);delay(400);digitalWrite(BUZZER_PIN,LOW);digitalWrite(LED_ROUGE_PIN,LOW);delay(150);} delay(200); for(int i=0;i<3;i++){digitalWrite(BUZZER_PIN,HIGH);digitalWrite(LED_ROUGE_PIN,HIGH);delay(150);digitalWrite(BUZZER_PIN,LOW);digitalWrite(LED_ROUGE_PIN,LOW);delay(150);} }
  else if(type==5){ digitalWrite(BUZZER_PIN,HIGH); digitalWrite(LED_ROUGE_PIN,HIGH); delay(500); digitalWrite(BUZZER_PIN,LOW); digitalWrite(LED_ROUGE_PIN,LOW); }
  else if(type==6){ for(int i=0;i<10;i++){digitalWrite(BUZZER_PIN,HIGH);digitalWrite(LED_ROUGE_PIN,HIGH);delay(80);digitalWrite(BUZZER_PIN,LOW);digitalWrite(LED_ROUGE_PIN,LOW);delay(80);} }
}

void lireBatterie(){
  if(millis()-dernierBat < 5000) return;
  dernierBat = millis();
  long somme=0; for(int i=0;i<10;i++){ somme+=analogRead(BAT_ADC_PIN); delay(5); }
  float adc = somme/10.0;
  float vADC = adc * 3.3 / 4095.0;
  batterieVolt = vADC * 2.0; // diviseur 100k/100k
  int pct = map(batterieVolt*1000, 3300, 4200, 0, 100);
  batteriePct = constrain(pct,0,100);
}

void sauverBlackbox(float g, float tilt){
  preferences.begin("bikeguard", false);
  preferences.putFloat("last_g", g);
  preferences.putFloat("last_tilt", tilt);
  preferences.putULong("last_time", millis());
  preferences.putString("box", nomBoitier);
  preferences.end();
  dernierCrash.g = g;
  dernierCrash.tilt = tilt;
  dernierCrash.timestamp = millis();
  Serial.println("Blackbox sauvegardée G="+String(g)+" tilt="+String(tilt));
}

bool envoyerWiFiSOS(float g, float tilt){
  if(!wifiSOSActive) return false;
  Serial.println("WiFi SOS : scan réseaux...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  String ssidTrouve = "";
  for(int i=0;i<n;i++){
    String ssid = WiFi.SSID(i);
    for(int j=0;j<nbWifiSOS;j++){
      if(ssid.indexOf(wifiSOS_SSID[j])>=0 || WiFi.encryptionType(i)==WIFI_AUTH_OPEN){
        ssidTrouve = ssid;
        break;
      }
    }
    if(ssidTrouve!="") break;
  }
  if(ssidTrouve==""){
    Serial.println("WiFi SOS : aucun réseau ouvert trouvé");
    return false;
  }
  Serial.println("WiFi SOS : tentative "+ssidTrouve);
  WiFi.begin(ssidTrouve.c_str());
  unsigned long debut = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-debut<8000) delay(500);
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("WiFi SOS : échec connexion");
    return false;
  }
  // Envoi ntfy.sh
  HTTPClient http;
  String url = "https://ntfy.sh/"+ntfyTopic;
  http.begin(url);
  http.addHeader("Title", "BikeGuard ALERTE WiFi SOS");
  http.addHeader("Tags", "rotating_light,bike");
  String message = "🚨 CHUTE BikeGuard "+nomBoitier+"\nG="+String(g,1)+" Tilt="+String(tilt,0)+"°\nBat="+String(batteriePct)+"%\nBoitier: "+nomBoitier+" v"+versionFirmware+"\nTéléphone cassé ou déconnecté - SOS WiFi auto";
  int code = http.POST(message);
  Serial.println("WiFi SOS ntfy code "+String(code));
  http.end();
  WiFi.disconnect();
  return code>=200 && code<300;
}

class CallbacksServeur: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s){ bleConnecte=true; debutSansBLE=0; bip(3); Serial.println("BLE Connecté"); }
  void onDisconnect(NimBLEServer* s){ bleConnecte=false; debutSansBLE=millis(); NimBLEDevice::startAdvertising(); Serial.println("BLE Déconnecté"); }
};

class CallbacksConfig: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c){
    String v = c->getValue().c_str();
    // Format: G:2.8,A:60,N:BG-V8-390,T:bikeguard-topic
    int gIdx = v.indexOf("G:"); int aIdx = v.indexOf("A:"); int nIdx = v.indexOf("N:"); int tIdx = v.indexOf("T:");
    if(gIdx>=0){ int fin = v.indexOf(',', gIdx); if(fin<0) fin=v.length(); seuilG = v.substring(gIdx+2,fin).toFloat(); }
    if(aIdx>=0){ int fin = v.indexOf(',', aIdx); if(fin<0) fin=v.length(); seuilAngle = v.substring(aIdx+2,fin).toInt(); }
    if(nIdx>=0){ int fin = v.indexOf(',', nIdx); if(fin<0) fin=v.length(); nomBoitier = v.substring(nIdx+2,fin); nomBoitier.trim(); }
    if(tIdx>=0){ int fin = v.indexOf(',', tIdx); if(fin<0) fin=v.length(); ntfyTopic = v.substring(tIdx+2,fin); ntfyTopic.trim(); }
    Serial.println("Config: G="+String(seuilG)+" A="+String(seuilAngle)+" N="+nomBoitier+" T="+ntfyTopic);
    preferences.begin("bikeguard", false);
    preferences.putFloat("seuilG", seuilG);
    preferences.putInt("seuilA", seuilAngle);
    preferences.putString("nom", nomBoitier);
    preferences.putString("ntfy", ntfyTopic);
    preferences.end();
    bip(2);
  }
};

class CallbacksOTA: public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c){
    std::string val = c->getValue();
    if(val=="START"){
      otaEnCours=true; otaRecu=0;
      if(!Update.begin(UPDATE_SIZE_UNKNOWN)){
        Serial.println("OTA début échec");
      } else {
        Serial.println("OTA BLE début");
      }
      return;
    }
    if(val=="END"){
      if(Update.end(true)){
        Serial.println("OTA BLE OK - reboot");
        bip(3); delay(500); ESP.restart();
      } else {
        Serial.println("OTA BLE échec");
      }
      otaEnCours=false;
      return;
    }
    if(otaEnCours){
      if(Update.write((uint8_t*)val.data(), val.length()) != val.length()){
        Serial.println("OTA write échec");
      }
      otaRecu+=val.length();
      // Notif progression
      if(otaRecu%4096==0) Serial.println("OTA "+String(otaRecu)+" octets");
    }
  }
};

void setup(){
  Serial.begin(115200);
  Serial.println("BikeGuard V8 PRO PREMIUM FR v"+versionFirmware+" - BG-V8-390");
  pinMode(BUZZER_PIN,OUTPUT);
  pinMode(LED_ROUGE_PIN,OUTPUT);
  pinMode(LED_VERTE_PIN,OUTPUT);
  pinMode(BTN_SOS_PIN,INPUT_PULLUP);
  pinMode(BTN_STOP_PIN,INPUT_PULLUP);
  pinMode(MPU_INT_PIN,INPUT);
  pinMode(BAT_ADC_PIN,INPUT);
  analogReadResolution(12);
  Wire.begin(SDA_PIN,SCL_PIN);

  // Prefs
  preferences.begin("bikeguard", true);
  seuilG = preferences.getFloat("seuilG", 2.8);
  seuilAngle = preferences.getInt("seuilA", 60);
  nomBoitier = preferences.getString("nom", "BG-V8-390");
  ntfyTopic = preferences.getString("ntfy", "bikeguard-gae-demo");
  float last_g = preferences.getFloat("last_g", 0);
  if(last_g>0){
    Serial.println("Blackbox trouvée G="+String(last_g)+" - renvoi au prochain BLE");
    dernierCrash.g = last_g;
  }
  preferences.end();

  // MPU init
  mpu.initialize();
  mpuOK = mpu.testConnection();
  if(mpuOK){
    long ax=0,ay=0,az=0,gx=0,gy=0,gz=0;
    for(int i=0;i<500;i++){ int16_t aX,aY,aZ,gX,gY,gZ; mpu.getMotion6(&aX,&aY,&aZ,&gX,&gY,&gZ); ax+=aX; ay+=aY; az+=aZ; gx+=gX; gy+=gY; gz+=gZ; delay(10); }
    baseGx=gx/500.0; baseGy=gy/500.0; baseGz=gz/500.0;
    bip(2);
    Serial.println("MPU6050 OK calibré");
  } else {
    digitalWrite(BUZZER_PIN,HIGH); delay(1000); digitalWrite(BUZZER_PIN,LOW);
    Serial.println("MPU6050 FAIL");
  }

  // BLE
  NimBLEDevice::init(nomBoitier.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  pServeur = NimBLEDevice::createServer();
  pServeur->setCallbacks(new CallbacksServeur());
  NimBLEService* pService = pServeur->createService(SERVICE_UUID);
  pAlerteChar = pService->createCharacteristic(ALERT_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pStatusChar = pService->createCharacteristic(STATUS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pConfigChar = pService->createCharacteristic(CONFIG_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pConfigChar->setCallbacks(new CallbacksConfig());
  pOTAChar = pService->createCharacteristic(OTA_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pOTAChar->setCallbacks(new CallbacksOTA());
  NimBLEService* pBatService = pServeur->createService(NimBLEUUID((uint16_t)0x180F));
  pBatChar = pBatService->createCharacteristic(NimBLEUUID((uint16_t)0x2A19), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pService->start();
  pBatService->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setName(nomBoitier.c_str());
  adv->start();
  Serial.println("BLE démarré "+nomBoitier);

  // OTA WiFi si bouton SOS maintenu au boot 3s
  if(digitalRead(BTN_SOS_PIN)==LOW){
    delay(3000);
    if(digitalRead(BTN_SOS_PIN)==LOW){
      WiFi.mode(WIFI_STA);
      WiFi.begin("BikeGuard-OTA","bikeguard123");
      unsigned long deb=millis();
      while(WiFi.status()!=WL_CONNECTED && millis()-deb<5000) delay(500);
      if(WiFi.status()==WL_CONNECTED){
        ArduinoOTA.begin();
        Serial.println("OTA WiFi prêt");
        bip(3);
        // Boucle OTA 30s
        for(int i=0;i<30;i++){ ArduinoOTA.handle(); delay(1000); bip(1); }
      }
    }
  }
  debutSansBLE = millis();
}

void loop(){
  lireBatterie();

  // Boutons
  if(digitalRead(BTN_STOP_PIN)==LOW){
    if(etatActuel!=REPOS){ etatActuel=REPOS; pAlerteChar->setValue((uint8_t)0); pAlerteChar->notify(); bip(1); Serial.println("STOP"); }
    // Désactive antivol si appui long 3s
    delay(3000);
    if(digitalRead(BTN_STOP_PIN)==LOW){ modeAntivol=!modeAntivol; Serial.println("Antivol "+String(modeAntivol)); bip(modeAntivol?2:1); delay(500); }
    delay(300);
  }
  if(digitalRead(BTN_SOS_PIN)==LOW){
    if(millis()-derniereAlerte > 10000){ etatActuel=COMPTEUR; entreeEtat=millis(); derniereAlerte=millis(); Serial.println("SOS Manuel"); }
    delay(300);
  }

  // MPU
  if(mpuOK){
    int16_t ax,ay,az,gx,gy,gz;
    mpu.getMotion6(&ax,&ay,&az,&gx,&gy,&gz);
    float fAx = (ax)/16384.0;
    float fAy = (ay)/16384.0;
    float fAz = (az)/16384.0;
    float gForce = sqrt(fAx*fAx + fAy*fAy + fAz*fAz);
    gFiltre = 0.9*gFiltre + 0.1*gForce;
    float inclinaison = atan2(sqrt(fAx*fAx+fAy*fAy), fAz) * 180 / PI;

    // Anti-vol : mouvement sans BLE
    if(modeAntivol && !bleConnecte && gFiltre>1.5 && etatActuel==REPOS){
      if(millis()-derniereAlerte>15000){
        etatActuel=ANTIVOL;
        entreeEtat=millis();
        derniereAlerte=millis();
        Serial.println("ANTIVOL déclenché");
      }
    }

    // Algorithme V8 PREMIUM
    if(etatActuel==REPOS){
      if(gFiltre > seuilG){
        dernierGrosG = millis();
        etatActuel = SUSPECT;
        entreeEtat = millis();
        Serial.println("SUSPECT G="+String(gFiltre)+" incli="+String(inclinaison));
      }
    } else if(etatActuel==SUSPECT){
      if(millis()-entreeEtat > 1000){
        if(abs(inclinaison) > seuilAngle && gFiltre < 1.5){
          if(millis()-derniereAlerte > 10000){
            etatActuel = COMPTEUR;
            entreeEtat = millis();
            derniereAlerte = millis();
            sauverBlackbox(gFiltre, inclinaison);
            Serial.println("COMPTEUR confirmé");
            bip(5);
          }
        } else {
          etatActuel = REPOS;
          Serial.println("Fausse alerte rejetée");
        }
      }
      if(gFiltre < 1.2 && millis()-dernierGrosG > 500 && abs(inclinaison) < 30){
        etatActuel = REPOS;
      }
    } else if(etatActuel==COMPTEUR){
      if(millis()-entreeEtat > compteRebours*1000){
        etatActuel = ALERTE;
        Serial.println("ALERTE !");
        bip(4);
        // Si BLE déconnecté, lance WiFi SOS après 30s
        if(!bleConnecte) debutSansBLE = millis();
      }
      if((millis()/1000)%2==0){ digitalWrite(BUZZER_PIN,HIGH); digitalWrite(LED_ROUGE_PIN,HIGH); } else { digitalWrite(BUZZER_PIN,LOW); digitalWrite(LED_ROUGE_PIN,LOW); }
    } else if(etatActuel==ALERTE){
      // Buzzer SOS 10min + LED strobe
      static unsigned long lastBip=0;
      static bool wifiTente=false;
      if(millis()-lastBip>2500){ bip(4); lastBip=millis(); }
      digitalWrite(LED_ROUGE_PIN, (millis()/200)%2);

      // Si BLE déconnecté >30s, tente WiFi SOS
      if(!bleConnecte && !wifiTente && millis()-debutSansBLE>30000){
        wifiTente = envoyerWiFiSOS(gFiltre, inclinaison);
        Serial.println("WiFi SOS tenté");
      }
      // Après 10min, repasse REPOS mais garde blackbox
      if(millis()-entreeEtat > 600000){ // 10min
        etatActuel=REPOS;
        digitalWrite(LED_ROUGE_PIN,LOW);
        wifiTente=false;
      }
    } else if(etatActuel==ANTIVOL){
      if(millis()-entreeEtat < 30000){ // 30s d'alarme
        if((millis()/150)%2==0){ digitalWrite(BUZZER_PIN,HIGH); digitalWrite(LED_ROUGE_PIN,HIGH); } else { digitalWrite(BUZZER_PIN,LOW); digitalWrite(LED_ROUGE_PIN,LOW); }
      } else {
        etatActuel=REPOS;
        digitalWrite(BUZZER_PIN,LOW); digitalWrite(LED_ROUGE_PIN,LOW);
      }
    }

    // Envoi BLE STATUS 200ms
    static unsigned long lastStatus=0;
    if(millis()-lastStatus>200){
      lastStatus=millis();
      String json = "{\"s\":"+String((int)etatActuel)+",\"g\":"+String(gFiltre,2)+",\"ax\":"+String(fAx,2)+",\"ay\":"+String(fAy,2)+",\"az\":"+String(fAz,2)+",\"tilt\":"+String(inclinaison,0)+",\"thrG\":"+String(seuilG,1)+",\"thrA\":"+String(seuilAngle)+",\"bat\":"+String(batteriePct)+",\"v\":"+String(batterieVolt,2)+",\"box\":\""+nomBoitier+"\",\"ver\":\""+versionFirmware+"\",\"antivol\":"+String(modeAntivol)+",\"wifi\":"+String(wifiSOSActive)+"}";
      pStatusChar->setValue(json.c_str());
      if(bleConnecte) pStatusChar->notify();
      pAlerteChar->setValue((uint8_t)etatActuel);
      if(bleConnecte) pAlerteChar->notify();
      pBatChar->setValue((uint8_t)batteriePct);
      if(bleConnecte) pBatChar->notify();
      // Si blackbox en attente et BLE connecté, envoie
      if(dernierCrash.g>0 && bleConnecte){
        String crashJson = "{\"blackbox\":true,\"g\":"+String(dernierCrash.g)+",\"tilt\":"+String(dernierCrash.tilt)+"}";
        // Envoi via alerte char avec flag
        pAlerteChar->setValue(crashJson.c_str());
        pAlerteChar->notify();
        preferences.begin("bikeguard", false);
        preferences.remove("last_g");
        preferences.end();
        dernierCrash.g=0;
      }
    }

    // Deep Sleep léger si REPOS et pas de mouvement et pas BLE depuis 30s
    if(!bleConnecte && etatActuel==REPOS && millis()>30000 && !modeAntivol){
      if(gFiltre > 0.9 && gFiltre < 1.1 && abs(inclinaison)<20){
        esp_sleep_enable_gpio_wakeup();
        gpio_wakeup_enable((gpio_num_t)BTN_SOS_PIN, GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable((gpio_num_t)BTN_STOP_PIN, GPIO_INTR_LOW_LEVEL);
        esp_light_sleep_start();
      }
    }
  }
  delay(20);
}
