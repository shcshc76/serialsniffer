// Mit einem ESP32 getestet.
// Die Daten werden an einen Syslog-Server gesendet.
// Die Daten können auch an eine URL gesendet werden, z.B. an einen Webserver.
// Die Konfiguration wird in den Preferences gespeichert und beim Start geladen.
// Die Baudrate, Datenbits, Parität und Stoppbits können konfiguriert werden.
// Über h oder ? wird eine Hilfe angezeigt.

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Syslog.h>
#include <HTTPClient.h>
#include <UrlEncode.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define MON_RX 17 // RX pin
#define MON_TX 16 // TX pin

#define MON_BAUD 9600 // initial baud rate

#define MON_BUF_SIZE 128

#define DUMMY_PIN1 13 // Dummy pin for unused TX
#define DUMMY_PIN2 14 // Dummy pin for unused TX

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

HardwareSerial SerialRX(1); // Receiver RX
HardwareSerial SerialTX(2); // Receiver TX

uint8_t rxBuf[MON_BUF_SIZE], txBuf[MON_BUF_SIZE];
size_t rxLen = 0, txLen = 0;
unsigned long rxLast = 0, txLast = 0;

// Serial config state
unsigned long currentBaud = MON_BAUD;
uint8_t currentDataBits = 8;
char currentParity = 'N';
uint8_t currentStopBits = 1;
SerialConfig currentContig = SERIAL_8N1;
bool rxInvert = false;
bool txInvert = false;
uint16_t timeout = 15;
bool eolDetect = false;

// WLAN configuration
String wifiSSID = "WLAN_SSID";
String wifiPass = "WLAN_PASS";
String targetURL = "";
bool wifiConnected = false;

// 🔹 Syslog Server Settings (Replace with your server IP)
String syslog_ip = "SYSLOG_IP"; // Syslog Server IP
const int syslog_port = 514;    // Default UDP Syslog port

// WiFiUDP ntpUDP;
WiFiUDP udpClient;
NTPClient timeClient(udpClient, "pool.ntp.org", 0, 600000); // Refresh every 10 minutes

// Timezone offsets for Central Europe
const long standardOffset = 3600; // GMT+1
const long daylightOffset = 7200; // GMT+2

//  🔹 Create Syslog Client wifiSSID.c_str()
Syslog syslog(udpClient, syslog_ip.c_str(), syslog_port, "esp32", "serialsniffer", LOG_LOCAL0);

//  🔹 Create AsyncWebServer instance
AsyncWebServer server(80);
const char *PARAM_INPUT_1 = "input1";
// HTML web page to handle 3 input fields (input1)
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <title>ESP32 Serial Sniffer</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <!-- Bootstrap 5 CDN -->
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
  <style>
    body {
      background-color: #0e0e0e;
      color: #0f0;
      font-family: monospace;
      padding-top: 2rem;
    }
    .card {
      background-color: #111;
      border: 1px solid #333;
      color: #0f0;
    }
    .form-control, .btn {
      border-radius: 0;
    }
    label {
      font-size: 0.9rem;
    }
    .btn-success {
      background-color: #28a745;
      border-color: #28a745;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1 class="mb-4 text-center">ESP32 Serial Sniffer</h1>
<div class="card p-3 mb-4">
<h5 class="card-title">Connect to WLAN</h1>
  <form id="wifiForm">
    <label for="ssid">WLAN SSID</label>
    <input type="text" id="ssid" name="ssid" placeholder="SSID" required />

    <label for="password">WLAN Passwort</label>
    <input type="password" id="password" name="password" placeholder="Password" />

    <button type="submit" class="btn btn-success">Connect</button>
        <div class="d-grid gap-2 d-md-flex justify-content-md-between">
      <a href="/log" class="btn btn-outline-light">Show Live Log</a>
    </div>

  </form>
  <div class="info" id="responseMsg"></div>
  </div>
  <script>
    const form = document.getElementById('wifiForm');
    const responseMsg = document.getElementById('responseMsg');

    form.addEventListener('submit', function(e) {
      e.preventDefault();
      const ssid = document.getElementById('ssid').value;
      const password = document.getElementById('password').value;

      fetch(`/connect?ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`)
        .then(response => response.text())
        .then(text => {
          responseMsg.textContent = text;
        })
        .catch(err => {
          responseMsg.textContent = 'Fehler beim Verbinden: ' + err;
        });
    });
  </script>

    <div class="card p-3 mb-4">
      <h5 class="card-title">Device status</h5>
      <div id="status">Wird geladen...</div>
    </div>

    <div class="card p-3 mb-4">
      <h5 class="card-title">Send Command</h5>
      <form id="cmdForm" onsubmit="sendCommand(); return false;">
        <div class="input-group">
          <input type="text" id="cmdInput" class="form-control" placeholder="e. g. b9600, Ti, X">
          <button type="submit" class="btn btn-success">Send</button>
        </div>
      </form>
    </div>


    <div class="card p-3 mt-4">
  <h5 class="card-title">Available commands</h5>
  <pre style="color: #0f0; background-color: #000; padding: 1rem;">
b(baud)        - Set baud rate (e.g., b9600)
B(data_bits)   - Set data bits (5-8, e.g., B8)
s(stop_bits)   - Set stop bits (1 or 2, e.g., s2)
N              - No parity
E              - Even parity
O              - Odd parity
Ri|RI          - Disable or enable RX pin inversion
Ti|TI          - Disable or enable TX pin inversion
p              - Print current serial configuration
f              - Flush RX and TX buffers
r              - Reinitialize serial ports
Y(ip)          - Set syslog IP (e.g., Y192.168.1.100)
U(url)         - Set HTTP target URL
D(level)       - Set debug level (e.g. D1 for basic, D2 for verbose)
t(timeout)     - Set timeout in ms (e.g., t1000)
L              - Enable EOL detection
l              - Disable EOL detection
W(WLAN_SSID)   - Set WiFi SSID (e.g., WMyNetwork)
w(WLAN_PASS)   - Set WiFi password (e.g., wMyPassword)
S              - Save config
X              - Restart device
? or h         - Help
Note: Commands are case-sensitive.
  </pre>
</div>


    <footer class="mt-5 text-center text-muted" style="font-size: 0.8rem;">
      ESP32 Serial Sniffer – Webinterface v1.0
    </footer>
  </div>

  <script>
    function fetchStatus() {
      fetch('/status')  // passt du ggf. an deine API an
        .then(res => res.text())
        .then(data => {
          document.getElementById('status').innerText = data;
        });
    }

    function sendCommand() {
      const cmd = document.getElementById('cmdInput').value;
      if (!cmd) return;
      fetch('/get?input1=' + encodeURIComponent(cmd)).then(() => {
        document.getElementById('cmdInput').value = '';
        fetchStatus();
      });
    }

    fetchStatus();
  </script>
</body>
</html>
)rawliteral";

