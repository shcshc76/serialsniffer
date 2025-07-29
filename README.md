# SerialSniffer

Monitor two UART lines (RX+TX) using an esp32-s3-devkitc-1. Serial parameters can be set using a CLI and webinterface. Data output using USB/ display and (optional) HTTP and syslog as date;direction;HEX;ASCII. Ignore lines starting with '#' to get CSV compatible data. Send ?\n on USB and webinterface for supported configuration.
- TFT Display
- OLED Display
- WLAN Hotspot if no connection to WLAN
- ESPA Call to JSON 
<img width="1755" height="1141" alt="serialsniffer" src="https://github.com/user-attachments/assets/ee364f00-b2f1-43fc-9ce2-14d8ac180d9a" />
<img width="1504" height="1016" alt="serialsniffer_Schaltplan" src="https://github.com/user-attachments/assets/9ba16bb3-804e-474c-814c-f082e4d2c535" />
