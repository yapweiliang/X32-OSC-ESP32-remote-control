// ***************************************************************
// Behringer X32 stompbox for assigns and mute groups
// ***************************************************************
// 2023-03-04 initial draft
// 2023-03-13 about as much can be done now without actual hardware
#define VERSION "2023-04-06"
//
// Supports:
// - mutes        /ch/01/mix/on,i     Led state is reversed
// - mute groups  /config/mute/1,i  
// - faders       /ch/02/mix/09/level,f
// - snippets     /load,snippet i
// Features:
// - one-way (just send) - in case we don't want to hog the bandwidth
// - two-way (receive confirmation and update LED)
// - monitor battery voltage, and flash GPIO LED if low
// - long press button (to prevent accidental presses, e.g. if scene change)
// - more than one widget can monitor the same GPIO button (e.g. short press and long press; short press event will be generated even if long press)
// Issues:
// - excess power wasted trying to reconnect to WiFi if unable to connect (extra 70mA approx)
// - battery voltage divider may drain battery
// - WiFi password is hardcoded 
// Limitations
// - short press button event will be generated even if long press
// Thoughts:
// - subscribe vs xremote?  subscribe gives stream of data even if no changes
// ***************************************************************
// TODO
// [ ] test implementation of MIDI output; does MIDI SysEx method accept float?
// [ ] does actual X32 echo mute, fader, mutegroup?
// [ ] reliability of toggles - udp not always sent????
// [ ] does actual X32 echo /load snippet ; and how can we match that return?
// ***************************************************************
// FUTURE
// [ ] find other way to save energy usage if cannot connect to WiFi (WiFi.disconnect doesn't seem to stop the process)
// [ ] have a MIDI-only payload
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
  int actionTrigger;       // action_PRESS or action_LONG_PRESS
  unsigned long pressedMillis; // when was the button pressed?
  bool wasPressed;

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
            int theTrigger,
            bool theOscType, 
            bool theLedResponse,
            char *theOscAddress,
            char *theOscPayload_s,
            int theOscIndex = -1,
            float theOscPayload_f = -1)
      : button(theButtonPin),
        friendlyDebugName(theFriendlyName),
        buttonPin(theButtonPin),
        ledPin(theLedPin),
        actionTrigger(theTrigger),
        isOscToggle(theOscType),
        isReverseLed(theLedResponse),
        oscAddress(theOscAddress),
        oscPayload_s(theOscPayload_s),// use "" if not used
        oscPayload_f(theOscPayload_f),// use -1 if not used
        oscPayload_i(theOscIndex),    // use -1 if not used
        oscState(0),
        wasPressed(false)
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
    Serial.print(actionTrigger);
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

#define action_NOTHING -1
#define action_PRESS 1
#define action_LONG_PRESS 2
#define LONG_PRESS_DURATION 3000  // 3 seconds

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
    //         friendly_name      action_trigger                    oscAddress
    //                    button_pin                  isOscToggle                           payload_s
    //                        led_pin                        isReverseLed                         [payload_i], [payload_f]
    OSCWidget("Bttn A__", 12, 13, action_LONG_PRESS,  false, false, "/load",                "snippet", 99),   // reset speech
    OSCWidget("Button A", 12, 13, action_PRESS,       false, false, "/load",                "snippet", 13),   // 13 = lectern on and reset band
    OSCWidget("Button B", 14, 15, action_PRESS,       false, false, "/load",                "snippet", 15),   // 15 = band speak louder
    OSCWidget("Button C", 27,  2, action_PRESS,       false, false, "/load",                "snippet", 12),   // 12 = band speak
    OSCWidget("Button D", 26,  0, action_PRESS,       false, false, "/load",                "snippet", 11),   // 11 = band sing
    OSCWidget("Button E", 25,  4, action_PRESS,       true,  true , "/dca/5/on",            ""),              // DCA 5 = speech
    OSCWidget("Button F", 33,  5, action_PRESS,       true,  false, "/config/mute/6",       "")};             // Mute Group 6 = all band

//    OSCWidget("Button G", 32, 18, action_NOTHING,     true,  false, "/config/mute/6",       ""),              // Mute Group 6 = all band
//    OSCWidget("Button H", 35, 23, action_NOTHING,     true,  true , "/dca/5/on",            "")};             // DCA 5 = speech

//    OSCWidget("Example", 35, 23, action_PRESS,       true,  true , "/ch/01/mix/on",        ""),
//    OSCWidget("Example", 35, 23, action_NOTHING,     true,  true , "/dca/5/on",            ""),
//    OSCWidget("Example", 35, 23, action_NOTHING,     true,  false, "/config/mute/1",       ""),
//    OSCWidget("Example", 35, 23, action_LONG_PRESS,  false, false, "/load",                "snippet", 99),
//    OSCWidget("Example", 35, 23, action_NOTHING,     false, false, "/ch/02/mix/09/level",  "", -1 , 0.75),

