#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <RF24.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

#define CE_PIN 4
#define CSN_PIN 5

RF24 radio(CE_PIN, CSN_PIN);
const byte address[6] = "00001";

// WiFi Configuration
const char* ssid = "NARZO N65 5G";
const char* password = "12345678";

// Hardware Configuration
const int moistureSensorPin = A0;
const int relayPin = 0;

// Thresholds
int lowerThreshold = 400;
int upperThreshold = 500;

// System State
String mode = "Manual";
bool pumpStatus = false;
unsigned long timerDuration = 0;
unsigned long startTime = 0;

// Web Server
ESP8266WebServer server(80);
unsigned long lastWiFiCheck = 0;
const unsigned long checkInterval = 30000; // 30 seconds

// NRF24L01 Data
int receivedSoilMoisture = 0;

// API Configuration
const char* OPENWEATHER_API_KEY = "AnkitIsHero";
const char* GEMINI_API_KEY = "Welcome";
const char* LOCATION = "Kolkata";

// Weather and AI Data
String weatherForecast = "";
String aiRecommendation = "No recommendation yet";
unsigned long lastWeatherCheck = 0;
const unsigned long weatherCheckInterval = 3600000; // 1 hour

// Function Prototypes
void fetchWeatherForecast();
void analyzeWeatherWithAI();
int readSoilMoisture();
void controlPump(bool state);
String getMotorStatus();
void handleStatusUpdate();
void handlePumpControl();
void handleSetTimer();
void handleTimer();
void checkWiFiConnection();
void checkAutoMode();
String generatePage();
String escapeJsonString(const String& input);

void setup() {
    Serial.begin(115200);
    pinMode(relayPin, OUTPUT);
    digitalWrite(relayPin, LOW); // Ensure motor is off initially

    // Static IP Configuration
    IPAddress staticIP(192, 168, 182, 99);
    IPAddress gateway(192, 168, 216, 1);
    IPAddress subnet(255, 255, 255, 0);
    IPAddress dns(8, 8, 8, 8);

    WiFi.config(staticIP, gateway, subnet, dns);
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println("\nConnected to WiFi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Web Server Routes
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/html", generatePage());
    });
    server.on("/status", HTTP_GET, handleStatusUpdate);
    server.on("/control", HTTP_POST, handlePumpControl);
    server.on("/set_timer", HTTP_POST, handleSetTimer);
    server.begin();

    // NRF24L01 Setup
    if (!radio.begin()) {
        Serial.println("NRF24L01 initialization failed!");
        while (1);
    }
    radio.setPALevel(RF24_PA_LOW);
    radio.setChannel(108);
    radio.openReadingPipe(1, address);
    radio.startListening();
    Serial.println("NRF24L01 Receiver ready");

    // Initial weather check
    fetchWeatherForecast();
}

void loop() {
    server.handleClient();
    handleTimer();
    checkAutoMode();
    
    // Periodic WiFi check
    if (millis() - lastWiFiCheck > checkInterval) {
        lastWiFiCheck = millis();
        checkWiFiConnection();
    }

    // Periodic weather check
    if (millis() - lastWeatherCheck > weatherCheckInterval) {
        lastWeatherCheck = millis();
        fetchWeatherForecast();
    }

    // Read NRF24L01 data
    if (radio.available()) {
        int soilMoistureValue = 0;
        radio.read(&soilMoistureValue, sizeof(soilMoistureValue));
        receivedSoilMoisture = soilMoistureValue;
        Serial.print("Received Soil Moisture: ");
        Serial.println(receivedSoilMoisture);
    }
    
    delay(100);
}

void fetchWeatherForecast() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected - can't fetch weather");
        return;
    }
    
    WiFiClient client;
    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/forecast?q=" + String(LOCATION) + 
               "&appid=" + OPENWEATHER_API_KEY + "&units=metric&cnt=4"; // Get 4 forecasts (12 hours)
    
    Serial.println("Fetching weather data...");
    if (http.begin(client, url)) {
        int httpCode = http.GET();
        
        if (httpCode == HTTP_CODE_OK) {
            weatherForecast = http.getString();
            Serial.println("Weather data received");
            analyzeWeatherWithAI();
        } else {
            Serial.print("Error fetching weather data. Code: ");
            Serial.println(httpCode);
        }
        http.end();
    } else {
        Serial.println("Failed to begin HTTP connection");
    }
}

