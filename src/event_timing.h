/**
 * @file event_timing.h
 * @brief Event-based timing tracker for maintaining timing between events
 * 
 * This module provides event-driven timing that tracks time changes between
 * events to maintain proper timing intervals without using usleep polling.
 * It consolidates with existing poll/epoll systems for efficient event handling.
 */

#ifndef _EVENT_TIMING_H
#define _EVENT_TIMING_H

#include "card_frequency_result.h"
#include "dvb_events.h"
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Timing event types */
typedef enum {
    TIMING_EVENT_FREQUENCY_TEST,    // Frequency test event
    TIMING_EVENT_SIGNAL_LOCK,       // Signal lock event
    TIMING_EVENT_POLL_INTERVAL,     // Polling interval event
    TIMING_EVENT_BACKGROUND_SCAN,   // Background scan event
    TIMING_EVENT_THREAD_CREATION,   // Thread creation event
    TIMING_EVENT_TUNING_TIMEOUT,    // Card tuning timeout event
    TIMING_EVENT_CUSTOM             // Custom timing event
} timing_event_type_t;

/** @brief Timing event structure */
typedef struct {
    timing_event_type_t event_type; // Type of timing event
    int card_id;                    // Card ID (if applicable)
    double frequency;               // Frequency (if applicable)
    struct timespec timestamp;      // Event timestamp
    struct timespec next_expected;  // Next expected event time
    int interval_ms;                // Interval in milliseconds
    void *user_data;                // User data
} timing_event_t;

/** @brief Event-based timing tracker */
typedef struct {
    multi_fe_event_monitor_t *fe_monitor;  // Frontend event monitor
    timing_event_t *events;                // Array of timing events
    int max_events;                        // Maximum number of events
    int num_events;                        // Current number of events
    pthread_mutex_t timing_mutex;          // Mutex for thread safety
    volatile int active;                   // Whether timing tracker is active
    pthread_t timing_thread;               // Background timing thread
    void (*event_callback)(const timing_event_t *event, void *user_data);
    void *callback_user_data;              // User data for callback
} event_timing_tracker_t;

/** @brief Timing configuration */
typedef struct {
    int frequency_test_interval_ms;        // Interval between frequency tests
    int signal_lock_timeout_ms;            // Signal lock timeout
    int poll_interval_ms;                  // Polling interval
    int background_scan_delay_ms;          // Background scan delay
    int thread_creation_delay_ms;          // Thread creation delay
    int main_loop_sleep_ms;                // Main loop sleep
} timing_config_t;

/**
 * @brief Create an event-based timing tracker
 * @param max_events Maximum number of timing events to track
 * @param fe_monitor Frontend event monitor (can be NULL)
 * @param tracker Output timing tracker structure
 * @return 0 on success, -1 on error
 */
int create_event_timing_tracker(int max_events, multi_fe_event_monitor_t *fe_monitor, 
                               event_timing_tracker_t *tracker);

/**
 * @brief Destroy an event-based timing tracker
 * @param tracker The timing tracker to destroy
 */
void destroy_event_timing_tracker(event_timing_tracker_t *tracker);

/**
 * @brief Set timing configuration
 * @param tracker The timing tracker
 * @param config Timing configuration
 * @return 0 on success, -1 on error
 */
int set_timing_config(event_timing_tracker_t *tracker, const timing_config_t *config);

/**
 * @brief Start event-based timing tracking
 * @param tracker The timing tracker
 * @param callback Function to call on timing events (NULL for no callback)
 * @param user_data User data passed to callback
 * @return 0 on success, -1 on error
 */
int start_event_timing_tracking(event_timing_tracker_t *tracker,
                               void (*callback)(const timing_event_t *event, void *user_data),
                               void *user_data);

/**
 * @brief Stop event-based timing tracking
 * @param tracker The timing tracker
 */
void stop_event_timing_tracking(event_timing_tracker_t *tracker);

/**
 * @brief Schedule a timing event
 * @param tracker The timing tracker
 * @param event_type Type of timing event
 * @param card_id Card ID (if applicable)
 * @param frequency Frequency (if applicable)
 * @param interval_ms Interval in milliseconds
 * @param user_data User data
 * @return 0 on success, -1 on error
 */