String webLogBuffer = "";
const size_t WEB_LOG_MAX = 8192; // max. Größe in Bytes

uint8_t outputLevel = 2; // Verbosity

// Preferences for saving configuration
Preferences prefs;

String outBuffer = "";

void parseSerialCommand(String cmd); // Parse and execute serial commands
void tryWiFiConnect();               // Attempt to connect to WiFi
void displayMessage(String message); // Display message on OLED

// webserver request handler
void notFound(AsyncWebServerRequest *request)
{
  request->send(404, "text/plain", "Not found");
}

// Function to get current date and time as a formatted string
String getDateTimeString()
{
  if (timeClient.isTimeSet())
  {
    time_t rawTime = timeClient.getEpochTime();
    struct tm *timeInfo = gmtime(&rawTime);

    int day = timeInfo->tm_mday;
    int month = timeInfo->tm_mon + 1;
    int weekday = timeInfo->tm_wday;

    // DST logic: Central Europe (last Sunday of March to last Sunday of October)
    bool dstActive = false;
    if (month > 3 && month < 10)
    {
      dstActive = true;
    }
    else if (month == 3 || month == 10)
    {
      int lastSunday = 31 - ((weekday + 31 - day) % 7);
      if (month == 3 && day >= lastSunday)
        dstActive = true;
      if (month == 10 && day < lastSunday)
        dstActive = true;
    }

    // Apply correct offset
    long offset = dstActive ? daylightOffset : standardOffset;
    timeInfo->tm_hour += offset / 3600;

    // Format string
    char buffer[30];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
             timeInfo->tm_year + 1900,
             timeInfo->tm_mon + 1,
             timeInfo->tm_mday,
             timeInfo->tm_hour,
             timeInfo->tm_min,
             timeInfo->tm_sec);

    return "NTP " + String(buffer);
  }
  else
  {
    return "LOCAL " + String(millis());
  }
}

void sendBuffer()
{
  if (wifiConnected && outBuffer.length() > 0)
  {
    if (targetURL.length() > 0) // Send Data to URL
    {
      HTTPClient http;
      http.begin(targetURL);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");

      String postBody = "data=" + urlEncode(outBuffer);
      int httpResponseCode = http.POST(postBody);

      http.end();
      //@TODO
      if (httpResponseCode > 0)
      {
        if (outputLevel >= 4)
        {
          Serial.println("#### HTTP data sent: " + outBuffer);
        }
      }
      else
      {
        if (outputLevel >= 2)
        {
          Serial.print("HTTP call failed - Error code: ");
          Serial.println(httpResponseCode);
        }
      }
    }
    if (syslog_ip.length() > 0) // Send Data to Syslog
    {
      syslog.log(LOG_INFO, outBuffer.c_str()); // Send to Syslog server
    }
  }
  else if (outputLevel >= 4)
  {
    Serial.println("#### WiFi not connected or buffer empty, skipping send");
  }

  outBuffer = ""; // Clear buffer
}

