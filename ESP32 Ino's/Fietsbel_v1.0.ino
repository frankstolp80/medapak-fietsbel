#include "Arduino.h"
#include "Audio.h" 
#include "ESP_I2S.h"         
#include "BluetoothA2DPSink.h"
#include "SPIFFS.h"
#include <WiFi.h>
#include <WebServer.h>
#include "esp_coexist.h"
#include <DNSServer.h>     

#define I2S_DOUT      22
#define I2S_BCLK      26
#define I2S_LRC       25



const int KNOP_PIN = 13;
const int BATTERIJ_PIN = 34; // Analoge pin voor spanningsmeting

// TP4057 Status pinnen
const int TP4057_CHRG_PIN = 35;  // LOW = Aan het laden
const int TP4057_STDBY_PIN = 32; // LOW = Volledig opgeladen aan de lader


const char* ssid = "Medapak Fietsbel Instellingen";
const char* password = "medapak1234";

WebServer server(80);
I2SClass i2s;                         
BluetoothA2DPSink a2dp_sink(i2s); 
Audio audio; // Maak het audio-object aan voor de MP3-bibliotheek    

const int MAX_GELUIDEN = 5;
String geluiden[MAX_GELUIDEN] = {"/Fietsbel.mp3", "/Politie.mp3", "/Brandweer.mp3", "/Ambulance.mp3", "/EigenGeluid.mp3"};
int actueelAantalGeluiden = 4; 
int gekozenGeluidIndex = 0; 
String origineleBestandsnaam = ""; 

bool belActief = false;
int volumeInstelling = 18; 

File belBestand; 
File uploadBestand; // Opgelost: Nu correct gedeclareerd
String actieveLogoExtensie = ""; 

// Opgelost: Correcte functie-signatuur voor de data callback conform de nieuwste A2DP bibliotheek
void audio_data_callback(const uint8_t *data, uint32_t len) {
  // Deze callback leest de binnenkomende bluetooth data (leeg laten houdt de stream open)
}

// Geeft true als de TP4057 actief stroom in de accu pompt
bool isAanHetLaden() {
  return (digitalRead(TP4057_CHRG_PIN) == LOW);
}

// Geeft true als de USB is aangesloten en de accu 100% vol is
bool isVolledigOpgeladen() {
  return (digitalRead(TP4057_STDBY_PIN) == LOW);
}