int schedule_timing_event(event_timing_tracker_t *tracker, timing_event_type_t event_type,
                         int card_id, double frequency, int interval_ms, void *user_data);

/**
 * @brief Wait for next timing event
 * @param tracker The timing tracker
 * @param event_type Type of timing event to wait for
 * @param timeout_ms Timeout in milliseconds (0 = no timeout, -1 = infinite)
 * @param event Output event structure
 * @return 0 on success, -1 on timeout, -2 on error
 */
int wait_for_timing_event(event_timing_tracker_t *tracker, timing_event_type_t event_type,
                         int timeout_ms, timing_event_t *event);

/**
 * @brief Check if timing event is due
 * @param tracker The timing tracker
 * @param event_type Type of timing event to check
 * @param card_id Card ID (if applicable)
 * @return 1 if due, 0 if not due, -1 on error
 */
int is_timing_event_due(event_timing_tracker_t *tracker, timing_event_type_t event_type, int card_id);

/**
 * @brief Get time until next timing event
 * @param tracker The timing tracker
 * @param event_type Type of timing event to check
 * @param card_id Card ID (if applicable)
 * @return Milliseconds until next event, -1 on error
 */
int get_time_until_next_event(event_timing_tracker_t *tracker, timing_event_type_t event_type, int card_id);

// Event-based frequency test and background scan functions removed
// due to missing dependencies - can be re-added when needed

/**
 * @brief Get current time as timespec (monotonic - not affected by NTP)
 * @return Current monotonic time
 */
struct timespec get_current_time(void);

/**
 * @brief Get current real time as timespec (affected by NTP - for compatibility)
 * @return Current real time
 */
struct timespec get_current_realtime(void);

/**
 * @brief Calculate time difference in milliseconds
 * @param start Start time
 * @param end End time
 * @return Difference in milliseconds
 */
long get_time_diff_ms(const struct timespec *start, const struct timespec *end);

/**
 * @brief Add milliseconds to timespec
 * @param ts Timespec to modify
 * @param ms Milliseconds to add
 */
void add_ms_to_timespec(struct timespec *ts, long ms);

/**
 * @brief Handle clock backward jumps due to NTP corrections
 * @param tracker The timing tracker
 * @param current_time Current time
 * @return 1 if clock jumped backward, 0 otherwise
 */
int handle_clock_backward_jump(event_timing_tracker_t *tracker, const struct timespec *current_time);

// usleep replacement functions
/**
 * @brief Event-based sleep replacement for usleep
 * @param usec Microseconds to sleep
 * @return 0 on success, -1 on error
 */
int event_sleep_us(unsigned int usec);

/**
 * @brief Event-based sleep replacement for usleep (milliseconds)
 * @param msec Milliseconds to sleep
 * @return 0 on success, -1 on error
 */
int event_sleep_ms(unsigned int msec);

/**
 * @brief Event-based sleep with interrupt checking
 * @param usec Microseconds to sleep
 * @return 0 on success, -1 on interrupt, -2 on error
 */
int event_sleep_interruptible(unsigned int usec);

// Convenience macros for usleep replacement
#define EVENT_USLEEP(usec) event_sleep_us(usec)
#define EVENT_MSLEEP(msec) event_sleep_ms(msec)
#define EVENT_SLEEP_INTERRUPTIBLE(usec) event_sleep_interruptible(usec)

// Tuning timeout functions
/**
 * @brief Start tuning timeout for a specific card
 * @param tracker Event timing tracker
 * @param card_id Card ID
 * @param timeout_seconds Timeout in seconds
 * @return 0 on success, -1 on error
 */
int start_tuning_timeout(event_timing_tracker_t *tracker, int card_id, int timeout_seconds);

/**
 * @brief Cancel tuning timeout for a specific card
 * @param tracker Event timing tracker
 * @param card_id Card ID
 * @return 0 on success, -1 on error
 */
int cancel_tuning_timeout(event_timing_tracker_t *tracker, int card_id);

/**
 * @brief Check if tuning timeout is active for a card
 * @param tracker Event timing tracker
 * @param card_id Card ID
 * @return 1 if active, 0 if not active, -1 on error
 */
int is_tuning_timeout_active(event_timing_tracker_t *tracker, int card_id);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_TIMING_H */