void analyzeWeatherWithAI() {
    // Step 1: Check connection and weather data
    if (WiFi.status() != WL_CONNECTED) {
        aiRecommendation = "No WiFi connection";
        return;
    }

    if (weatherForecast.isEmpty()) {
        aiRecommendation = "No weather data";
        return;
    }

    // Step 2: Prepare HTTPS client and HTTP request
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    http.setTimeout(15000);

    String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" + String(GEMINI_API_KEY);

    // Step 3: Create prompt
    String prompt = "Give irrigation advice (1 sentence) for: ";
    prompt += "Temperature: 25°C, ";
    prompt += "Rainfall: 3 mm. ";
    prompt += "Soil moisture thresholds: " + String(lowerThreshold) + "-" + String(upperThreshold) + ". ";
    prompt += "Format exactly: 'PumpStatus: [on/off] Recommendation: [action]. Reason: [reason]. '";

    // Step 4: Send request
    if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        String payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + prompt + "\"}]}]}";

        int httpCode = http.POST(payload);

        // Step 5: Handle response
        if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            int start = response.indexOf("\"text\": \"") + 9;

            if (start >= 9) {
                aiRecommendation = response.substring(start, response.indexOf("\"", start));
                
              Serial.print("Raw AI Response: ");
              Serial.println(aiRecommendation.c_str());

                              
            }
        } else {
            Serial.println("AI request failed. HTTP Code: " + String(httpCode));
        }

        http.end();
    } else {
        Serial.println("Failed to connect to Gemini API.");
    }
}



void checkAutoMode() {
    // Skip if not in Auto mode
    if (mode != "Auto") return;

    Serial.print("Processing AI Recommendation: ");
    Serial.println(aiRecommendation);

    bool aiCommandProcessed = false;
    int soilMoisture = readSoilMoisture();
    int currentReceivedSoilMoisture = receivedSoilMoisture;

    // Step 1: Always check if moisture exceeds or equals upper threshold
    if ((soilMoisture >= upperThreshold && currentReceivedSoilMoisture >= upperThreshold)) {
        if (pumpStatus) {
            controlPump(false);
            Serial.println("Pump OFF - Moisture >= upper threshold");
        }
        // Refresh AI analysis since moisture level changed
        analyzeWeatherWithAI();
        return; // Exit early, no need to process AI or fallback logic
    }

    // Step 2: Try to process AI command
    int index = aiRecommendation.indexOf("PumpStatus:");
    if (index != -1) {
        String pumpStateValue = aiRecommendation.substring(index + 11);
        pumpStateValue.trim();
        
        int endPos = pumpStateValue.indexOf(' ');
        if (endPos == -1) endPos = pumpStateValue.length();
        pumpStateValue = pumpStateValue.substring(0, endPos);
        pumpStateValue.toLowerCase();
        pumpStateValue.trim();

        Serial.print("Extracted PumpState: [");
        Serial.print(pumpStateValue);
        Serial.println("]");

        if (pumpStateValue == "on" || pumpStateValue == "1" || pumpStateValue == "true") {
            if (!pumpStatus) {
                controlPump(true);
                Serial.println("Pump ON - AI Command");
            }
            aiCommandProcessed = true;
        } 
        else if (pumpStateValue == "off" || pumpStateValue == "0" || pumpStateValue == "false") {
            if (pumpStatus) {
                controlPump(false);
                Serial.println("Pump OFF - AI Command");
            }
            aiCommandProcessed = true;
        }
    }

    // Step 3: Fallback to sensor-based control
    if (!aiCommandProcessed) {
        Serial.println("Falling back to sensor-based control");

        if (soilMoisture < lowerThreshold || currentReceivedSoilMoisture < lowerThreshold) {
            if (!pumpStatus) {
                controlPump(true);
                Serial.println("Pump ON - Soil moisture below threshold");
            }
        } 
        else {
            Serial.println("No action needed - Moisture in acceptable range");
        }
    }

    // Debug Info
    Serial.print("Soil Moisture: Local=");
    Serial.print(soilMoisture);
    Serial.print(", Remote=");
    Serial.println(currentReceivedSoilMoisture);
}



void handlePumpControl() {
    if (server.hasArg("action")) {
        String action = server.arg("action");

        if (action == "on") {
            controlPump(true);
        }

        if (action == "off") {
            controlPump(false);
        }

        if (action == "auto") {
            mode = "Auto";
            Serial.println("Mode set to Auto");

            // Run AI logic immediately
           // analyzeWeatherWithAI();
            checkAutoMode();
        }

        if (action == "manual") {
            mode = "Manual";
            Serial.println("Mode set to Manual");
        }
    }

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "OK");
}

void controlPump(bool state) {
    pumpStatus = state;
    digitalWrite(relayPin, state ? HIGH : LOW); // LOW turns motor ON
    Serial.print("Pump turned ");
    Serial.println(state ? "ON" : "OFF");
}