void textOutln(String text = "", uint8_t level = 1)
{
  if (level > outputLevel)
    return;
  Serial.println(text);
  outBuffer += text + "\n";
  // WebLog anhängen
  webLogBuffer += text + "\n";
  if (webLogBuffer.length() > WEB_LOG_MAX)
  {
    webLogBuffer.remove(0, webLogBuffer.length() / 2);
  }
  sendBuffer();
}

void textOut(String text = "", uint8_t level = 1)
{
  if (level > outputLevel)
    return;
  Serial.print(text);
  outBuffer += text;

  // WebLog anhängen
  webLogBuffer += text;
  if (webLogBuffer.length() > WEB_LOG_MAX)
  {
    webLogBuffer.remove(0, webLogBuffer.length() / 2); // ältere Hälfte löschen
  }

  if (outBuffer.length() > 1024)
  { // Limit buffer size to prevent overflow
    Serial.println("# Output buffer overflow, clearing...");
    sendBuffer();
  }
}

void saveSerialConfig() // Save alle Data
{
  prefs.begin("serialsniff", false);
  prefs.putUInt("baud", currentBaud);
  prefs.putUChar("bits", currentDataBits);
  prefs.putChar("parity", currentParity);
  prefs.putUChar("stop", currentStopBits);
  prefs.putBool("rxInvert", rxInvert);
  prefs.putBool("txInvert", txInvert);
  prefs.putString("ssid", wifiSSID);
  prefs.putString("wpass", wifiPass);
  prefs.putString("url", targetURL);
  prefs.putString("syslog_ip", syslog_ip);
  prefs.putUShort("debug", outputLevel);
  prefs.putUInt("timeout", timeout);
  prefs.putBool("eoldetect", eolDetect);
  prefs.end();
  textOutln("# Config saved");
}

bool loadSerialConfig() // Load saved config
{
  prefs.begin("serialsniff", true);
  if (!prefs.isKey("baud"))
  {
    prefs.end();
    textOutln("# Keine Daten gespeichert");
    return false;
  }
  currentBaud = prefs.getUInt("baud", MON_BAUD);
  currentDataBits = prefs.getUChar("bits", 8);
  currentParity = prefs.getChar("parity", 'N');
  currentStopBits = prefs.getUChar("stop", 1);
  rxInvert = prefs.getBool("rxInvert", false);
  txInvert = prefs.getBool("txInvert", false);
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("wpass", "");
  targetURL = prefs.getString("url", "");
  syslog_ip = prefs.getString("syslog_ip", "");
  outputLevel = prefs.getUShort("debug", 2);
  timeout = prefs.getUInt("timeout", 15);
  eolDetect = prefs.getBool("eoldetect", false);
  prefs.end();
  textOutln("# Saved config restored");
  return true;
}

SerialConfig calcSerialConfig(void) // Calculate current serial config based on data bits, parity and stop bits
{
  if (currentDataBits == 5)
  {
    if (currentStopBits == 1)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_5N1;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_5E1;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_5O1;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else if (currentStopBits == 2)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_5N2;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_5E2;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_5O2;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else
    {
      textOutln("# Invalid stop bits");
    }
  }
  else if (currentDataBits == 6)
  {
    if (currentStopBits == 1)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_6N1;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_6E1;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_6O1;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else if (currentStopBits == 2)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_6N2;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_6E2;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_6O2;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else
    {
      textOutln("# Invalid stop bits");
    }
  }
  else if (currentDataBits == 7)
  {
    if (currentStopBits == 1)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_7N1;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_7E1;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_7O1;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else if (currentStopBits == 2)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_7N2;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_7E2;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_7O2;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else
    {
      textOutln("# Invalid stop bits");
    }
  }
  else if (currentDataBits == 8)
  {
    if (currentStopBits == 1)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_8N1;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_8E1;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_8O1;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else if (currentStopBits == 2)
    {
      if (currentParity == 'N')
      {
        currentContig = SERIAL_8N2;
      }
      else if (currentParity == 'E')
      {
        currentContig = SERIAL_8E2;
      }
      else if (currentParity == 'O')
      {
        currentContig = SERIAL_8O2;
      }
      else
      {
        textOutln("# Invalid parity");
      }
    }
    else
    {
      textOutln("# Invalid stop bits");
    }
  }
  else
  {
    textOutln("# Invalid data bits");
  }
  return currentContig;
}

void applySerialConfig(bool calc = false, bool init = false)
{
  if (calc)
  {
    calcSerialConfig();
  }
  if (!init)
  {
    SerialRX.end();
    SerialTX.end();
  }

  SerialRX.begin(currentBaud, currentContig, MON_RX, DUMMY_PIN1);
  SerialTX.begin(currentBaud, currentContig, MON_TX, DUMMY_PIN2);
  SerialRX.setRxInvert(rxInvert);
  SerialTX.setRxInvert(txInvert);
  textOutln("## Serial ports reconfigured", 2);
}

