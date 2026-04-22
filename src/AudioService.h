#pragma once
#include <Arduino.h>

// undgå at trække hele AudioTools ind i alle filer:
namespace audio_tools {
  class I2SStream;
  class AudioOutput;
}

/**
 * @brief Initializes the audio service and associated amplifier.
 * @note Must be called once during setup().
 */
void audioInit();                        // kaldes i setup()

/**
 * @brief Plays a local MP3 file from the SD card.
 * @param path Full path to the file (e.g. "/folder1/00000001.mp3").
 */
void audioPlay(const char* path);        // lokal/MP3

/**
 * @brief Stops current playback and frees decoder memory.
 */
void audioStop();

/**
 * @brief Forces immediate shutdown of all audio processes for safe reboot.
 */
void audioForceShutdown();               // brugt af RebootService

/**
 * @brief Applies the requested volume percentage.
 * @param percent Volume level from 0 to 100.
 */
void audioSetVolume(uint8_t percent);

/**
 * @brief Mutes or unmutes the amplifier.
 * @param on True to mute, false to unmute.
 */
void audioMute(bool on);

/**
 * @brief Checks if the audio is currently muted.
 * @return True if muted.
 */
bool audioIsMuted();

/**
 * @brief Checks if the amplifier was detected on the I2C bus.
 * @return True if TAS5828M is detected.
 */
bool audioAmpDetected();

/**
 * @brief Ensures the amplifier is awake, out of fault state, and ready for I2S.
 */
void audioEnsurePlaybackPath();

/**
 * @brief Checks if a local audio file is currently streaming.
 * @return True if active and not paused.
 */
bool audioIsPlaying();

/**
 * @brief Attempts to acquire I2S hardware ownership for Local/MP3 mode.
 * @return True if successfully acquired.
 */
bool audioAcquireLocal();

/**
 * @brief Releases I2S hardware ownership from Local/MP3 mode.
 */
void audioReleaseLocal();

/**
 * @brief Attempts to acquire I2S hardware ownership for Bluetooth mode.
 * @return True if successfully acquired.
 */
bool audioAcquireBt();

/**
 * @brief Releases I2S hardware ownership from Bluetooth mode.
 */
void audioReleaseBt();

/**
 * @brief Attempts to acquire I2S hardware ownership for Network mode.
 * @return True if successfully acquired.
 */
bool audioAcquireNet();

/**
 * @brief Releases I2S hardware ownership from Network mode.
 */
void audioReleaseNet();

/**
 * @brief Pauses the current local MP3 playback.
 */
void audioPause();

/**
 * @brief Resumes a paused local MP3 playback.
 */
void audioResume();

/**
 * @brief Checks if the local MP3 playback is currently paused.
 * @return True if paused.
 */
bool audioIsPaused();

/**
 * @brief Wrapper to start I2S for MP3 Mode.
 */
bool audioStartMp3Mode();

/**
 * @brief Wrapper to setup I2S for Bluetooth Mode.
 */
bool audioStartBtMode();

/**
 * @brief Wrapper to setup I2S for Network Mode.
 */
bool audioStartNetMode();

/**
 * @brief Gets the guarded I2S audio output for network streaming.
 * @return Reference to the protected AudioOutput.
 */
audio_tools::AudioOutput& audioGetOutput();
