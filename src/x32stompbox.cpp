// ***************************************************************
// Behringer X32 stompbox for assigns and mute groups
// ***************************************************************
// 2023-03-04 initial draft
// Supports:
// - mutes        /ch/01/mix/on,i     Led state is reversed
// - mute groups  /config/mute/1,i  
// - faders       /ch/02/mix/09/level,f
// - snippets     /load,snippet i
// Features:
// - one-way (just send) - in case we don't want to hog the bandwidth
// - two-way (receive confirmation and update LED)
// - monitor battery voltage
// Issues:
// - excess power wasted trying to reconnect to WiFi if unable to connect (extra 70mA approx)
// - WiFi password is hardcoded 
// Thoughts:
// - subscribe vs xremote?  subscribe gives stream of data even if no changes
// ***************************************************************
// TODO
// [ ] hostname to work? esp32-086628 seems to be the default
// [ ] battery monitor implementation
// [x] rewrite without String class (midi message)
// [x] convert fader f to midi 0-127 sysex
// [ ] test implementation of MIDI output; does MIDI SysEx method accept float?
// [ ] does actual X32 echo mute, fader, mutegroup?
// [ ] reliability of toggles - udp not always sent????
// ***************************************************************
// FUTURE
// [ ] find other way to save energy usage if cannot connect to WiFi (WiFi.disconnect doesn't seem to stop the process)
// ***************************************************************

// #include <Arduino.h> // this is already called by Button.h, etc

// button library https://github.com/madleech/Button
#include <Button.h>

// wifi library https://www.arduino.cc/en/Reference/WiFi
#include <WiFi.h>
#include <WiFiUdp.h>

// osc message library https://github.com/CNMAT/OSC
#include <OSCMessage.h>

// MIDI support https://github.com/FortySevenEffects/arduino_midi_library
#include <MIDI.h>
#include <midi_Defs.h>
#include <midi_Message.h>
#include <midi_Namespace.h>
#include <midi_Settings.h>

// ***************************************************************
// debug
// ***************************************************************
#undef VERBOSE_DEBUG

// ***************************************************************
// Site settings
// ***************************************************************
#define USE_SECRETS
#ifdef USE_SECRETS
#include <secrets.h>
#else
#define MYX32ADDRESS (192, 168, 32, 32) // IP of target X32
#define MYSSID "the_ssid"
#define MYPASS "the_password"
#endif

// ***************************************************************
// constructs
// ***************************************************************
class OSCWidget
{
  // depends on Button.h
  // depends on pinMode, digitalWrite
public:
  char *friendlyDebugName; // e.g. Button 1, Button 2
  uint8_t buttonPin;       // corresponding GPIO pin
  uint8_t ledPin;          // corresponding GPIO pin
  Button button;

  bool isOscToggle;   // is this a toggle (like Mute) or snippet
  bool isReverseLed;  // LED state opposite to boolean state
  char *oscAddress;   // OSC address
  char *oscPayload_s; // OSC payload - relevent only for snippets
  int oscState;       // OSC state (for toggle values like Mute)
  int oscPayload_i;   // for loading snippets
  float oscPayload_f; // for fader values

  OSCWidget(char *theFriendlyName,
            int theButtonPin,
            int theLedPin,
            int theActionType,
            int theLedResponse,
            char *theOscAddress,
            char *theOscPayload_s,
            int theOscIndex = -1,
            float theOscPayload_f = -1)
      : button(theButtonPin),
        friendlyDebugName(theFriendlyName),
        buttonPin(theButtonPin),
        ledPin(theLedPin),
        isOscToggle(theActionType),
        isReverseLed(theLedResponse),
        oscAddress(theOscAddress),
        oscPayload_s(theOscPayload_s),// use "" if not used
        oscPayload_f(theOscPayload_f),// use -1 if not used
        oscPayload_i(theOscIndex),    // use -1 if not used
        oscState(0)
  {
    pinMode(buttonPin, INPUT_PULLUP); // initialise the pin for input
    pinMode(ledPin, OUTPUT);          // initialise the pin for LED
    button.begin();
  };

  void doDigitalWrite(uint8_t val)
  {
    digitalWrite(ledPin, val);
  };

