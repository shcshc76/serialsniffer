#include <Arduino.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Syslog.h>
#include <HTTPClient.h>
#include <UrlEncode.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "SPIFFS.h"
#include <TFT_eSPI.h> // Graphics and font library for ILI9341 driver chip
#include <SPI.h>
#include <IRremote.h>
#include <PubSubClient.h>

// TFT_eSPI settings
SPIClass hspi = SPIClass(HSPI);
bool tftOk = false;
int lineHeight = 12; // Höhe einer Textzeile (anpassen je nach Schriftart)
int currentLine = 0; // Aktuelle Zeilenposition
int maxLines;        // Maximale Anzahl Zeilen pro Bildschirmhöhe
const int maxCharsPerLine = 38;
TFT_eSPI tft = TFT_eSPI(); // Invoke library

#define MON_RX 5 // RX pin
#define MON_TX 6 // TX pin

#define MON_BAUD 9600 // initial baud rate

#define MON_BUF_SIZE 128

#define DUMMY_PIN1 45 // Dummy pin for unused TX
#define DUMMY_PIN2 42 // Dummy pin for unused TX

#define IR_RECEIVE_PIN 4 // Pin for IR receiver

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool displayOk = false;

// Serial configuration constants
#define SOH 0x01
#define STX 0x02
#define ETX 0x03
#define US 0x1F
#define RS 0x1E

// Clear OLED display afer x seconds
unsigned long displayClearTime = 0;
bool displayClearScheduled = false;

HardwareSerial SerialRX(1); // Receiver RX
HardwareSerial SerialTX(2); // Receiver TX

uint8_t rxBuf[MON_BUF_SIZE], txBuf[MON_BUF_SIZE];
size_t rxLen = 0, txLen = 0;
unsigned long rxLast = 0, txLast = 0;

// RX simulation state
bool rxSimActive = false;
unsigned long rxSimLastTime = 0;     // Letzte Zeit, zu der SOH Rundruf gesendet wurde
unsigned long rxSimInterval = 20000; // alle 20 Sekunden SOH Rundruf auf RX senden

unsigned long rxSimLastTimeHB = 0;    // Letzte Zeit, zu der Heartbeat gesendet wurde
unsigned long rxSimIntervalHB = 5000; // alle 5 Sekunden Heartbeat an RX senden

bool showHB = true;        // Heartbeat anzeigen
bool updateDisplay = true; // Flag to update display

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
String wifiPass = "WLAN_PASSWORD";
String targetURL = "";
bool wifiConnected = false;
IPAddress IP;

// MQTT settings
String mqttServer = "MQTT_SERVER"; // MQTT Server IP or hostname
int mqttPort = 1883;               // Default MQTT port
String mqttUser = "MQTT_USER";     // MQTT Username
String mqttPass = "MQTT_PASS";     // MQTT Password
WiFiClient espClient;
PubSubClient mqttclient(espClient);
bool mqttON = false; // MQTT nutzen
TaskHandle_t mqttTaskHandle;

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

String webLogBuffer = "";
const size_t WEB_LOG_MAX = 8192; // max. Größe in Bytes

uint8_t outputLevel = 2; // Verbosity

// Preferences for saving configuration
Preferences prefs;

String outBuffer = "";
String lastJsonString = "{}"; // Last JSON string

void parseSerialCommand(String cmd);        // Parse and execute serial commands
void tryWiFiConnect();                      // Attempt to connect to WiFi
void displayMessage(String message);        // Display message on OLED
void clearLog();                            // Clear the log buffer
void reconnectMQTT();                       // Reconnect to MQTT broker
void textOutln(String text, uint8_t level); // Output text with newline