void startWebserver()
{
  // Send web page with input fields to client
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/html", index_html); });

  // Status anzeigen
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  String status = "IP: " + WiFi.localIP().toString();
  status += "\nSSID: " + wifiSSID;
  status += "\nBaudrate: " + String(currentBaud);
  status += ", Data Bits: " + String(currentDataBits);
  status += ", Parity: " + String(currentParity);
  status += ", Stop Bits: " + String(currentStopBits);
  status += "\nRX Invert: " + String(rxInvert ? "Enabled" : "Disabled");
  status += ", TX Invert: " + String(txInvert ? "Enabled" : "Disabled");
  status += "\nSyslog IP: " + syslog_ip;
  status += ", Target URL: " + targetURL;
  status += "\nOutput Level: " + String(outputLevel);
  status += "\nEOL Detect: " + String(eolDetect ? "Enabled" : "Disabled");
  status += "\nTimeout: " + String(timeout) + " ms";
  // weitere Infos
  request->send(200, "text/plain", status); });

  server.on("/connect", HTTP_GET, [](AsyncWebServerRequest *request)
            {

  if (request->hasParam("ssid")) {
    wifiSSID = request->getParam("ssid")->value();
  }
  if (request->hasParam("password")) {
    wifiPass = request->getParam("password")->value();
  }

  // Hier WLAN-Verbindungslogik einbauen, z.B. speichern und neustarten
  tryWiFiConnect();
  // Dann Antwort senden:

  String response = "Verbindungsversuch mit SSID: " + wifiSSID + " gestartet.";
  request->send(200, "text/plain", response); });

  // Serve the log data
  server.on("/logdata", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", webLogBuffer); });

  server.on("/clearlog", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  webLogBuffer = "";
  request->send(200, "text/plain", "Log gelöscht."); });

  server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String html = R"rawliteral(
      <!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <title>ESP32 Serial Log (Live)</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <!-- Bootstrap 5 CDN -->
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
  <style>
    body {
      background-color: #0e0e0e;
      color: #00ff00;
      font-family: monospace;
      padding: 2rem;
    }
    #log {
      white-space: pre-wrap;
      background-color: #000;
      padding: 1rem;
      border: 1px solid #333;
      height: 60vh;
      overflow-y: auto;
      font-size: 0.95rem;
    }
    input.form-control {
      background-color: #111;
      color: #0f0;
      border: 1px solid #333;
    }
  </style>
</head>
<body>
  <div class="container">
    <h5 class="mb-4">ESP32 Serial Log</h1>
    <div id="log">Lade Log...</div>
<div class="mt-3">
      <button onclick="clearLog()" class="btn btn-danger">Clear Log</button>
      <a href="/" class="btn btn-secondary ms-2">Back to Home</a>
    </div>
    <form id="cmdForm" class="row g-2 mt-3" onsubmit="sendCommand(); return false;">
      <div class="col-md-8">
        <h5 class="card-title">Send Command</h5>
        <div class="input-group">
        <input type="text" id="cmdInput" class="form-control" placeholder="Insert Command (e. g. 'b9600', 'h')" required>
        <button type="submit" class="btn btn-success">Send</button>
      </div>
      </div>
    </form>

    
  </div>
 <div class="container">
  <h5 class="card-title">Available commands</h5>
  <pre style="color: #0f0; background-color: #000; padding: 1rem;">
b(baud)        - Set baud rate (e.g., b9600)
B(data_bits)   - Set data bits (5-8, e.g., B8)
s(stop_bits)   - Set stop bits (1 or 2, e.g., s2)
N              - No parity
E              - Even parity
O              - Odd parity
Ri|RI          - Disable or enable RX pin inversion
Ti|TI          - Disable or enable TX pin inversion
p              - Print current serial configuration
f              - Flush RX and TX buffers
r              - Reinitialize serial ports
Y(ip)          - Set syslog IP (e.g., Y192.168.1.100)
U(url)         - Set HTTP target URL
D(level)       - Set debug level (e.g. D1 for basic, D2 for verbose)
t(timeout)     - Set timeout in ms (e.g., t1000)
L              - Enable EOL detection
l              - Disable EOL detection
W(WLAN_SSID)   - Set WiFi SSID (e.g., WMyNetwork)
w(WLAN_PASS)   - Set WiFi password (e.g., wMyPassword)
S              - Save config
X              - Restart device
? or h         - Help
Note: Commands are case-sensitive.
  </pre>
</div>
  <script>
    function fetchLog() {
      fetch('/logdata')
        .then(res => res.text())
        .then(data => {
          const logElem = document.getElementById('log');
          logElem.textContent = data;
          logElem.scrollTop = logElem.scrollHeight;
        });
    }

    function clearLog() {
      fetch('/clearlog').then(() => fetchLog());
    }

    function sendCommand() {
      const cmd = document.getElementById('cmdInput').value;
      if (!cmd) return;
      fetch('/get?input1=' + encodeURIComponent(cmd)).then(() => {
        document.getElementById('cmdInput').value = '';
        fetchLog();
      });
    }

    setInterval(fetchLog, 2000);
    fetchLog();
  </script>