  void print()
  {
    Serial.print(friendlyDebugName);
    Serial.print(",\t");
    Serial.print(buttonPin);
    Serial.print(",\t");
    Serial.print(ledPin);
    Serial.print(",\t");
    Serial.print(isOscToggle);
    Serial.print(",\t");
    Serial.print(isReverseLed);
    Serial.print(",\t");
    Serial.print(oscAddress);
    Serial.print(", ");
    Serial.print(oscPayload_s);
    Serial.print(", i ");
    Serial.print(oscPayload_i);
    Serial.print(", f ");
    Serial.print(oscPayload_f);    
    Serial.print(" (");
    Serial.print(oscState);
    Serial.println(")");
  };
};

// MIDI OSC Hex converter
char bigMidiCommand[64];                            // maximum command length in bytes
char midiHeader[] = {0xF0, 0x00, 0x20, 0x32, 0x32}; // X32 OSC preamble
char midiSpacer[] = {0x20};                         // X32 OSC spacer
char midiFooter[] = {0xF7};                         // X32 OSC post-amble

char *stringOFF = "OFF";
char *stringON = "ON";

// ***************************************************************
// site settings, network configuration, etc
// ***************************************************************
char const *ssid = MYSSID;
char const *pass = MYPASS;

const IPAddress X32Address MYX32ADDRESS;
const unsigned int X32Port = 10023;  // X-AIR is 10024, X32 is 10023
const unsigned int localPort = 8888; // local port to listen for OSC packets (also sends UDP from this port)
#define MY_HOSTNAME "X32_StompBox"

// ***************************************************************
// payload and button configuration, including pin configuration
// ***************************************************************
OSCWidget myWidgets[] = {
    // friendly_name, button_pin, led_pin, isOscToggle, isReverseLed, oscAddress, oscPayload_s, [oscPayload_i]
    OSCWidget("Button A", 32, 19, true,  true , "/ch/01/mix/on",        ""),
    OSCWidget("Button B", 33, 23, false, false, "/load",                "snippet", 99),
    OSCWidget("Button C", 25, 18, true,  false,  "/config/mute/1",       ""),
    OSCWidget("Button D", 26,  5, false, false, "/ch/02/mix/09/level",  "", -1 , 0.75)};
// GPIO INPUTS 34,35,36,39 do not have internal pull-up/pull-down
#define MIDI_UART 2
#define PIN_FOR_BATTERY_STATUS_LED 4
#define PIN_FOR_WIFI_STATUS_LED 22      // internal LED is 22 for my LOLIN32
#define PIN_FOR_BATTERY_VOLTAGE 36      // cannot use ADC2 pins (needed for WiFi)
#define BATTERY_VOLTAGE_LOW_CUTOFF 3000 // 0 - 4095; value depends on voltage divider circuit
#define PIN_FOR_MODE_SWITCH 14

#if true
// LED lights up if pin pulls voltage down
#define LED_PIN_ON LOW
#define LED_PIN_OFF HIGH
#else
// LED is powered from pin
#define LED_PIN_ON HIGH
#define LED_PIN_OFF LOW
#endif

// ******************************************************
// other variables
// ******************************************************
Button modeButton(PIN_FOR_MODE_SWITCH);
bool do_xRemote = true;
bool do_Refresh = true;
WiFiUDP Udp;
HardwareSerial SerialMIDI(MIDI_UART);
MIDI_CREATE_INSTANCE(HardwareSerial, SerialMIDI, midiOut); // create a MIDI object called midiOut
TaskHandle_t xUDPLoopHandle = NULL;
TaskHandle_t xPokeOSCLoopHandle = NULL;

// ***************************************************************
// ***************************************************************
// ***************************************************************

// ***************************************************************
// various minor helper functions
// ***************************************************************

void printMillis()
{
  Serial.print("[");
  Serial.print(millis());
  Serial.print("] ");
}

// ***************************************************************
// void midiBuildCommand
// - construct a MIDI SysEx from the OSC command
// ***************************************************************
void midiBuildCommand(char* oscCommand, char* oscArgument)
{
  // update global variable: bigMidiCommand
  // no error checking to ensure that bigMidiCommand does not exceed 64 char
  bigMidiCommand[0] = 0;
  strcat(bigMidiCommand,midiHeader);
  strcat(bigMidiCommand,oscCommand);
  strcat(bigMidiCommand,midiSpacer);
  strcat(bigMidiCommand,oscArgument);
  strcat(bigMidiCommand,midiFooter);

  // DEBUG print the HEX string generated
#ifdef VERBOSE_DEBUG
  Serial.print("MIDI Message in HEX: ");
  for (int j = 0; j < strlen(bigMidiCommand); j++) // why 29???
  {
    if (bigMidiCommand[j] < 0x10)
    {
      Serial.print("0");
    };
    Serial.print(bigMidiCommand[j], HEX);
    Serial.print(" ");
  }
  Serial.println("");
#endif
}

