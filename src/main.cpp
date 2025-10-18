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
#include "LGFX_ST7789.hpp"
#include <SD.h>
#include <IRremote.hpp>
#include <PubSubClient.h>
#include "time.h"
#include <ESPmDNS.h>

// TFT_eSPI settings
// SPIClass hspi = SPIClass(HSPI);

bool tftOk = false;  // TFT Status
int lineHeight = 12; // Höhe einer Textzeile (anpassen je nach Schriftart)
int currentLine = 0; // Aktuelle Zeilenposition
int maxLines;        // Maximale Anzahl Zeilen pro Bildschirmhöhe
const int maxCharsPerLine = 38;
LGFX tft;
int tftLight = 255; // Hintergrundbeleuchtung (0-255)

#define MON_RX 6 // RX pin
#define MON_TX 5 // TX pin

#define MON_BAUD 9600 // initial baud rate

#define MON_BUF_SIZE 128

#define DUMMY_PIN1 45 // Dummy pin for unused TX
#define DUMMY_PIN2 42 // Dummy pin for unused TX

#define IR_RECEIVE_PIN 4 // Pin for IR receiver

// SD card settings
bool useSD = false; // SD Card nutzen?
bool sdOk = false;  // SD Card Status
constexpr int PIN_SCK = 10;
constexpr int PIN_MISO = 2;
constexpr int PIN_MOSI = 11;
constexpr int SD_CS = 1;
SPIClass spi(HSPI);

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
std::vector<String> ssids;
std::vector<String> passwords;

// MQTT settings
String mqttServer = "MQTT_SERVER"; // MQTT Server IP or hostname
int mqttPort = 1883;               // Default MQTT port
String mqttUser = "MQTT_USER";     // MQTT Username
String mqttPass = "MQTT_PASS";     // MQTT Password
WiFiClient espClient;
PubSubClient mqttclient(espClient);
bool mqttON = false; // MQTT nutzen
TaskHandle_t mqttTaskHandle;

// ESPAX
bool espaxon = false;        // ESPAX nutzen
bool espaxConnected = false; // ESPAX verbunden
bool showESPA = false;       // ESPAX Nachrichten anzeigen/ senden und speichern
WiFiClient espaxclient;
#define MAGIC 0x4558
const uint8_t FLAGS[4] = {0x00, 0x00, 0x02, 0x2a};
String espaxserverIP = "ESPAX_SERVER";
int espaxserverPort = 2023;
String sessionID = "";
String espaxuser = "ESPAX_USER"; // ESPAX User
String espaxpass = "ESPAX_PASS"; // ESPAX Password
unsigned long lastLoginAttempt = 0;
const unsigned long loginRetryInterval = 60000; // 60 Sekunden
// Invoke-ID Zähler
uint32_t invokeCounter = 0;
// Heartbeat
unsigned long lastHeartbeat = 0;
const unsigned long heartbeatInterval = 50000; // 50 Sekunden

// Espa444
String espa444Calladr = "1002";
String espa444msg = "Testnachricht vom Serialsniffer";
int espa444att = 1;
String espa444prio = "Standard";
String espa444callback = "";
bool espa444sendcb = true; // Wird Callbackrufnummer in Message mitgesendet?

// NTP Server
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;     // falls UTC
const int daylightOffset_sec = 0; // Sommerzeit-Anpassung falls nötig

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

void parseSerialCommand(String cmd);               // Parse and execute serial commands
void tryWiFiConnect();                             // Attempt to connect to WiFi
void displayMessage(String message, int colortft); // Display message on OLED
void clearLog();                                   // Clear the log buffer
void reconnectMQTT();                              // Reconnect to MQTT broker
void textOutln(String text, uint8_t level);        // Output text with newline
void sendBuffer(String msg);                       // Send buffer to target URL
String getDateTimeString();                        // Get current date and time as string

String getUptimeString()
{
  unsigned long ms = millis() / 1000;
  unsigned long days = ms / 86400;
  ms %= 86400;
  unsigned long hours = ms / 3600;
  ms %= 3600;
  unsigned long minutes = ms / 60;
  unsigned long seconds = ms % 60;
  char buf[32];
  snprintf(buf, sizeof(buf), "%lud %02luh %02lum %02lus", days, hours, minutes, seconds);
  return String(buf);
}

// ---- Hilfsfunktionen Time ----
String getTimestamp()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Time not available");
    return "1970-01-01T00:00:00";
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf);
}

String getInvokeID() // Generate a unique Invoke ID for each command
{
  return String(invokeCounter++);
}

void showESPAX(String msg, String dir = "")
{
  if (showESPA)
  {
    // displayMessage(msg);                        // TFT Anzeige
    displayMessage("ESPA-X " + dir, TFT_GOLD);          // TFT Anzeige
    sendBuffer("# " + dir + " ESPA-X Message: " + msg); // an div. Ziele senden
    // Auf SD Karte schreiben
    if (useSD && sdOk)
    {
      File file = SD.open("/espax.log", FILE_APPEND);
      if (file)
      {
        file.println("# NTP " + getDateTimeString() + " " + dir + " ESPA-X Message: ");
        file.println(msg);
        file.close();
      }
    }
    // WebLog anhängen
    webLogBuffer += "# " + dir + " ESPA-X Message: \n" + msg + "\n";
    if (webLogBuffer.length() > WEB_LOG_MAX)
    {
      webLogBuffer.remove(0, webLogBuffer.length() / 2);
    }
  }
}

// ---- Sende ESPA-X Nachricht ----
void sendMessage(const String &xml)
{
  size_t xmlLen = xml.length();
  uint32_t totalLen = 10 + xmlLen;

  uint8_t header[10];
  header[0] = (MAGIC >> 8) & 0xFF;
  header[1] = MAGIC & 0xFF;
  header[2] = (totalLen >> 24) & 0xFF;
  header[3] = (totalLen >> 16) & 0xFF;
  header[4] = (totalLen >> 8) & 0xFF;
  header[5] = totalLen & 0xFF;
  memcpy(header + 6, FLAGS, 4);

  // Senden: erst Header, dann XML
  espaxclient.write(header, 10);
  espaxclient.write((const uint8_t *)xml.c_str(), xmlLen);
  espaxclient.flush();

  Serial.printf("ESPAX gesendet: %u Bytes (Header + %u Bytes XML)\n", totalLen, xmlLen);
  Serial.println(xml);
  showESPAX(xml, "Send"); // Wenn gewünscht alles übertragen und anzeigen
}

// ---- ESPAX Login ----
void sendLogin()
{
  String xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<ESPA-X version=\"1.00\" timestamp=\"" +
      getTimestamp() + "\">\n"
                       "  <REQ.LOGIN invokeID=\"" +
      getInvokeID() + "\">\n"
                      "    <LI-CLIENT>SerialSniffer</LI-CLIENT>\n"
                      "    <LI-CLIENTSW>SN_V0.1.0</LI-CLIENTSW>\n"
                      "    <LI-USER>" +
      espaxuser + "</LI-USER>\n"
                  "    <LI-PASSWORD>" +
      espaxpass + "</LI-PASSWORD>\n"
                  "  </REQ.LOGIN>\n"
                  "</ESPA-X>";
  sendMessage(xml);
  Serial.println("ESPA X Login gesendet");
}

