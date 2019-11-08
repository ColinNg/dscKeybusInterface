/*
 *  Twilio Push Notification 1.0 (esp8266)
 *
 *  Processes the security system status and demonstrates how to send a push notification when the status has changed.
 *  This example sends notifications via Twilio: https://www.twilio.com
 *
 *  Wiring:
 *      DSC Aux(-) --- esp8266 ground
 *
 *                                         +--- dscClockPin (esp8266: D1, D2, D8)
 *      DSC Yellow --- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *                                         +--- dscReadPin (esp8266: D1, D2, D8)
 *      DSC Green ---- 15k ohm resistor ---|
 *                                         +--- 10k ohm resistor --- Ground
 *
 *  Virtual keypad (optional):
 *      DSC Green ---- NPN collector --\
 *                                      |-- NPN base --- 1k ohm resistor --- dscWritePin (esp8266: D1, D2, D8)
 *            Ground --- NPN emitter --/
 *
 *  Power (when disconnected from USB):
 *      DSC Aux(+) ---+--- 5v voltage regulator --- esp8266 development board 5v pin (NodeMCU, Wemos)
 *                    |
 *                    +--- 3.3v voltage regulator --- esp8266 bare module VCC pin (ESP-12, etc)
 *
 *  Virtual keypad uses an NPN transistor to pull the data line low - most small signal NPN transistors should
 *  be suitable, for example:
 *   -- 2N3904
 *   -- BC547, BC548, BC549
 *
 *  Issues and (especially) pull requests are welcome:
 *  https://github.com/taligentx/dscKeybusInterface
 *
 *  This example code is in the public domain.
*/

#include <ESP8266WiFi.h>
#include <dscKeybusInterface.h>
#include "Secrets.h"

/*
 * Define the following in a Secrets.h file in the same folder as this .ino file. 
 *

  // Twilio SMS Settings
  // Set the access token generated in the Twilio account settings
  const char* AccountSID = "";
  const char* AuthToken = "";
  
  // echo -n "[AccountSID]:[AuthToken]" | openssl base64 -base64
  const char* Base64EncodedAuth = "";
  
  // i.e. 16046707979
  const char* From = "";
  // i.e. 16041234567
  const char* To = "";

*/

// WiFi Manager
// https://github.com/tzapu/WiFiManager
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     //Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic


// ESP8266 OTA
// https://esp8266.github.io/Arduino/versions/2.0.0/doc/ota_updates/ota_updates.html
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <FS.h>
#include <ArduinoOTA.h>

/**
   @brief mDNS and OTA Constants
   @{
*/
#define HOSTNAME "ESP8266-OTA-" ///< Hostname. The setup function adds the Chip ID at the end.
/// @}

/// Uncomment the next line for verbose output over UART.
//#define SERIAL_VERBOSE

#define AP_NAME "DSC Setup"
#define AP_PASSWORD "DSCSetup8266"


WiFiClientSecure pushClient;

// Configures the Keybus interface with the specified pins.
#define dscClockPin 15  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
#define dscReadPin 5   // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
// If the hardware Write pin is connected, you must specify it below, even if you do not plan to write.
// Otherwise it will cause system faults and attached keypads will beep nonstop.
#define dscWritePin 4  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);

const char* PushMessagePrefix = "Security system ";

