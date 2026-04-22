# Vintage Radio 📻

An ESP32-WROVER-E based Service-Oriented Architecture (SOA) framework that blends vintage radio mechanical aesthetics with modern digital audio.

### 🌐 [View the Technical Dashboard & Documentation](https://jaislundahl.github.io/Vintage-Radio/)

## Overview
This project manages high-fidelity audio by isolating time-critical tasks into dedicated FreeRTOS tasks. It features a "Controlled Reboot Model" to ensure system stability when switching between complex Bluetooth and Network stacks.

## 🕹️ How It Works

### 1. The Mode Selector
The radio features a main switch that allows you to flip between three distinct operational modes:
* **Bluetooth Mode**: Stream directly from your phone or tablet.
* **MP3 Mode**: Listen to music stored on the SD card.
* **Network Radio**: Connect to the internet to listen to live global radio stations.

### 2. Tuning & Navigation
To maintain an authentic feel, the radio uses a traditional Tuning Knob for navigation:
* **Virtual Stations**: The system treats folders on your SD card like radio stations, providing **16 virtual "bins"** (folders).
* **Authentic Tuning Static**: While moving between stations or waiting for a network stream to buffer, the system plays a soft **"static" noise** to emulate the behavior of vintage hardware.

### 3. Smart Volume Control
The system synchronizes physical hardware with digital inputs:
* **Physical Potentiometer**: The master volume is controlled by a physical knob, processed with filters to ensure smooth adjustment.
* **Bluetooth Sync**: In Bluetooth mode, changing the volume on the radio sends a command back to your smartphone to adjust its internal volume, ensuring both devices stay in sync.

### 4. Battery & Safety
The radio includes built-in protection logic to ensure SD-card function
* **Low Voltage Protection**: The system continuously monitors the battery. If it drops below **12V** (adjustable), the radio will automatically save its state and enter a idles.

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

## 📂 Preparing Your SD Card
For a new setup, the system expects the following structure on the SD card:
* **`/folder1` to `/folder16`**: Place your MP3 files here; these correspond to the tuner positions.
* **`/system/sound/`**: Contains the `wait.mp3` (static noise) and volume limits.
* **`/system/netradio/`**: Contains `WIFI.txt` and `stations.txt` for internet radio setup.

## Tech Stack
* **Framework**: Arduino / PlatformIO.
* **Key Libraries**: [Arduino-Audio-Tools](https://github.com/pschatzmann/arduino-audio-tools), [Libhelix](https://github.com/pschatzmann/arduino-libhelix).
