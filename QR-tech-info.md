# X32 Supervillian Gizmo - more information

Date: `12 April 2023`

## Power Supply

From USB or internal Li-Po battery.

Ensure mode switch is in 'Normal Mode' when charging.

Ensure mode switch is OFF when not used, otherwise battery will be depleted.

## Mode Switch

Position | Meaning
--: | ---
1-way   | sends OSC instruction but does not monitor response
off     | disconnects battery (but will function as *normal* mode below if USB powered)
normal  | sends OSC instructions, `/xremote` used to monitor response from X32

## Buttons

Button | Short Press | Long Press | LED meaning (in *normal* mode)
---: | --- | --- | ---
Yellow  | snippet 13 | snippet 99 | X32 has received and echoed back
White   | snippet 15 | - | X32 has received and echoed back 
Green   | snippet 12 | - | X32 has received and echoed back
Blue    | snippet 11 | - | X32 has received and echoed back
Red (L) | mute group 6 | - | mute status
Red (R) | mute DCA 5 | - | mute status

## Status Indicators

Status Indicator | Meaning
--- | ---
Green LED flashing  | trying to connect to WiFi
Green LED solid     | WiFi connection established
Red LED flashing    | battery low (maybe < 45 minutes remaining)
Red LED off         | battery level OK
Red LED on          | battery charging nearly complete
Red LED random (1)  | battery approaching *"nearly complete"* state
Red LED random (2)  | **battery is disconnected** (mode switch is off) whilst unit connected to USB

---