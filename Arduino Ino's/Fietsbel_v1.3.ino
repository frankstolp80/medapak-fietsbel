#include "Arduino.h"
#include "SPIFFS.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h> 
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include "esp_coexist.h"
#include "driver/rtc_io.h"

// Lichtgewicht MP3 bibliotheek (geen PSRAM nodig, geen 720KB OOM crash!)
#include "AudioFileSourceSPIFFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// Bluetooth A2DP Sink
#include "ESP_I2S.h"
#include "BluetoothA2DPSink.h"

#define I2S_DOUT      22
#define I2S_BCLK      26
#define I2S_LRC       25

const int KNOP_PIN = 13;         // Originele belknop (werkt altijd direct, ook uit Deep Sleep)
const int SLAAP_KNOP_PIN = 14;   // 5 sec = Aan | 1x klik = Play/Pauze | 2x klik = Wi-Fi 10 min aan | 5 sec = Deep Sleep
const int VOL_UP_PIN = 27;       // Bluetooth Volume Omhoog
const int VOL_DOWN_PIN = 33;     // Bluetooth Volume Omlaag
const int AMP_SD_PIN = 18;       // Versterker Shutdown (HIGH = aan, LOW = uit)

const int BATTERIJ_PIN = 34;     // Analoge pin voor spanningsmeting (input-only)

// TP4056 / TP4057 Status pinnen
const int TP4057_CHRG_PIN = 35;  // LOW = Aan het laden (input-only -> INPUT)
const int TP4057_STDBY_PIN = 32; // LOW = Volledig opgeladen aan de lader

// Bitmasker voor Pin 14 (ext1 wakeup)
#define WAKEUP_PIN14_MASK (1ULL << 14)

// WiFi Bewaren
WiFiMulti wifiMulti;
const int MAX_WIFI_NETWERKEN = 50;
String gescandeNetwerkenHTML = "";

// Hotspot-Wifi 
const char* ssid = "Digitale fietsbel Instellingen";
const char* password = "12345678";

// URL's op jouw server (GitHub Raw links)
const String HUIDIGE_VERSIE   = "1.2";
const char* UPDATES_LIJST_URL = "https://raw.githubusercontent.com/frankstolp80/medapak-fietsbel/refs/heads/main/updates.txt";
const String GITHUB_BASE_URL  = "https://raw.githubusercontent.com/frankstolp80/medapak-fietsbel/refs/heads/main/";
String beschikbareUpdatesHTML = "";
String nieuweVersieLabel = "";
bool opstartUpdateCheckGedaan = false;

WebServer server(80);
I2SClass i2s;
BluetoothA2DPSink a2dp_sink(i2s);

const int MAX_GELUIDEN = 5;
String geluiden[MAX_GELUIDEN] = {"/Fietsbel.mp3", "/Politie.mp3", "/Brandweer.mp3", "/Ambulance.mp3", "/EigenGeluid.mp3"};
int actueelAantalGeluiden = 4;

// Bewaar gekozen geluid en volume ook tijdens Deep Sleep
Preferences preferences;
int gekozenGeluidIndex = 0;
int volumeInstelling = 18; // Schaal 0 t/m 21

String origineleBestandsnaam = "";

bool belActief = false;
bool draadloosActief = false;
bool wifiActief = false;

// Timers voor energiebesparende Wi-Fi hotspot (10 minuten)
unsigned long wifiLaatsteActiviteit = 0;
const unsigned long WIFI_TIMEOUT_MS = 600000; // 10 minuten = 600.000 ms

// Variabelen voor slaapknop, dubbelklik & Bluetooth bediening
unsigned long slaapKnopTijd = 0;
bool slaapKnopInGedrukt = false;
int pin14KlikTeller = 0;
unsigned long pin14EersteKlikTijd = 0;
const unsigned long DUBBELKLIK_TIJD_MS = 500;

bool bluetoothSpeeltAf = true;
int btVolume = 80; // Schaal 0 t/m 127
unsigned long laatsteVolDruk = 0;

// Variabelen voor lage batterij waarschuwing
unsigned long laatsteAccuCheck = 0;
unsigned long laatsteLowBatMelding = 0;
const unsigned long INTERVAL_10_PROCENT = 300000; // 5 minuten bij 6% - 10%
const unsigned long INTERVAL_5_PROCENT  = 60000;  // 1 minuut bij 2% - 5%

File uploadBestand;
String actieveLogoExtensie = "";

void gaInDeepSleep() {
  Serial.println("[Systeem] Ga in Deep Sleep (incl. 1-minuut accu-timer).");
  digitalWrite(AMP_SD_PIN, LOW);
  rtc_gpio_pullup_en((gpio_num_t)KNOP_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)KNOP_PIN);
  rtc_gpio_pullup_en((gpio_num_t)SLAAP_KNOP_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)SLAAP_KNOP_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)KNOP_PIN, 0);
  esp_sleep_enable_ext1_wakeup(WAKEUP_PIN14_MASK, ESP_EXT1_WAKEUP_ALL_LOW);
  esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
  esp_deep_sleep_start();
  
}

// Variabelen voor de subtiele digitale Bass Boost op Bluetooth-audio
float bassBoostVersterking = 0.45; // 0.0 = uit, 0.45 = warme bass, 0.70 = extra zware bass
float lpLinks = 0.0;
float lpRechts = 0.0;

void audio_data_callback(const uint8_t *data, uint32_t len) {
  int16_t *samples = (int16_t *)data;
  uint32_t aantalSamples = len / 2; // 16-bit audio = 2 bytes per sample

  for (uint32_t i = 0; i < aantalSamples; i += 2) {
    // 1. Linkerkanaal (lage tonen rond 120 Hz uitfilteren en bijmengen)
    float inL = (float)samples[i];
    lpLinks += 0.018f * (inL - lpLinks);
    float uitL = inL + (lpLinks * bassBoostVersterking);

    // Begrens op 16-bit maximum zodat de JBL-speaker niet gaat kraken (anti-clipping)
    if (uitL > 32767.0f) uitL = 32767.0f;
    else if (uitL < -32768.0f) uitL = -32768.0f;
    samples[i] = (int16_t)uitL;

    // 2. Rechterkanaal
    if (i + 1 < aantalSamples) {
      float inR = (float)samples[i + 1];
      lpRechts += 0.018f * (inR - lpRechts);
      float uitR = inR + (lpRechts * bassBoostVersterking);

      if (uitR > 32767.0f) uitR = 32767.0f;
      else if (uitR < -32768.0f) uitR = -32768.0f;
      samples[i + 1] = (int16_t)uitR;
    }
  }
}

