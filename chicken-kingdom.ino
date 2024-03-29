#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>

// Wifi settings
#include "my-keys.h"
#define ServerQuery "http://192.168.0.100:1880/update-chicken-sensors?temp=%2.1f&RH=%2.1f&Light=%d&door=%s&LED=%s"
IPAddress staticIP(192,168,0,99);
IPAddress gateway(192,168,0,254);
IPAddress subnet(255,255,255,0);

//Calculation of the number of steps necessary for the motor(s) to open and close the door
const int stepsPerRevolution = 2038;  // the number of steps per revolution
const float doorDisplacementHeight = 360.0; //in mm the height of open door, also the length of rope to get
const float shaftDiameter = 4.4; //in mm the diameter of the shaft on which the rope is rolled, for accuracy, please include the rope diameter if non negligible (twice radius)
const float reductionFactor = 1.0;// the reduction factor between the rotation of the motor and the shaft
const float correctionFactor = 1.0;// use to tweak easily from experience
const int turnsToOpenOrClose = correctionFactor*doorDisplacementHeight / shaftDiameter / 3.14 * reductionFactor;
const long int stepsToOpenOrClose = stepsPerRevolution*turnsToOpenOrClose;
const int millsBetweenSteps= 10;//10ms corresponds to 100Hz supposed for max torque with 28BYJ-48

//Light Thresholds
const int lightOpeningThreshold = 100;
const int lightClosingThreshold = 17;

//Additional timer before closing
const unsigned long millsToWaitBeforeClosing= 1000*60*15;//15 minutes * 60 secondes/minutes * 1000 millis/sec
boolean IsDoorPendingClosing=false;

//minimum light time per day
const unsigned long minimumLightTime=11*60*60*1000;//milliseconds
unsigned long dayStart=0;//milliseconds
unsigned long dayStop=0;//milliseconds
unsigned long lightStop;//milliseconds
unsigned long addLightDuration=60*1000;//milliseconds

//status booleans
boolean IsOpen;
boolean IsOpening=false;
boolean IsClosing=false;
boolean DoorAuto=true;
boolean IsLEDOn=false;
const boolean verbose=true;

//LDR pin
#define LDRpin A0 // ESP8266 Analog Pin ADC0 = A0

//stepper pins GPIO
#define GPIOStep1 15
#define GPIOStep2 13
#define GPIOStep3 12
#define GPIOStep4 14

// Si7021 pins GPIO and I2C address
#define si7021SdaGPIO 4 // pins D1=5
#define si7021SclGPIO 5 // pins D2=4
#define si7021Addr 0x40 // SI7021 I2C address is 0x40(64)

// Light pin
#define LEDGPIO 2 // D4


// initialize web server
ESP8266WebServer server(80);

//frequency of luminosity check and of data sending
const int secondsBetweenChecks=2;
const int secondsBetweenSend=60*15;//as to be longer than time between checks
unsigned long lastSendingTime = secondsBetweenSend*1000;//in milliseconds
float lightValue  = (float) analogRead(LDRpin);

//////////////////////////////////////////////////
/// Function to get Humidity data from sensor ///
////////////////////////////////////////////////
float getHumidity()
{
  unsigned int data[2];
 
  Wire.beginTransmission(si7021Addr);
  //Send humidity measurement command
  Wire.write(0xF5);
  Wire.endTransmission();
  delay(250);
 
  // Request 2 bytes of data
  Wire.requestFrom(si7021Addr, 2);
  // Read 2 bytes of data to get humidity
  if(Wire.available() == 2)
  {
    data[0] = Wire.read();
    data[1] = Wire.read();
  }
 
  // Convert the data
  float humidity  = ((data[0] * 256.0) + data[1]);
  humidity = ((125 * humidity) / 65536.0) - 6;
 
  // Output data to serial monitor
  if (verbose) 
  {
    Serial.print("Humidity : ");
    Serial.print(humidity);
    Serial.println(" % RH");
  }

  return humidity;  
}

/////////////////////////////////////////////////////
/// Function to get Temperature data from sensor ///
///////////////////////////////////////////////////
float getTemperatureC()
{
  unsigned int data[2];
 
  Wire.beginTransmission(si7021Addr);
  // Send temperature measurement command
  Wire.write(0xF3);
  Wire.endTransmission();
  delay(250);
 
  // Request 2 bytes of data
  Wire.requestFrom(si7021Addr, 2);
 
  // Read 2 bytes of data for temperature
  if(Wire.available() == 2)
  {
    data[0] = Wire.read();
    data[1] = Wire.read();
  }
 
  // Convert the data
  float temp  = ((data[0] * 256.0) + data[1]);
  float celsTemp = ((175.72 * temp) / 65536.0) - 46.85;
//  float fahrTemp = celsTemp * 1.8 + 32;
 
  // Output data to serial monitor
  if (verbose)
  {
    Serial.print("Celsius : ");
    Serial.print(celsTemp);
    Serial.println(" C");
//    Serial.print("Fahrenheit : ");
//    Serial.print(fahrTemp);
//    Serial.println(" F");
  }
  return celsTemp;
}

