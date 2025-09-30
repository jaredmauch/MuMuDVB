/**
 * @file timing_compatibility.c
 * @brief Timing compatibility layer implementation
 * 
 * This module provides compatibility between the event-based timing system
 * and the original single channel timing behavior, ensuring proper handling
 * of NTP corrections and clock changes.
 */

#include "timing_compatibility.h"
#include "log.h"
#include "errors.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

static char *log_module = "Timing-Compatibility: ";

/** @brief Global timing compatibility state */
static timing_compatibility_state_t *global_timing_state = NULL;

/**
 * @brief Initialize timing compatibility system
 */
int init_timing_compatibility(const timing_compatibility_config_t *config)
{
    if (!config) {
        return -1;
    }
    
    // Allocate global state
    global_timing_state = malloc(sizeof(timing_compatibility_state_t));
    if (!global_timing_state) {
        log_message(log_module, MSG_ERROR, "Cannot allocate timing compatibility state");
        return -1;
    }
    
    // Initialize state
    memcpy(&global_timing_state->config, config, sizeof(timing_compatibility_config_t));
    global_timing_state->last_monotonic_time = get_current_time();
    global_timing_state->last_realtime_time = get_current_realtime();
    global_timing_state->clock_jump_detected = 0;
    global_timing_state->ntp_correction_active = 0;
    
    log_message(log_module, MSG_INFO, "Timing compatibility initialized (mode=%d, handle_jumps=%d)", 
                config->mode, config->handle_clock_jumps);
    
    return 0;
}

/**
 * @brief Cleanup timing compatibility system
 */
void cleanup_timing_compatibility(void)
{
    if (global_timing_state) {
        free(global_timing_state);
        global_timing_state = NULL;
    }
    
    log_message(log_module, MSG_INFO, "Timing compatibility cleaned up");
}

/**
 * @brief Get current time based on compatibility mode
 */
struct timespec get_compatible_time(void)
{
    if (!global_timing_state) {
        // Fallback to monotonic if not initialized
        return get_current_time();
    }
    
    struct timespec current_time;
    
    switch (global_timing_state->config.mode) {
        case TIMING_MODE_MONOTONIC:
            current_time = get_current_time();
            break;
            
        case TIMING_MODE_REALTIME:
            current_time = get_current_realtime();
            break;
            
        case TIMING_MODE_AUTO:
            // Auto-detect: use real-time if NTP correction is active, monotonic otherwise
            if (global_timing_state->ntp_correction_active) {
                current_time = get_current_realtime();
            } else {
                current_time = get_current_time();
            }
            break;
            
        default:
            current_time = get_current_time();
            break;
    }
    
    // Check for clock jumps if enabled
    if (global_timing_state->config.handle_clock_jumps) {
        detect_ntp_correction(&current_time);
    }
    
    return current_time;
}

/**
 * @brief Calculate time difference with compatibility handling
 */
long get_compatible_time_diff_ms(const struct timespec *start, const struct timespec *end)
{
    if (!start || !end) {
        return 0;
    }
    
    // Use the same clock type for both times
    long diff_sec = end->tv_sec - start->tv_sec;
    long diff_nsec = end->tv_nsec - start->tv_nsec;
    
    if (diff_nsec < 0) {
        diff_sec--;
        diff_nsec += 1000000000L;
    }
    
    long diff_ms = diff_sec * 1000 + diff_nsec / 1000000;
    
    // Handle negative differences (clock backward jumps)
    if (diff_ms < 0) {
        if (global_timing_state && global_timing_state->config.handle_clock_jumps) {
            log_message(log_module, MSG_WARNING, "Negative time difference detected: %ld ms", diff_ms);
            return 0; // Treat as no time passed
        }
    }
    
    return diff_ms;
}

/**
 * @brief Check if timing should use real-time mode (like original single channel)
 */
int should_use_realtime_timing(void)
{
    if (!global_timing_state) {
        return 0; // Default to monotonic
    }
    
    switch (global_timing_state->config.mode) {
        case TIMING_MODE_REALTIME:
            return 1;
            
        case TIMING_MODE_AUTO:
            return global_timing_state->ntp_correction_active;
            
        case TIMING_MODE_MONOTONIC:
        default:
            return 0;
    }
}