void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Nachricht erhalten: ");
  Serial.print(topic);
  Serial.print(" => ");
  for (unsigned int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

// 🧵 FreeRTOS Task für MQTT loop()
void mqttTask(void *parameter)
{
  for (;;)
  {
    if (!mqttclient.connected() && mqttON)
    {
      reconnectMQTT(); // Reconnect if not connected
    }

    if (mqttON)
    {
      mqttclient.loop(); // Process MQTT messages
    }

    vTaskDelay(10 / portTICK_PERIOD_MS); // kleine Pause, um den Core zu entlasten
  }
}

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

void sendBuffer() // Send outBuffer to Syslog or HTTP URL
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

    // Send to mqtt broker if enabled
    if (mqttON && mqttclient.connected() && lastJsonString != "{}" && outBuffer.startsWith("# JSON"))
    {
      const size_t capacity = 1024;
      DynamicJsonDocument doc(capacity);
      String datetime = "";
      String direction = "";
      String soh_code = "";
      String soh_desc = "";

      // JSON parsen
      DeserializationError error = deserializeJson(doc, lastJsonString);
      if (error)
      {
        Serial.print("JSON Parse Error: ");
        Serial.println(error.f_str());
      }
      else
      { // Hauptfelder auslesen
        datetime = doc["datetime"] | "N/A";
        direction = doc["direction"] | "N/A";
        soh_code = doc["SOH_code"] | "N/A";
        soh_desc = doc["SOH_description"] | "N/A";

        mqttclient.publish("serialsniffer/raw", lastJsonString.c_str(), lastJsonString.length());
        mqttclient.publish("serialsniffer/datetime", datetime.c_str(), datetime.length());
        mqttclient.publish("serialsniffer/direction", direction.c_str(), direction.length());
        mqttclient.publish("serialsniffer/soh_code", soh_code.c_str(), soh_code.length());
        mqttclient.publish("serialsniffer/soh_description", soh_desc.c_str(), soh_desc.length());

        // Einzelne Records verarbeiten
        JsonArray records = doc["records"];
        for (JsonObject record : records)
        {
          String recordType = record["Record type"] | "N/A";
          String recordData = record["Data"] | "N/A";
          String topic = "serialsniffer/" + recordType;
          mqttclient.publish(topic.c_str(), recordData.c_str(), recordData.length());
        }
      }
    }
    if (mqttON && mqttclient.connected() && outBuffer.startsWith("#") && !outBuffer.startsWith("# JSON"))
    {
      // Fallback to sending outBuffer if lastJsonString is empty
      mqttclient.publish("serialsniffer/input/command", outBuffer.c_str(), outBuffer.length());
    }
    if (mqttON && !mqttclient.connected())
    {
      Serial.println("#### MQTT not connected, skipping send");
    }
  }
  else if (outputLevel >= 4)
  {
    Serial.println("#### WiFi not connected or buffer empty, skipping send");
  }

  outBuffer = ""; // Clear buffer
}

void textOutln(String text = "", uint8_t level = 1) // Output text with newline
{
  if (level > outputLevel)
    return;
  if (!showHB) // Heartbeat ausblenden
  {
    if (text.indexOf("<EOT>1<ENQ>2<ENQ>") != -1 ||
        text.indexOf("<EOT>") != -1 ||
        text.indexOf("<ACK") != -1)
    {
      return; // Ignore heartbeat messages
    }
  }

  Serial.println(text);
  outBuffer += text + "\n";
  // WebLog anhängen
  webLogBuffer += text + "\n";
  if (webLogBuffer.length() > WEB_LOG_MAX)
  {
    webLogBuffer.remove(0, webLogBuffer.length() / 2);
  }
  sendBuffer();         // Send buffer to Syslog or HTTP URL
  displayMessage(text); // Display message on Displays
}

void textOut(String text = "", uint8_t level = 1) // Output text without newline
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
  prefs.putString("mqttserver", mqttServer);
  prefs.putInt("mqttport", mqttPort);
  prefs.putString("mqttuser", mqttUser);
  prefs.putString("mqttpass", mqttPass);
  prefs.putBool("mqtton", mqttON);
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
  mqttServer = prefs.getString("mqttserver", "MQTT_SERVER");
  mqttPort = prefs.getInt("mqttport", 1883);
  mqttUser = prefs.getString("mqttuser", "MQTT_USER");
  mqttPass = prefs.getString("mqttpass", "MQTT_PASS");
  mqttON = prefs.getBool("mqtton", false);
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

void applySerialConfig(bool calc = false, bool init = false) // Apply serial configuration
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

void clearLog() // Clear the log buffer
{
  textOutln("# Log buffer cleared");
  webLogBuffer = "";
  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(0, 10);
  tft.println("  IP:" + IP.toString());
  currentLine = 3;
}