void speelMP3EnRuimOp(const char* pad, bool herhaalEnStopBijLoslaten = false, unsigned long minSpeelTijdMs = 0) {
  if (!SPIFFS.exists(pad)) {
    Serial.print("[Info] Bestand niet gevonden: ");
    Serial.println(pad);
    return;
  }

  digitalWrite(AMP_SD_PIN, HIGH);

  if (draadloosActief) {
    a2dp_sink.pause();
    delay(60);
    i2s.end();
    delay(60); // Geef ESP32 driver tijd om oude DMA-buffers volledig vrij te geven
  }

  AudioFileSourceSPIFFS *file = new AudioFileSourceSPIFFS(pad);
  // Gebruik I2S poort 0 met compacte DMA-buffers (8) zodat hij nooit OOM crasht naast BT + Wi-Fi
  AudioOutputI2S *out = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S, 8, AudioOutputI2S::APLL_DISABLE);
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  float gain = (volumeInstelling / 21.0) * 1.0;
  out->SetGain(gain);

  AudioGeneratorMP3 *mp3 = new AudioGeneratorMP3();
  unsigned long startAfspelen = millis();

  do {
    if (mp3->begin(file, out)) {
      while (mp3->isRunning()) {
        if (herhaalEnStopBijLoslaten && (millis() - startAfspelen >= minSpeelTijdMs) && digitalRead(KNOP_PIN) == HIGH) {
          Serial.println("[Systeem] Belknop losgelaten: geluid gestopt.");
          mp3->stop();
          break;
        }

        if (!mp3->loop()) {
          mp3->stop();
        }
        if (wifiActief) {
          server.handleClient();
        }
      }
    }

    if (herhaalEnStopBijLoslaten && digitalRead(KNOP_PIN) == LOW) {
      file->seek(0, SEEK_SET);
    }

  } while (herhaalEnStopBijLoslaten && digitalRead(KNOP_PIN) == LOW);

  delete mp3;
  delete out;
  delete file;
  delay(40);

  if (draadloosActief) {
    i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
    i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  } else {
    digitalWrite(AMP_SD_PIN, LOW);
  }
}

void kritiekeAccuUitschakeling() {
  Serial.println("[KRITIEK] Accu onder 2%! Speel uitschakelgeluid, schakel timer en Pin 14 uit (Pin 13 blijft stand-by)...");
  
  if (SPIFFS.exists("/shutdown.mp3")) {
    Serial.println("[Systeem] Afsluit geluid afspelen...");
    speelMP3EnRuimOp("/shutdown.mp3", false);
  } else {
    Serial.println("[Systeem] lage batterij geluid afspelen...");
    speelMP3EnRuimOp("/lowbat.mp3", false);
  }

  digitalWrite(AMP_SD_PIN, LOW);

  rtc_gpio_pullup_en((gpio_num_t)KNOP_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)KNOP_PIN);
  rtc_gpio_deinit((gpio_num_t)SLAAP_KNOP_PIN);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)KNOP_PIN, 0);
  
  esp_deep_sleep_start();
}

bool isAanHetLaden() {
  return (digitalRead(TP4057_CHRG_PIN) == LOW);
}

bool isVolledigOpgeladen() {
  return (digitalRead(TP4057_STDBY_PIN) == LOW);
}

int meetAccuPercentage() {
  int analogewaarde = analogRead(BATTERIJ_PIN);
  float spanning = (analogewaarde / 4095.0) * 3.3 * 2.0;
  int percentage = map(spanning * 100, 340 * 100, 420 * 100, 0, 100);
  if (percentage > 100) percentage = 100;
  if (percentage < 0) percentage = 0;
  //return percentage; // <-- Zet dit weer aan zodra de echte accu aangesloten is!
  return 100;
}

void controleerBestanden() {
  if (SPIFFS.exists("/EigenGeluid.mp3")) {
    actueelAantalGeluiden = 5;
    if (SPIFFS.exists("/naam.txt")) {
      File f = SPIFFS.open("/naam.txt", FILE_READ);
      origineleBestandsnaam = f.readString();
      f.close();
    } else {
      origineleBestandsnaam = "EigenGeluid.mp3";
    }
  } else {
    actueelAantalGeluiden = 4;
    if (gekozenGeluidIndex >= 4) gekozenGeluidIndex = 0;
  }

  if (SPIFFS.exists("/logo.png")) actieveLogoExtensie = "png";
  else if (SPIFFS.exists("/logo.jpg")) actieveLogoExtensie = "jpg";
  else actieveLogoExtensie = "default";
}

void laadOpgeslagenWifiNetwerken() {
  preferences.begin("wifi_lijst", true);
  int aantal = preferences.getInt("aantal", 0);
  for (int i = 0; i < aantal; i++) {
    String s = preferences.getString(("ssid_" + String(i)).c_str(), "");
    String p = preferences.getString(("pass_" + String(i)).c_str(), "");
    if (s != "") {
      if (p == "") {
        wifiMulti.addAP(s.c_str());
      } else {
        wifiMulti.addAP(s.c_str(), p.c_str());
      }
    }
  }
  preferences.end();
}

void voegWifiNetwerkToe(String nieuwSSID, String nieuwPass) {
  preferences.begin("wifi_lijst", false);
  int aantal = preferences.getInt("aantal", 0);

  for (int i = 0; i < aantal; i++) {
    if (preferences.getString(("ssid_" + String(i)).c_str(), "") == nieuwSSID) {
      preferences.putString(("pass_" + String(i)).c_str(), nieuwPass);
      preferences.end();
      laadOpgeslagenWifiNetwerken();
      return;
    }
  }

  int index = (aantal < MAX_WIFI_NETWERKEN) ? aantal : 0;
  preferences.putString(("ssid_" + String(index)).c_str(), nieuwSSID);
  preferences.putString(("pass_" + String(index)).c_str(), nieuwPass);
  if (aantal < MAX_WIFI_NETWERKEN) {
    preferences.putInt("aantal", aantal + 1);
  }
  preferences.end();

  laadOpgeslagenWifiNetwerken();
}

void verwijderWifiNetwerk(int verwijderIndex) {
  preferences.begin("wifi_lijst", false);
  int aantal = preferences.getInt("aantal", 0);
  if (verwijderIndex < 0 || verwijderIndex >= aantal) {
    preferences.end();
    return;
  }

  for (int i = verwijderIndex; i < aantal - 1; i++) {
    String volgendeSSID = preferences.getString(("ssid_" + String(i + 1)).c_str(), "");
    String volgendePass = preferences.getString(("pass_" + String(i + 1)).c_str(), "");
    preferences.putString(("ssid_" + String(i)).c_str(), volgendeSSID);
    preferences.putString(("pass_" + String(i)).c_str(), volgendePass);
  }

  preferences.remove(("ssid_" + String(aantal - 1)).c_str());
  preferences.remove(("pass_" + String(aantal - 1)).c_str());
  preferences.putInt("aantal", aantal - 1);
  preferences.end();
}