// ---- ESPAX Nach Login ----
void sendSCondition()
{
  String xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<ESPA-X version=\"1.00\" timestamp=\"" +
      getTimestamp() + "\">\n"
                       "  <REQ.S-CONDITION invokeID=\"" +
      getInvokeID() + "\" sessionID=\"" + sessionID + "\"/>\n"
                                                      "</ESPA-X>";
  sendMessage(xml);
  Serial.println("ESPA X S-Condition gesendet");
}

// ---- ESPAX Heartbeat ----
void sendHeartbeat()
{
  String xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<ESPA-X version=\"1.00\" timestamp=\"" +
      getTimestamp() + "\">\n"
                       "  <REQ.HEARTBEAT invokeID=\"" +
      getInvokeID() + "\" sessionID=\"" + sessionID + "\"/>\n"
                                                      "</ESPA-X>";

  sendMessage(xml);
  Serial.println("ESPA X Heartbeat gesendet");
}

// ---- ESPAX PREF generieren ----
String createPRRef()
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu-%u", millis(), random(1000, 9999));
  return String(buf);
}

void sendCall(const String &groupID,
              const String &callingNo,
              const String &textMsg,
              const String &prio,
              const String &callback,
              int delaySec,
              int attempts)
{
  String prioused = "";
  if (prio.length() <= 1)
    prioused = "Standard";
  else
    prioused = prio;
  /*
 <?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<ESPA-X version=\"1.00\" xmlns=\"http://ns.espa-x.org/espa-x\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:schemaLocation=\"http://ns.espa-x.org/espa-x http://schema.espa-x.org/espa-x100.xsd\" timestamp=\"2025-08-15T12:21:11\">\n"
"  <REQ.P-START invokeID=\"5\" sessionID=\"20250815142057192.168.1.045904\">\n"
"    <CP-PR-REF>3vdmsafkmecsrjdx</CP-PR-REF>\n"
"    <CP-PHONENO/>\n"
"    <CP-GROUPID>1002</CP-GROUPID>\n"
"    <CP-CALLINGNO>1900</CP-CALLINGNO>\n"
"    <CP-CALLINGNAME/>\n"
"    <CP-TEXTMSG>Test Rundruf von Daniel per ESPA-X</CP-TEXTMSG>\n"
"    <CP-WARD/>\n"
"    <CP-BED/>\n"
"    <CP-SIGNAL>Standard</CP-SIGNAL>\n"
"    <CP-CALLBACK>Phone</CP-CALLBACK>\n"
"    <CP-DELAY>0</CP-DELAY>\n"
"    <CP-ATTEMPTS>1</CP-ATTEMPTS>\n"
"    <CP-PRIO>Standard</CP-PRIO>\n"
"    <CP-CBCKNO>1900</CP-CBCKNO>\n"
"    <CP-NCIFNO/>\n"
"    <CP-PR-DETAILS>All</CP-PR-DETAILS>\n"
"    <PROPRIETARY>\n"
"      <DAKS_ESPA-X version=\"1.14\" xmlns=\"http://ns.tetronik.com/DAKS_ESPA-X\" xsi:schemaLocation=\"http://ns.tetronik.com/DAKS_ESPA-X http://schema.tetronik.com/DAKS/DAKS_ESPA-X114.xsd\">\n"
"        <START1>\n"
"          <SA-ANNIDS/>\n"
"          <CONFIRMATION/>\n"
"          <SGL_CONNTYPE/>\n"
"        </START1>\n"
"      </DAKS_ESPA-X>\n"
"    </PROPRIETARY>\n"
"  </REQ.P-START>\n"
"</ESPA-X>
  */
  String xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<ESPA-X version=\"1.00\" xmlns=\"http://ns.espa-x.org/espa-x\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
      "xsi:schemaLocation=\"http://ns.espa-x.org/espa-x http://schema.espa-x.org/espa-x100.xsd\" "
      "timestamp=\"" +
      getTimestamp() + "\">\n"
                       "  <REQ.P-START invokeID=\"" +
      getInvokeID() + "\" sessionID=\"" + sessionID + "\">\n"
                                                      "    <CP-PR-REF>" +
      createPRRef() + "</CP-PR-REF>\n"
                      "    <CP-PHONENO/>\n"
                      "    <CP-GROUPID>" +
      groupID + "</CP-GROUPID>\n"
                "    <CP-CALLINGNO>" +
      callingNo + "</CP-CALLINGNO>\n"
                  "    <CP-CALLINGNAME/>\n"
                  "    <CP-TEXTMSG>" +
      textMsg + "</CP-TEXTMSG>\n"
                "    <CP-WARD/>\n"
                "    <CP-BED/>\n"
                "    <CP-SIGNAL>Standard</CP-SIGNAL>\n"
                "    <CP-CALLBACK>" +
      callback + "</CP-CALLBACK>\n"
                 "    <CP-DELAY>" +
      String(delaySec) + "</CP-DELAY>\n"
                         "    <CP-ATTEMPTS>" +
      String(attempts) + "</CP-ATTEMPTS>\n"
                         "    <CP-PRIO>" +
      prioused + "</CP-PRIO>\n"
                 "    <CP-CBCKNO>" +
      callingNo + "</CP-CBCKNO>\n"
                  "    <CP-NCIFNO/>\n"
                  "    <CP-PR-DETAILS>All</CP-PR-DETAILS>\n"
                  "    <PROPRIETARY>\n"
                  "      <DAKS_ESPA-X version=\"1.14\" xmlns=\"http://ns.tetronik.com/DAKS_ESPA-X\" "
                  "xsi:schemaLocation=\"http://ns.tetronik.com/DAKS_ESPA-X http://schema.tetronik.com/DAKS/DAKS_ESPA-X114.xsd\">\n"
                  "        <START1>\n"
                  "          <SA-ANNIDS/>\n"
                  "          <CONFIRMATION/>\n"
                  "          <SGL_CONNTYPE/>\n"
                  "        </START1>\n"
                  "      </DAKS_ESPA-X>\n"
                  "    </PROPRIETARY>\n"
                  "  </REQ.P-START>\n"
                  "</ESPA-X>";

  sendMessage(xml);
  Serial.println("Call durchgeführt");
}

// ---- ESPAX SessionID extrahieren ----
String extractSessionID(const String &resp)
{
  const String key = "sessionID=\"";
  int pos = resp.indexOf(key);
  if (pos < 0)
    return "";
  pos += key.length();
  int endPos = resp.indexOf("\"", pos);
  if (endPos < 0)
    return "";
  return resp.substring(pos, endPos);
}