void startWebserver() // Start the web server
{
  // Serve static files
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", "text/html"); });

  server.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/bootstrap.min.css", "text/css"); });

  server.on("/bootstrap.bundle.min.js", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/bootstrap.bundle.min.js", "text/javascript"); });

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
  status += "\nMQTT Server: " + mqttServer + ":" + String(mqttPort);
  status += "\nMQTT User: " + mqttUser;
  status += "\nMQTT ON: " + String(mqttON ? "Enabled" : "Disabled");
  status += "\nNTP Time: " + getDateTimeString();
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
saveSerialConfig(); // Save WiFi settings
  // Hier WLAN-Verbindungslogik einbauen, z.B. speichern und neustarten
  tryWiFiConnect();
  // Dann Antwort senden:

  String response = "Verbindungsversuch mit SSID: " + wifiSSID + " gestartet.";
  request->send(200, "text/plain", response); });

  // Serve the log data
  server.on("/logdata", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", webLogBuffer); });

  server.on("/json", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", lastJsonString); });

  server.on("/clearlog", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  clearLog(); // Clear the log buffer
  request->send(200, "text/plain", "Log gelöscht."); });

  server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/log.html", "text/html"); });

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
        IP = WiFi.localIP();
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
        IP = WiFi.softAPIP();
        textOutln("## Fallback-AP gestartet. IP: " + IP.toString());
        textOutln("## WiFi connection failed, check SSID and password", 2);
        textOutln("## Please connect to the fallback AP and configure WiFi settings", 2);
        textOutln("## Use the web server to set WiFi SSID and password", 2);
        displayMessage("Fallback-AP (SerialSniffer_Config) gestartet. IP: " + IP.toString());
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

void appendHex(String &str, uint8_t val) // Append a byte value as hex to a string
{
  char buf[6]; // "0x" + 2 digits + null = 5, 6 for safety
  snprintf(buf, sizeof(buf), "0x%02X", val);
  str += buf;
}

String decodeSOH(const String &code)
{ // Decode SOH code
  if (code == "1")
    return "Call to pager";
  if (code == "2")
    return "Status Information";
  if (code == "3")
    return "Status Request";
  if (code == "4")
    return "Call to subscriber line";
  if (code == "5")
    return "Other Information (manufacturer defined)";
  return "Unknown SOH code";
}

String decodeField0(const String &code)
{ // Decode Field 0 code
  if (code == "1")
    return "Call address";
  if (code == "2")
    return "Display message";
  if (code == "3")
    return "Beep coding";
  if (code == "4")
    return "Call type";
  if (code == "5")
    return "Number of transmissions";
  if (code == "6")
    return "Priority";
  if (code == "7")
    return "Call Status";
  if (code == "8")
    return "System Status";
  return "Unbekannt";
}

String symbolicToControlChars(const String &input) // Convert symbolic control characters to actual control characters
{
  String output = input;
  output.replace("<SOH>", String((char)0x01));
  output.replace("<STX>", String((char)0x02));
  output.replace("<ETX>", String((char)0x03));
  output.replace("<US>", String((char)0x1F));
  output.replace("<RS>", String((char)0x1E));
  return output;
}

String parseRawData(const String &rawData) // Parse raw data string into JSON format
{                                          // Parse raw data string into JSON format
  StaticJsonDocument<1024> doc;

  // SOH auslesen
  int sohIndex = rawData.indexOf((char)SOH);

  String sohCode = "unknown";
  String sohDesc = "Unknown SOH code";
  if (sohIndex != -1 && sohIndex + 1 < rawData.length())
  {
    sohCode = rawData.substring(sohIndex + 1, sohIndex + 2);
    sohDesc = decodeSOH(sohCode);
  }

  doc["datetime"] = getDateTimeString();

  // Richtung ermitteln
  int directionIndex = rawData.indexOf("RX");
  if (directionIndex != -1 && directionIndex + 1 < rawData.length())
  {
    doc["direction"] = "RX";
  }
  else
  {
    directionIndex = rawData.indexOf("TX");
    if (directionIndex != -1 && directionIndex + 1 < rawData.length())
    {
      doc["direction"] = "TX";
    }
    else
    {
      doc["direction"] = "unknown";
    }
  }

  if (sohCode != "unknown") // Only parse further if SOH code is send
  {
    // JSON-Daten füllen

    doc["SOH_code"] = sohCode;
    doc["SOH_description"] = sohDesc;

    // Text zwischen STX und ETX
    int stxIndex = rawData.indexOf((char)STX);
    int etxIndex = rawData.indexOf((char)ETX);
    if (stxIndex == -1 || etxIndex == -1 || etxIndex <= stxIndex)
    {
      doc["error"] = "STX/ETX not found";
      String output;
      serializeJson(doc, output);
      return output;
    }

    String content = rawData.substring(stxIndex + 1, etxIndex);

    // Records parsen
    JsonArray records = doc.createNestedArray("records");
    int start = 0;
    int rsIndex;

    while ((rsIndex = content.indexOf(RS, start)) != -1)
    {
      String record = content.substring(start, rsIndex);
      start = rsIndex + 1;

      int usIndex = record.indexOf(US);
      String field0 = usIndex != -1 ? record.substring(0, usIndex) : record;
      String field1 = usIndex != -1 ? record.substring(usIndex + 1) : "";

      JsonObject rec = records.createNestedObject();
      rec["Data Identifier"] = field0;
      rec["Record type"] = decodeField0(field0);
      rec["Data"] = field1;
    }

    // Letztes Record (nach letztem RS)
    String lastRecord = content.substring(start);
    int usIndex = lastRecord.indexOf(US);
    String field0 = usIndex != -1 ? lastRecord.substring(0, usIndex) : lastRecord;
    String field1 = usIndex != -1 ? lastRecord.substring(usIndex + 1) : "";

    JsonObject rec = records.createNestedObject();
    rec["Data Identifier"] = field0;
    rec["Record type"] = decodeField0(field0);
    rec["Data"] = field1;
  }
  // JSON serialisieren
  String output = "Kein SOH";
  serializeJson(doc, output);
  if (output.endsWith("}]}")) // Nur JSON sichern, wenn Records vorhanden sind
    lastJsonString = output;  // Save last JSON string for later use
  return output;
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
  String json = parseRawData(symbolicToControlChars(line)); // Parse the line into JSON format
  if (json.endsWith("}]}"))                                 // Only print JSON if records are present
    textOutln("# JSON: " + json, 2);
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
    textOutln("# MQTT Server: " + mqttServer + ", Port: " + String(mqttPort) + ", User: " + mqttUser);
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
  else if (c == 'Z')
  { // enable RX simulation
    rxSimActive = true;
    textOutln("# RX simulation enabled");
  }
  else if (c == 'z')
  { // disable RX simulation
    rxSimActive = false;
    textOutln("# RX simulation disabled");
  }
  else if (c == 'V')
  { // enable heartbeat display
    showHB = true;
    textOutln("# Heartbeat display enabled");
  }
  else if (c == 'v')
  { // disable heartbeat display
    showHB = false;
    textOutln("# Heartbeat display disabled");
  }
  else if (c == 'Q')
  { // disable TFT display update
    updateDisplay = true;
    textOutln("# TFT display update enabled");
  }
  else if (c == 'q')
  { // enable TFT display update
    updateDisplay = false;
    textOutln("# TFT display update disabled");
  }
  else if (cmd == "clr")
  { // clear log buffer
    clearLog();
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
    textOutln("# M<MQTT_Server> - Set mqtt server target (e.g., MMyMQTTServer)");
    textOutln("# m<MQTT_Port> - Set mqtt server port (e.g., m1883)");
    textOutln("# K<MQTT_User> - Set mqtt user (e.g., KMyMQTTUser)");
    textOutln("# k<MQTT_Pass> - Set mqtt password (e.g., kMyMQTTPass)");
    textOutln("# J - enable MQTT connection");
    textOutln("# j - disable MQTT connection");
    textOutln("# z|Z - Disable or enable RX simulation");
    textOutln("# v|V - Disable or enable display heartbeat");
    textOutln("# q|Q - Disable or enable display update on TFT");
    textOutln("# clr - Clear log buffer");
    textOutln("# ?/h - Show this help");
    textOutln("# Note: Commands are case-sensitive.");
  }
  else if (c == 'X')
  { // Restart Device
    textOutln("# Restarting device...");
    saveSerialConfig();
    ESP.restart();
  }
  else if (c == 'M')
  { // Set MQTT server
    mqttServer = val;
    textOutln("# MQTT server set to: " + mqttServer);
    mqttclient.setServer(mqttServer.c_str(), mqttPort);
  }
  else if (c == 'm')
  { // Set MQTT port
    mqttPort = val.toInt();
    textOutln("# MQTT port set to: " + String(mqttPort));
    mqttclient.setServer(mqttServer.c_str(), mqttPort);
  }
  else if (c == 'K')
  { // Set MQTT user
    mqttUser = val;
    textOutln("# MQTT user set to: " + mqttUser);
  }
  else if (c == 'k')
  { // Set MQTT password
    mqttPass = val;
    textOutln("# MQTT password set");
  }
  else if (c == 'J')
  { // Enable MQTT connection

    if (!mqttclient.connected())
    {
      mqttclient.setServer(mqttServer.c_str(), mqttPort); // Set server if not already set
      mqttclient.subscribe("serialsniffer/send/befehl");  // Subscribe to command topic
      mqttON = true;                                      // Enable MQTT connection
      reconnectMQTT();                                    // Try to connect to MQTT server
      vTaskResume(mqttTaskHandle);                        // Resume MQTT task
      textOutln("# MQTT connection enabled");
    }
  }
  else if (c == 'j')
  { // Disable MQTT connection
    if (mqttclient.connected())
    {
      mqttclient.unsubscribe("serialsniffer/send/befehl"); // Unsubscribe from command topic
      mqttclient.disconnect();                             // Disconnect from MQTT server
    }
    if (!mqttclient.connected())
    {
      mqttON = false;
      vTaskSuspend(mqttTaskHandle);
      textOutln("# MQTT connection disabled");
    }
  }
  else
  {
    textOutln("# Unknown command: '" + cmd + "' - '?' or 'h' for help");
  }
}

bool isEOLChar(uint8_t c) // Check if character is an EOL character
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

void displayMessage(String message) // Display a message on the displays
{
  if (message.length() == 0)
  {
    // textOutln("## Empty message, skipping displayMessage");
    return;
  }
  // Filter message to remove text between second pair of semicolons
  // and the semicolons themselves
  String filteredMessage = "";

  int semicolonCount = 0;
  bool skip = false;

  for (size_t i = 0; i < message.length(); i++)
  {
    char c = message[i];

    if (c == ';')
    {
      semicolonCount++;
      // Beim zweiten Paar Semikolons skip einschalten oder ausschalten
      if (semicolonCount == 2)
      {
        skip = true; // Nach erstem Semikolon des zweiten Paares starten überspringen
        continue;    // Semikolon nicht mitnehmen
      }
      else if (semicolonCount == 3)
      {
        skip = false; // Nach zweitem Semikolon des zweiten Paares stoppen mit skip
        continue;     // Semikolon nicht mitnehmen
      }
    }

    if (!skip)
    {
      filteredMessage += c;
    }
  }
  if (displayOk && filteredMessage.startsWith("#") && !filteredMessage.startsWith("# JSON")) // OLED display OK? und nur Befehle ausgeben
  {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.println(filteredMessage.substring(0, 140)); // Display first 90 characters
    display.display();

    // Lösch-Timer setzen
    displayClearTime = millis() + 15000; // 15 Sekunden
    displayClearScheduled = true;
  }
  if (tftOk && updateDisplay && (!filteredMessage.startsWith("#") || filteredMessage.startsWith("# JSON"))) // TFT Display OK und keine Befehle ausgeben?
  {
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    int start = 0;
    int len = filteredMessage.length();

    while (start < len)
    {
      // Zeile extrahieren
      String line = filteredMessage.substring(start, start + maxCharsPerLine);
      start += maxCharsPerLine;

      // Wenn Display voll, leeren und neu anfangen
      if (currentLine >= maxLines)
      {
        tft.fillScreen(TFT_BLUE);
        tft.setTextSize(2);
        tft.setCursor(0, 10);
        tft.println("  IP:" + IP.toString());
        tft.setTextSize(1);
        currentLine = 3;
      }

      tft.setCursor(0, currentLine * lineHeight);
      tft.println(line);
      currentLine++;
    }
  }
}

void reconnectMQTT()
{
  if (mqttServer == "MQTT_SERVER" || !mqttON)
  {
    textOutln("### MQTT server not set, skipping reconnection", 2);
    mqttON = false;
    return;
  }

  const int maxRetries = 10;
  int attempt = 0;

  while (!mqttclient.connected() && attempt < maxRetries)
  {
    textOutln("## Trying MQTT connection...", 2);

    // Verbindungsversuch
    bool connected = mqttclient.connect(
        mqttServer.c_str(),
        mqttUser.c_str(),
        mqttPass.c_str());

    if (connected)
    {
      textOutln("#### MQTT connected", 2);
      mqttclient.subscribe("serialsniffer/send/befehl");
      mqttON = true;
      return;
    }
    else
    {
      String err = "MQTT failed, rc=" + String(mqttclient.state()) + " retrying...";
      textOutln(err, 2);
      delay(200); // Minimal warten, kein zu langer Block
    }

    attempt++;
  }

  // Nach max. Versuchen
  textOutln("### MQTT connection failed after 10 attempts", 2);
  mqttON = false;
}

void setup()
{
  Serial.begin(115200); // Initialize USB console
  delay(5000);          // allow USB to initialize
                        // Initialize OLED display
  Wire.begin(7, 8);     // SDA, SCL pins for I2C
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  { // 0x3C ist die Standard-I2C-Adresse
    Serial.println("SSD1306-Initialisierung fehlgeschlagen");
    displayOk = false;
  }
  else
  {
    Serial.println("SSD1306-Initialisierung OK");
    displayOk = true;
  }

  if (!SPIFFS.begin(true))
  { // Initialize SPIFFS
    textOutln("## SPIFFS konnte nicht gestartet werden.", 2);
    return;
  }

  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK); // Start the receiver

  // saveSerialConfig(); // Save initial config if not already done
  if (loadSerialConfig())
  {
    tryWiFiConnect();
    calcSerialConfig();
  }
  applySerialConfig(false, true); // Apply initial serial configuration
  parseSerialCommand("p");        // Print initial config
  // textOutln("## Monitoring RX pin: " + String(MON_RX), 2);
  // textOutln("## Monitoring TX pin: " + String(MON_TX), 2);

  mqttclient.setServer(mqttServer.c_str(), mqttPort);
  mqttclient.setCallback(callback);

  textOutln("## IP:" + IP.toString(), 2);

  tft.init();
  // tft.setRotation(2);
  tftOk = true;

  // large block of text
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.println();
  tft.println("  Serial Sniffer");
  tft.println();
  tft.println("  IP:" + IP.toString());
  tft.println("    115200 8N1");
  maxLines = tft.height() / lineHeight;
  delay(5000);
  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(0, 10);
  tft.println("  IP:" + IP.toString());
  currentLine = 3;

  // Starte den MQTT Task
  xTaskCreatePinnedToCore(
      mqttTask,         // Funktion
      "MQTT Loop Task", // Name
      4096,             // Stack-Größe
      NULL,             // Parameter
      1,                // Priorität
      &mqttTaskHandle,  // TaskHandle
      1                 // Core 1 (0 wäre auch möglich)
  );
}

