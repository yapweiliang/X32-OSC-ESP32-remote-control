# X32 OSC ESP32 remote control

Control the Behringer X32 via WiFi using ESP32, e.g. as a stompbox

## Supports:

control | OSC example | comment
--- | --- | ---
mutes       | ``/ch/01/mix/on,i`` |   LED state can be reversed
mute groups | ``/config/mute/1,i``  |
faders      | ``/ch/02/mix/09/level,f`` |
snippets    | ``/load,snippet i`` |

## Features:

- one-way (just send) - in case we don't want to hog the bandwidth
- two-way (receive confirmation and update LED)
- monitor battery voltage, and flash GPIO LED if low
- long press button (to prevent accidental presses, e.g. for scenes/snippets)
- more than one widget can monitor the same GPIO button (e.g. short press and long press; short press event will be generated even if long press)
- indicate when battery nearly full

## Issues:

- excess power wasted trying to reconnect to WiFi if unable to connect (extra 70mA approx)
- battery voltage divider may drain battery
- WiFi password is hardcoded 

## Limitations:

- short press button event will be generated even if long press
- X32 echoes `/load snippet` but does not say which snippet
- battery power switch disconnects battery (i.e. cannot charge if 'off')
- battery-full indication turns off when my ESP32-Lolin stops charging the LiPo

## Thoughts:

- subscribe vs xremote?  subscribe gives stream of data even if no changes

## Project milestones:

* `2023-03-04` initial draft
* `2023-03-13` about as much can be done now without actual hardware
* `2023-04-06` first site release

## TODO:

- [ ] test implementation of MIDI output; does MIDI SysEx method accept float?
- [ ] investigate - does actual X32 echo mute, fader, mutegroup?
- [ ] indicate battery level at start-up

## FUTURE:

- [ ] find other way to save energy usage if cannot connect to WiFi (WiFi.disconnect doesn't seem to stop the process)
- [ ] have a MIDI-only payload

## Thanks and Inspirations

- https://community.musictribe.com/discussion/solutions/10/0/257791/diy-wifi%252Fmidi-remote-control-box-for-x-air%253A-working!
- [Patrick-Gilles Maillot](https://sites.google.com/site/patrickmaillot/x32)
    - X32 emulator
    - Unofficial X32 OSC protocol document
    - and other utilities

---