///////////////////////////////////////////
/// Function to send data through Wifi ///
/////////////////////////////////////////
//Function to describe door status in a simple string based on status booleans
String DoorStatus(){
  String doorstatus = "";
  if (IsOpening){
    doorstatus = "opening" ;
  } else if (IsClosing) {
    doorstatus = "closing" ;
  } else if (IsOpen) {
    doorstatus = "open" ;
  } else {
    doorstatus = "closed" ;
  }
  return doorstatus;
}

void sendData()
{
  //sends 
  if(WiFi.status()== WL_CONNECTED){
      WiFiClient client;
      HTTPClient http;
      char serverPath[100]="";
      snprintf(serverPath, 250, ServerQuery, getTemperatureC(), getHumidity(), (int)round(lightValue), DoorStatus().c_str(),IsLEDOn?"on":"off");
      http.begin(client,serverPath);
      // Send HTTP GET request
      int httpResponseCode = http.GET();
      
      if (httpResponseCode>0 && verbose) {
        Serial.print("HTTP Response code: ");
        Serial.println(httpResponseCode);
        String payload = http.getString();
        Serial.println(payload);
      }
      else if (verbose) {
        Serial.print("Error code: ");
        Serial.println(httpResponseCode);
      }
      // Free resources
      http.end();
    }
    else if (verbose) {
      setupWifi();
      if (verbose){
        Serial.println("WiFi Disconnected");
      }
  }
  
}

////////////////////////////////////////
///  HTTP server handling functions  ///
//////////////////////////////////////
void handle_OnConnect() {
  //for main http server page, return the page built by HTMLPage() function
  server.send(200, "text/html", HTMLPage()); 
}

void handle_doorisopen() {
  //force opening status as true then redirect to main page
  //this is used in automatic mode if the system thinks that door is closed while it is actually open
  IsOpen = true;
  server.send(200, "text/html", HTMLRedirect(1,"OK, door is open")); 
}

void handle_doorisclosed() {
  //force opening status as false then redirect to main page
  //this is used in automatic mode if the system thinks that door is open while it is actually closed
  IsOpen = false;
  server.send(200, "text/html", HTMLRedirect(1,"OK, door is closed")); 
}

void handle_doorAutoOn() {
  //set automatic mode on then redirect to main page
  DoorAuto = true;
  server.send(200, "text/html", HTMLRedirect(1,"Door opening now automatic")); 
}

void handle_doorAutoOff() {
  //set automatic mode off then redirect to main page
  DoorAuto = false;
  server.send(200, "text/html", HTMLRedirect(1,"Door opening now manual")); 
}

void handle_openDoor() {
  //open the door and redirect to main page
  //this is used in manual mode (doorAuto = false)
  server.send(200, "text/html", HTMLRedirect(3,"OK, start opening the door")); 
  openDoor();
}

void handle_closeDoor() {
  //close the door and redirect to main page
  //this is used in manual mode (doorAuto = false)
  server.send(200, "text/html", HTMLRedirect(3,"OK, start closing the door")); 
  closeDoor();
}

void handle_reboot() {
  //release motor current, restart the microcontroller and redirect to main page after enough seconds for reboot
  freeMotor();
  server.send(200, "text/html", HTMLRedirect(12,"Rebooting, please wait...")); 
  ESP.restart(); 
}

void handle_NotFound(){
  //return error 404
  server.send(404, "text/plain", "Not found");
}

void handle_addlighton() {
  //set light on for a period of time if it is it not already, otherwise add the amount of time to light duration
  if (IsLEDOn){
    lightStop=(unsigned long)(lightStop + addLightDuration);
//    dayStart=(unsigned long)(dayStart - addLightDuration);
    server.send(200, "text/html", HTMLRedirect(1,"OK, added light duration")); 
  }
  else {
    IsLEDOn=true;
    digitalWrite(LEDGPIO, HIGH);
    lightStop=(unsigned long)(millis() + addLightDuration);
//    dayStart=(unsigned long)(millis() - addLightDuration);
    server.send(200, "text/html", HTMLRedirect(1,"OK, light turned ON for short duration")); 
  }
}