</body>
</html>

    )rawliteral";

    request->send(200, "text/html", html); });

  // Send a GET request to <ESP_IP>/get?input1=<inputMessage>
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        String inputMessage;
        String inputParam;
        // GET input1 value on <ESP_IP>/get?input1=<inputMessage>
        if (request->hasParam(PARAM_INPUT_1)) {
          inputMessage = request->getParam(PARAM_INPUT_1)->value();
          inputParam = PARAM_INPUT_1;
        }
        else {
          inputMessage = "No message sent";
          inputParam = "none";
        }
        // Serial.println(inputMessage);
        parseSerialCommand(inputMessage);
        request->send(200, "text/html", "HTTP GET request sent to your ESP (" 
                                     + inputParam + ") with value: " + inputMessage +
                                     "<br><a href=\"/\">Return to Home Page</a>"); });
  server.onNotFound(notFound);
  server.begin();
  textOutln("## Web server started on port 80", 2);
}

void tryWiFiConnect() // Connect to WiFi and NTP server
{
  if (wifiSSID.length() > 0)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      textOut("# Connecting to WiFi: " + wifiSSID);
      WiFi.mode(WIFI_STA);
      WiFi.setHostname("SerialSniffer"); // Set hostname for WiFi
      WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
      {
        delay(500);
        textOut(".");
      }
      if (WiFi.status() == WL_CONNECTED)
      {
        wifiConnected = true;
        delay(250); // DNS might fail otherwise
        textOutln("OK");
        textOutln("## WiFi connected, IP: " + WiFi.localIP().toString(), 2);
        displayMessage("OK IP: " + WiFi.localIP().toString());
        startWebserver(); // Start web server

        // Start the NTP client
        timeClient.begin();
        timeClient.forceUpdate();
        if (timeClient.isTimeSet())
        {
          textOutln("### NTP time synced successfully", 3);
        }
        else
        {
          textOutln("### NTP time NOT synced.", 2);
        }
      }
      else
      {
        textOutln("ERR");
        wifiConnected = false;
        WiFi.mode(WIFI_AP);
        WiFi.softAP("SerialSniffer_Config");
        IPAddress IP = WiFi.softAPIP();
        textOutln("## Fallback-AP gestartet. IP: " + IP.toString());
        textOutln("## WiFi connection failed, check SSID and password", 2);
        textOutln("## Please connect to the fallback AP and configure WiFi settings", 2);
        textOutln("## Use the web server to set WiFi SSID and password", 2);
        displayMessage("NOK IP: " + IP.toString());
        startWebserver(); // Start web server
      }
    }
    else
    {
      wifiConnected = true;
    }
  }
  else
  {
    textOutln("## WiFi SSID or URL target IP/port not set, skipping network connection", 2);
    wifiConnected = false;
  }
}

void appendHex(String &str, uint8_t val)
{
  char buf[6]; // "0x" + 2 digits + null = 5, 6 for safety
  snprintf(buf, sizeof(buf), "0x%02X", val);
  str += buf;
}

