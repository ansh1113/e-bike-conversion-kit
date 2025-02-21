# e-Bike Conversion Kit

## Overview
This project is a custom e-bike conversion kit featuring:
- **STM32-based VESC ESC** for motor control
- **ESP32/ESP8266** for wireless control and battery monitoring
- **36V 10.4Ah Lithium Power Supply** with buck circuits (36V->24V, 24V->12V, 12V->5V)
- **Wireless throttle module** (mode increase/decrease/start switches)
- **GSM/GPS integration** for tracking and security
- **Battery level and mode indicator LEDs**
- **Electronic latch locking system**
- **Phone charging port**

## Features
- Wireless throttle control using ESP32
- Real-time battery monitoring and motor control
- GPS tracking for security
- Compact 4-layer PCB design

## Installation
## 1. Hardware Assembly
- Mount the **VESC ESC** with proper cooling.
- Connect the **36V battery** to the power input.
- Wire the **ESP module** to the ESC for PPM signal transmission.
- Connect **buck converters** for proper voltage regulation.
- Attach **LEDs**, **GSM/GPS module**, and **gyro sensor**.
- Install **wireless throttle module** on the handlebar.

## 2. Software Setup
- Install **VESC Tool** for ESC configuration.
- Flash the **ESP firmware** using Arduino IDE.
- Configure **PPM values** for different speed modes.

# Firmware Installation
## 1. Flashing VESC Firmware
- Download and install **VESC Tool**.
- Connect STM32 to your PC via USB.
- Select the correct **firmware version** and upload.

## 2. Flashing ESP Firmware
- Install **Arduino IDE** with ESP32/ESP8266 support.
- Install necessary libraries (`WiFi`, `TinyGPS++`, `Adafruit_GFX` etc.).
- Compile and upload `Firmware/hook_receiver/hook_receiver.ino`.