void startWiFiHotspot() {
  if (!wifiActief) {
    Serial.println("[Wi-Fi] Hotspot inschakelen voor 10 minuten...");
    WiFi.disconnect(true);
    delay(100);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password, 1);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setSleep(WIFI_PS_NONE);
esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
server.begin();
wifiActief = true;
Serial.print("[Wi-Fi] Hotspot actief! IP: ");
Serial.println(WiFi.softAPIP());
}
wifiLaatsteActiviteit = millis();
}

void stopWiFiHotspot() {
  if (wifiActief) {
    Serial.println("[Wi-Fi] 10 minuten voorbij: Hotspot uitschakelen voor accubesparing.");
    server.stop();
    WiFi.enableAP(false);
    WiFi.enableSTA(false);
    WiFi.setSleep(WIFI_PS_MAX_MODEM);
    wifiActief = false;
  }
}

void haalUpdatesLijstOp() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (http.begin(client, UPDATES_LIJST_URL)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      payload.trim();
      nieuweVersieLabel = "";
      
      String lijstHTML = "<form method='GET' action='/start-server-update'>";
      lijstHTML += "<select name='file' style='width:100%;padding:12px;margin:10px 0;border-radius:8px;border:1px solid #ccc;font-size:15px'>";
      
      int startIdx = 0;
      bool eersteRegel = true;
      while (startIdx < payload.length()) {
        int eindIdx = payload.indexOf('\n', startIdx);
        if (eindIdx == -1) eindIdx = payload.length();
        
        String regel = payload.substring(startIdx, eindIdx);
        regel.trim();
        
        int scheiding = regel.indexOf('|');
        if (scheiding != -1) {
          String label   = regel.substring(0, scheiding);
          String bestand = regel.substring(scheiding + 1);
          lijstHTML += "<option value='" + bestand + "'>" + label + "</option>";

          if (eersteRegel) {
            eersteRegel = false;
            if (!label.startsWith("v" + HUIDIGE_VERSIE) && !label.startsWith(HUIDIGE_VERSIE)) {
              nieuweVersieLabel = label;
            }
          }
        }
        startIdx = eindIdx + 1;
      }
      
      lijstHTML += "</select>";
      lijstHTML += "<input type='submit' value='✅ Gekozen Versie Installeren' style='background:#27AE60;color:#fff;font-weight:bold;padding:14px;border-radius:25px'>";
      lijstHTML += "</form>";
      
      beschikbareUpdatesHTML = lijstHTML;
    }
    http.end();
  }
}

