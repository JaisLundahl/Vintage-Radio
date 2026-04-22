#ifndef TAS5828M_H
#define TAS5828M_H

#include <stdint.h>

/**
 * @class TAS5828M
 * @brief Service class for configuring and controlling the TAS5828M I2C Amplifier.
 */
class TAS5828M {
public:
    /**
     * @brief Initializes the I2C bus and attempts to detect the TAS5828M.
     */
    static void init();

    /**
     * @brief Checks if the amplifier was successfully detected during initialization.
     * @return true if detected, false otherwise.
     */
    static bool isDetected();

    /**
     * @brief Sets the volume percentage and mute state.
     * @param volPct  Volume level (0-100).
     * @param mute    True to mute, false to unmute.
     * @param force   True to force writing I2C registers even if state hasn't changed.
     */
    static void setVolumeAndMute(uint8_t volPct, bool mute, bool force = false);

    /**
     * @brief Ensures the amplifier is awake, correctly routed for I2S, and clears faults.
     */
    static void ensurePlaybackPath();
};

#endif // TAS5828M_H