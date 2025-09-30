/**
 * @file timing_compatibility.h
 * @brief Timing compatibility layer for single channel behavior
 * 
 * This module provides compatibility between the event-based timing system
 * and the original single channel timing behavior, ensuring proper handling
 * of NTP corrections and clock changes.
 */

#ifndef _TIMING_COMPATIBILITY_H
#define _TIMING_COMPATIBILITY_H

#include "mumudvb.h"
#include "event_timing.h"
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Timing mode for compatibility */
typedef enum {
    TIMING_MODE_MONOTONIC,    // Use monotonic time (not affected by NTP)
    TIMING_MODE_REALTIME,     // Use real time (affected by NTP, like original)
    TIMING_MODE_AUTO          // Auto-detect based on system behavior
} timing_mode_t;

/** @brief Timing compatibility configuration */
typedef struct {
    timing_mode_t mode;                    // Timing mode to use
    int handle_clock_jumps;                // Whether to handle clock backward jumps
    int ntp_correction_threshold_ms;      // Threshold for detecting NTP corrections
    int max_clock_jump_ms;                // Maximum acceptable clock jump
} timing_compatibility_config_t;

/** @brief Global timing compatibility state */
typedef struct {
    timing_compatibility_config_t config;
    struct timespec last_monotonic_time;
    struct timespec last_realtime_time;
    int clock_jump_detected;
    int ntp_correction_active;
} timing_compatibility_state_t;

/**
 * @brief Initialize timing compatibility system
 * @param config Timing compatibility configuration
 * @return 0 on success, -1 on error
 */
int init_timing_compatibility(const timing_compatibility_config_t *config);

/**
 * @brief Cleanup timing compatibility system
 */
void cleanup_timing_compatibility(void);

/**
 * @brief Get current time based on compatibility mode
 * @return Current time as timespec
 */
struct timespec get_compatible_time(void);

/**
 * @brief Calculate time difference with compatibility handling
 * @param start Start time
 * @param end End time
 * @return Difference in milliseconds
 */
long get_compatible_time_diff_ms(const struct timespec *start, const struct timespec *end);

/**
 * @brief Check if timing should use real-time mode (like original single channel)
 * @return 1 if should use real-time, 0 if monotonic
 */
int should_use_realtime_timing(void);

/**
 * @brief Handle NTP correction detection
 * @param current_time Current time
 * @return 1 if NTP correction detected, 0 otherwise
 */
int detect_ntp_correction(const struct timespec *current_time);

/**
 * @brief Get timing behavior that matches single channel system
 * @return Timing configuration that matches original behavior
 */
timing_compatibility_config_t get_single_channel_timing_config(void);

/**
 * @brief Convert monotonic time to real-time equivalent
 * @param monotonic_time Monotonic time
 * @return Real-time equivalent
 */
struct timespec monotonic_to_realtime(const struct timespec *monotonic_time);

/**
 * @brief Convert real-time to monotonic equivalent
 * @param realtime Real-time
 * @return Monotonic equivalent
 */
struct timespec realtime_to_monotonic(const struct timespec *realtime);

#ifdef __cplusplus
}
#endif

#endif /* _TIMING_COMPATIBILITY_H */
