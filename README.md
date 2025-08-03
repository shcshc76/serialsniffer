# SerialSniffer ![Logo](pic/logo.png)


Monitor two UART lines (RX+TX) using an esp32-s3-devkitc-1. Serial parameters can be set using a CLI and webinterface. Data output using USB/ display and (optional) HTTP and syslog as date;direction;HEX;ASCII. Ignore lines starting with '#' to get CSV compatible data. Send ?\n on USB and webinterface for supported configuration.
- TFT Display
- OLED Display
- WLAN Hotspot if no connection to WLAN
- ESPA Call to JSON 
![Logo](pic/steckbrett.png)
![Logo](pic/plan.png)

TFT Pins
gnd  br
vcc  rt
scl  or
sda  ge
res  gn
dc  bl
cs  lila
blk  gr

oled Pins
gnd  sw
vcc  ws
scl  gr
sda  lila
