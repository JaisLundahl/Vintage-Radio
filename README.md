# Vintage Radio 📻

An ESP32-WROVER-E based Service-Oriented Architecture (SOA) framework that blends vintage radio mechanical aesthetics with modern digital audio.

### 🌐 [View the Technical Dashboard & Documentation]https://jaislundahl.github.io/Vintage-Radio/

## Overview
This project manages high-fidelity audio by isolating time-critical tasks into dedicated FreeRTOS tasks. It features a "Controlled Reboot Model" to ensure system stability when switching between complex Bluetooth and Network stacks.

## Key Features
* **Bluetooth A2DP**: High-quality wireless streaming.
* **Network Radio**: Support for AAC/MP3 web streams with PLS/M3U parsing.
* **Local MP3**: 16 virtual station bins with persistent history tracking on SD.
* **Analog Smoothness**: Exponential Moving Average (EMA) filtering for jitter-free volume and tuning.
* **Battery Protection**: Automatic safe-shutdown when voltage drops below 12V.

## Hardware Requirements
* **Core**: ESP32-WROVER-E (PSRAM enabled).
* **Amplifier**: TAS5828M (I2C controlled).
* **Storage**: SD Card via SPI for media and system configuration.

## Tech Stack
* **Framework**: Arduino / PlatformIO.
* **Key Libraries**: [Arduino-Audio-Tools](https://github.com/pschatzmann/arduino-audio-tools), [Libhelix](https://github.com/pschatzmann/arduino-libhelix).