// ***************************************************************
// WiFiStationConnected
// WiFiGotIP
// WiFiStationDisconnected
// wl_status_to_string
// - example Serial.println(wl_status_to_string(WiFi.status()));
// ***************************************************************

void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
  printMillis();
  Serial.println("Connected to AP");
}

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
  printMillis();
  Serial.print("Obtained local IP address: ");
  Serial.println(WiFi.localIP());
  printMillis();
  Serial.print("Udp.begin(");
  Serial.print(localPort);
  Serial.println(") and Resuming taskUDPLoop");

  Udp.begin(localPort);
  vTaskResume(xUDPLoopHandle);
  vTaskResume(xPokeOSCLoopHandle);
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
  vTaskSuspend(xUDPLoopHandle);

  printMillis();
  Serial.print("WiFi disconnected. Reason: ");
  Serial.print(info.wifi_sta_disconnected.reason);
  Serial.println(". Suspended taskUDPLoop.");
  printMillis();
  Serial.println("Trying to reconnect WiFi...");

  WiFi.begin(ssid, pass);
}

const char *wl_status_to_string(wl_status_t status)
{
  switch (status)
  {
  case WL_NO_SHIELD:
    return "WL_NO_SHIELD";
  case WL_IDLE_STATUS:
    return "WL_IDLE_STATUS";
  case WL_NO_SSID_AVAIL:
    return "WL_NO_SSID_AVAIL";
  case WL_SCAN_COMPLETED:
    return "WL_SCAN_COMPLETED";
  case WL_CONNECTED:
    return "WL_CONNECTED";
  case WL_CONNECT_FAILED:
    return "WL_CONNECT_FAILED";
  case WL_CONNECTION_LOST:
    return "WL_CONNECTION_LOST";
  case WL_DISCONNECTED:
    return "WL_DISCONNECTED";
  }
}

// ***************************************************************
// void taskLedFlash
// - multiple instances of this task can be started
//
// call with:-
// xTaskCreate(taskLedFlash, "taskLedFlash", 10000, (void*)22, 1, NULL);
// xTaskCreate(taskLedFlash, "taskLedFlash", 10000, (void*)(uint32_t)theWidget.ledPin, 1, NULL);
// ***************************************************************
void taskLedFlash(void *parameters)
{
  uint8_t ledPin = (uint32_t)parameters;
  // if want to read state it may need different mode GPIO_MODE_INPUT_OUTPUT / GPIO_MODE_INPUT_PUTPUT_OD

#ifdef VERBOSE_DEBUG
  printMillis();
  Serial.print("Flashing pin: ");
  Serial.println(ledPin);
#endif
  digitalWrite(ledPin, LED_PIN_ON);
  vTaskDelay(((do_xRemote)?750:250) / portTICK_PERIOD_MS);
  digitalWrite(ledPin, LED_PIN_OFF);
  // delete myself on completion
  vTaskDelete(NULL);   
}

