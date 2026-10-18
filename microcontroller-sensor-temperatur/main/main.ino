#include <WiFi.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- Konfigurasi OLED ---
#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define SDA_PIN 21
#define SCL_PIN 22
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- Konfigurasi DHT11 ---
#define DHTPIN 4 
#define DHTTYPE DHT11     
DHT dht(DHTPIN, DHTTYPE);

const int RESET_BTN_PIN = 0; 

Preferences pref;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
AsyncWebServer server(80);

// Access token dihapus, hanya menyisakan SSID, Pass, dan IP MQTT Broker
String ssid, pass, mqtt_ip;
unsigned long lastMsg = 0;
const unsigned long interval = 2000; // Kirim data setiap 2 detik

// --- Fungsi Update Tampilan OLED ---
void updateDisplay(String mode, String s, String m, float t, float h, bool mqttOk, bool sensorOk) {
  display.clearDisplay();
  display.setTextSize(1); 
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(0, 0);
  display.print("Mode : "); display.println(mode);
  display.print("SSID : "); display.println(s.substring(0, 15));
  display.print("MQTT : "); display.println(m);
  
  display.setCursor(0, 30);
  if(mode == "Wifi") {
    display.println(mqttOk ? "Status: MQTT Online" : "Status: MQTT Error");
    display.drawLine(0, 42, 128, 42, SSD1306_WHITE);

    if(!sensorOk) {
      display.setCursor(0, 50);
      display.println("Sensor: Error");
    } else {
      display.setCursor(0, 48);
      display.printf("Temp     : %.1f C", t);
      display.setCursor(0, 56);
      display.printf("Humidity : %.1f %%", h);
    }
  } else {
    display.println("IP   : 192.168.4.1");
    display.println("Pass : 12345678");
    display.setCursor(0, 50);
    display.println("Setup via Browser...");
  }
  display.display();
}

// --- Fungsi Access Point Mode ---
void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32 Weather", "12345678");
    updateDisplay("Config", "Setup_AP", "-", 0, 0, false, false);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif; background:#f4f7f6; display:flex; justify-content:center; padding:20px;}";
        html += ".card{background:white; padding:20px; border-radius:10px; box-shadow:0 4px 8px rgba(0,0,0,0.1); width:100%; max-width:350px;}";
        html += "input{width:100%; padding:10px; margin:10px 0; border:1px solid #ddd; border-radius:5px; box-sizing:border-box;}";
        html += "button{width:100%; padding:10px; background:#3498db; color:white; border:none; border-radius:5px; cursor:pointer; font-weight:bold;}</style></head>";
        html += "<body><div class='card'><h2>IoT Config</h2><form action='/save' method='POST'>";
        html += "<label>WiFi SSID</label><input type='text' name='ssid' required>";
        html += "<label>WiFi Password</label><input type='password' name='pass' required>";
        html += "<label>MQTT Broker IP</label><input type='text' name='mqtt' required>";
        html += "<button type='submit'>SAVE & CONNECT</button></form></div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        pref.begin("config", false);
        pref.putString("ssid", request->arg("ssid"));
        pref.putString("pass", request->arg("pass"));
        pref.putString("mqtt", request->arg("mqtt"));
        pref.end();
        request->send(200, "text/plain", "Data Saved. Device Restarting...");
        delay(2000); ESP.restart();
    });
    server.begin();
}

void setup() {
    Serial.begin(115200);
    pinMode(RESET_BTN_PIN, INPUT_PULLUP);
    
    Wire.begin(SDA_PIN, SCL_PIN);

    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED Gagal");
    }
    display.clearDisplay(); 
    display.display();
    
    dht.begin();

    pref.begin("config", true);
    ssid = pref.getString("ssid", "");
    pass = pref.getString("pass", "");
    mqtt_ip = pref.getString("mqtt", "");
    pref.end();

    // Validasi token dihapus, pengecekan beralih ke SSID & IP Broker
    if (ssid == "" || mqtt_ip == "") {
        startAP();
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long startWait = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startWait < 15000)) { 
            delay(500); 
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            mqttClient.setServer(mqtt_ip.c_str(), 1883);
        } else {
            startAP();
        }
    }
}

void loop() {
    // Tombol Factory Reset (Tahan 3 detik)
    if (digitalRead(RESET_BTN_PIN) == LOW) {
        static unsigned long holdTime = 0;
        if (holdTime == 0) holdTime = millis();
        if (millis() - holdTime > 3000) {
            pref.begin("config", false); pref.clear(); pref.end();
            ESP.restart();
        }
    } else {
        static unsigned long holdTime = 0;
        holdTime = 0;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) {
            static unsigned long lastRetry = 0;
            if (millis() - lastRetry > 5000) {
                lastRetry = millis();
                String clientId = "ESP32_Weather_" + WiFi.macAddress();
                
                // Terhubung ke broker Docker lokal secara anonim (tanpa token)
                if (mqttClient.connect(clientId.c_str())) {
                    Serial.println("Connected to Docker MQTT Broker!");
                }
            }
        }
        mqttClient.loop();

        unsigned long now = millis();
        if (now - lastMsg > interval) {
            lastMsg = now;
            float h = dht.readHumidity();
            float t = dht.readTemperature();
            bool sensorOk = !(isnan(h) || isnan(t));

            updateDisplay("Wifi", ssid, mqtt_ip, t, h, mqttClient.connected(), sensorOk);

            if (sensorOk && mqttClient.connected()) {
                String payload = "{";
                payload += "\"temperature\":" + String(t, 1) + ",";
                payload += "\"humidity\":" + String(h, 1);
                payload += "}";
                
                // Mengirim payload ke topik MQTT
                mqttClient.publish("v1/devices/me/telemetry", payload.c_str());
                Serial.println("Payload Sent: " + payload);
            }
        }
    }
}