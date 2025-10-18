# SerialSniffer ![Logo](pic/logo.png)


Monitor two UART lines (RX+TX) using an esp32-s3-devkitc-1. Serial parameters can be set using a CLI and webinterface. Data output using USB/ display and (optional) HTTP and syslog as date;direction;HEX;ASCII. Ignore lines starting with '#' to get CSV compatible data. Send ?\n on USB and webinterface for supported configuration.
- TFT Display
- OLED Display
- WLAN Hotspot if no connection to WLAN
- ESPA Call to JSON 
![Logo](pic/steckbrett.png)
![Logo](pic/plan.png)

- IR Pins von unten: gn,bl,lila
- Klemmstein von links:
  - GND
  - GND
  - TTX (Send to TX)
  - RTX (Receive from TX)
  - TRX (Send to TX)
  - RRX (Receive from RX)
- Serial hinten links RX
- Serial hinten rechts TX

- 9pol Adapter:
-   TX-> RS232 IN
-   GND-> RS232 -