/**
 * @brief Handle NTP correction detection
 */
int detect_ntp_correction(const struct timespec *current_time)
{
    if (!global_timing_state || !current_time) {
        return 0;
    }
    
    struct timespec monotonic_time = get_current_time();
    struct timespec realtime = get_current_realtime();
    
    // Check for clock backward jumps in real-time
    long realtime_diff = get_time_diff_ms(&global_timing_state->last_realtime_time, &realtime);
    long monotonic_diff = get_time_diff_ms(&global_timing_state->last_monotonic_time, &monotonic_time);
    
    // If real-time jumped backward significantly more than monotonic, it's likely NTP
    if (realtime_diff < -global_timing_state->config.ntp_correction_threshold_ms &&
        monotonic_diff >= 0) {
        
        log_message(log_module, MSG_INFO, "NTP correction detected: realtime_diff=%ld, monotonic_diff=%ld", 
                   realtime_diff, monotonic_diff);
        
        global_timing_state->ntp_correction_active = 1;
        global_timing_state->clock_jump_detected = 1;
        
        // Update last times
        global_timing_state->last_monotonic_time = monotonic_time;
        global_timing_state->last_realtime_time = realtime;
        
        return 1;
    }
    
    // Update last times
    global_timing_state->last_monotonic_time = monotonic_time;
    global_timing_state->last_realtime_time = realtime;
    
    return 0;
}

/**
 * @brief Get timing behavior that matches single channel system
 */
timing_compatibility_config_t get_single_channel_timing_config(void)
{
    timing_compatibility_config_t config = {
        .mode = TIMING_MODE_REALTIME,              // Original uses gettimeofday/time()
        .handle_clock_jumps = 1,                   // Handle NTP corrections
        .ntp_correction_threshold_ms = 1000,       // 1 second threshold
        .max_clock_jump_ms = 5000                  // 5 second max jump
    };
    
    return config;
}

/**
 * @brief Convert monotonic time to real-time equivalent
 */
struct timespec monotonic_to_realtime(const struct timespec *monotonic_time)
{
    if (!monotonic_time) {
        struct timespec zero = {0, 0};
        return zero;
    }
    
    // Get current real-time and monotonic times
    struct timespec current_realtime = get_current_realtime();
    struct timespec current_monotonic = get_current_time();
    
    // Calculate the offset
    long offset_sec = current_realtime.tv_sec - current_monotonic.tv_sec;
    long offset_nsec = current_realtime.tv_nsec - current_monotonic.tv_nsec;
    
    if (offset_nsec < 0) {
        offset_sec--;
        offset_nsec += 1000000000L;
    }
    
    // Apply offset to monotonic time
    struct timespec realtime_equivalent = *monotonic_time;
    realtime_equivalent.tv_sec += offset_sec;
    realtime_equivalent.tv_nsec += offset_nsec;
    
    if (realtime_equivalent.tv_nsec >= 1000000000L) {
        realtime_equivalent.tv_sec++;
        realtime_equivalent.tv_nsec -= 1000000000L;
    }
    
    return realtime_equivalent;
}

/**
 * @brief Convert real-time to monotonic equivalent
 */
struct timespec realtime_to_monotonic(const struct timespec *realtime)
{
    if (!realtime) {
        struct timespec zero = {0, 0};
        return zero;
    }
    
    // Get current real-time and monotonic times
    struct timespec current_realtime = get_current_realtime();
    struct timespec current_monotonic = get_current_time();
    
    // Calculate the offset
    long offset_sec = current_monotonic.tv_sec - current_realtime.tv_sec;
    long offset_nsec = current_monotonic.tv_nsec - current_realtime.tv_nsec;
    
    if (offset_nsec < 0) {
        offset_sec--;
        offset_nsec += 1000000000L;
    }
    
    // Apply offset to real-time
    struct timespec monotonic_equivalent = *realtime;
    monotonic_equivalent.tv_sec += offset_sec;
    monotonic_equivalent.tv_nsec += offset_nsec;
    
    if (monotonic_equivalent.tv_nsec >= 1000000000L) {
        monotonic_equivalent.tv_sec++;
        monotonic_equivalent.tv_nsec -= 1000000000L;
    }
    
    return monotonic_equivalent;
}