// ---- Antwort lesen ----
String readXMLResponse(unsigned long timeoutMs = 5000)
{
  unsigned long start = millis();

  uint8_t header[10];
  int readBytes = 0;

  // --- Header lesen (10 Bytes) ---
  while (readBytes < 10 && (millis() - start < timeoutMs))
  {
    if (espaxclient.available())
    {
      header[readBytes++] = espaxclient.read();
    }
  }

  if (readBytes < 10)
  {
    Serial.println("Fehler: Timeout beim Header-Lesen");
    return "";
  }

  // --- Länge extrahieren ---
  uint32_t totalLen = ((uint32_t)header[2] << 24) |
                      ((uint32_t)header[3] << 16) |
                      ((uint32_t)header[4] << 8) |
                      (uint32_t)header[5];

  if (totalLen < 10)
  {
    Serial.println("Fehler: Ungültige Länge im Header");
    return "";
  }

  uint32_t xmlLen = totalLen - 10;

  // --- Rest (XML) lesen ---
  String response = "";
  response.reserve(xmlLen); // Speicher reservieren (optimiert)

  uint32_t received = 0;
  while (received < xmlLen && (millis() - start < timeoutMs))
  {
    while (espaxclient.available() && received < xmlLen)
    {
      char c = espaxclient.read();
      response += c;
      received++;
    }
  }

  if (received < xmlLen)
  {
    Serial.printf("Fehler: Timeout beim XML-Lesen (%u/%u Bytes)\n", received, xmlLen);
    return "";
  }

  showESPAX(response, "Received"); // Wenn gewünscht alles übertragen und anzeigen
  return response;
}

// ---- Verbindung zum ESPAX Server sicherstellen ----
bool ensureConnected()
{
  if (espaxclient.connected())
    return true;

  Serial.println("Verbindung zum Server verloren. Versuche Reconnect...");

  if (!espaxclient.connect(espaxserverIP.c_str(), espaxserverPort))
  {
    Serial.println("Reconnect fehlgeschlagen");
    return false;
  }

  Serial.println("Erneut mit Server verbunden");
  return true;
}

// ---- ESPAX Login-Prozess durchführen ----
bool doLogin()
{
  sendLogin();
  String loginresponse = readXMLResponse();
  Serial.println("Login-Antwort:");
  Serial.println(loginresponse);

  sessionID = extractSessionID(loginresponse);
  Serial.println("SessionID: " + sessionID);

  if (sessionID.length() > 0)
  {
    sendSCondition();
    Serial.println("sendSCondition-Antwort:");
    Serial.println(readXMLResponse());
    espaxConnected = true;
    return true;
  }
  return false;
}

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