void printBuffer(const char *type, uint8_t *buf, size_t len) // Print buffer with time, type, HEX and ASCII representation
{
  // Print time and type
  String line;
  char code[8];

  line += getDateTimeString() + ';' + String(type) + ';';

  // Print HEX
  for (size_t i = 0; i < len; i++)
  {
    if (i)
      line += " ";
    appendHex(line, buf[i]);
  }
  line += ";";
  // Print ASCII
  for (size_t i = 0; i < len; i++)
  {
    if (buf[i] >= 32 && buf[i] <= 126)
    {
      line += (String((char)buf[i]));
    }
    else if (buf[i] >= 0xA0)
    {
      line += (String((char)buf[i]));
    }
    else if (buf[i] == 0x00)
    {
      line += ("<NUL>");
    }
    else if (buf[i] == 0x01)
    {
      line += ("<SOH>");
    }
    else if (buf[i] == 0x02)
    {
      line += ("<STX>");
    }
    else if (buf[i] == 0x03)
    {
      line += ("<ETX>");
    }
    else if (buf[i] == 0x04)
    {
      line += ("<EOT>");
    }
    else if (buf[i] == 0x05)
    {
      line += ("<ENQ>");
    }
    else if (buf[i] == 0x06)
    {
      line += ("<ACK>");
    }
    else if (buf[i] == 0x07)
    {
      line += ("<BEL>");
    }
    else if (buf[i] == 0x08)
    {
      line += ("<BS>");
    }
    else if (buf[i] == 0x09)
    {
      line += ("<HT>");
    }
    else if (buf[i] == 0x0A)
    {
      line += ("<LF>");
    }
    else if (buf[i] == 0x0B)
    {
      line += ("<VT>");
    }
    else if (buf[i] == 0x0C)
    {
      line += ("<FF>");
    }
    else if (buf[i] == 0x0D)
    {
      line += ("<CR>");
    }
    else if (buf[i] == 0x0E)
    {
      line += ("<SO>");
    }
    else if (buf[i] == 0x0F)
    {
      line += ("<SI>");
    }
    else if (buf[i] == 0x10)
    {
      line += ("<DLE>");
    }
    else if (buf[i] == 0x11)
    {
      line += ("<DC1>");
    }
    else if (buf[i] == 0x12)
    {
      line += ("<DC2>");
    }
    else if (buf[i] == 0x13)
    {
      line += ("<DC3>");
    }
    else if (buf[i] == 0x14)
    {
      line += ("<DC4>");
    }
    else if (buf[i] == 0x15)
    {
      line += ("<NAK>");
    }
    else if (buf[i] == 0x16)
    {
      line += ("<SYN>");
    }
    else if (buf[i] == 0x17)
    {
      line += ("<ETB>");
    }
    else if (buf[i] == 0x18)
    {
      line += ("<CAN>");
    }
    else if (buf[i] == 0x19)
    {
      line += ("<EM>");
    }
    else if (buf[i] == 0x1A)
    {
      line += ("<SUB>");
    }
    else if (buf[i] == 0x1B)
    {
      line += ("<ESC>");
    }
    else if (buf[i] == 0x1C)
    {
      line += ("<FS>");
    }
    else if (buf[i] == 0x1D)
    {
      line += ("<GS>");
    }
    else if (buf[i] == 0x1E)
    {
      line += ("<RS>");
    }
    else if (buf[i] == 0x1F)
    {
      line += ("<US>");
    }
    else if (buf[i] == 0x7F)
    {
      line += ("<DEL>");
    }
    else if (buf[i] == 0x80)
    {
      line += ("<PAD>");
    }
    else if (buf[i] == 0x81)
    {
      line += ("<HOP>");
    }
    else if (buf[i] == 0x82)
    {
      line += ("<BPH>");
    }
    else if (buf[i] == 0x83)
    {
      line += ("<NBH>");
    }
    else if (buf[i] == 0x84)
    {
      line += ("<IND>");
    }
    else if (buf[i] == 0x85)
    {
      line += ("<NEL>");
    }
    else if (buf[i] == 0x86)
    {
      line += ("<SSA>");
    }
    else if (buf[i] == 0x87)
    {
      line += ("<ESA>");
    }
    else if (buf[i] == 0x88)
    {
      line += ("<HTS>");
    }
    else if (buf[i] == 0x89)
    {
      line += ("<HTJ>");
    }
    else if (buf[i] == 0x8A)
    {
      line += ("<VTS>");
    }
    else if (buf[i] == 0x8B)
    {
      line += ("<PLD>");
    }
    else if (buf[i] == 0x8C)
    {
      line += ("<PLU>");
    }
    else if (buf[i] == 0x8D)
    {
      line += ("<RI>");
    }
    else if (buf[i] == 0x8E)
    {
      line += ("<SS2>");
    }
    else if (buf[i] == 0x8F)
    {
      line += ("<SS3>");
    }
    else if (buf[i] == 0x90)
    {
      line += ("<DCS>");
    }
    else if (buf[i] == 0x91)
    {
      line += ("<PU1>");
    }
    else if (buf[i] == 0x92)
    {
      line += ("<PU2>");
    }
    else if (buf[i] == 0x93)
    {
      line += ("<STS>");
    }
    else if (buf[i] == 0x94)
    {
      line += ("<CCH>");
    }
    else if (buf[i] == 0x95)
    {
      line += ("<MW>");
    }
    else if (buf[i] == 0x96)
    {
      line += ("<SPA>");
    }
    else if (buf[i] == 0x97)
    {
      line += ("<EPA>");
    }
    else if (buf[i] == 0x98)
    {
      line += ("<SOS>");
    }
    else if (buf[i] == 0x99)
    {
      line += ("<SGCI>");
    }
    else if (buf[i] == 0x9A)
    {
      line += ("<SCI>");
    }
    else if (buf[i] == 0x9B)
    {
      line += ("<CSI>");
    }
    else if (buf[i] == 0x9C)
    {
      line += ("<ST>");
    }
    else if (buf[i] == 0x9D)
    {
      line += ("<OSC>");
    }
    else if (buf[i] == 0x9E)
    {
      line += ("<PM>");
    }
    else if (buf[i] == 0x9F)
    {
      line += ("<APC>");
    }
    else
    {
      char code[8];
      snprintf(code, sizeof(code), "<%02X>", buf[i]);
      line += code;
    }
  }
  textOutln(line, 0);
}