void setup() {
  Serial.begin(115200);
  Serial.println();

  Serial.println(F("Starting WiFi Manager"));

  // Start WiFi Manager
  WiFiManager wifiManager;
  wifiManager.autoConnect(AP_NAME, AP_PASSWORD);

  delay(100);

  Serial.print("Chip ID: 0x");
  Serial.println(ESP.getChipId(), HEX);

  // Set Hostname.
  String hostname(HOSTNAME);
  hostname += String(ESP.getChipId(), HEX);
  WiFi.hostname(hostname);

  // Print hostname.
  Serial.println("Hostname: " + hostname);


  // ... Give ESP 10 seconds to connect to station.
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    Serial.write('.');
    //Serial.print(WiFi.status());
    delay(250);
  }
  Serial.println();

  // ... print IP Address
  Serial.print(F("WiFi connected, IP address: "));
  Serial.println(WiFi.localIP());

  // Start OTA server.
  Serial.println(F("Starting OTA server."));
  ArduinoOTA.setHostname((const char *)hostname.c_str());
  ArduinoOTA.begin();


  // Sends a push notification on startup to verify connectivity
  if (sendPush(PushMessagePrefix, "initializing")) Serial.println(F("Initialization push notification sent successfully."));
  else Serial.println(F("Initialization push notification failed to send."));

  // Starts the Keybus interface
  dsc.begin();

  Serial.println(F("DSC Keybus Interface is online."));
}


void loop() {

  // Reads from serial input and writes to the Keybus as a virtual keypad
  if (Serial.available() > 0 && dsc.writeReady) {
    dsc.write(Serial.read());
  }

  if (dsc.handlePanel() && dsc.statusChanged) {  // Processes data only when a valid Keybus command has been read
    dsc.statusChanged = false;                   // Resets the status flag

    // If the Keybus data buffer is exceeded, the sketch is too busy to process all Keybus commands.  Call
    // handlePanel() more often, or increase dscBufferSize in the library: src/dscKeybusInterface.h
    if (dsc.bufferOverflow) Serial.println(F("Keybus buffer overflow"));
    dsc.bufferOverflow = false;

    if (dsc.keypadFireAlarm) {
      dsc.keypadFireAlarm = false;  // Resets the keypad fire alarm status flag
      sendPush(PushMessagePrefix, "fire alarm button pressed");
    }

    if (dsc.keypadAuxAlarm) {
      dsc.keypadAuxAlarm = false;  // Resets the keypad auxiliary alarm status flag
      sendPush(PushMessagePrefix, "aux alarm button pressed");
    }

    if (dsc.keypadPanicAlarm) {
      dsc.keypadPanicAlarm = false;  // Resets the keypad panic alarm status flag
      sendPush(PushMessagePrefix, "panic alarm button pressed");
    }

    if (dsc.powerChanged) {
      dsc.powerChanged = false;  // Resets the battery trouble status flag
      if (dsc.powerTrouble) sendPush(PushMessagePrefix, "AC power trouble");
      else sendPush(PushMessagePrefix, "AC power restored");
    }

    // Checks status per partition
    for (byte partition = 0; partition < dscPartitions; partition++) {

      if (dsc.alarmChanged[partition]) {
        dsc.alarmChanged[partition] = false;  // Resets the partition alarm status flag

        char pushMessage[22] = "in alarm, partition ";
        char partitionNumber[2];
        itoa(partition + 1, partitionNumber, 10);
        strcat(pushMessage, partitionNumber);

        if (dsc.alarm[partition]) sendPush(PushMessagePrefix, pushMessage);
        else sendPush(PushMessagePrefix, "disarmed after alarm");
      }

      if (dsc.fireChanged[partition]) {
        dsc.fireChanged[partition] = false;  // Resets the fire status flag

        char pushMessage[24] = "fire alarm, partition ";
        char partitionNumber[2];
        itoa(partition + 1, partitionNumber, 10);
        strcat(pushMessage, partitionNumber);

        if (dsc.fire[partition]) sendPush(PushMessagePrefix, pushMessage);
        else sendPush(PushMessagePrefix, "fire alarm restored");
      }

      if (dsc.armedChanged[partition]) {
        dsc.armedChanged[partition] = false;  // Resets the armed status flag

        char pushMessage[29] = "armed, partition ";
        char partitionNumber[2];
        itoa(partition + 1, partitionNumber, 10);
        strcat(pushMessage, partitionNumber);

        if (dsc.armed[partition])
        {
          strcat(pushMessage, "       ");
          sendPush(PushMessagePrefix, pushMessage);
        }
        else if (dsc.armedAway[partition])
        {
          strcat(pushMessage, " (Away)");
          sendPush(PushMessagePrefix, pushMessage);
        }
        else if (dsc.armedStay[partition])
        {
          strcat(pushMessage, " (Stay)");
          sendPush(PushMessagePrefix, pushMessage);
        }
        else
        {
          char pushMessage[29] = "disarmed, partition ";
          strcat(pushMessage, partitionNumber);
          sendPush(PushMessagePrefix, pushMessage);
        }
      }
    }

    printTimestamp();
    Serial.print(" ");
    dsc.printPanelCommand();
    Serial.print(" ");
    dsc.printPanelMessage();
    Serial.println();
  }
  else if (dsc.handleModule()) {
    printTimestamp();
    Serial.print(" ");
    dsc.printModuleMessage();
    Serial.println();
  }

  // Handle OTA server.
  ArduinoOTA.handle();
  yield();
  
}


