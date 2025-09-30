/**
 * @file event_timer.h
 * @brief Event-based timer system to replace usleep calls
 * 
 * This module provides a comprehensive event-driven timing system that replaces
 * all usleep calls with efficient event-based waiting, improving performance
 * and responsiveness.
 */

#ifndef _EVENT_TIMER_H
#define _EVENT_TIMER_H

#include <pthread.h>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Timer event types */
typedef enum {
    TIMER_EVENT_MAIN_LOOP,          // Main loop timing
    TIMER_EVENT_POLL_INTERVAL,      // Polling interval
    TIMER_EVENT_FREQUENCY_TEST,     // Frequency testing
    TIMER_EVENT_BACKGROUND_SCAN,    // Background scanning
    TIMER_EVENT_THREAD_CREATION,    // Thread creation delays
    TIMER_EVENT_CARD_TEST,          // Card testing delays
    TIMER_EVENT_CAM_POLL,           // CAM polling
    TIMER_EVENT_SCAM_DECSA,         // SCAM decryption
    TIMER_EVENT_SCAM_SEND,          // SCAM sending
    TIMER_EVENT_BITRATE_MONITOR,    // Bitrate monitoring
    TIMER_EVENT_DVB_EVENTS,         // DVB events monitoring
    TIMER_EVENT_STRENGTH_POLL,      // Signal strength polling
    TIMER_EVENT_CUSTOM              // Custom timing events
} timer_event_type_t;

/** @brief Timer event structure */
typedef struct {
    timer_event_type_t event_type;  // Type of timer event
    int card_id;                    // Card ID (if applicable)
    int channel_id;                 // Channel ID (if applicable)
    struct timespec timestamp;      // Event timestamp
    struct timespec next_expected;  // Next expected event time
    int interval_ms;                // Interval in milliseconds
    void *user_data;                // User data
    volatile int active;            // Whether event is active
} timer_event_t;

/** @brief Event timer manager */
typedef struct {
    timer_event_t *events;          // Array of timer events
    int max_events;                 // Maximum number of events (0 = unlimited)
    int num_events;                 // Current number of events
    int allocated_events;           // Currently allocated array size
    pthread_mutex_t mutex;          // Mutex for thread safety
    pthread_cond_t condition;       // Condition variable for waiting
    volatile int active;            // Whether timer manager is active
    pthread_t timer_thread;         // Background timer thread
} event_timer_manager_t;

/**
 * @brief Initialize the event timer manager
 * @param max_events Maximum number of events (0 = unlimited, -1 = default)
 * @return 0 on success, -1 on error
 */
int init_event_timer_manager(int max_events);

/**
 * @brief Cleanup the event timer manager
 */
void cleanup_event_timer_manager(void);

/**
 * @brief Create a timer event
 * @param event_type Type of timer event
 * @param card_id Card ID (if applicable)
 * @param channel_id Channel ID (if applicable)
 * @param interval_ms Interval in milliseconds
 * @param user_data User data
 * @return Event ID on success, -1 on error
 */
int create_timer_event(timer_event_type_t event_type, int card_id, int channel_id, 
                      int interval_ms, void *user_data);

/**
 * @brief Wait for a timer event
 * @param event_type Type of timer event to wait for
 * @param card_id Card ID (if applicable)
 * @param channel_id Channel ID (if applicable)
 * @param timeout_ms Timeout in milliseconds (0 = no timeout, -1 = infinite)
 * @return 0 on success, -1 on timeout, -2 on error
 */
int wait_for_timer_event(timer_event_type_t event_type, int card_id, int channel_id, 
                        int timeout_ms);

/**
 * @brief Wait for any timer event
 * @param timeout_ms Timeout in milliseconds (0 = no timeout, -1 = infinite)
 * @param event Output event structure
 * @return 0 on success, -1 on timeout, -2 on error
 */
int wait_for_any_timer_event(int timeout_ms, timer_event_t *event);

/**
 * @brief Check if a timer event is due
 * @param event_type Type of timer event to check
 * @param card_id Card ID (if applicable)
 * @param channel_id Channel ID (if applicable)
 * @return 1 if due, 0 if not due, -1 on error
 */
int is_timer_event_due(timer_event_type_t event_type, int card_id, int channel_id);

/**
 * @brief Get time until next timer event
 * @param event_type Type of timer event to check
 * @param card_id Card ID (if applicable)
 * @param channel_id Channel ID (if applicable)
 * @return Milliseconds until next event, -1 on error
 */
int get_time_until_next_timer_event(timer_event_type_t event_type, int card_id, int channel_id);

/**
 * @brief Cancel a timer event
 * @param event_type Type of timer event to cancel
 * @param card_id Card ID (if applicable)
 * @param channel_id Channel ID (if applicable)
 * @return 0 on success, -1 on error
 */
int cancel_timer_event(timer_event_type_t event_type, int card_id, int channel_id);

/**
 * @brief Cancel all timer events of a specific type
 * @param event_type Type of timer event to cancel
 * @return Number of events cancelled
 */
int cancel_all_timer_events(timer_event_type_t event_type);

/**
 * @brief Get current timer event count
 * @return Number of active timer events
 */
int get_timer_event_count(void);

/**
 * @brief Get timer event statistics
 * @param total_events Output total events created
 * @param active_events Output active events
 * @param due_events Output events that are due
 * @return 0 on success, -1 on error
 */
int get_timer_event_stats(int *total_events, int *active_events, int *due_events);

// Convenience macros for common timing patterns
#define WAIT_FOR_MAIN_LOOP() wait_for_timer_event(TIMER_EVENT_MAIN_LOOP, -1, -1, -1)
#define WAIT_FOR_POLL_INTERVAL() wait_for_timer_event(TIMER_EVENT_POLL_INTERVAL, -1, -1, -1)
#define WAIT_FOR_FREQUENCY_TEST(card_id) wait_for_timer_event(TIMER_EVENT_FREQUENCY_TEST, card_id, -1, -1)
#define WAIT_FOR_BACKGROUND_SCAN() wait_for_timer_event(TIMER_EVENT_BACKGROUND_SCAN, -1, -1, -1)
#define WAIT_FOR_CAM_POLL() wait_for_timer_event(TIMER_EVENT_CAM_POLL, -1, -1, -1)
#define WAIT_FOR_SCAM_DECSA(channel_id) wait_for_timer_event(TIMER_EVENT_SCAM_DECSA, -1, channel_id, -1)
#define WAIT_FOR_SCAM_SEND(channel_id) wait_for_timer_event(TIMER_EVENT_SCAM_SEND, -1, channel_id, -1)

// Legacy usleep replacement macros
#define EVENT_SLEEP_MS(ms) do { \
    if (wait_for_timer_event(TIMER_EVENT_POLL_INTERVAL, -1, -1, ms) < 0) { \
        usleep(MS_TO_US(ms)); /* Fallback to usleep */ \
    } \
} while(0)

#define EVENT_SLEEP_US(us) EVENT_SLEEP_MS((us) / 1000)

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_TIMER_H */