void handleRoot() {
  Serial.println("[Web] Safari vraagt hoofdpagina op...");
  wifiLaatsteActiviteit = millis();
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

  int accu = meetAccuPercentage(); 
  bool aanHetLaden = isAanHetLaden();
  bool volGelaaden = isVolledigOpgeladen();
  bool isVol = (accu >= 100) || volGelaaden;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");

  // Deel 1: CSS & Web-App instellingen voor iPhone Beginscherm
  server.sendContent_P(PSTR(
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no'>"
    "<meta name='apple-mobile-web-app-capable' content='yes'>"
    "<meta name='apple-mobile-web-app-status-bar-style' content='default'>"
    "<meta name='apple-mobile-web-app-title' content='Fietsbel'>"
    "<link rel='apple-touch-icon' href='/logo.png'>"
    "<meta charset='UTF-8'><title>Fietsbel Menu</title>"
    "<script>"
    "function toggleSettings(){"
      "var m=document.getElementById('geavanceerdMenu');"
      "if(!m)return;"
      "if(m.style.display==='block'){"
        "m.style.display='none';"
        "document.body.style.overflow='auto';"
        "var w=document.getElementById('wifiLijst');if(w)w.innerHTML='';"
        "var u=document.getElementById('updateBox');if(u)u.innerHTML='';"
      "}else{"
        "m.style.display='block';"
        "document.body.style.overflow='hidden';"
        "m.scrollTop=0;"
      "}"
    "}"
    "function openUpdateMenu(){"
      "var m=document.getElementById('geavanceerdMenu');"
      "if(!m)return;"
      "m.style.display='block';document.body.style.overflow='hidden';m.scrollTop=0;checkUpdates();"
    "}"
    "</script><style>"
    "body{font-family:Arial,sans-serif;text-align:center;background:#f4f4f9;padding:20px;padding-top:max(20px,env(safe-area-inset-top))}"
    ".btn{display:block;width:85%;max-width:300px;margin:12px auto;padding:15px;font-size:18px;color:#fff;background:#000;border:2px solid #373737;border-radius:25px;text-decoration:none;font-weight:bold}"
    ".btn-actief{background:#ef171c;border-color:#d02832}"
    ".box{background:#fff;padding:20px;border-radius:15px;max-width:300px;margin:20px auto;box-shadow:0 4px 6px rgba(0,0,0,0.05)}"
    ".btn-delete{background:#E74C3C;color:#fff;padding:10px 15px;border-radius:20px;text-decoration:none;display:inline-block;font-size:14px;font-weight:bold;margin-top:10px}"
    "input[type='file'],input[type='text'],input[type='password'],input[type='submit']{width:100%;margin:8px 0;padding:10px;box-sizing:border-box;border-radius:5px;border:1px solid #ccc}"
    "input[type='submit']{background:#E67E22;color:#fff;font-weight:bold;border:none;cursor:pointer}"
    "input[type='range']{width:85%;margin:15px 0}h1{color:#333;margin-bottom:5px;font-size:28px}h3{margin-top:0;color:#000;text-align:left}"
    ".mini-logo{width:160px;height:50px;object-fit:cover;border-radius:6px;margin-bottom:10px}"
    "@keyframes knipperen{0%,100%{opacity:1}50%{opacity:0.2}}@keyframes opladen{0%,100%{opacity:0.3}50%{opacity:1}}.knipper-blok{animation:knipperen 1s infinite}"
  ));

  if (aanHetLaden && !isVol) {
    server.sendContent_P(PSTR(".laad-blok-1{animation:opladen 1.5s infinite 0s}.laad-blok-2{animation:opladen 1.5s infinite .3s}.laad-blok-3{animation:opladen 1.5s infinite .6s}.laad-blok-4{animation:opladen 1.5s infinite .9s}"));
  } else {
    server.sendContent_P(PSTR(".laad-blok-1,.laad-blok-2,.laad-blok-3,.laad-blok-4{opacity:1}"));
  }
  server.sendContent_P(PSTR(
    "#splash{position:fixed;inset:0;background:#fff;color:#222;display:flex;flex-direction:column;align-items:center;justify-content:center;z-index:99999;transition:opacity 0.5s ease}"
    ".spinner{width:36px;height:36px;border:4px solid rgba(0,0,0,0.1);border-top-color:#ef171c;border-radius:50%;animation:draai 0.8s linear infinite;margin-top:20px}"
    "@keyframes draai{to{transform:rotate(360deg)}}"
    "</style></head><body>"
  ));

  // Opstartscherm met jouw eigen logo (witte achtergrond, 5 sec bij eerste keer openen)
  String splash = "<div id='splash'>";
  if (actieveLogoExtensie != "default" && actieveLogoExtensie != "") {
    splash += "<img id='splashLogo' data-src='/logo.jpg' style='width:200px;max-width:75%;border-radius:12px;margin-bottom:15px;display:none' alt='Logo'>";
  } else {
    splash += "<div style='font-size:64px;margin-bottom:10px'>🚲</div>";
  }
  splash += "<h2 style='margin:0;font-size:24px;letter-spacing:1px;color:#111'>Digitale fietsbel</h2>"
            "<p style='color:#666;font-size:13px;margin:6px 0 0 0'>Versie " + HUIDIGE_VERSIE + "</p>"
            "<div class='spinner'></div></div>"
            "<script>"
            "(function(){"
              "var s=document.getElementById('splash');"
              "var eersteKeer=!sessionStorage.getItem('splashGezien');"
              "if(!eersteKeer && s){s.style.display='none';}"
              "window.addEventListener('load',function(){"
                "sessionStorage.setItem('splashGezien','1');"
                "var sImg=document.getElementById('splashLogo');"
                "if(sImg && sImg.dataset.src){sImg.src=sImg.dataset.src;sImg.style.display='block';}"
                "var hImg=document.getElementById('headerLogo');"
                "if(hImg && hImg.dataset.src)hImg.src=hImg.dataset.src;"
                "if(eersteKeer && s){setTimeout(function(){s.style.opacity='0';setTimeout(function(){s.style.display='none';},500);},10000);}"
              "});"
              "setTimeout(function(){if(!window.paginaCompleet){location.reload();}},4000);"
            "})();"
            "</script>";
  server.sendContent(splash);

  // Deel 2: Kop & Batterij-icoon
  String kop = "<div class='box' style='padding:15px;margin-bottom:15px'><div style='display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid #000;padding-bottom:8px'>";
  if (actieveLogoExtensie != "default" && actieveLogoExtensie != "") {
    kop += "<img id='headerLogo' data-src='/logo.jpg' class='mini-logo' alt='Logo'>";
  } else {
    kop += "<span style='font-size:24px'>🚲</span>";
  }
  String stip = (nieuweVersieLabel != "") ? "<span style='font-size:10px;vertical-align:top'>🔴</span>" : "";
  kop += "<div style='display:flex;align-items:center;gap:14px'>";
  
  // 1. Eerst het batterij-icoon (links)
  kop += "<div style='border:2px solid #333;width:30px;height:14px;padding:2px;border-radius:6px;display:flex;gap:2px;background:#fff;position:relative'>";
  
  if (aanHetLaden && !isVol) {
    for (int i = 1; i <= 4; i++) kop += "<div class='laad-blok-" + String(i) + "' style='background:#2ECC71;width:13px;height:100%;border-radius:2px'></div>";
  } else if (isVol) {
    for (int i = 0; i < 4; i++) kop += "<div style='background:#2ECC71;width:13px;height:100%;border-radius:2px'></div>";
  } else {
    String kleur = "#2ECC71"; int blokken = 0; bool knipper = false;
    if (accu >= 75) { blokken = 4; }
    else if (accu >= 50) { blokken = 3; kleur = "#3498DB"; }
    else if (accu >= 25) { blokken = 2; kleur = "#E67E22"; }
    else if (accu >= 10) { blokken = 1; kleur = "#E74C3C"; knipper = true; }
    else { blokken = 0; kleur = "#95A5A6"; }

    for (int i = 0; i < 4; i++) {
      if (i < blokken) {
        kop += "<div" + String((knipper && i == 0) ? " class='knipper-blok'" : "") + " style='background:" + kleur + ";width:13px;height:100%;border-radius:2px'></div>";
      } else {
        kop += "<div style='background:#EAEDED;width:13px;height:100%;border-radius:2px'></div>";
      }
    }
  }
  kop += "<div style='position:absolute;right:-6px;top:3px;width:4px;height:10px;background:#333;border-radius:0 2px 2px 0'></div></div>";

  // 2. Daarna pas het tandwiel-icoon (rechts van de batterij, met iets extra ruimte voor het batterij-puntje)
  kop += "<button type='button' onclick='toggleSettings()' title='Wi-Fi & Updates' style='background:none;border:none;font-size:26px;padding:8px 10px;margin:-8px -6px;cursor:pointer;line-height:1;touch-action:manipulation;-webkit-tap-highlight-color:transparent'>⚙️" + stip + "</button>";
  
  kop += "</div></div><h1>Fietsbel Instellingen</h1></div>";
  server.sendContent(kop);

  // Deel 3: Groene Update-Banner bovenaan
  if (nieuweVersieLabel != "") {
    String banner = "<div id='updateBanner' style='background:#27AE60;color:#fff;padding:14px;border-radius:15px;max-width:300px;margin:15px auto;box-shadow:0 4px 10px rgba(39,174,96,0.3);text-align:center'>"
                    "<div style='font-weight:bold;font-size:16px'>🎉 Nieuwe Update Beschikbaar!</div>"
                    "<div style='font-size:13px;margin:6px 0 10px 0'>" + nieuweVersieLabel + "</div>"
                    "<button type='button' onclick='openUpdateMenu()' style='background:#fff;color:#27AE60;border:none;padding:8px 16px;border-radius:20px;font-weight:bold;cursor:pointer'>Bekijk &amp; Installeer ⬇️</button>"
                    "</div>";
    server.sendContent(banner);
  }

  // Deel 4: Geluiden & Volume
  String gel = "<div class='box'><h3>🔊 Fietsbel geluid:</h3>";
  for (int i = 0; i < actueelAantalGeluiden; i++) {
    String cls = (i == gekozenGeluidIndex) ? "geluid-btn btn btn-actief" : "geluid-btn btn";
    String naam = (i == 4) ? "⭐ " + origineleBestandsnaam : geluiden[i].substring(1, geluiden[i].length() - 4);
    gel += "<a href='#' onclick=\"kiesGeluid(" + String(i) + ",this);return false;\" class='" + cls + "'>" + naam + "</a>";
  }
  gel += "<script>function kiesGeluid(id,el){document.querySelectorAll('.geluid-btn').forEach(b=>b.classList.remove('btn-actief'));el.classList.add('btn-actief');fetch('/kies?id='+id,{cache:'no-store'});}</script></div>";

  int pct = map(volumeInstelling, 0, 21, 0, 100);
  gel += "<div class='box'><h3>🔊 Fietsbel Volume: <span id='volVal'>" + String(pct) + "</span>%</h3>"
         "<input type='range' min='0' max='100' value='" + String(pct) + "' onchange='updateVolume(this.value)'>"
         "<script>function updateVolume(v){document.getElementById('volVal').innerText=v;fetch('/volume?val='+v);}</script></div>";
  server.sendContent(gel);

  // Deel 5 & 6: Volledig schermvullend Geavanceerd Menu (Eigen MP3 + OTA + Wi-Fi)
  server.sendContent_P(PSTR(
    "<div id='geavanceerdMenu' style='display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(244,244,249,0.98);z-index:9999;overflow-y:auto;padding:20px;box-sizing:border-box'>"
    "<div style='max-width:340px;margin:0 auto;display:flex;justify-content:space-between;align-items:center;padding-bottom:10px'>"
    "<h2 style='margin:0;color:#333;font-size:22px'>⚙️ Instellingen</h2>"
    "<button type='button' onclick='toggleSettings()' style='background:#E74C3C;color:#fff;border:none;border-radius:50%;width:38px;height:38px;font-size:20px;font-weight:bold;cursor:pointer;line-height:1'>✕</button>"
    "</div>"
    "<div class='box'><h3>🎵 Eigen Fietsbel uploaden</h3><form method='POST' action='/upload-mp3' enctype='multipart/form-data'>"
    "<p style='font-size:13px;color:#888'>Selecteer een korte <b>.mp3</b> om te uploaden.</p>"
    "<input type='file' name='update' accept='.mp3' required><input type='submit' value='Geluid Overschrijven'></form>"
  ));
  if (actueelAantalGeluiden == 5) {
    server.sendContent_P(PSTR("<hr style='border:0;border-top:1px solid #eee;margin:20px 0'><a href='/verwijder' class='btn-delete' onclick=\"return confirm('Weet je zeker dat je jouw eigen geluid wilt wissen?');\">❌ Eigen geluid wissen</a>"));
  }
  server.sendContent_P(PSTR("</div>"));

  // Gecombineerd Firmware Update blok
  String upd = "<div class='box' style='background:#FEF9E7;border:2px solid #F1C40F'>"
               "<h3 style='color:#B7950B;text-align:center'>🔄 Firmware Updates</h3>"
               "<p style='font-size:13px;color:#555'>Huidige versie: <b>v" + HUIDIGE_VERSIE + "</b></p>"
               "<div id='updateBox'></div>"
               "<button type='button' onclick='checkUpdates()' id='updBtn' class='btn' style='background:#2980B9;font-size:15px;padding:10px;cursor:pointer'>🔍 Controleer op Updates</button>"
               "<script>function checkUpdates(){let b=document.getElementById('updBtn');b.innerText='⏳ Verbinden & zoeken...';fetch('/get-updates').then(r=>r.text()).then(h=>{document.getElementById('updateBox').innerHTML=h;b.innerText='🔍 Controleer op Updates';});}</script>"
               "<hr style='border:0;border-top:1px solid #F9E79F;margin:18px 0'>"
               "<form method='POST' action='/update-fw' enctype='multipart/form-data'>"
               "<p style='font-size:13px;color:#666;margin-bottom:5px'>Of kies handmatig een lokaal <b>.bin</b> bestand:</p>"
               "<input type='file' name='firmware' accept='.bin' required>"
               "<input type='submit' value='Lokaal Bestand Updaten' style='background:#34495E'>"
               "</form></div>";
  server.sendContent(upd);

  // Wi-Fi Manager
  String wifiHtml = "<div class='box'><h3>📶 Wi-Fi voor Updates</h3>";
  if (WiFi.status() == WL_CONNECTED) {
    wifiHtml += "<p style='color:#27AE60;font-weight:bold;font-size:14px'>✅ Verbonden met: " + WiFi.SSID() + "</p>";
  } else {
    wifiHtml += "<p style='color:#E67E22;font-size:13px'>⚠️ Niet verbonden met internet</p>";
  }
  wifiHtml += "<a href='#' onclick='scanWifi();return false;' id='scanBtn' class='btn' style='background:#34495E;font-size:15px;padding:12px'>🔍 Zoek Wi-Fi Netwerken</a>"
              "<div id='wifiLijst'></div>"
              "<script>function scanWifi(){let b=document.getElementById('scanBtn');b.innerText='⏳ Bezig met zoeken...';fetch('/scan-wifi').then(r=>r.text()).then(h=>{document.getElementById('wifiLijst').innerHTML=h;b.innerText='🔍 Zoek Wi-Fi Netwerken';}).catch(()=>{b.innerText='⚠️ Probeer opnieuw';});}</script>"
              "<form method='POST' action='/save-wifi' style='margin-top:10px'>"
              "<input type='text' id='wifi_ssid' name='ssid' placeholder='Netwerknaam (SSID)' required>"
              "<input type='password' name='pass' placeholder='Wachtwoord (leeg = openbaar)'>"
              "<input type='submit' value='Opslaan & Verbinden' style='background:#27AE60'>"
              "</form>";
  server.sendContent(wifiHtml);

  // Deel 7: Opgeslagen Wi-Fi lijst & Afsluiten
  preferences.begin("wifi_lijst", true);
  int opgeslagenAantal = preferences.getInt("aantal", 0);
  if (opgeslagenAantal > 0) {
    String lijst = "<div style='text-align:left;margin-top:15px;font-size:13px'><b>Bewaarde netwerken:</b><ul style='list-style:none;padding-left:0;margin:8px 0'>";
    for (int i = 0; i < opgeslagenAantal; i++) {
      String s = preferences.getString(("ssid_" + String(i)).c_str(), "");
      String p = preferences.getString(("pass_" + String(i)).c_str(), "");
      String slotje = (p == "") ? "🔓" : "🔒";
      lijst += "<li style='display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #f0f0f0'>"
               "<span>" + slotje + " " + s + "</span>"
               "<a href='/del-wifi?id=" + String(i) + "' onclick=\"return confirm('Netwerk " + s + " verwijderen?');\" style='color:#E74C3C;text-decoration:none;font-weight:bold;padding:2px 6px'>❌</a></li>";
    }
    lijst += "</ul><a href='/wis-wifi' style='color:#E74C3C;font-size:12px'>🗑️ Hele lijst wissen</a></div>";
    server.sendContent(lijst);
  }
  preferences.end();

  server.sendContent_P(PSTR(
    "<button type='button' onclick='toggleSettings()' class='btn' style='background:#7F8C8D;margin-top:20px;touch-action:manipulation'>✕ Menu Sluiten</button>"
    "</div>"
    "<script>window.paginaCompleet=true;</script>"
    "<script>"
    "function toggleSettings(){"
      "var m=document.getElementById('geavanceerdMenu');"
      "if(m.style.display==='block'){"
        "m.style.display='none';"
        "document.body.style.overflow='auto';"
        "document.getElementById('wifiLijst').innerHTML='';"
        "document.getElementById('updateBox').innerHTML='';"
      "}else{"
        "m.style.display='block';"
        "document.body.style.overflow='hidden';"
        "m.scrollTop=0;"
      "}"
    "}"
    "function openUpdateMenu(){"
      "var m=document.getElementById('geavanceerdMenu');"
      "m.style.display='block';document.body.style.overflow='hidden';m.scrollTop=0;checkUpdates();"
    "}"
    "</script></body></html>"
  ));
  server.sendContent("");
  delay(20); // Geef Wi-Fi radio tijd om het laatste pakket af te ronden voor BT weer start

  Serial.println("[Web] Hoofdpagina volledig verstuurd!");
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
}