void sendBuffer(String msg = "") // msg optional
{
  // Falls kein Parameter mitgegeben wurde, nutze outBuffer
  String sendData = (msg.length() > 0) ? msg : outBuffer;

  if (!wifiConnected || sendData.length() == 0)
  {
    if (outputLevel >= 4)
    {
      Serial.println("#### WiFi not connected or nothing to send");
    }
    return;
  }

  bool transmissionSuccess = false;

  // === HTTP SEND ===
  if (targetURL.length() > 0)
  {
    if (targetURL.startsWith("http://") || targetURL.startsWith("https://"))
    {
      HTTPClient http;
      http.begin(targetURL);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");

      String postBody = "data=" + urlEncode(sendData);
      int httpResponseCode = http.POST(postBody);
      http.end();

      if (httpResponseCode >= 200 && httpResponseCode < 300)
      {
        transmissionSuccess = true;
        if (outputLevel >= 4)
        {
          Serial.println("#### HTTP data sent: " + sendData);
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
    else if (outputLevel >= 2)
    {
      Serial.println("Invalid URL: Must start with http:// or https://");
    }
  }

  // === SYSLOG SEND ===
  if (syslog_ip.length() > 0)
  {
    syslog.log(LOG_INFO, sendData.c_str());
    transmissionSuccess = true;
  }

  // === MQTT SEND ===
  if (mqttON && mqttclient.connected())
  {
    if (sendData.indexOf("<SOH>") > -1 && lastJsonString != "{}")
    {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, lastJsonString);

      if (error)
      {
        Serial.print("JSON Parse Error: ");
        Serial.println(error.f_str());
      }
      else
      {
        transmissionSuccess = true;
        String datetime = doc["datetime"] | "N/A";
        String direction = doc["direction"] | "N/A";
        String soh_code = doc["SOH_code"] | "N/A";
        String soh_desc = doc["SOH_description"] | "N/A";

        mqttclient.publish("serialsniffer/raw", lastJsonString.c_str(), lastJsonString.length());
        mqttclient.publish("serialsniffer/datetime", datetime.c_str(), datetime.length());
        mqttclient.publish("serialsniffer/direction", direction.c_str(), direction.length());
        mqttclient.publish("serialsniffer/soh_code", soh_code.c_str(), soh_code.length());
        mqttclient.publish("serialsniffer/soh_description", soh_desc.c_str(), soh_desc.length());

        JsonArray records = doc["records"];
        for (JsonObject record : records)
        {
          String recordType = record["Record type"] | "N/A";
          recordType.replace(' ', '_');
          String recordData = record["Data"] | "N/A";
          String topic = "serialsniffer/" + recordType;
          mqttclient.publish(topic.c_str(), recordData.c_str(), recordData.length());
        }
      }
    }
    else if (sendData.startsWith("#"))
    {
      mqttclient.publish("serialsniffer/input/command", sendData.c_str(), sendData.length());
      transmissionSuccess = true;
    }
  }
  else if (mqttON && !mqttclient.connected())
  {
    Serial.println("#### MQTT not connected, skipping send");
  }

  // === CLEAR BUFFER ONLY IF outBuffer genutzt wurde ===
  if (transmissionSuccess)
  {
    if (msg.length() == 0) // Nur löschen, wenn wir wirklich den outBuffer benutzt haben
      outBuffer = "";
  }
  else if (outputLevel >= 2)
  {
    Serial.println("#### No transmission succeeded, data retained");
  }
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
  sendBuffer();                    // Send buffer to Syslog or HTTP URL
  displayMessage(text, TFT_WHITE); // Display message on Displays
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
  prefs.putString("espax_server", espaxserverIP);
  prefs.putInt("espax_port", espaxserverPort);
  prefs.putString("espax_user", espaxuser);
  prefs.putString("espax_pass", espaxpass);
  prefs.putBool("espaxon", espaxon);
  prefs.putInt("tftLight", tftLight); // TFT Hintergrundbeleuchtung speichern
  prefs.putBool("useSD", useSD);
  prefs.putBool("showESPA", showESPA);
  prefs.putBool("espa444sendcb", espa444sendcb);
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
  espaxserverIP = prefs.getString("espax_server", "ESPAX_SERVER");
  espaxserverPort = prefs.getInt("espax_port", 2023);
  espaxuser = prefs.getString("espax_user", "ESPAX_USER");
  espaxpass = prefs.getString("espax_pass", "ESPAX_PASS");
  espaxon = prefs.getBool("espaxon", false);
  tftLight = prefs.getInt("tftLight", 255); // TFT Hintergrundbeleuchtung laden
  useSD = prefs.getBool("useSD", false);
  showESPA = prefs.getBool("showESPA", false);
  espa444sendcb = prefs.getBool("espa444sendcb", true);
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
  tft.println("  " + IP.toString());
  currentLine = 3;
}

void startWebserver() // Start the web server
{
  // Serve static files
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", "text/html"); });

  server.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/bootstrap.min.css", "text/css"); });

  server.on("/bootstrap-icons.css", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/bootstrap-icons.css", "text/css"); });
  server.on("/fonts/bootstrap-icons.woff2", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/fonts/bootstrap-icons.woff2", "font/woff2"); });

  server.on("/fonts/bootstrap-icons.woff", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/fonts/bootstrap-icons.woff", "font/woff"); });

  server.on("/bootstrap.bundle.min.js", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/bootstrap.bundle.min.js", "text/javascript"); });

  // Status anzeigen
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  String status = "IP: " + WiFi.localIP().toString();
  status += "\nSSID: " + wifiSSID;
  status += ", WiFi RSSI: " + String(WiFi.RSSI()) + " dBm";   
  status += "\nNTP Time: " + getDateTimeString();  
  status += "\nUptime: " + getUptimeString();
  status += "\nCPU Temp: " + String(temperatureRead(),1) + " °C";
  status += "\nFree Heap: " + String(ESP.getFreeHeap()) + " bytes";  
  status += "\nBaudrate: " + String(currentBaud);
  status += ", Data Bits: " + String(currentDataBits);
  status += ", Parity: " + String(currentParity);
  status += ", Stop Bits: " + String(currentStopBits);
  status += "\nRX Invert: " + String(rxInvert ? "Enabled" : "Disabled");
  status += ", TX Invert: " + String(txInvert ? "Enabled" : "Disabled");
  status += "\nCallback in ESPA MSG: " + String(espa444sendcb ? "Enabled" : "Disabled");
  status += "\nRX Simulation: " + String(rxSimActive ? "Enabled" : "Disabled");
  status += "\nDisplay Heartbeat: " + String(showHB ? "Enabled" : "Disabled");
  status += "\nSyslog IP: " + syslog_ip;
  status += ", Target URL: " + targetURL;
  status += "\nMQTT Server: " + mqttServer + ":" + String(mqttPort);
  status += "\nMQTT User: " + mqttUser;
  status += "\nUse MQTT: " + String(mqttON ? "Enabled" : "Disabled");
  status += "\nESPAX Server: " + espaxserverIP + ":" + String(espaxserverPort);
  status += "\nESPAX User: " + espaxuser;
  status += "\nSend ESPAX: " + String(espaxon ? "Enabled" : "Disabled");
  status += "\nShow ESPAX LOG: " + String(showESPA ? "Enabled" : "Disabled");
  status += "\nUse SD Card: " + String(useSD ? "Enabled" : "Disabled");
  status += "\nOutput Level: " + String(outputLevel);
  status += "\nEOL Detect: " + String(eolDetect ? "Enabled" : "Disabled");
  status += "\nTimeout: " + String(timeout) + " ms";
  status += "\nTFT Update: " + String(updateDisplay ? "Enabled" : "Disabled");
 
  request->send(200, "text/plain", status); });

  // Dateien auflisten (SD)
  server.on("/filelist", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    String html = "";
    if (sdOk) {
      File root = SD.open("/");
      File file = root.openNextFile();
      while (file) {
        String fname = String(file.name());
        if(fname.startsWith(".")) { // Skip hidden files
          file = root.openNextFile();
          continue;
        }
        html += "<li>" + fname;
        html += " [<a href=\"/download?file=" + fname + "\">Download</a>]";
        html += " [<a href=\"/delete?file=" + fname + "\" data-delete=\"1\">Delete</a>]";
        html += "</li>";
        file = root.openNextFile();
      }
      root.close();
    } else {
      html += "<li>SD card not available</li>";
    }
    html += "</ul>";
    request->send(200, "text/html", html); });

  // Datei-Download
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Parameter 'file' missing.");
      return;
    }
    String fname = request->getParam("file")->value();
    if (SD.exists("/"+fname)) {
      request->send(SD, "/"+fname, "application/octet-stream", true);
    } else {
      request->send(404, "text/plain", "File not found");
    } });

  // Datei löschen
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Parameter 'file' missing.");
      return;
    }
    String fname = request->getParam("file")->value();
    if (SD.exists("/"+fname)) {
      SD.remove("/"+fname);
      request->send(200, "text/html", "File deleted.<br><a href=\"/\">Back to home</a>");
    } else {
      request->send(404, "text/plain", "File not found.");
    } });

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

  String response = "Trying to connect to SSID: " + wifiSSID + " gestartet.";
  request->send(200, "text/plain", response); });

  // Serve the log data
  server.on("/logdata", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", webLogBuffer); });

  server.on("/json", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", lastJsonString); });

  server.on("/espax", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/plain", "Call Address: " + espa444Calladr + "\nCallback: " + espa444callback + "\nMessage: " + espa444msg + "\nAttempts: " + espa444att); });

  server.on("/clearlog", HTTP_GET, [](AsyncWebServerRequest *request)
            {
  clearLog(); // Clear the log buffer
  request->send(200, "text/plain", "Log cleared."); });

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
        displayMessage("OK IP: " + WiFi.localIP().toString(), TFT_WHITE);
        startWebserver(); // Start web server

        // mDNS initialisieren:
        if (MDNS.begin("serialsniffer"))
        {
          textOutln("# mDNS responder started as serialsniffer.local");
        }
        else
        {
          textOutln("# Error setting up MDNS responder!");
        }

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
        textOutln("## Fallback AP started. IP: " + IP.toString());
        textOutln("## WiFi connection failed, check SSID and password", 2);
        textOutln("## Please connect to the fallback AP and configure WiFi settings", 2);
        textOutln("## Use the web server to set WiFi SSID and password", 2);
        displayMessage("Fallback AP (SerialSniffer_Config) started. IP: " + IP.toString(), TFT_WHITE);
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
  return "Unknown";
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

String parseRawData(const String &rawData)
{
  // Neues API: nur noch JsonDocument
  JsonDocument doc;

  //
  // --- SOH prüfen ---
  //
  int sohIndex = rawData.indexOf((char)SOH);
  if (sohIndex == -1 || sohIndex + 1 >= rawData.length())
  {
    return "No SOH";
  }

  String sohCode = rawData.substring(sohIndex + 1, sohIndex + 2);
  String sohDesc = decodeSOH(sohCode);

  doc["datetime"] = getDateTimeString();
  doc["SOH_code"] = sohCode;
  doc["SOH_description"] = sohDesc;

  //
  // --- Richtung bestimmen ---
  //
  if (rawData.indexOf("RX") != -1)
    doc["direction"] = "RX";
  else if (rawData.indexOf("TX") != -1)
    doc["direction"] = "TX";
  else
    doc["direction"] = "unknown";

  //
  // --- STX / ETX prüfen ---
  //
  int stxIndex = rawData.indexOf((char)STX);
  int etxIndex = rawData.indexOf((char)ETX);

  if (stxIndex == -1 || etxIndex == -1 || etxIndex <= stxIndex)
  {
    doc["error"] = "STX/ETX not found or invalid positions";

    String output;
    serializeJson(doc, output);
    return output;
  }

  //
  // --- Inhalt extrahieren ---
  //
  String content = rawData.substring(stxIndex + 1, etxIndex);

  // Array für Records anlegen
  JsonArray records = doc["records"].to<JsonArray>();

  //
  // --- Hilfsfunktion für Record-Parsing ---
  //
  auto parseRecord = [&](const String &recordStr)
  {
    int usIndex = recordStr.indexOf(US);
    String field0 = (usIndex != -1) ? recordStr.substring(0, usIndex) : recordStr;
    String field1 = (usIndex != -1) ? recordStr.substring(usIndex + 1) : "";

    JsonObject rec = records.add<JsonObject>();
    rec["Data Identifier"] = field0;

    String decodedType = decodeField0(field0); // Decode Field 0 to string
    rec["Record type"] = decodedType.length() > 0 ? decodedType : "Unknown";
    rec["Data"] = field1;
    if (field0.toInt() == 1)
    {
      espa444Calladr = field1; // Calladresse für ESPA-444 speichern
      Serial.println("ESPA444 Call Address: " + espa444Calladr);
    }
    else if (field0.toInt() == 2)
    {
      if (espa444sendcb) // Wenn callback Rufnummer in Message
      {

        // Erstes Leerzeichen finden
        int posSpace = field1.indexOf(' ');

        // Rufnummer extrahieren
        espa444callback = field1.substring(0, posSpace);
        Serial.println("ESPA444 Callback: " + espa444callback);
        espa444msg = field1.substring(posSpace + 1, field1.length()); // Display message ohne Rufnummer
      }
      else
        espa444msg = field1; // Display message
      Serial.println("ESPA444 Message: " + espa444msg);
    }
    else if (field0.toInt() == 3)
    {
      // Beep coding
    }
    else if (field0.toInt() == 4)
    {
      // Call type
    }
    else if (field0.toInt() == 5)
    {
      espa444att = field1.toInt(); // Number of transmissions
      Serial.println("ESPA444 Attemps: " + String(espa444att));
    }
    else if (field0.toInt() == 6)
    {
      espa444prio = field1; // Priority
      Serial.println("ESPA444 Priority: " + espa444prio);
    }
    else if (field0.toInt() == 7)
    {
      // Call Status
    }
    else if (field0.toInt() == 8)
    {
      // System Status
    }
  };

  //
  // --- Records zerlegen (per RS) ---
  //
  int start = 0;
  int rsIndex = 0;

  while ((rsIndex = content.indexOf(RS, start)) != -1)
  {
    parseRecord(content.substring(start, rsIndex));
    start = rsIndex + 1;
  }

  // Letzten Record hinzufügen
  parseRecord(content.substring(start));

  //
  // --- Metadaten ergänzen ---
  //
  doc["record_count"] = records.size();

  //
  // --- JSON serialisieren ---
  //
  String output;
  serializeJson(doc, output);

  if (records.size() > 0)
    lastJsonString = output;

  return output;
}

void printBuffer(const char *type, uint8_t *buf, size_t len)
{
  // Lookup-Tabelle für Steuerzeichen (C0 + C1)
  static const char *controlCodes[256] = {
      // C0 control codes (0x00–0x1F)
      "<NUL>", "<SOH>", "<STX>", "<ETX>", "<EOT>", "<ENQ>", "<ACK>", "<BEL>", // 0x00–0x07
      "<BS>", "<HT>", "<LF>", "<VT>", "<FF>", "<CR>", "<SO>", "<SI>",         // 0x08–0x0F
      "<DLE>", "<DC1>", "<DC2>", "<DC3>", "<DC4>", "<NAK>", "<SYN>", "<ETB>", // 0x10–0x17
      "<CAN>", "<EM>", "<SUB>", "<ESC>", "<FS>", "<GS>", "<RS>", "<US>",      // 0x18–0x1F

      // 0x20–0x7E: normale druckbare ASCII-Zeichen → handled separat
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x20–0x27
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x28–0x2F
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x30–0x37
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x38–0x3F
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x40–0x47
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x48–0x4F
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x50–0x57
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x58–0x5F
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x60–0x67
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x68–0x6F
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x70–0x77
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, // 0x78–0x7F

      // DEL
      "<DEL>",

      // C1 control codes (0x80–0x9F)
      "<PAD>", "<HOP>", "<BPH>", "<NBH>", "<IND>", "<NEL>", "<SSA>", "<ESA>", // 0x80–0x87
      "<HTS>", "<HTJ>", "<VTS>", "<PLD>", "<PLU>", "<RI>", "<SS2>", "<SS3>",  // 0x88–0x8F
      "<DCS>", "<PU1>", "<PU2>", "<STS>", "<CCH>", "<MW>", "<SPA>", "<EPA>",  // 0x90–0x97
      "<SOS>", "<SGCI>", "<SCI>", "<CSI>", "<ST>", "<OSC>", "<PM>", "<APC>",  // 0x98–0x9F

      // 0xA0–0xFF: druckbare/erweiterte Zeichen → separat behandelt
      nullptr};

  // Zeitstempel + Typ
  String line = getDateTimeString() + ';' + String(type) + ';';

  // HEX-Ausgabe
  for (size_t i = 0; i < len; i++)
  {
    if (i)
      line += " ";
    appendHex(line, buf[i]);
  }
  line += ";";

  // ASCII / Symbolische Ausgabe
  for (size_t i = 0; i < len; i++)
  {
    uint8_t b = buf[i];
    if (b >= 32 && b <= 126)
    {
      // normale druckbare ASCII-Zeichen
      line += (char)b;
    }
    else if (b >= 0xA0)
    {
      // druckbare Erweiterung (Latin-1, UTF-8 etc.)
      line += (char)b;
    }
    else if (controlCodes[b])
    {
      // bekannte Steuerzeichen aus Tabelle
      line += controlCodes[b];
    }
    else
    {
      // Fallback: Hex-Code in spitzen Klammern
      char code[8];
      snprintf(code, sizeof(code), "<%02X>", b);
      line += code;
    }
  }

  // Ausgabe
  textOutln(line, 0);

  // Nur wenn SD-Karte vorhanden ist, dort speichern
  if (sdOk)
  {
    File file = SD.open("/system.log", FILE_APPEND);
    if (file)
    {
      file.println(line);
      file.close();
    }
  }

  // JSON-Ausgabe, falls Daten erkannt werden
  String json = parseRawData(symbolicToControlChars(line)); // JSON parsing and storing
  if (json.endsWith("}]}"))
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
    textOut("# ESPAX Server: " + espaxserverIP + ", Port: " + String(espaxserverPort) + ", User: " + espaxuser);
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
  else if (cmd == "lup")
  { // Light up TFT
    tftLight += 10;
    if (tftLight > 255)
      tftLight = 255;
    analogWrite(3, tftLight);
  }
  else if (cmd == "ldn")
  { // Light down TFT
    tftLight -= 10;
    if (tftLight < 0)
      tftLight = 0;
    analogWrite(3, tftLight);
  }
  else if (cmd == "xcall")
  { // Send ESPAX data
    if (espaxon && espaxConnected)
    {
      textOutln("# Sending ESPAX data...");
      // Example call, replace with actual data
      sendCall(espa444Calladr, espa444callback, espa444msg, espa444prio, "Phone", 0, espa444att);
      Serial.println("sendCall-Antwort:");
      Serial.println(readXMLResponse());
    }
    else
    {
      textOutln("# ESPAX  is disabled or not connected, cannot send data");
    }
  }
  else if (c == 'I' || c == 'i')
  { // Enable/Disable ESPAX connection
    if (c == 'I')
    {
      espaxon = true;
      textOutln("# ESPAX connection enabled");
    }
    else
    {
      espaxon = false;
      textOutln("# ESPAX connection disabled");
    }
  }
  else if (c == 'G')
  { // Set ESPAX server IP
    espaxserverIP = val;
    textOutln("# ESPAX server set to: " + espaxserverIP);
  }
  else if (c == 'g')
  { // Set ESPAX server port
    espaxserverPort = val.toInt();
    textOutln("# ESPAX server port set to: " + String(espaxserverPort));
  }
  else if (c == 'A')
  { // Set ESPAX user
    espaxuser = val;
    textOutln("# ESPAX user set to: " + espaxuser);
  }
  else if (c == 'a')
  { // Set ESPAX password
    espaxpass = val;
    textOutln("# ESPAX password set");
  }
  else if (c == 'j' || c == 'J')
  { // Enable/Disable MQTT connection
    if (c == 'J')
    {
      mqttON = true;
      textOutln("# MQTT connection enabled");
    }
    else
    {
      mqttON = false;
      textOutln("# MQTT connection disabled");
    }
  }
  else if (c == 'C')
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
    textOutln("# j|J - Disable or enable MQTT connection");
    textOutln("# G<ESPAX_Server> - Set ESPAX server target (e.g., GMyESPAXServer)");
    textOutln("# g<ESPAX_Port> - Set ESPAX server port (e.g., g2023)");
    textOutln("# A<ESPAX_User> - Set ESPAX user (e.g., AMyESPAXUser)");
    textOutln("# a<ESPAX_Pass> - Set ESPAX password (e.g., aMyESPAXPass)");
    textOutln("# i|I - Disable or enable ESPAX connection");
    textOutln("# z|Z - Disable or enable RX simulation");
    textOutln("# v|V - Disable or enable display heartbeat");
    textOutln("# q|Q - Disable or enable display update on TFT");
    textOutln("# clr - Clear log buffer");
    textOutln("# xcall - Send ESPAX data");
    textOutln("# lup - Increase TFT backlight");
    textOutln("# ldn - Decrease TFT backlight");
    textOutln("# csdon - Use SD");
    textOutln("# csdoff - Do not use SD");
    textOutln("# csespaxon - Show espa-x LOG");
    textOutln("# csespaxoff - Do not show espa-x LOG");
    textOutln("# cespa444cbon - Enable callback number in display message");
    textOutln("# cespa444cboff - Disable callback number in display message");
    textOutln("# ?/h - Show this help");
    textOutln("# Note: Commands are case-sensitive.");
  }
  else if (c == 'X')
  { // Restart Device
    textOutln("# Restarting device...");
    saveSerialConfig();
    ESP.restart();
  }
  else if (cmd == "csdon")
  { // Enable SD card usage
    if (!useSD)
    {
      useSD = true;

      textOutln("# SD card enabled");
    }
    else
    {
      textOutln("# SD card already enabled");
    }
  }
  else if (cmd == "csdoff")
  { // Disable SD card usage
    if (useSD)
    {
      useSD = false;
      textOutln("# SD card disabled");
    }
    else
    {
      textOutln("# SD card already disabled");
    }
  }
  else if (cmd == "csespaxon")
  { // Enable ESPAX logging
    if (!showESPA)
    {
      showESPA = true;

      textOutln("# ESPAX log display enabled");
    }
    else
    {
      textOutln("# ESPAX log display already enabled");
    }
  }
  else if (cmd == "csespaxoff")
  { // Disable ESPAX logging
    if (showESPA)
    {
      showESPA = false;
      textOutln("# ESPAX log display disabled");
    }
    else
    {
      textOutln("# ESPAX log display already disabled");
    }
  }
  else if (cmd == "cespa444cbon")
  { // Enable ESPA-444 callback number in display message
    if (!espa444sendcb)
    {
      espa444sendcb = true;

      textOutln("# ESPA-444 callback number in display message enabled");
    }
    else
    {
      textOutln("# ESPA-444 callback number in display message already enabled");
    }
  }
  else if (cmd == "cespa444cboff")
  { // Disable ESPA-444 callback number in display message
    if (espa444sendcb)
    {
      espa444sendcb = false;
      textOutln("# ESPA-444 callback number in display message disabled");
    }
    else
    {
      textOutln("# ESPA-444 callback number in display message already disabled");
    }
  }
  else if (c == 'Y')
  { // Set syslog server
    syslog_ip = val;
    textOutln("# syslog server set to: " + syslog_ip);
    syslog.server(syslog_ip.c_str(), syslog_port); // aktualisiere Ziel-IP direkt
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
  else if (c == 'G')
  { // Set ESPAX server
    espaxserverIP = val;
    textOutln("# ESPAX server set to: " + espaxserverIP);
  }
  else if (c == 'g')
  { // Set ESPAX port
    espaxserverPort = val.toInt();
    textOutln("# ESPAX port set to: " + String(espaxserverPort));
  }
  else if (c == 'A')
  { // Set ESPAX user
    espaxuser = val;
    textOutln("# ESPAX user set to: " + espaxuser);
  }
  else if (c == 'a')
  { // Set ESPAX password
    espaxpass = val;
    textOutln("# ESPAX password set");
  }
  else if (c == 'i' || c == 'I')
  {               // Enable or disable ESPAX connection
    if (c == 'i') // Disable
    {
      espaxon = false;

      textOutln("# ESPAX connection disabled");
    }
    else // Enable
    {
      espaxon = true;

      textOutln("# ESPAX connection enabled");
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

void displayMessage(String message, int colortft) // Display a message on the displays
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
    tft.setTextColor(colortft);
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
        tft.setTextColor(TFT_LIGHTGREY);
        tft.println("  " + IP.toString());
        tft.setTextSize(1);
        currentLine = 3;
      }
      tft.setTextColor(colortft);
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

void readWifiFromSD()
{
  if (!sdOk)
    return;

  File file = SD.open("/wifi.txt", FILE_READ);
  if (!file)
  {
    Serial.println("❌ wifi.txt not found on SD!");
    return;
  }

  Serial.println("📄 Reading WiFi credentials from SD...");

  while (file.available())
  {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() == 0 || line.startsWith("#"))
      continue; // Kommentare/Leerzeilen überspringen

    int sep = line.indexOf(';');
    if (sep > 0)
    {
      String ssid = line.substring(0, sep);
      String pass = line.substring(sep + 1);
      ssid.trim();
      pass.trim();

      ssids.push_back(ssid);
      passwords.push_back(pass);

      Serial.printf("✅ Found: SSID='%s', PASS='%s'\n", ssid.c_str(), pass.c_str());
    }
  }

  file.close();
}

void setup()
{
  Serial.begin(115200); // Initialize USB console
  delay(1000);

  // Initialize OLED display
  Wire.begin(7, 8); // SDA, SCL pins for I2C
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  { // 0x3C ist die Standard-I2C-Adresse
    Serial.println("SSD1306 initialization failed");
    displayOk = false;
  }
  else
  {
    Serial.println("SSD1306 initialization OK");
    displayOk = true;
  }

  if (!SPIFFS.begin(true))
  { // Initialize SPIFFS
    textOutln("## SPIFFS could not be started.", 2);
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

  if (wifiConnected)
  {
    // NTP initialisieren
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("Waiting for time sync...");
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo))
    {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nTime synchronized: " + getDateTimeString());

    if (espaxon) // Wenn ESPAX aktiviert ist, verbinde
    {
      if (!espaxclient.connect(espaxserverIP.c_str(), espaxserverPort))
      {
        Serial.println("Connection to espa-x server failed");
        while (true)
          delay(1000);
      }
      Serial.println("Connected to the espa-x server");

      if (!ensureConnected()) // Verbunden?
      {
        Serial.println("Initial connection to espa-x server failed, will try again later");
      }
      else if (!doLogin()) // Login
      {
        Serial.println("Login to espa-x server failed – will try again later");
      }
      lastLoginAttempt = millis();
    }
  }

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
  tft.println("  " + IP.toString());
  tft.println("    115200 8N1");
  maxLines = tft.height() / lineHeight;
  delay(5000);
  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_LIGHTGREY);
  tft.setTextSize(2);
  tft.setCursor(0, 10);
  tft.println("  " + IP.toString());
  currentLine = 3;
  // Helligkeit des TFT-Backlights
  pinMode(3, OUTPUT);       // Pin 3 für Backlight
  analogWrite(3, tftLight); // Set initial brightness

  // SPI starten
  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, SD_CS);

  if (useSD) // Wenn SD Card genutzt wird
  {
    //  SD card initialisieren
    if (!SD.begin(SD_CS, spi, 25000000))
    {
      Serial.println("❌ SD not found!");
    }
    else
    {
      Serial.println("✅ SD OK!");
      sdOk = true;
      File file = SD.open("/system.log", FILE_APPEND);
      if (file)
      {
        file.println("Serialsniffer started");
        file.println("Timestamp: " + getDateTimeString());
        file.close();
      }
    }
  }

  // Datei lesen
  readWifiFromSD();

  // Beispiel: alle SSIDs auflisten
  Serial.println("\n📶 Available networks from SD:");
  for (size_t i = 0; i < ssids.size(); i++)
  {
    Serial.printf("%d: %s / %s\n", i + 1, ssids[i].c_str(), passwords[i].c_str());
  }

  // Starte den MQTT Task
  xTaskCreatePinnedToCore(
      mqttTask,         // Funktion
      "MQTT Loop Task", // Name
      8192,             // Stack-Größe
      NULL,             // Parameter
      1,                // Priorität
      &mqttTaskHandle,  // TaskHandle
      1                 // Core 1 (0 wäre auch möglich)
  );
}

void handleSerial(
    HardwareSerial &serial,
    const char *type,
    uint8_t *buf,
    size_t &len,
    unsigned long &last)
{
  while (serial.available() > 0)
  {
    char c = serial.read();
    last = millis();

    bool bufferFull = (len >= MON_BUF_SIZE - 1); // leave room for terminator
    bool eolDetected = (eolDetect &&
                        len > 0 &&
                        isEOLChar(buf[len - 1]) &&
                        !isEOLChar(c));

    // Flush buffer if full or if EOL boundary detected
    if (bufferFull || eolDetected)
    {
      printBuffer(type, buf, len);
      len = 0;
    }

    // Store character only if space remains
    if (len < MON_BUF_SIZE)
    {
      buf[len++] = static_cast<uint8_t>(c);
    }
  }

  // Timeout-based flush
  if (len > 0 && (millis() - last >= timeout))
  {
    printBuffer(type, buf, len);
    len = 0;
  }
}

// =================== IR-Befehls-Tabelle ===================
struct IRCommandEntry
{
  uint8_t command;     // IR Command Code
  bool usesToggle;     // Toggle-Bit auswerten?
  const char *cmdOn;   // Befehl bei Toggle=0
  const char *cmdOff;  // Befehl bei Toggle=1
  const char *message; // Nur Nachricht anzeigen (optional)
};

const char *helpPages[] = {
    "# Seite 1\n"
    " 1 RX Simulation\n"
    " 2 Display heartbeat\n"
    " 3 TFT Update\n"
    " 4 Use SD card",

    "# Seite 2\n"
    " 6 ESPAX on/off\n"
    " 7 Clear LOG\n"
    " 8 Save config\n"
    " 9 enable mqtt",

    "# Seite 3\n"
    " ON Restart device\n"
    " CH UP/DWN Display brightness\n"
    " VOL- / VOL+ WLAN 1-3\n"
    " Weitere Hilfe folgt..."};

const uint8_t helpPageCount = sizeof(helpPages) / sizeof(helpPages[0]);
uint8_t currentHelpPage = 0;

IRCommandEntry irCommands[] = {
    {0x1, true, "Z", "z", nullptr},          // RX simulation
    {0x2, true, "V", "v", nullptr},          // Heartbeat
    {0x3, true, "Q", "q", nullptr},          // TFT update
    {0x4, true, "csdon", "csdoff", nullptr}, // Use SD card
    {0x6, true, "I", "i", nullptr},          // ESPAX on/off
    {0x7, false, "clr", nullptr, nullptr},   // Clear log
    {0x20, false, "lup", nullptr, nullptr},  // Display Light Up
    {0x21, false, "ldn", nullptr, nullptr},  // Display Light Down
    {0x8, false, "S", nullptr, nullptr},     // Save config
    {0x9, true, "J", "j", nullptr},          // MQTT
    {0xC, false, "X", nullptr, nullptr},     // Restart
    {0x0, false, nullptr, nullptr, "HELP"}   // Platzhalter
};

// =================== Wi-Fi ===================
void handleWiFi()
{
  if (wifiConnected && WiFi.status() != WL_CONNECTED)
  {
    textOutln("### Wifi connection dropped. Reconnecting.", 3);
    displayMessage("Wifi connection dropped. Reconnecting.", TFT_WHITE);
    tryWiFiConnect();
  }
}

void handleTimeUpdate()
{
  if (wifiConnected && WiFi.status() == WL_CONNECTED)
  {
    timeClient.update();
  }
}

// =================== Serial Input (non-blocking) ===================
void handleSerialInput()
{
  static String serialCmdBuffer = "";

  while (Serial.available())
  {
    char c = Serial.read();

    if (c == '\n') // Ende der Eingabe
    {
      serialCmdBuffer.trim();
      if (serialCmdBuffer.length() > 0)
      {
        Serial.print("You entered: ");
        Serial.println(serialCmdBuffer);
        parseSerialCommand(serialCmdBuffer);
      }
      serialCmdBuffer = "";
    }
    else if (c != '\r')
    {
      serialCmdBuffer += c;
    }
  }
}
// =================== IR Command Processing ===================
void processIRCommandTable(uint8_t command, uint8_t flags)
{
  bool toggle = flags & IRDATA_FLAGS_TOGGLE_BIT;

  for (auto &entry : irCommands)
  {
    if (entry.command == command)
    {
      if (entry.command == 0x0)
      {
        // Sonderfall: Hilfeseiten blättern
        displayMessage(helpPages[currentHelpPage], TFT_WHITE);
        currentHelpPage = (currentHelpPage + 1) % helpPageCount;
        return;
      }

      if (entry.message && strcmp(entry.message, "HELP") != 0)
      {
        displayMessage(entry.message, TFT_WHITE);
      }
      else if (entry.usesToggle)
      {
        parseSerialCommand(toggle ? entry.cmdOff : entry.cmdOn);
      }
      else if (entry.cmdOn)
      {
        parseSerialCommand(entry.cmdOn);
      }
      return;
    }
  }

  Serial.print("Unknown IR command: 0x");
  Serial.println(command, HEX);
}

// =================== IR Remote ===================
void handleIRRemote()
{
  if (!IrReceiver.decode())
    return;

  if (IrReceiver.decodedIRData.protocol == UNKNOWN)
  {
    // Serial.println(F("Received noise or an unknown (or not yet enabled) protocol"));
    // IrReceiver.printIRResultRawFormatted(&Serial, true);
    IrReceiver.resume();
    return;
  }

  IrReceiver.resume();
  IrReceiver.printIRResultShort(&Serial);
  IrReceiver.printIRSendUsage(&Serial);
  Serial.println();

  processIRCommandTable(IrReceiver.decodedIRData.command, IrReceiver.decodedIRData.flags);
}

// =================== Display Handling ===================
void handleDisplayClear()
{
  if (displayClearScheduled && millis() >= displayClearTime)
  {
    display.clearDisplay();
    display.display();
    displayClearScheduled = false;
  }
}

// =================== Simulation ===================
void handleRxSimulation()
{
  if (rxSimActive && millis() - rxSimLastTime >= rxSimInterval)
  {
    const uint8_t simulatedData[] = {
        0x01, 0x31, 0x02, 0x31, 0x1F, 0x35, 0x30, 0x30, 0x32,
        0x1E, 0x33, 0x1F, 0x31, 0x1E, 0x32, 0x1F,
        0x4E, 0x4F, 0x52, 0x20, 0x53, 0x57, 0x5A, 0x2E,
        0x45, 0x47, 0x20, 0x57, 0x1E, 0x35, 0x1F, 0x31, 0x03};
    size_t len = sizeof(simulatedData);

    memcpy(rxBuf, simulatedData, len);
    printBuffer("RX", rxBuf, len);
    handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);
    rxSimLastTime = millis();
  }
}

void handleHeartbeatSimulation()
{
  if (rxSimActive && millis() - rxSimLastTimeHB >= rxSimIntervalHB)
  {
    // Heartbeat
    const uint8_t hbData[] = {0x04, 0x31, 0x05, 0x32, 0x05};
    memcpy(rxBuf, hbData, sizeof(hbData));
    printBuffer("RX", rxBuf, sizeof(hbData));
    handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);

    // ACK
    const uint8_t ackData[] = {0x06};
    memcpy(txBuf, ackData, sizeof(ackData));
    printBuffer("TX", txBuf, sizeof(ackData));
    handleSerial(SerialTX, "TX", txBuf, txLen, txLast);

    // EOT
    const uint8_t eotData[] = {0x04};
    memcpy(rxBuf, eotData, sizeof(eotData));
    printBuffer("RX", rxBuf, sizeof(eotData));
    handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);

    rxSimLastTimeHB = millis();
  }
}

