# chicken-kingdom
Arduino-like code for a Nodemcu to automatically open and close a chicken door, record temperature and humidity

## Full system documentation
The link below describe the full system to perform the functions, including mechanics, electronic board and sensors:
https://docs.google.com/document/d/1NRBMtOAgGudBAg5cqmmfRA5f5Oi3NSKeDBwCM5mq0wo/edit?usp=sharing

## How to use the code:
Use the "template-keys.h" to create a "my-keys.h" file containing your Wifi SSID and key.
The code implements OTA (Over The Air), so that after the first upload, the next updates can rely on wifi only.