String escapeJsonString(const String& input) {
    String output;
    output.reserve(input.length() * 2); // Reserve space for escaped characters
    
    for (size_t i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c <= '\x1f') {
                    // Convert control characters to Unicode escape sequence
                    output += "\\u";
                    char buf[5];
                    sprintf(buf, "%04x", c);
                    output += buf;
                } else {
                    output += c;
                }
        }
    }
    
    return output;
}

int readSoilMoisture() {
    int rawValue = analogRead(moistureSensorPin);
    rawValue = 1024 - rawValue; // Invert the reading
    return rawValue;
}


String getMotorStatus() {
    return pumpStatus ? "ON" : "OFF";
}

void handleStatusUpdate() {
    DynamicJsonDocument doc(512);
    doc["soilMoisture"] = readSoilMoisture();
    doc["receivedSoilMoisture"] = receivedSoilMoisture;
    doc["motorStatus"] = getMotorStatus();
    doc["lowerThreshold"] = lowerThreshold;
    doc["upperThreshold"] = upperThreshold;
    doc["mode"] = mode;
    doc["aiRecommendation"] = aiRecommendation;
    
    unsigned long remainingTime = (timerDuration > 0) ? 
        (startTime + timerDuration - millis()) / 1000 : 0;
    doc["remainingTime"] = remainingTime;
    
    String json;
    serializeJson(doc, json);
    
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}




void handleSetTimer() {
    if (server.hasArg("timer")) {
        unsigned long minutes = server.arg("timer").toInt();
        timerDuration = minutes * 60000;
        startTime = millis();
        controlPump(true);
        Serial.print("Timer set for ");
        Serial.print(minutes);
        Serial.println(" minutes");
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "Timer set");
}

void handleTimer() {
    if (timerDuration > 0 && millis() - startTime >= timerDuration) {
        controlPump(false);
        timerDuration = 0;
        Serial.println("Timer completed - pump turned off");
    }
}

void checkWiFiConnection() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected, reconnecting...");
        WiFi.begin(ssid, password);
        unsigned long startAttemptTime = millis();
        
        while (WiFi.status() != WL_CONNECTED && 
               millis() - startAttemptTime < 15000) {
            delay(500);
            Serial.print(".");
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\nReconnected to WiFi");
        } else {
            Serial.println("\nFailed to reconnect");
        }
    }
}