void handle_setlightoff() {
  //set light off, whatever the remaining time of light duration
  digitalWrite(LEDGPIO, LOW);
  IsLEDOn=false;      
  server.send(200, "text/html", HTMLRedirect(1,"OK, light turned OFF")); 
}


/////////////////////////////////////////////
///      Main HTML Page for server       ///
///////////////////////////////////////////
String HTMLPage(){
  //builds the source of the main HTML page with sensor and status values plus buttons for interactions
  String ptr = "<!DOCTYPE html>\n";
  ptr +="<html>\n";
  ptr +="<head>\n";
  ptr +="<title>Chickens Kingdom</title>\n";
  ptr +="<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
  ptr +="</head>\n";
  ptr +="<body>\n";
  ptr +="<h1>Status</h1>\n";
  ptr +="<p>Temperature: ";
  ptr += String(getTemperatureC());
  ptr +="</p>\n";
  ptr +="<p>Humidity: ";
  ptr += String(getHumidity());
  ptr +="</p>\n";
  ptr +="<p>Light value (0 to 1024): ";
  ptr += String(lightValue);
  ptr +="</p>\n";
  if (IsDoorPendingClosing==true){
    ptr += "<p> the door is pending closing for "+millisToNiceStr((unsigned long)(millis() - dayStop))+"</p>\n";
  }
  ptr +="<p>The door is ";
  ptr += DoorStatus();
  if (DoorStatus()=="open"){
    ptr += " for last " + millisToNiceStr((unsigned long)(millis()- dayStart));
  }
  else if (DoorStatus()=="closed"){
    ptr += " for last " + millisToNiceStr((unsigned long)(millis()- dayStop - millsToWaitBeforeClosing));
  }
  ptr +="</p>\n";
  ptr +="<p>The LED is ";
  if (IsLEDOn){
    ptr += "ON for next " + millisToNiceStr((unsigned long)(lightStop - millis()));
  } 
  else {
    ptr += "OFF";
  }
  ptr +="</p>\n";
  ptr +="<p>The door opening mode is ";
  ptr += DoorAuto ? "automatic" : "manual" ;
  ptr +="</p>\n";
  ptr +="<h1>Actions</h1>\n";
  ptr +="<form method=\"get\">\n";
  if (DoorAuto) {
    // in automatic mode, buttons to tell the door is actually open if the system thinks it is close and vice versa
    // plus a button to turn to manual mode
    if (IsOpen) {
      ptr +="<input type=\"button\" value=\"Tell door is closed\" onclick=\"window.location.href='/doorisclosed'\">\n";
    }
    else {
      ptr +="<input type=\"button\" value=\"Tell door is open\" onclick=\"window.location.href='/doorisopen'\">\n";
    }
    ptr +="<input type=\"button\" value=\"Switch to door manual mode\" onclick=\"window.location.href='/doorautooff'\">\n";
  }
  else {
    // in manual mode, buttons open and close the door plus a button to turn to automatic mode
    ptr +="<input type=\"button\" value=\"Open the door\" onclick=\"window.location.href='/opendoor'\">\n";
    ptr +="<input type=\"button\" value=\"Close the door\" onclick=\"window.location.href='/closedoor'\">\n";
    ptr +="<input type=\"button\" value=\"Switch to door automatic mode\" onclick=\"window.location.href='/doorautoon'\">\n";
  }
  ptr +="<br/>";
  if (IsLEDOn){
    ptr +="<input type=\"button\" value=\"Add light ON duration\" onclick=\"window.location.href='/addlighton'\">\n";
    ptr +="<input type=\"button\" value=\"Turn light OFF\" onclick=\"window.location.href='/setlightoff'\">\n";
  }
  else{
    ptr +="<input type=\"button\" value=\"Turn light ON\" onclick=\"window.location.href='/addlighton'\">\n";    
  }
  ptr +="<br/>";
  ptr +="<input type=\"button\" value=\"Reboot\" onclick=\"window.location.href='/reboot'\">\n";
  ptr +="</form>\n";
  ptr +="<h1>Parameters</h1>\n";
  ptr +="<p>Opening if light over: ";
  ptr += String(lightOpeningThreshold);
  ptr +="</p>\n";
  ptr +="<p>Closing if light below: ";
  ptr += String(lightClosingThreshold);
  ptr +="</p>\n";
  ptr +="<p>Additional time before closing the door at sunset: ";
  ptr += millisToNiceStr(millsToWaitBeforeClosing);
  ptr +="</p>\n";
  ptr +="<p>Minimum duration of light: ";
  ptr += millisToNiceStr(minimumLightTime);
  ptr +="</p>\n";
  ptr +="<p>Closing and opening duration (due to motor speed): ";
  ptr += millisToNiceStr(stepsToOpenOrClose*millsBetweenSteps);
  ptr +="</p>\n";
  ptr +="<p>Time between light checks: ";
  ptr += millisToNiceStr((unsigned long)secondsBetweenChecks*1000);
  ptr +="</p>\n";
  ptr +="<p>Time between data send: ";
  ptr += millisToNiceStr((unsigned long)secondsBetweenSend*1000);
  ptr +="</p>\n";
  ptr +="</body>\n";
  ptr +="</html>\n";
  return ptr;
}