void handleKies() {
  wifiLaatsteActiviteit = millis();
  if (server.hasArg("id")) {
    int id = server.arg("id").toInt();
    if (id >= 0 && id < actueelAantalGeluiden) {
      gekozenGeluidIndex = id;
      Serial.print("[Web] Geluid gewijzigd naar: ");
      Serial.println(geluiden[gekozenGeluidIndex]);
      
      preferences.begin("fietsbel", false);
      preferences.putInt("geluid", gekozenGeluidIndex);
      preferences.end();
    }
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "OK");
}

void handleVolume() {
  wifiLaatsteActiviteit = millis();
  if (server.hasArg("val")) {
    int ontvangenWaarde = server.arg("val").toInt();
    if (ontvangenWaarde < 0) ontvangenWaarde = 0;
    if (ontvangenWaarde > 100) ontvangenWaarde = 100;
    volumeInstelling = map(ontvangenWaarde, 0, 100, 0, 21);
    
    preferences.begin("fietsbel", false);
    preferences.putInt("volume", volumeInstelling);
    preferences.end();
  }
  server.send(200, "text/plain", "OK");
}

void handleVerwijder() {
  wifiLaatsteActiviteit = millis();
  if (SPIFFS.exists("/EigenGeluid.mp3")) SPIFFS.remove("/EigenGeluid.mp3");
  if (SPIFFS.exists("/naam.txt")) SPIFFS.remove("/naam.txt");
  controleerBestanden();

  preferences.begin("fietsbel", false);
  preferences.putInt("geluid", gekozenGeluidIndex);
  preferences.end();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleLogoJPG() {
  wifiLaatsteActiviteit = millis();
  if (SPIFFS.exists("/logo.jpg")) {
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    File f = SPIFFS.open("/logo.jpg", FILE_READ);
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.streamFile(f, "image/jpeg");
    f.close();
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  } else {
    server.send(404, "text/plain", "Logo niet gevonden.");
  }
}

void handleLogoPNG() {
  wifiLaatsteActiviteit = millis();
  if (SPIFFS.exists("/logo.png")) {
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    File f = SPIFFS.open("/logo.png", FILE_READ);
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.streamFile(f, "image/png");
    f.close();
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  } else {
    server.send(404, "text/plain", "Logo niet gevonden.");
  }
}

void handleUploadMP3() {
  wifiLaatsteActiviteit = millis();
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String name = upload.filename;
    if (name.endsWith(".mp3") || name.endsWith(".MP3")) {
      File nameFile = SPIFFS.open("/naam.txt", FILE_WRITE);
      nameFile.print(name.substring(0, name.length() - 4));
      nameFile.close();
      uploadBestand = SPIFFS.open("/EigenGeluid.mp3", FILE_WRITE);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && uploadBestand) {
    uploadBestand.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END && uploadBestand) {
    uploadBestand.close();
    controleerBestanden();
  }
}

void startDraadloos() {
  if (draadloosActief) return;

  Serial.println("[Systeem] Opstartgeluid afspelen...");
  speelMP3EnRuimOp("/startup.mp3", false);
  Serial.printf("[RAM] Vrij heap geheugen: %u bytes\n", ESP.getFreeHeap());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/kies", HTTP_GET, handleKies);
  server.on("/volume", HTTP_GET, handleVolume);
  server.on("/verwijder", HTTP_GET, handleVerwijder);
  server.on("/logo.jpg", HTTP_GET, handleLogoJPG);
  server.on("/logo.png", HTTP_GET, handleLogoPNG);
  server.on("/upload-mp3", HTTP_POST, []() {
    server.sendHeader("Location", "/");
    server.send(303);
  }, handleUploadMP3);
  
  // Draadloze Firmware Update (OTA)
  server.on("/update-fw", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "Update mislukt!" : "Update geslaagd! Fietsbel start opnieuw op...");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    wifiLaatsteActiviteit = millis();

    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("[OTA] Start update: %s\n", upload.filename.c_str());
      if (draadloosActief) a2dp_sink.pause();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("[OTA] Update voltooid: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.on("/start-server-update", HTTP_GET, []() {
    if (WiFi.status() != WL_CONNECTED) {
      // Maak eerst verbinding met opgeslagen netwerk als hij net weer op pure AP stond
      WiFi.mode(WIFI_AP_STA);
      laadOpgeslagenWifiNetwerken();
      unsigned long startV = millis();
      while (wifiMulti.run() != WL_CONNECTED && millis() - startV < 8000) {
        delay(200);
      }
    }
    if (WiFi.status() != WL_CONNECTED) {
      server.send(200, "text/plain", "Geen internetverbinding met een opgeslagen netwerk.");
      return;
    }
    if (!server.hasArg("file")) {
      server.send(400, "text/plain", "Geen firmware-bestand gekozen.");
      return;
    }

    String gekozenBestand = server.arg("file");
    if (!gekozenBestand.endsWith(".bin") || gekozenBestand.indexOf('/') != -1) {
      server.send(400, "text/plain", "Ongeldige bestandsnaam.");
      return;
    }

    String volledigeUrl = GITHUB_BASE_URL + gekozenBestand;
    server.send(200, "text/plain", "Bezig met installeren van " + gekozenBestand + "... Wacht 15 seconden tot de fietsbel herstart.");
    delay(500);
    if (draadloosActief) a2dp_sink.pause();

    WiFiClientSecure client;
    client.setInsecure();
    t_httpUpdate_return ret = httpUpdate.update(client, volledigeUrl);

    if (ret == HTTP_UPDATE_FAILED) {
      Serial.printf("[Update] Fout (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    }
  });

  server.on("/scan-wifi", HTTP_GET, []() {
    wifiLaatsteActiviteit = millis();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    if (WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA);
      delay(100);
    }

    int n = WiFi.scanNetworks(false, false, false, 120);
    String html = "<div style='margin:10px 0;text-align:left;max-height:140px;overflow-y:auto;border:1px solid #eee;padding:5px;border-radius:8px'>";
    if (n <= 0) {
      html += "<div style='padding:6px;font-size:13px;color:#888'>Geen netwerken gevonden.</div>";
    } else {
      for (int i = 0; i < n; i++) {
        String naam = WiFi.SSID(i);
        if (naam == "") continue;
        String slot = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "🔓" : "🔒";
        html += "<div style='padding:6px;border-bottom:1px solid #f0f0f0;cursor:pointer;font-size:13px' onclick=\"document.getElementById('wifi_ssid').value='" + naam + "'\">" + slot + " <b>" + naam + "</b> (" + String(WiFi.RSSI(i)) + " dBm)</div>";
      }
    }
    html += "</div>";
    WiFi.scanDelete();
    
    gescandeNetwerkenHTML = html;
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", html);
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  });

  server.on("/save-wifi", HTTP_POST, []() {
    wifiLaatsteActiviteit = millis();
    if (server.hasArg("ssid")) {
      String s = server.arg("ssid");
      String p = server.hasArg("pass") ? server.arg("pass") : "";
      s.trim();
      if (s.length() > 0) {
        voegWifiNetwerkToe(s, p);
      }
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.on("/del-wifi", HTTP_GET, []() {
    wifiLaatsteActiviteit = millis();
    if (server.hasArg("id")) {
      int id = server.arg("id").toInt();
      verwijderWifiNetwerk(id);
    }
    server.sendHeader("Location", "/");
    server.send(303);
    delay(300);
    ESP.restart();
  });

  server.on("/wis-wifi", HTTP_GET, []() {
    wifiLaatsteActiviteit = millis();
    preferences.begin("wifi_lijst", false);
    preferences.clear();
    preferences.end();
    gescandeNetwerkenHTML = "";
    server.sendHeader("Location", "/");
    server.send(303);
    delay(300);
    ESP.restart();
  });

  server.on("/get-updates", HTTP_GET, []() {
    wifiLaatsteActiviteit = millis();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    preferences.begin("wifi_lijst", true);
    int aantal = preferences.getInt("aantal", 0);
    preferences.end();

    if (aantal > 0 && WiFi.status() != WL_CONNECTED) {
      WiFi.mode(WIFI_AP_STA);
      laadOpgeslagenWifiNetwerken();
      
      unsigned long startVerbinden = millis();
      while (wifiMulti.run() != WL_CONNECTED && millis() - startVerbinden < 8000) {
        delay(250);
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      haalUpdatesLijstOp();
      server.sendHeader("Connection", "close");
      server.send(200, "text/html", beschikbareUpdatesHTML);
    } else {
      server.sendHeader("Connection", "close");
      server.send(200, "text/html", "<p style='color:#E74C3C;font-size:13px'>⚠️ Geen verbinding met opgeslagen Wi-Fi netwerk.</p>");
    }
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  });

   // Voorkom dat Safari op de achtergrond de zware hoofdpagina dubbel laadt voor icoontjes
  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  server.on("/apple-touch-icon.png", HTTP_GET, []() { server.send(204); });
  server.on("/apple-touch-icon-precomposed.png", HTTP_GET, []() { server.send(204); });
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302);
  });
  startWiFiHotspot();
  Serial.println("[Systeem] I2S en Bluetooth A2DP starten...");
  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
  i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  a2dp_sink.set_stream_reader(audio_data_callback, false);
  a2dp_sink.set_auto_reconnect(true);
  a2dp_sink.start("Digitale fietsbel Speaker");
  delay(250);
  a2dp_sink.set_volume(btVolume);
  digitalWrite(AMP_SD_PIN, HIGH);
  draadloosActief = true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  preferences.begin("fietsbel", true);
  gekozenGeluidIndex = preferences.getInt("geluid", 0);
  volumeInstelling   = preferences.getInt("volume", 18);
  preferences.end();
  delay(200);
  rtc_gpio_deinit((gpio_num_t)KNOP_PIN);
  rtc_gpio_deinit((gpio_num_t)SLAAP_KNOP_PIN);
  pinMode(SLAAP_KNOP_PIN, INPUT_PULLUP);
  pinMode(VOL_UP_PIN, INPUT_PULLUP);
  pinMode(VOL_DOWN_PIN, INPUT_PULLUP);
  pinMode(KNOP_PIN, INPUT_PULLUP);
  pinMode(AMP_SD_PIN, OUTPUT);
  digitalWrite(AMP_SD_PIN, LOW);
  pinMode(BATTERIJ_PIN, INPUT);
  pinMode(TP4057_CHRG_PIN, INPUT);
  pinMode(TP4057_STDBY_PIN, INPUT);
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();

   // 1. Wakker geworden door de BELKNOP (Pin 13 via ext0)
  if (wakeupReason == ESP_SLEEP_WAKEUP_EXT0) {
  if (!isAanHetLaden() && meetAccuPercentage() < 2) {
  rtc_gpio_pullup_en((gpio_num_t)KNOP_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)KNOP_PIN);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)KNOP_PIN, 0);
  esp_deep_sleep_start();
}
 if (SPIFFS.begin(true)) {
  controleerBestanden();
  Serial.print("[Systeem] Actief belgeluid: ");
  Serial.println(geluiden[gekozenGeluidIndex]);
  speelMP3EnRuimOp(geluiden[gekozenGeluidIndex].c_str(), true, 400);
}
  gaInDeepSleep();
}

   // 2. Wakker geworden door SLAAPKNOP (Pin 14 via ext1)
 if (wakeupReason == ESP_SLEEP_WAKEUP_EXT1) {
  unsigned long startTijd = millis();
  while (millis() - startTijd < 5000) {
 if (digitalRead(SLAAP_KNOP_PIN) == HIGH) {
  delay(150);
 if (digitalRead(SLAAP_KNOP_PIN) == HIGH) {
  gaInDeepSleep();
}
}
  delay(10);
}
}
   // 3. Wakker geworden door de 1-MINUUT TIMER
 if (wakeupReason == ESP_SLEEP_WAKEUP_TIMER) {
 if (!isAanHetLaden()) {
  int huidigeAccu = meetAccuPercentage();
 if (huidigeAccu < 2) {
 if (SPIFFS.begin(true)) {
 kritiekeAccuUitschakeling();
}
} else if (huidigeAccu <= 5) {
 if (SPIFFS.begin(true)) {
  speelMP3EnRuimOp("/lowbat.mp3", false);
}
}
}
  gaInDeepSleep();
}
 if (!SPIFFS.begin(true)) return;
  controleerBestanden();
  Serial.print("[Systeem] Actief belgeluid: ");
  Serial.println(geluiden[gekozenGeluidIndex]);
 if (wakeupReason == ESP_SLEEP_WAKEUP_EXT1) {
  startDraadloos();
 while (digitalRead(SLAAP_KNOP_PIN) == LOW) delay(50);
}
}

