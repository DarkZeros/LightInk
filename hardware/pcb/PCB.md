# KNOWN ISSUES

- The ANT pad in the WioSX1262 is connected to GND
- Therefore the current board will not get signal if soldered as is.
- The antenna in the PCB needs to be soldered manually to the SX1262 or used an external antenna.
- The GPS module slot will not fit properly and needs manual filing
- Use a battery >50mAh to avoid instability and other issues, or the board requires more capacitors.

# PCB documentation

The PCB has gone trough multiple revisions, but I think is not worth
covering the old ones since they are worse than the HW v4.

However, for reference the old boards were:
- v0.1 : Working version same as watchy, touch not working
- v0.2 : Working version same as watchy + touch working
- V1: Same as watchy and low voltage regulator
- V2: Regulator changed to have a default value of 3.0V
- V3: Rework of the board to fit the LORA/GPS, moved display busy to RTC
- V4: Minor changes to the pinout to make it fit in the smaller space

- V10: ESP32-S3 version. This used too much power and is a dead end! (Check History.md)

## Design

The board is a 2 layer, 1 side PCB. On the back of the PCB it rests the display.
No component is allowed there, since the PCB is the support of the display.

The size is 31.0mm x 41.0mm. And as standalone it can´t hold everything in place.
It needs the case to rest in there with the rest of the components.

The design does not follow any RF guidelines, so I am not responsible if
the PCB radiates more than the limit allowed in your country.
I did not follow any ESD guidelines either.

The battery and solar cell are connected together directly.
The solar cell can give up to 6V under the sun, but in normal cases gives 4.5V.
The battery should be able to get all this energy fine. But beware, if you leave it under the
sun for long periods of time and fully charged it might explode!

During the night the battery voltage is not enough to cause the cell to consume power.
I measured < 0.1uA. It should be ok.

The tolerances and path widths are the standard ones for components this size.
Most manufacture facilities will easily manage to build these boards.

I used JLCPCB for my prototypes and all of the boards were exactly as I designed them.
If there was a flaw, was a logical flaw in the desing not a manufacturing issue.

## Dimensions and manufacturing details

Dimension: 41.0mm31.0mm (For fabrication, JLCPCB added some breakable pads, final size: 70.8 mm 71.1 mm)
layers: 2 (top and bottom, but components only go in the top one)
thickness: 0.6mm (Any will do, but smaller better to fit in the space, 0.6mm is what I used)
Min via hole size/diameter: 0.3mm/(0.4/0.45mm)
min hole size: ?? I don't know this detail
Outer Copper Weight: 1 oz

# Bill of Materials (BOM)

## Core Components

| Qty | Component | Designators | Manufacturer Part | Manufacturer | Supplier Part |
|---|---|---|---|---|---|
| 1 | ESP32-PICO-D4 MCU | U3 | ESP32-PICO-D4 | Espressif | C193707 |
| 1 | USB to UART Bridge | U4 | CP2104-F03-GMR | Silicon Labs | C47742 |
| 1 | RTC | U8 | PCF8563TS/5,118 | NXP | C27397 |
| 1 | Buck-Boost Converter | U2 | TPS63900DSKR | TI | C1518762 |
| 1 | Li-Ion Charger IC | U5 | TP4054 | UMW | C668215 |
| 1 | Radio module | --- | WioSX1262 | --- | --- |

---

## Passive Components

### Capacitors

| Qty | Value | Designators | Package | Manufacturer Part | Supplier Part |
|---|---|---|---|---|---|
| 2 | 100nF | C15, C16 | 0402 | CC0402KRX7R7BB104 | C60474 |
| 1 | 22uF | C1 | 0603 | 0603X226M6R3CT | C93926 |
| 1 | 10uF | C2 | 0603 | GRM188R60J106ME47D | C77041 |
| 3 | 4.7uF | C6, C13, C14 | 0603 | CL10A475KP8NNNC / GRM188R6YA475KE15D | C1705 / C162274 |
| 6 | 1uF | C7, C8, C9, C10, C11, C12 | 0603 | 0603F105M500NT | C108343 |

### Resistors

| Qty | Value | Designators | Package | Manufacturer Part | Supplier Part |
|---|---|---|---|---|---|
| 3 | 10kΩ | R3, R12, R13 | 0603 | RMC060310KFN | C269701 |
| 1 | 3Ω | R8 | 0603 | 0603WAF300KT5E | C23157 |
| 2 | 1kΩ | R10, R11 | 0402 | 0402WGF1001TCE | C11702 |
| 1 | 511Ω | R2 | 0603 | RC0603FR-07511RL | C137703 |
| 1 | 16.2kΩ | R1 | 0603 | RMC 0603 16K2 F N | C269411 |
| 1 | 1kΩ | R4 | 0603 | RMC06031K1%N | C269704 |
| 2 | 10MΩ | R5, R6 | 0603 | RC0603FR-0710ML | C114606 |
| 1 | 20kΩ | R7 | 0603 | RT0603BRD0720KL | C723637 |
| 1 | 100kΩ | R9 | 0603 | RC0603FR-07100KL | C14675 |

### Inductors & Crystals

| Qty | Value | Designators | Manufacturer Part | Supplier Part |
|---|---|---|---|---|
| 1 | 2.2uH Inductor | L1 | DFE252010F-2R2M=P2 | C488330 |
| 1 | 10uH Inductor | U7 | DFE201610E-100M=P2 | C426327 |
| 1 | 32.768kHz Crystal | X1 | Q13FC13500005 | C841881 |

---

## Semiconductors

### Transistors & MOSFETs

| Qty | Part | Designators | Package | Supplier Part |
|---|---|---|---|---|
| 1 | UMH3N | Q4 | SC-70-6 | C62892 |
| 2 | CJ2301 S1 | Q2, Q6 | SOT-23 | C8547 |
| 1 | AO3400A | Q1 | SOT-23 | C344010 |
| 1 | S8050 | Q3 | SOT-23 | C5249674 |

### Diodes

| Qty | Part | Designators | Package | Supplier Part |
|---|---|---|---|---|
| 4 | MBR0530 | D1, D2, D3, D4 | SOD-123 | C475718 |
| 1 | 1N4001WS | D5 | SOD-323 | C2898400 |

### LEDs

| Qty | Part | Designators | Package | Supplier Part |
|---|---|---|---|---|
| 1 | NCD0603O3 | U6 | LED0603 | C7429913 |

---

## Connectors

| Qty | Component | Designators | Manufacturer Part | Supplier Part |
|---|---|---|---|---|
| 1 | Micro USB Connector | USB1 | MicroQTJ | C404968 |
| 1 | FPC Connector | FPC1 | AFC07-S24ECA-00 | C262643 |

---

## Antennas on PCB

| Qty | Component | Designators | Manufacturer Part | Supplier Part |
|---|---|---|---|---|
| 1 | 868MHz Antenna | 868MHZ | — | — |
| 1 | SWRA117D Antenna | AE1 | SWRA117D | — |

---

## Externally soldered

| Qty | Component | Designators | Manufacturer Part | Supplier Part |
|---|---|---|---|---|
| 1 | Vibrating Motor | M1 | — | — |
| 1 | Buzzer | BUZZ | — | — |
| 1 | Light Module | LIGHT | — | — |
| 4 | Touch Pads | Touch | M1*2 screws | — |

---