bool sendPush(const char* prefix, const char* pushMessage) {
  Serial.print(F("Pushing message: "));
  Serial.print(prefix);
  Serial.println(pushMessage);

  // Connects and sends the message as x-www-form-urlencoded
  if (!pushClient.connect("api.twilio.com", 443)) return false;
  pushClient.print(F("POST https://api.twilio.com/2010-04-01/Accounts/"));
  pushClient.print(AccountSID);
  pushClient.println(F("/Messages.json HTTP/1.1"));
  pushClient.print(F("Authorization: Basic "));
  pushClient.println(Base64EncodedAuth);
  pushClient.println(F("Host: api.twilio.com"));
  pushClient.println(F("User-Agent: ESP8266"));
  pushClient.println(F("Accept: */*"));
  pushClient.println(F("Content-Type: application/x-www-form-urlencoded"));
  pushClient.print(F("Content-Length: "));
  pushClient.println(strlen(To) + strlen(From) + strlen(prefix) + strlen(pushMessage) + 18);  // Length including data
  pushClient.println(F("Connection: Close"));
  pushClient.println();
  pushClient.print(F("To=+"));
  pushClient.print(To);
  pushClient.print(F("&From=+"));
  pushClient.print(From);
  pushClient.print(F("&Body="));
  pushClient.print(prefix);
  pushClient.println(pushMessage);

  // Waits for a response
  unsigned long previousMillis = millis();
  while (!pushClient.available()) {
    dsc.handlePanel();
    if (millis() - previousMillis > 3000) {
      Serial.println(F("Connection timed out waiting for a response."));
      pushClient.stop();
      return false;
    }
    yield();
  }

  // Reads the response until the first space - the next characters will be the HTTP status code
  while (pushClient.available()) {
    if (pushClient.read() == ' ') break;
  }

  // Checks the first character of the HTTP status code - the message was sent successfully if the status code
  // begins with "2"
  char statusCode = pushClient.read();

  // Successful, reads the remaining response to clear the client buffer
  if (statusCode == '2') {
    while (pushClient.available()) pushClient.read();
    pushClient.stop();
    return true;
  }

  // Unsuccessful, prints the response to serial to help debug
  else {
    Serial.println(F("Push notification error, response:"));
    Serial.print(statusCode);
    while (pushClient.available()) Serial.print((char)pushClient.read());
    Serial.println();
    pushClient.stop();
    return false;
  }
}


// Prints a timestamp in seconds (with 2 decimal precision) - this is useful to determine when
// the panel sends a group of messages immediately after each other due to an event.
void printTimestamp() {
  float timeStamp = millis() / 1000.0;
  if (timeStamp < 10) Serial.print("    ");
  else if (timeStamp < 100) Serial.print("   ");
  else if (timeStamp < 1000) Serial.print("  ");
  else if (timeStamp < 10000) Serial.print(" ");
  Serial.print(timeStamp, 2);
  Serial.print(F(":"));
}