String millisToNiceStr(unsigned long millisecondsValue){
  String niceStr = "";
  int niceHours=(int)((unsigned long)(millisecondsValue)/1000/60/60);
  int niceMin=(int)((unsigned long)(millisecondsValue)/1000/60)%60;
  int niceSec=(int)((unsigned long)(millisecondsValue)/1000)%60;
  if (niceHours>0){
    niceStr+= String(niceHours) + " hours ";
  }
  niceStr += String(niceMin) + "min "+ String(niceSec) + "sec ";
  return niceStr;
}


String HTMLRedirect(int seconds,String message){
  //builds an html page that redirects to main page after some seconds and with a message
  String ptr = "<!DOCTYPE html>\n<html>\n<head>\n<meta http-equiv=\"refresh\" content=\"";
  ptr +=String(seconds);
  ptr +="; url=http://";
  ptr += WiFi.localIP().toString(); 
  ptr +="\"/>\n</head>\n<body>\n";
  ptr +=message;
  ptr +="</body>\n</html>\n";
return ptr;
}

/////////////////////////////////////////////
///                SETUP                 ///
///////////////////////////////////////////
void setup()
{
  ////   HR-TEMP SETUP ////
  Wire.begin(si7021SdaGPIO,si7021SclGPIO); //Wire.begin(int sda, int scl) with GPIO number
  Serial.begin(115200);
  Wire.beginTransmission(si7021Addr);
  Wire.endTransmission();
  delay(300);

  
  ////   STEPPER, LDR AND EXTERNAL LED SETUP ////
  pinMode(LEDGPIO, OUTPUT);
  digitalWrite(LEDGPIO, LOW);
  pinMode(GPIOStep1, OUTPUT);
  pinMode(GPIOStep2, OUTPUT);
  pinMode(GPIOStep3, OUTPUT);
  pinMode(GPIOStep4, OUTPUT);
  freeMotor();
  lightValue  = (float)analogRead(LDRpin);
  // we consider that if the reset/start occur when light is on, it is the day, the door is already open 
  //(this logic provides robustness in case of short duration of power cut)
  if (lightValue > (lightOpeningThreshold + lightClosingThreshold)*0.5)
  {
  Serial.println("Starting with daylight, door considered open");
      IsOpen=true;
  }
  else if (lightValue < (lightOpeningThreshold + lightClosingThreshold)*0.5)
  {
  Serial.println("Starting at night, door considered closed");
      IsOpen=false;
  }


  //// WIFI SETUP ////
  setupWifi();

  //// HTTP SERVER SETUP ////
  server.on("/", handle_OnConnect);
  server.on("/doorisopen", handle_doorisopen);
  server.on("/doorisclosed", handle_doorisclosed);
  server.on("/doorautoon", handle_doorAutoOn);
  server.on("/doorautooff", handle_doorAutoOff);
  server.on("/opendoor", handle_openDoor);
  server.on("/closedoor", handle_closeDoor);
  server.on("/reboot", handle_reboot);
  server.on("/setlightoff", handle_setlightoff);
  server.on("/addlighton", handle_addlighton);
  server.onNotFound(handle_NotFound);
  server.begin();

  // setup for OTA update capabilities
  setupOTA();

}

void setupOTA()
{
  //setup the capabilities to update the code Over The Air (i.e. through wifi)
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
}

void setupWifi()
{
  //setup the wifi connection 
  WiFi.mode(WIFI_STA);
  WiFi.config(staticIP, gateway, subnet);
  WiFi.begin(WifiSSID,WifiKey);
  WiFi.reconnect();
  Serial.print("Connecting");
  int nTrials=0;
  int trialDurationSec=0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    trialDurationSec=trialDurationSec+1; 
    Serial.print(".");
    if (trialDurationSec==30) {
      trialDurationSec=0;
      nTrials=nTrials+1;
      Serial.println("try again");
      WiFi.reconnect();
      Serial.print("Connecting");
    }
    if (nTrials==5){
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED){
    Serial.println();
    Serial.print("Connected, IP address: ");
    Serial.println(WiFi.localIP());
  }
}