// ***************************************************************
// void taskButtonsLoop
// - respond to button presses by sending OSC instruction
// - (modeButton is handled elsewhere)
// ***************************************************************
void taskButtonsLoop(void *parameters)
{
  char stringNumber[4];

  for (;;)
  {
    // poll the service button(s)
    if (modeButton.toggled())
    {
      do_xRemote = (modeButton.read() == Button::RELEASED);
      if (do_xRemote) {
        do_Refresh = true;
        vTaskResume(xUDPLoopHandle);
      }
      printMillis();
      Serial.print("do_xRemote: ");
      Serial.println(do_xRemote, HEX);
    };
    // poll the OSC button(s)
    for (auto &theWidget : myWidgets)
    {
      if (theWidget.button.pressed())
      {
        // compose the OSC message
        OSCMessage msg(theWidget.oscAddress);
        if (theWidget.isOscToggle)
        {
          theWidget.oscState = (theWidget.oscState < 1) ? 1 : 0;                  // flip the state
          theWidget.oscPayload_s = (theWidget.oscState < 1) ? stringOFF : stringON; // compose text for MIDI SysEx
          msg.add(theWidget.oscState);
        }
        else
        {
          if (theWidget.oscPayload_f >= 0)
          {
            // assume fader-type OSC
            msg.add(theWidget.oscPayload_f);
            // convert float to string to compose text for MIDI SysEx; does MIDI SysEx method accept float?
            itoa((int)((theWidget.oscPayload_f*127) + 0.5),stringNumber,10);
            theWidget.oscPayload_s = stringNumber;
          }
          else
          {
            // assume snippet-type OSC
            if (*theWidget.oscPayload_s)
            {
              msg.add(theWidget.oscPayload_s); // send the payload string if defined
            };
            if (theWidget.oscPayload_i >= 0)
            {
              msg.add(theWidget.oscPayload_i); // send the payload int (index) if defined
            }
          }
        };

        // send OSC message
        Udp.beginPacket(X32Address, X32Port);
        msg.send(Udp);
        Udp.endPacket();
        msg.empty();

        // X32 does not seem to echo back the Fader and Mute commands or Mute Group. Or at least the X32 Emulator...
        if (do_xRemote && (theWidget.isOscToggle || theWidget.oscPayload_f >= 0))
        {
          // send OSC again for toggles (mutes) so we get an update
          OSCMessage msg2(theWidget.oscAddress);
          msg2.setAddress(theWidget.oscAddress);
          Udp.beginPacket(X32Address, X32Port);
          msg2.send(Udp);
          Udp.endPacket();
          msg2.empty();          
        };

        // send MIDI message for the same
        midiBuildCommand(theWidget.oscAddress, theWidget.oscPayload_s);
        //midiOut.sendSysEx(commandLength, (byte*)bigMidiCommand, true); // char
        midiOut.sendSysEx(strlen(bigMidiCommand), (byte*)bigMidiCommand, true); // char

        // flash the LED as local acknowledgement if we are not listening for response
        if (!do_xRemote) 
        {
            xTaskCreate(taskLedFlash, "taskLedFlash", 10000, (void*)(uint32_t)theWidget.ledPin, 1, NULL);
        }

        // DEBUG
        printMillis();
        theWidget.print();
      };
    };
    // no need to add delay here, we want to poll buttons quickly
  };
};