// LOLIN32 Lite
// GPIO INPUTS 34,35,36,39 do not have internal pull-up/pull-down therefore do not define in myWidgets unless actually needed
// GPIO 2 is pulled down at start so LED will initially look dimly lit
#define MIDI_UART 2                     // GPIO 16,17
// UNUSED_GPIO 39                       // unused GPIO pin
#define PIN_FOR_WIFI_STATUS_LED 22      // internal LED is 22 for my LOLIN32
#define PIN_FOR_MODE_SWITCH 36          // needs pull-up 
#define PIN_FOR_BATTERY_VOLTAGE 34      // cannot use ADC2 pins (needed for WiFi)
#define PIN_FOR_BATTERY_STATUS_LED 19
#define BATTERY_LOW_CUTOFF 3034         // 3034 is 20% between 3.10V and 4.16V using divider 68k/(68k+27k)
// BATTERY THRESHOLDS 0 (0V) - 4095 (3.3V); value depends on voltage divider circuit
// however apparently 3.2V gives 4095 therefore adjusted table below
// ---------------- ----- === 0.50 ====   === 0.67 ====   === 0.75 ====
// battery depleted 3.10V (1.55V, 1984)   (2.08V, 2658)   (2.33V, 2975)
// battery low 20%  3.31V (1.66V, 2119)   (2.22V, 2840)   (2.48V, 3179)
// battery full     4.16V (2.08V, 2662)   (2.79V, 3567)   (3.12V, 3993)
// battery charging 4.26V (2.13V, 2726)   (2.85V, 3652)   (3.20V, 4089)

#if true
// LED lights up if pin pulls voltage down (sink)
#define LED_PIN_ON LOW
#define LED_PIN_OFF HIGH
#else
// LED is powered from pin (source)
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
  for (int j = 0; j < strlen(bigMidiCommand); j++)
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
  vTaskDelay(((do_xRemote)?200:100) / portTICK_PERIOD_MS);
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
  int action = action_NOTHING;

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
      // how was the button pressed?
      if (theWidget.button.toggled())
      {
        if (theWidget.button.read() == Button::PRESSED) {
          theWidget.pressedMillis = millis();
          theWidget.wasPressed = true;
          action = action_PRESS;
        } else
        {
          theWidget.wasPressed = false;
          action = action_NOTHING;
        }
      }
      else if (theWidget.wasPressed && ((millis() - theWidget.pressedMillis) > LONG_PRESS_DURATION)) 
      {
        theWidget.wasPressed = false;
        action = action_LONG_PRESS;
      }
      else
      {
        action = action_NOTHING;
      }

#ifdef VERBOSE_DEBUG      
      if (action != action_NOTHING) {
        printMillis();
        Serial.print("button press action: ");
        Serial.println(action);
      }
#endif

      if (action == theWidget.actionTrigger && action != action_NOTHING)
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
    }; // end for
    // no need to add delay here, we want to poll buttons quickly
  }; // end for ever loop
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
  int batteryLevel;
  int batteryStatusLed = LED_PIN_ON;
  int wifiStatusLed = LED_PIN_ON;
  int lastWifiStatus = 99; // start with an undefined number
  wl_status_t wifiStatus;
  
  for (;;)
  {
    // check WiFi status and adjust led indicator
    wifiStatus = WiFi.status();
    if (wifiStatus == WL_CONNECTED)
    {
      wifiStatusLed = LED_PIN_ON;
      if (wifiStatus != lastWifiStatus)
      {
        lastWifiStatus = wifiStatus;
      }
    }
    else
    {
      wifiStatusLed = (wifiStatusLed == LED_PIN_ON) ? LED_PIN_OFF : LED_PIN_ON; // flip the state of the LED
      if (wifiStatus != lastWifiStatus)
      {
        lastWifiStatus = wifiStatus;
        printMillis();
        Serial.print("WiFi not connected.  WiFi.status() is: ");
        Serial.println(wl_status_to_string(wifiStatus));
      }
    };
    digitalWrite(PIN_FOR_WIFI_STATUS_LED, wifiStatusLed);

    // check battery status
    batteryLevel = analogRead(PIN_FOR_BATTERY_VOLTAGE);
    if (batteryLevel < BATTERY_LOW_CUTOFF)
    {
      batteryStatusLed = (batteryStatusLed == LED_PIN_ON) ? LED_PIN_OFF : LED_PIN_ON; // flip the state of the LED
    }
    else
    {
      batteryStatusLed = LED_PIN_OFF;
    }
    digitalWrite(PIN_FOR_BATTERY_STATUS_LED, batteryStatusLed);
#ifdef VERBOSE_DEBUG    
    Serial.print("Batt:");
    Serial.print(batteryLevel);
    Serial.print("   \b\b\b\b\b\b\b\b\b\b\b\b");
#endif
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
  Serial.print("Wei Liang's X32 Stomp Box.  Version: ");
  Serial.println(VERSION);
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
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("*******************************");

  // Connect to WiFi network
  WiFi.setHostname(MY_HOSTNAME); // need to set hostname before wifi mode
  WiFi.mode(WIFI_MODE_STA);
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