///////////////////////////////////
///      MOTOR FUNCTIONS      ////
/////////////////////////////////
void freeMotor()
{
  digitalWrite(GPIOStep1, LOW);
  digitalWrite(GPIOStep2, LOW);
  digitalWrite(GPIOStep3, LOW);
  digitalWrite(GPIOStep4, LOW);  
}

void openDoor()
{
  IsOpening=true;
  Serial.println("++ OPEN");
  sendData();
  for (int i = 0; i <= stepsToOpenOrClose*0.25; i++) {
    if (i%(int(1000/millsBetweenSteps))==0){
      server.handleClient();
    }
    digitalWrite(GPIOStep1, HIGH);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, HIGH);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, HIGH);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, HIGH);
    digitalWrite(GPIOStep4, HIGH);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, HIGH);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, HIGH);
    digitalWrite(GPIOStep3, HIGH);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, HIGH);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, HIGH);
    digitalWrite(GPIOStep2, HIGH);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, HIGH);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
  }
  freeMotor();
  IsOpening=false;
  IsOpen=true;
  sendData();
}

void closeDoor()
{
  IsClosing=true;
  Serial.println("++ CLOSE");
  sendData();
  for (int i = 0; i <= stepsToOpenOrClose*0.25; i++) {
    if (i%(int(1000/millsBetweenSteps))==0){
      server.handleClient();
    }
    digitalWrite(GPIOStep1, HIGH);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, HIGH);
    digitalWrite(GPIOStep2, HIGH);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, HIGH);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, HIGH);
    digitalWrite(GPIOStep3, HIGH);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, HIGH);
    digitalWrite(GPIOStep4, LOW);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, HIGH);
    digitalWrite(GPIOStep4, HIGH);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, LOW);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, HIGH);
    delay(millsBetweenSteps);
    digitalWrite(GPIOStep1, HIGH);
    digitalWrite(GPIOStep2, LOW);
    digitalWrite(GPIOStep3, LOW);
    digitalWrite(GPIOStep4, HIGH);
    delay(millsBetweenSteps);
  }
  freeMotor();
  IsClosing=false;
  IsOpen=false;
  sendData();
}
/////////////////////////////////////////////
///                LOOP                  ///
///////////////////////////////////////////
void loop()
{
  ArduinoOTA.handle();
  server.handleClient();

  // measure light and start stepper if necessary
  lightValue  = 0.9*lightValue+0.1*analogRead(LDRpin);
  if (verbose) {
    Serial.print("Light value [0-1023] : ");
    Serial.println(lightValue); //uncomment to ease calibration
  }
  if (lightValue > lightOpeningThreshold && IsOpen==false && DoorAuto)
  {
    // Opening:
    openDoor();
    // record time of day start
    dayStart=millis();
  }
  else if (lightValue < lightClosingThreshold && IsOpen && DoorAuto && IsDoorPendingClosing==false)
  {
    IsDoorPendingClosing=true;
    // record time of day stop
    dayStop=millis();
    //once sun is down, if the time of light was too low, light up the LED
    if ((unsigned long)(dayStop - dayStart) < minimumLightTime){
      lightStop=(unsigned long)(dayStart+minimumLightTime);
      digitalWrite(LEDGPIO, HIGH);
      IsLEDOn=true;
    }
  }

  if (IsDoorPendingClosing)
  {
    if ((unsigned long)(millis() - dayStop) > millsToWaitBeforeClosing)
    {
      IsDoorPendingClosing=false;
      // Closing:
      closeDoor();
    }
  }

  // if the LED is on, check the duration of light time to shut it down on time
  if (IsLEDOn) {
    //check if the light Stop time is passed. 
    //Second condition is to avoid misinterpretation in case of lightStop reaching long max size, considering each check is within 5 min !
    if ((millis()>lightStop) && ((unsigned long)(millis()-lightStop)<1000*60*5)){
      digitalWrite(LEDGPIO, LOW);
      IsLEDOn=false;      
    }
  }

  // send data through wifi
  if ((unsigned long)(millis() - lastSendingTime) >= secondsBetweenSend*1000){
    lastSendingTime=millis();
    sendData();
  }
  
  // wait before next loop
  delay(secondsBetweenChecks*1000);
}