String generatePage() {
  String html = R"=====( 
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Irrigation System</title>
    <style>
        body {
            font-family: 'Segoe UI', sans-serif;
            margin: 0;
            padding: 20px;
            background-color: #eef6f3;
            color: #333;
        }
        .container {
            max-width: 900px;
            margin: auto;
            background-color: #fff;
            padding: 25px;
            border-radius: 12px;
            box-shadow: 0 4px 12px rgba(0,0,0,0.1);
        }
        h1, h2 {
            text-align: center;
            margin: 0;
        }
        h1 { color: #2e7d32; }
        h2 { color: #388e3c; font-size: 1.1em; margin-bottom: 20px; }

        .status-panel {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            margin-bottom: 25px;
        }
        .status-item {
            background: #e8f5e9;
            padding: 15px;
            border-radius: 8px;
        }
        .status-label {
            font-weight: bold;
            color: #2e7d32;
        }
        .status-value {
            font-size: 1.2em;
            margin-top: 5px;
        }

        .field-grid {
            display: flex;
            justify-content: center;
            gap: 30px;
            margin: 30px 0;
        }
        .sensor-block {
            text-align: center;
        }
        .sensor-label {
            font-weight: bold;
            margin-bottom: 10px;
        }
        .moisture-bar {
            width: 40px;
            height: 160px;
            background-color: #cfd8dc;
            border-radius: 10px;
            position: relative;
            margin: 0 auto;
            overflow: hidden;
        }
        .moisture-fill {
            position: absolute;
            bottom: 0;
            width: 100%;
            border-radius: 10px;
            background: linear-gradient(to top, #43a047, #81c784);
            transition: height 0.3s ease;
        }

        .ai-recommendation {
            background-color: #e3f2fd;
            padding: 15px;
            border-left: 4px solid #1976d2;
            margin-bottom: 25px;
            border-radius: 8px;
        }

        .control-panel {
            display: flex;
            flex-wrap: wrap;
            justify-content: center;
            gap: 10px;
            margin-bottom: 20px;
        }
        button {
            padding: 12px 20px;
            border: none;
            border-radius: 5px;
            font-weight: bold;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 3px 6px rgba(0,0,0,0.15);
        }
        .btn-on { background-color: #4caf50; color: white; }
        .btn-off { background-color: #f44336; color: white; }
        .btn-mode { background-color: #2196f3; color: white; }

        .timer-section {
            background-color: #fff3e0;
            padding: 15px;
            border-radius: 8px;
            text-align: center;
        }
        .timer-input {
            padding: 10px;
            border: 1px solid #ffa000;
            border-radius: 5px;
            width: 100px;
            text-align: center;
            margin-right: 10px;
        }
        .timer-button {
            background-color: #ffa000;
            color: white;
        }

        @media (max-width: 600px) {
            .field-grid {
                flex-direction: column;
                align-items: center;
            }
            .control-panel {
                flex-direction: column;
                gap: 12px;
            }
            button {
                width: 100%;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>JAMBAVANTHA</h1>
        <h2>AI Powered Smart Irrigation System</h2>

        <div class="status-panel">
            <div class="status-item"><div class="status-label">Soil Moisture</div><div class="status-value" id="soilMoisture">--</div></div>
            <div class="status-item"><div class="status-label">Remote Moisture</div><div class="status-value" id="receivedSoilMoisture">--</div></div>
            <div class="status-item"><div class="status-label">Pump Status</div><div class="status-value" id="motorStatus">--</div></div>
            <div class="status-item"><div class="status-label">Operation Mode</div><div class="status-value" id="mode">--</div></div>
            <div class="status-item"><div class="status-label">Lower Threshold</div><div class="status-value" id="lowerThreshold">--</div></div>
            <div class="status-item"><div class="status-label">Upper Threshold</div><div class="status-value" id="upperThreshold">--</div></div>
            <div class="status-item"><div class="status-label">Timer Remaining</div><div class="status-value" id="remainingTime">--</div></div>
        </div>

        <div class="field-grid">
            <div class="sensor-block">
                <div class="sensor-label">Sensor 1</div>
                <div class="moisture-bar"><div class="moisture-fill" id="soilMoistureBar" style="height:0%;"></div></div>
            </div>
            <div class="sensor-block">
                <div class="sensor-label">Sensor 2</div>
                <div class="moisture-bar"><div class="moisture-fill" id="receivedSoilMoistureBar" style="height:0%;"></div></div>
            </div>
        </div>

        <div class="ai-recommendation">
            <h3>AI Recommendation</h3>
            <p id="aiRecommendation">Loading...</p>
        </div>

        <div class="control-panel">
            <button class="btn-mode" onclick="controlPump('manual')">Manual Mode</button>
            <button class="btn-mode" onclick="controlPump('auto')">Auto Mode</button>
            <button class="btn-on" onclick="controlPump('on')">Turn Pump ON</button>
            <button class="btn-off" onclick="controlPump('off')">Turn Pump OFF</button>
        </div>

        <div class="timer-section">
            <h3>Set Pump Timer</h3>
            <input type="number" id="timerInput" class="timer-input" placeholder="Minutes" min="1">
            <button class="timer-button" onclick="setTimer()">Set Timer</button>
        </div>
    </div>

    <script>
        function getColor(value) {
            return value < 30 ? 'red' : value < 60 ? 'orange' : 'green';
        }

        function updateStatus() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('soilMoisture').textContent = data.soilMoisture;
                    document.getElementById('receivedSoilMoisture').textContent = data.receivedSoilMoisture;
                    document.getElementById('motorStatus').textContent = data.motorStatus;
                    document.getElementById('mode').textContent = data.mode;
                    document.getElementById('lowerThreshold').textContent = data.lowerThreshold;
                    document.getElementById('upperThreshold').textContent = data.upperThreshold;
                    document.getElementById('aiRecommendation').textContent = data.aiRecommendation;

                    const remainingTime = data.remainingTime;
                    document.getElementById('remainingTime').textContent =
                        remainingTime > 0 ? `${Math.floor(remainingTime/60)}m ${remainingTime%60}s` : 'Not active';

                    const soilPercent = Math.min((data.soilMoisture / 1024) * 100, 100);
                    const remotePercent = Math.min((data.receivedSoilMoisture / 1024) * 100, 100);



                    const soilBar = document.getElementById("soilMoistureBar");
                    const remoteBar = document.getElementById("receivedSoilMoistureBar");

                    soilBar.style.height = `${soilPercent}%`;
                    soilBar.style.backgroundColor = getColor(soilPercent);

                    remoteBar.style.height = remotePercent + '%';
                    remoteBar.style.backgroundColor = getColor(remotePercent);
                })
                .catch(error => console.error('Error fetching status:', error));
        }

        function controlPump(action) {
            fetch('/control', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'action=' + action
            }).then(() => updateStatus());
        }

        function setTimer() {
            const minutes = document.getElementById('timerInput').value;
            if (minutes && minutes > 0) {
                fetch('/set_timer', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'timer=' + minutes
                }).then(() => updateStatus());
            }
        }

        setInterval(updateStatus, 1000); // auto-refresh every 5 sec
        updateStatus(); // initial call
    </script>
</body>
</html>
)=====";
  return html;
}