void handleSerial( // Handle incoming serial data for RX and TX
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
    displayMessage("Wifi connection dropped. Reconnecting.");
    tryWiFiConnect();
  }

  if (wifiConnected && WiFi.status() == WL_CONNECTED)
  {
    timeClient.update();
  }

  if (Serial.available())                        // Serial input handling
  {                                              // Soll die untere while Schleife ersetzen
    String input = Serial.readStringUntil('\n'); // Reads input until newline
    Serial.print("You entered: ");
    Serial.println(input);
    parseSerialCommand(input);
  }

  if (IrReceiver.decode())
  {

    /*
     * Print a summary of received data
     */
    if (IrReceiver.decodedIRData.protocol == UNKNOWN)
    {
      Serial.println(F("Received noise or an unknown (or not yet enabled) protocol"));
      // We have an unknown protocol here, print extended info
      IrReceiver.printIRResultRawFormatted(&Serial, true);
      IrReceiver.resume(); // Do it here, to preserve raw data for printing with printIRResultRawFormatted()
    }
    else
    {
      IrReceiver.resume(); // Early enable receiving of the next IR frame
      IrReceiver.printIRResultShort(&Serial);
      IrReceiver.printIRSendUsage(&Serial);
    }
    Serial.println();

    /*
     * Finally, check the received data and perform actions according to the received command
     */
    if (IrReceiver.decodedIRData.command == 0x1)
    {
      if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_TOGGLE_BIT)
        parseSerialCommand("z"); // Disable RX simulation
      else
        parseSerialCommand("Z"); // Enable RX simulation
    }

    else if (IrReceiver.decodedIRData.command == 0x2)
    {
      if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_TOGGLE_BIT)
        parseSerialCommand("v"); // Disable display heartbeat
      else
        parseSerialCommand("V"); // Enable display heartbeat
    }

    else if (IrReceiver.decodedIRData.command == 0x3)
    {
      if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_TOGGLE_BIT)
        parseSerialCommand("q"); // Disable TFT display update
      else
        parseSerialCommand("Q"); // Enable TFT display update
    }

    else if (IrReceiver.decodedIRData.command == 0x7)
    {
      parseSerialCommand("clr"); // Clear log buffer
    }
    else if (IrReceiver.decodedIRData.command == 0x8)
    {
      parseSerialCommand("S"); // Save config
    }
    else if (IrReceiver.decodedIRData.command == 0xC)
    {
      parseSerialCommand("X"); // Restart device
    }
    else if (IrReceiver.decodedIRData.command == 0x0)
    {
      displayMessage("# 1 RX Simulation\n  2 Display heartbeat\n  3 TFT Update\n  7 Clear LOG\n  8 Save config\n  9 enabele mqtt\n ON Restart device");
    }
    else if (IrReceiver.decodedIRData.command == 0x9)
    {
      if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_TOGGLE_BIT)
        parseSerialCommand("j"); // Disable mqtt connection
      else
        parseSerialCommand("J"); // Enable mqtt connection
    }
  }
  /*
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
      */

  // Display nach 15 Sekunden löschen
  if (displayClearScheduled && millis() >= displayClearTime)
  {
    display.clearDisplay();
    display.display();
    displayClearScheduled = false;
  }

  // Simulierte RX-Daten senden Rundruf
  if (rxSimActive && millis() - rxSimLastTime >= rxSimInterval)
  {
    // Binärdaten gemäß SOH Rundruf
    const uint8_t simulatedData[] = {
        0x01, 0x31, 0x02, 0x31, 0x1F, 0x35, 0x30, 0x30, 0x32,
        0x1E, 0x33, 0x1F, 0x31, 0x1E, 0x32, 0x1F,
        0x4E, 0x4F, 0x52, 0x20, 0x53, 0x57, 0x5A, 0x2E,
        0x45, 0x47, 0x20, 0x57, 0x1E, 0x35, 0x1F, 0x31, 0x03};
    size_t len = sizeof(simulatedData);

    memcpy(rxBuf, simulatedData, len); // in RX-Puffer kopieren
    printBuffer("RX", rxBuf, len);     // wie eingehenden RX verarbeiten
    rxSimLastTime = millis();          // Zeitstempel aktualisieren
  }

  // Simuliere RX/TX-Daten senden Heartbeat
  if (rxSimActive && millis() - rxSimLastTimeHB >= rxSimIntervalHB)
  {
    // Binärdaten gemäß Heartbeat
    const uint8_t simulatedData[] = {
        0x04, 0x31, 0x05, 0x32, 0x05};
    size_t len = sizeof(simulatedData);

    memcpy(rxBuf, simulatedData, len); // in RX-Puffer kopieren
    printBuffer("RX", rxBuf, len);     // wie eingehenden RX verarbeiten
    handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);
    // Binärdaten gemäß AK
    const uint8_t simulatedDataAK[] = {
        0x06};
    size_t lenAK = sizeof(simulatedDataAK);

    memcpy(txBuf, simulatedDataAK, lenAK); // in TX-Puffer kopieren
    printBuffer("TX", txBuf, lenAK);       // wie eingehenden TX verarbeiten
    handleSerial(SerialTX, "TX", txBuf, txLen, txLast);
    // Binärdaten gemäß Heartbeat
    const uint8_t simulatedDataEOT[] = {
        0x04};
    size_t lenEOT = sizeof(simulatedDataEOT);

    memcpy(rxBuf, simulatedDataEOT, lenEOT); // in RX-Puffer kopieren
    printBuffer("RX", rxBuf, lenEOT);        // wie eingehenden RX verarbeiten
    handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);
    rxSimLastTimeHB = millis(); // Zeitstempel aktualisieren
  }

  // Handle RX and TX serial data
  handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);
  handleSerial(SerialTX, "TX", txBuf, txLen, txLast);
}