void loop() {
 if (wifiActief) {
  server.handleClient();

   // 1x automatisch op de achtergrond checken bij opstarten als er een netwerk is opgeslagen
 if (!opstartUpdateCheckGedaan && millis() > 4000) {
  opstartUpdateCheckGedaan = true;
  preferences.begin("wifi_lijst", true);
  int aantal = preferences.getInt("aantal", 0);
  preferences.end();
 if (aantal > 0) {
  Serial.println("[Auto-Update] Controleer 1x op de achtergrond naar nieuwe firmware...");
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  WiFi.mode(WIFI_AP_STA);
  laadOpgeslagenWifiNetwerken();
  unsigned long startCheck = millis();
 while (wifiMulti.run() != WL_CONNECTED && millis() - startCheck < 6000) {
  delay(200);
}
 if (WiFi.status() == WL_CONNECTED) {
  haalUpdatesLijstOp();
 if (nieuweVersieLabel != "") {
  Serial.println("[Auto-Update] Nieuwe versie gevonden: " + nieuweVersieLabel);
} else {
  Serial.println("[Auto-Update] Firmware is helemaal up-to-date.");
}

// Verbreek thuis-WiFi weer zodat de hotspot (192.168.4.1) op kanaal 1 stabiel blijft!
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password, 1);
}
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
}
}
 if (millis() - wifiLaatsteActiviteit >= WIFI_TIMEOUT_MS) {
  stopWiFiHotspot();
}
}

   // --- 1. LEES PIN 14 (5s = Aan/Uit | 1x klik = Play/Pauze | 2x klik = Wi-Fi 10 min aan) ---
 if (digitalRead(SLAAP_KNOP_PIN) == LOW) {
 if (!slaapKnopInGedrukt) {
  slaapKnopTijd = millis();
  slaapKnopInGedrukt = true;
} else if (millis() - slaapKnopTijd >= 5000) {
 if (!draadloosActief) {
  startDraadloos();
  while (digitalRead(SLAAP_KNOP_PIN) == LOW) delay(50);
  slaapKnopInGedrukt = false;
  pin14KlikTeller = 0;
} else {
          Serial.println("[Systeem] Pin 14 (5 sec): Uitschakelen gestart...");
          
          if (wifiActief) {
            stopWiFiHotspot();
          }
          
          Serial.println("[Systeem] Bluetooth en I2S stoppen...");
          a2dp_sink.pause();
          i2s.end();
          draadloosActief = false;
          delay(50);

          Serial.println("[Systeem] Afsluitgeluid (/shutdown.mp3) afspelen...");
          speelMP3EnRuimOp("/shutdown.mp3", false);
          Serial.println("[Systeem] Afsluitgeluid klaar!");

          while (digitalRead(SLAAP_KNOP_PIN) == LOW) delay(50);
          delay(200);
          gaInDeepSleep();
        }
}
} else {
 if (slaapKnopInGedrukt) {
  delay(40);
 if (digitalRead(SLAAP_KNOP_PIN) == HIGH) {
  unsigned long duur = millis() - slaapKnopTijd;
 if (draadloosActief && duur >= 50 && duur < 1000) {
  pin14KlikTeller++;
 if (pin14KlikTeller == 1) {
  pin14EersteKlikTijd = millis();
} else if (pin14KlikTeller == 2) {
  Serial.println("[Systeem] Dubbelklik Pin 14: Wi-Fi 10 minuten ingeschakeld.");
  startWiFiHotspot();
  pin14KlikTeller = 0;
}
}
  slaapKnopInGedrukt = false;
}
}
}

   // Enkele klik op Pin 14 verwerken (Play / Pauze)
 if (draadloosActief && pin14KlikTeller == 1 && (millis() - pin14EersteKlikTijd > DUBBELKLIK_TIJD_MS)) {
 if (bluetoothSpeeltAf) {
  a2dp_sink.pause();
  bluetoothSpeeltAf = false;
} else {
  a2dp_sink.play();
  bluetoothSpeeltAf = true;
}
  pin14KlikTeller = 0;
}

   // --- 2. LEES DE BELKNOP (Pin 13) ---
  bool knopStaatNuIn = (digitalRead(KNOP_PIN) == LOW);
  if (knopStaatNuIn && !belActief) {
  belActief = true;
  Serial.print("[Bel] Speelt nu af: ");
  Serial.println(geluiden[gekozenGeluidIndex]);
  speelMP3EnRuimOp(geluiden[gekozenGeluidIndex].c_str(), true, 0);
}
 if (!knopStaatNuIn && belActief) {
  belActief = false;
  delay(50);
 if (draadloosActief && bluetoothSpeeltAf) {
  a2dp_sink.play();
}
}

   // --- 3. BLUETOOTH VOLUME KNOPPEN (Pin 27 & Pin 33) ---
 if (draadloosActief && (millis() - laatsteVolDruk >= 150)) {
 if (digitalRead(VOL_UP_PIN) == LOW) {
  btVolume = min(127, btVolume + 8);
  a2dp_sink.set_volume(btVolume);
  laatsteVolDruk = millis();
}
 if (digitalRead(VOL_DOWN_PIN) == LOW) {
  btVolume = max(0, btVolume - 8);
  a2dp_sink.set_volume(btVolume);
  laatsteVolDruk = millis();
}
}

   // --- 4. BATTERIJBEWAKING (Elke 10 seconden) ---
 if (!belActief && !isAanHetLaden() && (millis() - laatsteAccuCheck >= 10000)) {
  laatsteAccuCheck = millis();
  int huidigeAccu = meetAccuPercentage();
 if (huidigeAccu < 2) {
  kritiekeAccuUitschakeling();
} else if (huidigeAccu <= 10) {
  unsigned long huidigInterval = (huidigeAccu <= 5) ? INTERVAL_5_PROCENT : INTERVAL_10_PROCENT;
 if (laatsteLowBatMelding == 0 || (millis() - laatsteLowBatMelding >= huidigInterval)) {
  laatsteLowBatMelding = millis();
  speelMP3EnRuimOp("/lowbat.mp3", false);
 if (draadloosActief && bluetoothSpeeltAf) {
  a2dp_sink.play();
}
}
} else {
  laatsteLowBatMelding = 0;
}
}
  delay(2);
}