void parseSerialCommand(String cmd) // Parse and execute serial commands
{
  cmd.trim();
  if (cmd.length() < 1)
    return;
  char c = cmd[0];
  String val = cmd.substring(1);
  if (c == 'b')
  { // Set baud rate
    unsigned long baud = val.toInt();
    if (baud > 0)
    {
      currentBaud = baud;
      applySerialConfig();
      textOutln("# Baudrate changed to " + String(currentBaud));
    }
  }
  else if (c == 'B')
  { // Set data bits
    int bits = val.toInt();
    if (bits >= 5 && bits <= 8)
    {
      currentDataBits = bits;
      applySerialConfig(true);
      textOutln("# Data bits set to " + String(currentDataBits));
    }
  }
  else if (c == 's')
  { // Set stop bits
    int stops = val.toInt();
    if (stops == 1 || stops == 2)
    {
      currentStopBits = stops;
      applySerialConfig(true);
      textOutln("# Stop bits set to " + String(currentStopBits));
    }
  }
  else if (c == 'N' || c == 'E' || c == 'O')
  { // Set parity
    currentParity = c;
    applySerialConfig(true);
    textOutln("# Parity set to " + String(c));
  }
  else if (cmd == "Ri" || cmd == "RI")
  { // Disable RX pin inversion
    if (cmd == "Ri")
    {
      rxInvert = false;
      applySerialConfig(true);
      textOutln("# RX pin inversion disabled");
    }
    else
    {
      rxInvert = true;
      applySerialConfig(true);
      textOutln("# RX pin inversion enabled");
    }
  }
  else if (cmd == "Ti" || cmd == "TI")
  { // Disable TX pin inversion
    if (cmd == "Ti")
    {
      txInvert = false;
      applySerialConfig(true);
      textOutln("# TX pin inversion disabled");
    }
    else
    {
      txInvert = true;
      applySerialConfig(true);
      textOutln("# TX pin inversion enabled");
    }
  }
  else if (c == 'p')
  { // Print current config
    textOut("# Current Config: ");
    textOut("Baud: " + String(currentBaud) + ", ");
    textOut("Data Bits: " + String(currentDataBits) + ", ");
    textOut("Parity: " + String(currentParity) + ", ");
    textOut("Stop Bits: " + String(currentStopBits) + ", ");
    textOut("RX Invert: " + String(rxInvert ? "Enabled" : "Disabled") + ", ");
    textOut("TX Invert: " + String(txInvert ? "Enabled" : "Disabled"));
    textOutln();
    textOut("# Syslog IP: " + syslog_ip);
    textOutln();
    textOut("# WiFi SSID: " + wifiSSID + ", ");

    if (wifiPass.length() > 0)
    {
      textOut("WiFi Password: Set, ");
    }
    else
    {
      textOut("WiFi Password: Not Set, ");
    }
    textOut("URL Target: " + targetURL + ", ");
    if (wifiConnected)
    {
      textOut(" (Connected: " + WiFi.localIP().toString() + ")");
    }
    else
    {
      textOut(" (Not Connected)");
    }
    textOutln();
    textOut("# Debug Level: " + String(outputLevel) + ", ");
    textOut("Timeout: " + String(timeout) + " ms, ");
    textOut("EOL Detection: " + String(eolDetect ? "Enabled" : "Disabled"));
    textOutln();
  }
  else if (c == 'f')
  { // Flush buffers
    rxLen = 0;
    txLen = 0;
    rxLast = millis();
    txLast = millis();
    textOutln("# Buffers flushed");
  }
  else if (c == 'r')
  { // Reset Serial
    textOutln("# Resetting Serial...");
    applySerialConfig(false); // Reapply config
    rxLen = 0;
    txLen = 0;
    rxLast = millis();
    txLast = millis();
  }
  else if (c == 'W')
  { // Set WiFi SSID
    wifiSSID = val;
    textOutln("# WiFi SSID set to: " + wifiSSID);
    tryWiFiConnect();
  }
  else if (c == 'w')
  { // Set WiFi password
    wifiPass = val;
    textOutln("# WiFi password set");
    tryWiFiConnect();
  }
  else if (c == 'U')
  { // Set target URL
    targetURL = val;
    textOutln("# URL target set to: " + targetURL);
    tryWiFiConnect();
  }
  else if (c == 'Y')
  { // Set target URL
    syslog_ip = val;
    textOutln("# syslog target set to: " + syslog_ip);
    // textOutln("# Please restart the device with X to apply syslog changes");
    syslog.server(syslog_ip.c_str(), syslog_port); // aktualisiere Ziel-IP direkt
  }
  else if (c == 'D')
  { // Debug mode on
    outputLevel = val.toInt();
    textOutln("# Debug level " + String(outputLevel));
  }
  else if (c == 't')
  { // Set timeout
    unsigned long newTimeout = val.toInt();
    if (newTimeout > 0)
    {
      timeout = newTimeout;
      textOutln("# Timeout set to " + String(timeout) + " ms");
    }
    else
    {
      textOutln("# Invalid timeout value");
    }
  }
  else if (c == 'L')
  { // Enable EOL detection
    eolDetect = true;
    textOutln("# EOL detection enabled");
  }
  else if (c == 'l')
  { // Disable EOL detection
    eolDetect = false;
    textOutln("# EOL detection disabled");
  }
  else if (c == 'S')
  { // Save config
    saveSerialConfig();
  }
  else if (c == '?' || c == 'h')
  { // Print help
    textOutln("# Serial Sniffer Commands:");
    textOutln("# b<baud> - Set baud rate (e.g., b9600)");
    textOutln("# B<data_bits> - Set data bits (5-8, e.g., B8)");
    textOutln("# s<stop_bits> - Set stop bits (1 or 2, e.g., s1)");
    textOutln("# N   - Set parity to None");
    textOutln("# E   - Set parity to Even");
    textOutln("# O   - Set parity to Odd");
    textOutln("# Ri|RI - Disable or enable RX pin inversion");
    textOutln("# Ti|TI - Disable or enable TX pin inversion");
    textOutln("# p   - Print current serial configuration");
    textOutln("# f   - Flush RX and TX buffers");
    textOutln("# r   - Reinitialize serial ports");
    textOutln("# W<SSID> - Set WiFi SSID (e.g., WMyNetwork)");
    textOutln("# w<password> - Set WiFi password (e.g., wMyPassword)");
    textOutln("# U<URL> - Set URL target (e.g., Uhttp://example.com/data)");
    textOutln("# D<level> - Set debug level (e.g. D1 for basic, D2 for verbose)");
    textOutln("# t<timeout> - Set timeout in ms (e.g., t1000)");
    textOutln("# L   - Enable EOL detection");
    textOutln("# l   - Disable EOL detection");
    textOutln("# S   - Save current configuration");
    textOutln("# X   - Restart Device");
    textOutln("# Y<SYSLOG> - Set syslog target (e.g., YMySyslogServer)");
    textOutln("# ?/h - Show this help");
    textOutln("# Note: Commands are case-sensitive.");
  }
  else if (c == 'X')
  { // Restart Device
    textOutln("# Restarting device...");
    saveSerialConfig();
    ESP.restart();
  }
  else
  {
    textOutln("# Unknown command: '" + cmd + "' - '?' or 'h' for help");
  }
}