// =================== Serial Buffer Handling ===================
void handleSerialBuffers()
{
  handleSerial(SerialRX, "RX", rxBuf, rxLen, rxLast);
  handleSerial(SerialTX, "TX", txBuf, txLen, txLast);
}

void handleespax()
{
  unsigned long now = millis();
  // Verbindung prüfen
  if (!espaxclient.connected())
  {
    if (now - lastLoginAttempt >= loginRetryInterval)
    {
      if (ensureConnected())
      {
        if (!doLogin())
        {
          Serial.println("Login nach Reconnect fehlgeschlagen");
        }
      }
      lastLoginAttempt = now;
    }
    return; // nichts weiter machen, solange nicht verbunden
  }

  // Falls Session verloren oder nie erfolgreich, alle 60s neuen Versuch starten
  if (sessionID.length() == 0 && (now - lastLoginAttempt >= loginRetryInterval))
  {
    Serial.println("Versuche erneuten Login...");
    if (!doLogin())
    {
      Serial.println("Login erneut fehlgeschlagen");
      sessionID = ""; // sicherheitshalber
      espaxConnected = false;
    }
    lastLoginAttempt = now;
  }
  // Heartbeat senden, wenn verbunden und Session aktiv
  if (espaxclient.connected() && sessionID.length() > 0)
  {

    if (now - lastHeartbeat >= heartbeatInterval)
    {
      sendHeartbeat();

      Serial.println("sendHeartbeat-Antwort:");
      String xml = readXMLResponse();
      Serial.println(xml);
      int start = xml.indexOf("<RSP-CODE>") + 10;
      int end = xml.indexOf("</RSP-CODE>");
      String rspCode = xml.substring(start, end);
      if (rspCode != "200")
      {
        Serial.println("Espa-x heartbeat failed, session possibly expired");
        sessionID = ""; // Session ungültig
        espaxConnected = false;
      }
      else
      {
        espaxConnected = true;
      }
      lastHeartbeat = now;
    }
  }
}

// =================== LOOP ===================
void loop()
{
  handleWiFi();
  // handleTimeUpdate(); // Glaube ich brauche das nicht wirklich, weil der Client das jede Stunde selbst macht
  handleRxSimulation();
  handleHeartbeatSimulation();
  handleSerialBuffers();
  if (espaxon && wifiConnected) // Nur wenn ESPAX aktiv
    handleespax();
  handleSerialInput();
  handleIRRemote();
  handleDisplayClear();
}
