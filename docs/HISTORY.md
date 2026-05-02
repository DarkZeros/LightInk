# Introduction

In this file I will try to document from memory + Pictures all what i remember from the project.
Also what decision were done and why at each point in time.
If someone wants to read this, or use AI to read it for him and answer questions on the project, you are welcome here on the journey.

# How it started

The project aim was simple. Einks are low power, can we use one to build
a new generation watch (with BT, Wifi, etc) but power it like the 90's Casio watches
from only sunlight? That would be a cool upgrade!

I started with a module I though it had everything I wanted as a telecom enigneer: Helltec Wireless Stick Lite V2. This module had ESP32, BT, WiFI, LoRa, and it could drive an EInk without problems.
I bought one and a breadboard with a 1.54'' color ink, in late 2019.

I played with it and managed to have something rendered, but soon realized the endevour of making
a full feature watch would be hard. CASE? Custom PCB? Eink driver PCB? I had none of that...
It is not that i could not build it given enough time, it is that i didn't have enough time to do it.

Then pandemic arrived, and lockdown....

# The cooldown period

After the pandemic hit, I decided to create a PCB for my project based on the TPS63900 for low power.
Without it, I was sure I could not make it work, I tried some other DC/DC chips and I tohugh it would not be possible, vasically they had too much quiscent current.

However, my project will soon hit a dead end, since I could not build my PCB without the components.
It took 2 years for the components to be avilable for my project, during that time I did other things.

Like move to a new house, buy a car, etc. 

# The restart of the project

It was when I discoverd the Watchy project in 2021 when I said: "Hey! This is very close to what I wanted!". I immediately bought 2 units, to tinker with.
Measured the power usage, the components, the firmware code, etc...

Since I did not have my own PCB yet, I decided to start working on the software, and use watchy as base. I worked on the driver and contributed to the upstream watchy repo:
* The SPI speed settings were wrong (4MHz, but 20Mhz can be set up)
* It was sending buffer multiple times, when only 1 is required
* The wait during display update was an active wait, instead of light sleep.
* ... And many more small things

I managed to get thee watchy battery extend from 2 weeks to 4 weeks thanks to these software patches.

I also found a Hardware bug in the watchy board, the RES pin in the display was not being pulled up,
this would cause the display to think it was reset by the ESP and turn on sometimes using power.

After all these things I ended with an extremely modded watchy firmware (I did not push everything upstream). And a modded watchy hardware with a solar panel and change sin the PCB.

It was then that I decied to move to my own Hardware.

# The first PCB v0.0

One day in 2023 I received in my house a packed with TPS63900. Wow! I forgot i ordered these 18months ago!
Inmediately after, I started laying out the PCB, with everything I knew at that time.
I think in a matter of 2 or 3 weeks I have a version in EasyEDA I was happy with, and I click "order".

I was naive, very naive that i could solver the components myself. After receaving the order it was a mess. Took me days to solder the components, and only to find out the board had so many mistakes.

The 3.3V power rails were incorrect, there were traces soldered were they shouldn´t, etc...
I guess this is typicall for a first try but it was quite a moment to find out you can do so many mistakes.

At least I managed to debug the issues and I ordered the board v1

# The second PCB v0.1

The board was better, only 1 or 2 issues with it, this time I asked JLCPCB to solder it for me.
I managed to fix the remaining issues, and managed to boot it! yes!

But I was not happy with it, the touch was not working, the power usage was very high still.

# Wandering out of spec

My next experiments were solely on the voltage, how low could I get?
I bought a development TPS63900 board, where I could set the voltage manually, 
and solder this board output directly into my board.

3.3V worked perfectly
3.0V? Perfectly as well, couldn't see any problem
2.6V? The ESP32 works, but the display does not...
2.7V? Seems that everything works fine, display, ESP32, but WiFi breaks things.

I finally setteed into 2.7/3.0V (2 power levels).
This is supported by the TPS63900, and can be controlled from the ESP32.
Basically, keep everything 2.7V, until you have some WiFi to do, or high power stuff,
then switch to the high voltage, and go back to 2.7V.

With 2.7V the DC/DC converter uses less battery, but also the RTC uses less power.
And the ESP32, and the display, I measured easily -15% power consumption.

But I was still far away from the battery life expected, and I could only get 1 month.

# The first working board PCB v0.3 (or HWv1 in the code)

This was my first fully working board with the new 2.7/3V low voltage selector.
Still this board was very similar to watchy, and even had the SDA/SCL/Interrupt pins that were unused.
The touch pads were still in early stages, but it did work fine more or less...

However, the board worked fine and I managed to get 1month of battery on the 100mAh.
My previous record on the watchy board+200mAh were just 30 days.