bool isEOLChar(uint8_t c)
{
  // Check if character is something indicating a data end
  if (c == 0x00 || c == 0x03 || c == 0x04 || c == 0x05 || c == 0x06 ||
      c == 0x0A || c == 0x0C || c == 0x0D || c == 0x15 || c == 0x17 ||
      c == 0x18 || c == 0x19 || c == 0x1C || c == 0x1D || c == 0x1E ||
      c == 0x1F)
  {
    return true;
  }

  return false;
}

String serialCmd = "";

void displayMessage(String message)
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println(message);
  display.display();
}


void setup()
{
  Serial.begin(115200); // Initialize USB console
  delay(5000);          // allow USB to initialize
  textOutln("# Serial Sniffer log");

// Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  { // 0x3C is the default I2C address
    Serial.println("SSD1306 allocation failed");
    for (;;)
      ;
  }
  else
  {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("Serial Sniffer started");
    display.display();
  }

  

  // saveSerialConfig(); // Save initial config if not already done
  if (loadSerialConfig())
  {
    tryWiFiConnect();
    calcSerialConfig();
  }
  applySerialConfig(false, true); // Apply initial serial configuration
  parseSerialCommand("p");        // Print initial config
  textOutln("## Monitoring RX pin: " + String(MON_RX), 2);
  textOutln("## Monitoring TX pin: " + String(MON_TX), 2);
}

void handleSerial(
    HardwareSerial &serial,
    const char *type,
    uint8_t *buf,
    size_t &len,
    unsigned long &last)
{
  while (serial.available())
  {
    char c = serial.read();
    last = millis();

    if (len == MON_BUF_SIZE - 2 || // Buffer is almost full
        (eolDetect &&
         len > 1 &&
         (isEOLChar(buf[len - 1]) && !isEOLChar(c))))
    {
      printBuffer(type, buf, len);
      len = 0;
    }

    if (len < MON_BUF_SIZE)
      buf[len++] = c;
  }

  if (len > 0 && (millis() - last > timeout))
  {
    printBuffer(type, buf, len);
    len = 0;
  }
}

void loop()
{
  if (wifiConnected && WiFi.status() != WL_CONNECTED)
  {
    textOutln("### Wifi connection dropped. Reconnecting.", 3);
    tryWiFiConnect();
  }

  if (wifiConnected && WiFi.status() == WL_CONNECTED)
  {
    timeClient.update();
  }

  while (Serial.available())
  {
    char sc = Serial.read();
    if (sc == '\n')
    {
      parseSerialCommand(serialCmd);
      serialCmd = "";
    }
    else if (sc != '\r')
    {
      serialCmd += sc;
    }
  }

  handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);
  handleSerial(SerialTX, "TX", txBuf, txLen, txLast);
}