int meetAccuPercentage() {
  int analogewaarde = analogRead(BATTERIJ_PIN);
  float spanning = (analogewaarde / 4095.0) * 3.3 * 2.0;
  int percentage = map(spanning * 100, 340 * 100, 420 * 100, 0, 100);
  if (percentage > 100) percentage = 100;
  if (percentage < 0) percentage = 0;
  return percentage;
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

String genereerWebsite() {
  // REPARATIE 1: Gebruik de juiste functienaam 'meetAccuPercentage' conform jouw code
  int accu = meetAccuPercentage(); 
  bool aanHetLaden = isAanHetLaden();
  bool volGelaaden = isVolledigOpgeladen();
  
  // REPARATIE 2: Bepaal of de accu écht 100% vol is (of de hardware-indicator 'vol' is)
  bool isVol = (accu >= 100) || volGelaaden;
    
  String html = R"====(
<!DOCTYPE html>
<html>
<head>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <meta charset='UTF-8'>
    <title>Fietsbel Menu</title>
    <style>
        body { font-family: Arial, sans-serif; text-align: center; background-color: #f4f4f9; padding: 20px; }
        .logo { max-width: 150px; height: auto; margin: 10px auto 20px auto; display: block; border-radius: 10px; }
        .btn { display: block; width: 85%; max-width: 300px; margin: 12px auto; padding: 15px; font-size: 18px; color: white; background-color: #000; border: none; border-radius: 25px; cursor: pointer; text-decoration: none; font-weight: bold; box-shadow: 0 4px 6px rgba(0,0,0,0.1); border: 2px solid #373737;}
        .btn-actief { background-color: #ef171c; border: 2px solid #d02832; }
        .box { background: white; padding: 20px; border-radius: 15px; max-width: 300px; margin: 20px auto; box-shadow: 0 4px 6px rgba(0,0,0,0.05); }
        .btn-delete { background-color: #E74C3C; color: white; padding: 10px 15px; border-radius: 20px; text-decoration: none; display: inline-block; font-size: 14px; font-weight: bold; margin-top: 10px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        input[type='file'], input[type='submit'] { width: 100%; margin: 8px 0; padding: 10px; box-sizing: border-box; border-radius: 5px; border: 1px solid #ccc; }
        input[type='submit'] { background-color: #E67E22; color: white; font-weight: bold; border: none; cursor: pointer; }
        input[type='range'] { width: 85%; margin: 15px 0; }
        h1 { color: #333; margin-bottom: 5px; font-size: 30px; } p { color: #666; } h3 { margin-top: 0; color: #000; text-align: left; }

        .accu-container { font-size: 20px; margin: 20px auto; padding: 10px; width: 150px; background: white; border-radius: 10px; box-shadow: 0 2px 4px rgba(0,0,0,0.05); }
        .battery-icon { display: inline-block; width: 40px; height: 20px; border: 2px solid #333; border-radius: 3px; position: relative; vertical-align: middle; margin-right: 8px; }
        .battery-icon:after { content: ''; position: absolute; right: -6px; top: 4px; width: 4px; height: 8px; background: #333; border-radius: 0 2px 2px 0; }
        
    </style>
</head>
<body>
  )====";
  
  // CSS Animatie en algemene stijlen
  html += "<style>";
  html += "@keyframes knipperen { 0% { opacity: 1; } 50% { opacity: 0.2; } 100% { opacity: 1; } }";
  html += "@keyframes opladen { 0%, 100% { opacity: 0.3; } 50% { opacity: 1; } }";
  html += ".knipper-blok { animation: knipperen 1s infinite; }";
  
  // REPARATIE 3: Schakel de CSS animaties uit als de accu 100% ('isVol') bereikt heeft
  if (aanHetLaden && !isVol) {
    html += ".laad-blok-1 { animation: opladen 1.5s infinite; animation-delay: 0.0s; }";
    html += ".laad-blok-2 { animation: opladen 1.5s infinite; animation-delay: 0.3s; }";
    html += ".laad-blok-3 { animation: opladen 1.5s infinite; animation-delay: 0.6s; }";
    html += ".laad-blok-4 { animation: opladen 1.5s infinite; animation-delay: 0.9s; }";
  } else {
    // Statische weergave (geen animatie) als de lader klaar is
    html += ".laad-blok-1, .laad-blok-2, .laad-blok-3, .laad-blok-4 { opacity: 1; }";
  }
  
  html += ".mini-logo { width: 160px; height: 50px; object-fit: cover; border-radius: 6px; border: 0px solid #ccc; margin-bottom: 10px;}";
  html += "</style>";

  html += "<div class='box' style='padding: 15px; margin-bottom: 15px;'>";
  html += "<div style='display: flex; align-items: center; justify-content: space-between; border-bottom: 1px solid #000;'>";
  
  if (actieveLogoExtensie != "default" && actieveLogoExtensie != "") {
    html += "<img src='/logo." + actieveLogoExtensie + "?v=" + String(millis()) + "' class='mini-logo' alt='Logo'>";
  } else {
    html += "<span style='font-size: 24px;'>🚲</span>";
  }
  
  html += "<div style='border: 2px solid #333; width: 30px; height: 14px; padding: 2px; border-radius: 6px; display: flex; gap: 2px; background: #fff; position: relative;'>";
  
  // REPARATIE 4: Als hij aan het laden is MAAR nog niet vol, toon de animatie-blokken. 
  // Als hij aan het laden is EN vol (of de standby pin triggert), toon 4 vaste groene balken.
  if (aanHetLaden && !isVol) {
    html += "<div class='laad-blok-1' style='background-color: #2ECC71; width: 13px; height: 100%; border-radius: 2px;'></div>";
    html += "<div class='laad-blok-2' style='background-color: #2ECC71; width: 13px; height: 100%; border-radius: 2px;'></div>";
    html += "<div class='laad-blok-3' style='background-color: #2ECC71; width: 13px; height: 100%; border-radius: 2px;'></div>";
    html += "<div class='laad-blok-4' style='background-color: #2ECC71; width: 13px; height: 100%; border-radius: 2px;'></div>";
  } else if (isVol) {
    // 4 Statische vol-groene balkjes zonder geknipper of lopende animatie
    for(int i = 0; i < 4; i++) {
      html += "<div style='background-color: #2ECC71; width: 13px; height: 100%; border-radius: 2px;'></div>";
    }
  } else {
    // Normale batterijwerking op de fiets (ontladen)
    String batterijKleur = "#2ECC71";
    int aantalBlokken = 0;
    bool moetKnipperen = false;

    if (accu >= 75) { aantalBlokken = 4; batterijKleur = "#2ECC71"; }
    else if (accu >= 50) { aantalBlokken = 3; batterijKleur = "#3498DB"; }
    else if (accu >= 25) { aantalBlokken = 2; batterijKleur = "#E67E22"; }
    else if (accu >= 10) { aantalBlokken = 1; batterijKleur = "#E74C3C"; moetKnipperen = true; }
    else { aantalBlokken = 0; batterijKleur = "#95A5A6"; }

    for(int i = 0; i < 4; i++) {
      if (i < aantalBlokken) {
        String extraKlasse = (moetKnipperen && i == 0) ? " class='knipper-blok'" : "";
        html += "<div" + extraKlasse + " style='background-color: " + batterijKleur + "; width: 13px; height: 100%; border-radius: 2px;'></div>";
      } else {
        html += "<div style='background-color: #EAEDED; width: 13px; height: 100%; border-radius: 2px;'></div>";
      }
    }
  }
  
  html += "<div style='position: absolute; right: -6px; top: 3px; width: 4px; height: 10px; background: #333; border-radius: 0 2px 2px 0;'></div>";
  html += "</div>"; // Einde huls
  
  // Toon optioneel het percentage in tekstvorm naast de batterij
  
  
  html += "</div>"; // Einde flex-container
  html += "<h1>Fietsbel Instellingen</h1>";
  html += "</div>"; // Einde box
  
  

  html += "<div class='box'>";
  html += "<h3>🔊 Fietsbel geluid:</h3>";
  
  for(int i = 0; i < actueelAantalGeluiden; i++) {
    String classNaam = (i == gekozenGeluidIndex) ? "btn btn-actief" : "btn";
    String schoneNaam = (i == 4) ? "⭐ " + origineleBestandsnaam : geluiden[i].substring(1, geluiden[i].length() - 4); 
    html += "<a href='/kies?id=" + String(i) + "' class='" + classNaam + "'>" + schoneNaam + "</a>";
  }
  html += "</div>";

html += "<div class='box'>";
  // Bereken het huidige percentage om correct te tonen bij het laden van de pagina
  int actueelPercentage = map(volumeInstelling, 0, 21, 0, 100);
  html += "<h3>🔊 Fietsbel Volume: <span id='volVal'>" + String(actueelPercentage) + "</span>%</h3>";
  // De slider loopt nu van 0 tot 100
  html += "<input type='range' min='0' max='100' value='" + String(actueelPercentage) + "' onchange='updateVolume(this.value)'>";
  html += "<script>function updateVolume(val) { document.getElementById('volVal').innerText = val; fetch('/volume?val=' + val); }</script>";
  html += "</div>";
  
  html += "<div class='box'>";
  html += "<h3>🎵 Eigen Fietsbel uploaden</h3>";
  html += "<form method='POST' action='/upload-mp3' enctype='multipart/form-data'>";
  html += "<p style='font-size: 13px; color: #888;'>Selecteer een korte <b>.mp3</b> om te uploaden.</p>";
  html += "<input type='file' name='update' accept='.mp3' required>";
  html += "<input type='submit' value='Geluid Overschrijven'>";
  html += "</form>";
  
  if (actueelAantalGeluiden == 5) {
    html += "<hr style='border: 0; border-top: 1px solid #eee; margin: 20px 0;'>";
    html += "<a href='/verwijder' class='btn-delete' onclick=\"return confirm('Weet je zeker dat je jouw eigen geluid wilt wissen?');\">❌ Eigen geluid wissen</a>";
  }
  html += "</div>";
  
  html += "</body></html>";
  return html;
}

void handleRoot() { server.send(200, "text/html", genereerWebsite()); }

void handleKies() {
  if (server.hasArg("id")) {
    int id = server.arg("id").toInt();
    if (id >= 0 && id < actueelAantalGeluiden) {
      gekozenGeluidIndex = id;
      
      // --- Bepaal de naam van de zojuist gekozen MP3 ---
      String gekozenNaam;
      if (gekozenGeluidIndex == 4) {
        // Als het eigen geluid is gekozen, toon de originele uploadnaam
        gekozenNaam = "Eigen geluid (" + origineleBestandsnaam + ")";
      } else {
        // Haal de '/' aan het begin weg voor een schone weergave
        gekozenNaam = geluiden[gekozenGeluidIndex].substring(1);
      }
      
      // Print de keuze direct in de Seriële Monitor
      Serial.print("[Webapp] Nieuw geluid gekozen via app: ");
      Serial.println(gekozenNaam);
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleVolume() {
  if (server.hasArg("val")) {
    int ontvangenWaarde = server.arg("val").toInt();
    
    // Begrens de input tussen 0 en 100 voor de veiligheid
    if (ontvangenWaarde < 0) ontvangenWaarde = 0;
    if (ontvangenWaarde > 100) ontvangenWaarde = 100;
    
    // Reken de 0-100 waarde om naar de 0-21 schaal voor de audio-bibliotheek
    volumeInstelling = map(ontvangenWaarde, 0, 100, 0, 21);
    
    Serial.print("[Webapp] Volume aangepast naar: ");
    Serial.print(ontvangenWaarde);
    Serial.print("% (Interne schaal: ");
    Serial.print(volumeInstelling);
    Serial.println("/21)");
  }
  server.send(200, "text/plain", "OK");
}

void handleVerwijder() {
  if (SPIFFS.exists("/EigenGeluid.mp3")) SPIFFS.remove("/EigenGeluid.mp3");
  if (SPIFFS.exists("/naam.txt")) SPIFFS.remove("/naam.txt");
  controleerBestanden();
  server.sendHeader("Location", "/");
  server.send(303); 
}

void handleLogoJPG() {
  // Controleer of het bestand daadwerkelijk bestaat in het SPIFFS geheugen
  if (SPIFFS.exists("/logo.jpg")) {
    File f = SPIFFS.open("/logo.jpg", FILE_READ);
    
    // Voorkom dat browsers een oude/lege versie cachen
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    
    // Essentieel voor iPhone/Safari: Vertel de browser expliciet dat dit een JPEG is
    server.sendHeader("Content-Type", "image/jpeg"); 
    
    // Geef de exacte bestandsgrootte door zodat de browser weet hoeveel data er komt
    server.setContentLength(f.size()); 
    
    // Stream het bestand rechtstreeks naar de browser
    server.streamFile(f, "image/jpeg");
    f.close();
  } 
  else {
    // Als het bestand niet bestaat, stuur een nette 404 melding
    server.send(404, "text/plain", "Logo niet gevonden. Upload eerst een afbeelding via het menu.");
  }
}

void handleUploadMP3() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String name = upload.filename;
    if (name.endsWith(".mp3") || name.endsWith(".MP3")) {
      File nameFile = SPIFFS.open("/naam.txt", FILE_WRITE);
      nameFile.print(name.substring(0, name.length() - 4));
      nameFile.close();
      uploadBestand = SPIFFS.open("/EigenGeluid.mp3", FILE_WRITE);
    }
  } 
  else if (upload.status == UPLOAD_FILE_WRITE && uploadBestand) {
    uploadBestand.write(upload.buf, upload.currentSize);
  } 
  else if (upload.status == UPLOAD_FILE_END && uploadBestand) {
    uploadBestand.close();
    controleerBestanden(); 
  }
}

void setup() {
  Serial.begin(115200);
  if(!SPIFFS.begin(true)) return;

  controleerBestanden();
  pinMode(KNOP_PIN, INPUT_PULLUP);
  pinMode(BATTERIJ_PIN, INPUT);

  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE); 
  WiFi.softAP(ssid, password);
  WiFi.setSleep(WIFI_PS_NONE); 

  server.on("/", HTTP_GET, handleRoot);
  server.on("/kies", HTTP_GET, handleKies);
  server.on("/volume", HTTP_GET, handleVolume);
  server.on("/verwijder", HTTP_GET, handleVerwijder);

  // ----------------------------------------------------
  // HIER ZIT DE REPARATIE: De ontbrekende afbeeldings-routes!
  // ----------------------------------------------------
  server.on("/logo.jpg", HTTP_GET, handleLogoJPG);
  // ----------------------------------------------------

  server.on("/upload-mp3", HTTP_POST, []() {
    server.sendHeader("Location", "/");
    server.send(303);
  }, handleUploadMP3);
  
  server.begin();

  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
  
  // Opgelost: Juiste argumenten meegeven conform ESP32-A2DP v3.x specificaties
  a2dp_sink.set_stream_reader(audio_data_callback, false);
  a2dp_sink.start("Medapak Fietsbel Speaker");
}

void loop() {
  // Dit moet continu blijven draaien om de website in de lucht te houden
  server.handleClient(); 

  bool knopStaatNuIn = (digitalRead(KNOP_PIN) == LOW);

  // Knop ingedrukt -> Activeer tijdelijk een hardware-veilige volume-mute of pauze
  if (knopStaatNuIn && !belActief) {
    Serial.print("[Systeem] Knop in! Geluid index: ");
    Serial.println(gekozenGeluidIndex);
    
    a2dp_sink.pause();
    delay(100);
    
    belActief = true;
  }

  // Als de knop ingedrukt blijft, blijft hij loopen
  if (knopStaatNuIn && belActief) {
    // REPARATIE: Zorg dat de webserver en bluetooth ook bereikbaar blijven als de knop ingedrukt blijft!
    server.handleClient(); 
  }

  // Knop losgelaten -> Direct muziek hervatten
  if (!knopStaatNuIn && belActief) {
    Serial.println("[Systeem] Knop los: Terug naar Bluetooth muziek.");
    belActief = false;
    delay(50);
    a2dp_sink.play();
  }
}