This is when I started optimizing the Display code, I first played with the driver,
rewriting it from scratch, and reducing as much as possible the size and speed.
I got a few optimizations that got me around 40-50 days of battery.

I optimized the render code, the display code, and even cached the widgets to reduce render cost.
However, the killer part was always booting the ESP32 itself.

# The journey to RTC Wake up Stub

I decided i needed the RTC wakeup once I understood this was the only way to get the project
to the state it needed to be. Otherwise it was phisically impossible to make ESP32 use less power.

This involved writing the SPI driver, manually, in RTC code.
(RTC code can´t have any logs or be debugged!!!)

So it was basically, developing a driver, writing to registers manually, without a debugger and logs.
Took me a while just to get some code compiling, let alone, generating some SPI data.
Luckily I got a PicoScope device that I used to see the signals, 
and I could see if the SPI was configured correctly or not.

After around 1 month coding and debugging I got my first updated display from RTC.
That was such a moment!! 
I checked the power, 60% less!!!
The next week was basically me finishing the code, so it could render the watchface
but on the RTC code. After this was done I measured the power usage, and was easily in the 200 days.

After I flashed my wrist watch with the new code I could see the battery indicator stopped
dropping, and slowly started to increase over time (instead of -1%/day was +0.2%/day).

# The polishing

After I got it working around summer 2024, I shift the focus into adding more features.
I though I could add a radio (I was ready hyped with the new Meshtastic nodes) and why not a GPS.
If those components could be turned off then was fine, would not use power unless needed.

I also though I should upgrade to the ESP32-S3 (since it had better features).

# The ESP32-S3

This was a very very very bad idea, that made me loose around a year.
The ESP32-S3 is completely different to the ESP32-PICO-D4.
The layout is different, requiring a new PCB.
The touch pads work in a completely different way.
The features are different, and the drivers, so my code would not work, and the RTC wake up code
needed to be rewritten almost entirely.

After 4 months redoing it, I ordered the PCB, and nothing worked.
It looked like the ESP32-S3 does not like at all running at 2.7V, and requires at least 3.0V.

But even after fixing this, the ESP32-S3 has an external RTC crystal support, but uses way too much power.
More that the external RTC 32k osciallator I am using with the PICOD4. (Around 2uA vs 0.2uA).

If that is not enough... the PICOD4 boots in micro seconds to the RTC code, but the S3 requires
around 10ms to be fully booted (and that is without ROM logs, with logs around 30ms, and is the default).
Therefore uses too much power to be usable for this project. I think this is because IDF had
to load certain ROM parts from flash in order to run this RTC wakeup in the S3.

So after a lot of work I decided to scrap it. The S3 died for me, and I left the code as HWv10.
Then went back to my old HWv1/v2 (the V2 is just the V1 witha different setup in the voltage regulator).

# The HW v3

After going back to my known board, I manage to get the GPS and LORA chips added.
But they did not fit in the space, so i originally decided to place them separate from the main
board, into their own place. This proven to be extremely hard to do.

So I never really manufacture any HWv3 boards, only tested with them.

# The final HW v4

THis was the first board that had everything in it, and working.
The LORA chip, the GPS, the speaker, vibrator, LED, and the usual ESP32-PICOD4.

Soldering this was tricky, and the space cramped, giving me only space for a 30mAh battery.
You may think it doesn´t matter, sun recharges it. But it matters.

A 30mAh battery cannot handle spikes of 100mA properly. So when the display ask for power + ESP32, etc.. it may cause glitches.
Also the solar panel under the sun can give enough power to overwhelm the battery safety circuit.
Leading to strange behaviours if voltage goes to 6V.

# The release

The HW was done and final in Sept 2025, but there is always something more to do.
A new SW fix, a new HW board, a new Case, ...
Overall this project took 4years on/off, around 12 PCB boards and 10 cases iterations.
So everyone around me was pushing me to RELEASE it.

I did this project for me, for fun, to enjoy optimizing.
To feel I was building that watch I had in the 90's that used solar power to work, but better.

I didn´t do it for money, glory or benefit.
However, at this point I decided to release it, but was always delaying the part or
documenting it, recording it, taking pictures.

Until I saw the Hackady GreenPower challenge.
I though that my project fit the theme, and decided to prepare it for release.

# The future

I would like to continue developing the project. I will definitely build a HWv5
without the GPS, but with a good LORA antenna in the PCB and bigger battery.

I want to add compatibility to the Firmware for Watchy, or other boards.
As well as add new features like the BT Android APP, or multiple watchfaces.

I am happy if someone want to help me with the project:
- Push features and PRs to the code
- Manufacture some batches for personal use, etc.
- I welcome any ideas as well, as long as they are reasonable I could add them in.