// ***************************************************************
// void taskUDPLoop
// - watch state of the specified OSC states from UDP stream
// - update LED accordingly
// ***************************************************************
void taskUDPLoop(void *parameters)
{
  int size;
  byte n;
  char str[64];

  bool odd = false;
  unsigned long m = 0;
  int matched = 0;

  for (;;)
  {
    if (do_xRemote && WiFi.status() == WL_CONNECTED) {
      OSCMessage msg;
      size = Udp.parsePacket();

      if (millis() - m > 500)
      {
        m = millis();
        odd = !odd;
        Serial.print((odd) ? "*\b" : ".\b"); // display heartbeat
      }

      if (size > 0)
      {
        Serial.print("[");
        Serial.print(millis());
        Serial.print("] ");
        Serial.print(size);
        Serial.print(" bytes received: ");

        while (size--)
        {
          n = Udp.read();
          msg.fill(n);
          if (n < 16)
          {
            Serial.print(" ");
            Serial.print(n, HEX);
          }
          else
          {
            Serial.print((char)n);
          };
        }

        Serial.print(" --> ");
        matched = 0;

        if (!msg.hasError())
        {
          // do we recognise this OSC messsage?
          for (auto &theWidget : myWidgets)
          {
            if (msg.fullMatch(theWidget.oscAddress))
            {
              // yes we do, so let's take some action
              matched++;
              Serial.print("MATCHES ");
              Serial.print(theWidget.friendlyDebugName);

              if (msg.isInt(0) && theWidget.isOscToggle)
              {
                // for binary states 0 or 1
                theWidget.oscState = msg.getInt(0);
                if (theWidget.isReverseLed)
                {
                  theWidget.doDigitalWrite((theWidget.oscState > 0) ? LED_PIN_OFF : LED_PIN_ON);
                }
                else
                {
                  theWidget.doDigitalWrite((theWidget.oscState > 0) ? LED_PIN_ON : LED_PIN_OFF);
                }
              }
              else if (msg.isFloat(0))
              {
                // for fader-style
                Serial.print(" FLOAT: ");
                Serial.print(msg.getFloat(0));

                // visual acknowledgement
                xTaskCreate(taskLedFlash, "taskLedFlash", 10000, (void*)(uint32_t)theWidget.ledPin, 1, NULL);
              }
              else if (msg.isString(0))
              {
                msg.getString(0, str, 64);

                Serial.print(" STRING: '");
                Serial.print(str);
                if (msg.isInt(1))
                {
                  Serial.print("' INDEX: ");
                  Serial.print(msg.getInt(1));
                }
                // visual acknowledgement
                xTaskCreate(taskLedFlash, "taskLedFlash", 10000, (void*)(uint32_t)theWidget.ledPin, 1, NULL);
              };
              Serial.println();
            };
          };
          if (matched == 0)
          {
            Serial.println("NO MATCH");
          }
        }
        else
        {
          Serial.print("ERROR: ");
          Serial.println(msg.getError());
          // typedef enum { OSC_OK = 0, BUFFER_FULL, INVALID_OSC, ALLOCFAILED, INDEX_OUT_OF_BOUNDS } OSCErrorCode;
        };
      };
    } else
    {
      // else if no wifi, or not monitoring X32 then suspend myself
      printMillis();
      Serial.println("taskUDPLoop suspending itself.");
      // depends on WiFiGotIP or taskButtonsLoop to Resume
      vTaskSuspend(xUDPLoopHandle);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // looks like small delay is needed...
  };
};

// ***************************************************************
// void taskPokeOSCLoop
// -  ask the X32 for its values
// ***************************************************************
void taskPokeOSCLoop(void *parameters)
{
  int doneLedOff = false;
  for (;;)
  {
    if (do_xRemote && WiFi.status() == WL_CONNECTED)
    {
      // if we can be one of the allowed xRemote clients then renew the /xremote request
      Serial.print("/xremote\b\b\b\b\b\b\b\b");
      doneLedOff = false;

      OSCMessage msg("/xremote");
      Udp.beginPacket(X32Address, X32Port);
      msg.send(Udp);
      Udp.endPacket();
      msg.empty();

      if (do_Refresh) {
        do_Refresh = false;
        vTaskDelay(20 / portTICK_PERIOD_MS); // give a short while for xremote to take effect
        for (auto &theWidget : myWidgets)
        {
          if (theWidget.isOscToggle)
          {
            OSCMessage msg(theWidget.oscAddress);
            Udp.beginPacket(X32Address, X32Port);
            msg.send(Udp);
            Udp.endPacket();
            msg.empty();
          }
        };
      };
      vTaskDelay(9000 / portTICK_PERIOD_MS); // renew request before 10 seconds
    }
    else
    {
      // turn off all the LEDs after the 9 seconds above has expired
      // or if WiFi disconnected
      if (!doneLedOff)
      {
        doneLedOff = true;
        for (auto &theWidget : myWidgets)
        {
          theWidget.doDigitalWrite(LED_PIN_OFF);
        };
        Serial.print("/-------\b\b\b\b\b\b\b\b");
      };
    };
    vTaskDelay(10 / portTICK_PERIOD_MS); // looks like small delay is needed...
  };
};

// ***************************************************************
// void taskStatusLoop
// - monitor battery and wifi status
// ***************************************************************
void taskStatusLoop(void *parameters)
{
  int statusLed = LED_PIN_ON;
  int lastWifiStatus = 99; // start with an undefined number
  wl_status_t wifiStatus;
  float batteryLevel;
  for (;;)
  {
    // check WiFi status and adjust led indicator
    wifiStatus = WiFi.status();
    if (wifiStatus == WL_CONNECTED)
    {
      statusLed = LED_PIN_ON;
      if (wifiStatus != lastWifiStatus)
      {
        lastWifiStatus = wifiStatus;
      }
    }
    else
    {
      statusLed = (statusLed == LED_PIN_ON) ? LED_PIN_OFF : LED_PIN_ON; // flip the state of the LED
      if (wifiStatus != lastWifiStatus)
      {
        lastWifiStatus = wifiStatus;
        printMillis();
        Serial.print("WiFi not connected.  WiFi.status() is: ");
        Serial.println(wl_status_to_string(wifiStatus));
      }
    };
    digitalWrite(PIN_FOR_WIFI_STATUS_LED, statusLed);

    // check battery status
    batteryLevel = analogRead(PIN_FOR_BATTERY_VOLTAGE);
    digitalWrite(PIN_FOR_BATTERY_STATUS_LED, (batteryLevel < BATTERY_VOLTAGE_LOW_CUTOFF) ? LED_PIN_ON : LED_PIN_OFF);

    // delay for flashing LED and for this loop
    vTaskDelay(500 / portTICK_PERIOD_MS); // delay 500 ms
  }
};

// ***************************************************************
// void loop - MAIN LOOP
// ***************************************************************
void loop()
{
  // this appears not to be called; vTaskDelete was succesful.
  // but let's kill loop() if for some reason it gets called.
  vTaskDelete(NULL);
}

// ***************************************************************
// void setup
// - set up our system
// ***************************************************************
void setup()
{
  // initialise serial ports
  Serial.begin(115200);    // DEBUG window
  SerialMIDI.begin(31250); // setup MIDI output
  midiOut.begin();

  // button objects and pins should have been initialised already

  // initialise other pins
  pinMode(PIN_FOR_WIFI_STATUS_LED, OUTPUT);
  pinMode(PIN_FOR_BATTERY_STATUS_LED, OUTPUT);
  pinMode(PIN_FOR_BATTERY_VOLTAGE, INPUT);
  pinMode(PIN_FOR_MODE_SWITCH, INPUT_PULLUP);
  modeButton.begin();

  // flash all LED as self-test
  for (auto &theWidget : myWidgets)
  {
    theWidget.doDigitalWrite(LED_PIN_ON);
  }
  digitalWrite(PIN_FOR_WIFI_STATUS_LED, LED_PIN_ON);
  digitalWrite(PIN_FOR_BATTERY_STATUS_LED, LED_PIN_ON);
  delay(500); // shorten this if we want to start even faster
  for (auto &theWidget : myWidgets)
  {
    theWidget.doDigitalWrite(LED_PIN_OFF);
  };
  digitalWrite(PIN_FOR_WIFI_STATUS_LED, LED_PIN_OFF);
  digitalWrite(PIN_FOR_BATTERY_STATUS_LED, LED_PIN_OFF);

  // send greetings to debug screen
  Serial.println();
  Serial.println("*******************************");
  Serial.println("*  Wei Liang's X32 Stomp Box  *");
  Serial.println("*******************************");

  // show my contents
  for (auto &theWidget : myWidgets)
  {
    theWidget.print();
#ifdef VERBOSE_DEBUG
    midiBuildCommand(theWidget.oscAddress, theWidget.oscPayload_s);
#endif
  }
  Serial.println("*******************************");
  Serial.print("X32 Address: ");
  Serial.print(X32Address);
  Serial.print(":");
  Serial.println(X32Port);
  Serial.print("WiFi SSID:   ");
  Serial.println(ssid);
  Serial.print("Local Port:  ");
  Serial.println(localPort);
  Serial.println("*******************************");

  // Connect to WiFi network
  WiFi.mode(WIFI_MODE_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(MY_HOSTNAME);
  WiFi.begin(ssid, pass);

  // start our multitasking loops
  // xTaskCreate( function_name, "task name", stack_size, task_parameters, priority, task_handle );
  xTaskCreate(taskButtonsLoop,  "taskButtonsLoop",  10000,  NULL, 1, NULL);
  xTaskCreate(taskUDPLoop,      "taskUDPLoop",      10000,  NULL, 1, &xUDPLoopHandle);
  vTaskSuspend(xUDPLoopHandle); // wait until WiFI ok
  xTaskCreate(taskPokeOSCLoop,  "taskPokeOSCLoop",  10000,  NULL, 1, &xPokeOSCLoopHandle);
  vTaskSuspend(xPokeOSCLoopHandle); // wait until WiFI ok
  xTaskCreate(taskStatusLoop,   "taskStatusLoop",   10000,   NULL, 1, NULL);
  WiFi.onEvent(WiFiStationConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // suspend main loop (as not needed)
  vTaskDelete(NULL);

  // end of setup
};

// ***************************************************************
// ***************************************************************
// ***************************************************************
