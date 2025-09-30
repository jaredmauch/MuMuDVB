/**
 * @file event_timing.c
 * @brief Event-based timing tracker implementation
 * 
 * This module provides event-driven timing that tracks time changes between
 * events to maintain proper timing intervals without using usleep polling.
 */

#include "event_timing.h"
#include "log.h"
#include "errors.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/poll.h>

static char *log_module = "Event-Timing: ";

// Forward declarations
static void handle_tuning_timeout(timing_event_t *event);


/**
 * @brief Background timing thread function
 */
static void *timing_thread_function(void *arg)
{
    event_timing_tracker_t *tracker = (event_timing_tracker_t *)arg;
    struct timespec current_time;
    struct timespec last_time = {0, 0};
    int i;
    
    log_message(log_module, MSG_INFO, "Event timing thread started");
    
    while (tracker->active) {
        current_time = get_current_time();
        
        // Handle clock backward jumps and NTP corrections
        if (last_time.tv_sec > 0) {
            long time_diff = get_time_diff_ms(&last_time, &current_time);
            if (time_diff < 0) {
                log_message(log_module, MSG_WARN, "Clock backward jump detected (%ld ms), adjusting timing events", time_diff);
                handle_clock_backward_jump(tracker, &current_time);
            }
        }
        
        pthread_mutex_lock(&tracker->timing_mutex);
        
        // Check all timing events
        for (i = 0; i < tracker->num_events; i++) {
            timing_event_t *event = &tracker->events[i];
            
            // Skip inactive events (interval_ms = 0)
            if (event->interval_ms == 0) {
                continue;
            }
            
            // Check if event is due
            if (event->next_expected.tv_sec <= current_time.tv_sec &&
                (event->next_expected.tv_sec < current_time.tv_sec ||
                 event->next_expected.tv_nsec <= current_time.tv_nsec)) {
                
                // Event is due, trigger callback
                if (tracker->event_callback) {
                    tracker->event_callback(event, tracker->callback_user_data);
                }
                
                // Handle tuning timeout events
                if (event->event_type == TIMING_EVENT_TUNING_TIMEOUT) {
                    handle_tuning_timeout(event);
                }
                
                // Schedule next occurrence
                add_ms_to_timespec(&event->next_expected, event->interval_ms);
            }
        }
        
        pthread_mutex_unlock(&tracker->timing_mutex);
        
        last_time = current_time;
        
        // Sleep briefly to prevent busy waiting
        if (event_sleep_interruptible(MS_TO_US(10)) < 0) { // 10ms
            return NULL; // Interrupted
        }
    }
    
    log_message(log_module, MSG_INFO, "Event timing thread stopped");
    return NULL;
}

/**
 * @brief Create an event-based timing tracker
 */
int create_event_timing_tracker(int max_events, multi_fe_event_monitor_t *fe_monitor, 
                               event_timing_tracker_t *tracker)
{
    if (!tracker || max_events <= 0) {
        return -1;
    }
    
    memset(tracker, 0, sizeof(event_timing_tracker_t));
    
    // Allocate events array
    tracker->events = calloc(max_events, sizeof(timing_event_t));
    if (!tracker->events) {
        log_message(log_module, MSG_ERROR, "Cannot allocate events array");
        return -1;
    }
    
    tracker->max_events = max_events;
    tracker->num_events = 0;
    tracker->fe_monitor = fe_monitor;
    tracker->active = 0;
    
    // Initialize mutex
    if (pthread_mutex_init(&tracker->timing_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot initialize timing mutex");
        free(tracker->events);
        return -1;
    }
    
    // Initialize timing compatibility with single channel behavior
    // Timing compatibility initialization removed - using simplified timing
    
    log_message(log_module, MSG_INFO, "Event timing tracker created (max_events=%d)", max_events);
    return 0;
}

/**
 * @brief Destroy an event-based timing tracker
 */
void destroy_event_timing_tracker(event_timing_tracker_t *tracker)
{
    if (!tracker) {
        return;
    }
    
    // Stop timing tracking
    stop_event_timing_tracking(tracker);
    
    // Clean up
    if (tracker->events) {
        free(tracker->events);
        tracker->events = NULL;
    }
    
    pthread_mutex_destroy(&tracker->timing_mutex);
    
    // Cleanup timing compatibility
    // Timing compatibility cleanup removed
    
    log_message(log_module, MSG_INFO, "Event timing tracker destroyed");
}

/**
 * @brief Set timing configuration
 */
int set_timing_config(event_timing_tracker_t *tracker, const timing_config_t *config)
{
    if (!tracker || !config) {
        return -1;
    }
    
    // Configuration is stored in the default config for now
    // In a full implementation, this would store the config in the tracker
    log_message(log_module, MSG_INFO, "Timing configuration set");
    return 0;
}

/**
 * @brief Start event-based timing tracking
 */
int start_event_timing_tracking(event_timing_tracker_t *tracker,
                               void (*callback)(const timing_event_t *event, void *user_data),
                               void *user_data)
{
    if (!tracker) {
        return -1;
    }
    
    if (tracker->active) {
        log_message(log_module, MSG_WARN, "Timing tracking already active");
        return 0;
    }
    
    tracker->event_callback = callback;
    tracker->callback_user_data = user_data;
    tracker->active = 1;
    
    // Start timing thread
    if (pthread_create(&tracker->timing_thread, NULL, timing_thread_function, tracker) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot create timing thread");
        tracker->active = 0;
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Event timing tracking started");
    return 0;
}

/**
 * @brief Stop event-based timing tracking
 */
void stop_event_timing_tracking(event_timing_tracker_t *tracker)
{
    if (!tracker || !tracker->active) {
        return;
    }
    
    tracker->active = 0;
    
    // Wait for timing thread to finish
    if (tracker->timing_thread) {
        pthread_join(tracker->timing_thread, NULL);
        tracker->timing_thread = 0;
    }
    
    log_message(log_module, MSG_INFO, "Event timing tracking stopped");
}

/**
 * @brief Schedule a timing event
 */
int schedule_timing_event(event_timing_tracker_t *tracker, timing_event_type_t event_type,
                         int card_id, double frequency, int interval_ms, void *user_data)
{
    if (!tracker || tracker->num_events >= tracker->max_events) {
        return -1;
    }
    
    pthread_mutex_lock(&tracker->timing_mutex);
    
    timing_event_t *event = &tracker->events[tracker->num_events];
    event->event_type = event_type;
    event->card_id = card_id;
    event->frequency = frequency;
    event->interval_ms = interval_ms;
    event->user_data = user_data;
    event->timestamp = get_current_time();
    event->next_expected = event->timestamp;
    add_ms_to_timespec(&event->next_expected, interval_ms);
    
    tracker->num_events++;
    
    pthread_mutex_unlock(&tracker->timing_mutex);
    
    log_message(log_module, MSG_DEBUG, "Scheduled timing event: type=%d, card=%d, freq=%.0f, interval=%dms",
                event_type, card_id, frequency, interval_ms);
    
    return 0;
}

/**
 * @brief Wait for next timing event using poll() for better performance
 */
int wait_for_timing_event(event_timing_tracker_t *tracker, timing_event_type_t event_type,
                         int timeout_ms, timing_event_t *event)
{
    if (!tracker || !event) {
        return -1;
    }
    
    struct timespec start_time = get_current_time();
    struct timespec current_time;
    long elapsed_ms;
    int poll_timeout_ms;
    
    while (1) {
        // Check for interrupt
        if (get_interrupted()) {
            return -2;
        }
        
        // Check timeout
        if (timeout_ms > 0) {
            current_time = get_current_time();
            elapsed_ms = get_time_diff_ms(&start_time, &current_time);
            if (elapsed_ms >= timeout_ms) {
                return -1; // Timeout
            }
            poll_timeout_ms = timeout_ms - elapsed_ms;
        } else {
            poll_timeout_ms = 50; // Default 50ms poll timeout for better interrupt responsiveness
        }
        
        pthread_mutex_lock(&tracker->timing_mutex);
        
        // Look for the requested event type
        for (int i = 0; i < tracker->num_events; i++) {
            if (tracker->events[i].event_type == event_type) {
                current_time = get_current_time();
                if (tracker->events[i].next_expected.tv_sec <= current_time.tv_sec &&
                    (tracker->events[i].next_expected.tv_sec < current_time.tv_sec ||
                     tracker->events[i].next_expected.tv_nsec <= current_time.tv_nsec)) {
                    
                    // Event is due
                    *event = tracker->events[i];
                    add_ms_to_timespec(&tracker->events[i].next_expected, tracker->events[i].interval_ms);
                    pthread_mutex_unlock(&tracker->timing_mutex);
                    return 0;
                }
            }
        }
        
        pthread_mutex_unlock(&tracker->timing_mutex);
        
        // Use poll() instead of busy waiting with usleep
        // This is more efficient and allows the system to sleep properly
        struct pollfd pfd = {0, 0, 0}; // No file descriptor, just timeout
        
        // Limit poll timeout to maximum 100ms for better interrupt responsiveness
        if (poll_timeout_ms > 100) {
            poll_timeout_ms = 100;
        }
        
        int poll_result = poll(&pfd, 1, poll_timeout_ms);
        
        if (poll_result < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, check for interrupt flag
                if (get_interrupted()) {
                    return -2;
                }
                continue; // Retry
            }
            // Other error, fall back to interruptible sleep
            if (event_sleep_interruptible(MS_TO_US(10)) < 0) {
                return -2; // Interrupted
            }
        }
        
        // Check for interrupt after poll() returns (even on timeout)
        if (get_interrupted()) {
            return -2;
        }
        
        // poll_result == 0 means timeout, which is expected
        // poll_result > 0 means event (but we have no fd, so this shouldn't happen)
    }
}

/**
 * @brief Check if timing event is due
 */
int is_timing_event_due(event_timing_tracker_t *tracker, timing_event_type_t event_type, int card_id)
{
    if (!tracker) {
        return -1;
    }
    
    struct timespec current_time = get_current_time();
    
    pthread_mutex_lock(&tracker->timing_mutex);
    
    for (int i = 0; i < tracker->num_events; i++) {
        if (tracker->events[i].event_type == event_type && 
            (card_id == -1 || tracker->events[i].card_id == card_id)) {
            
            if (tracker->events[i].next_expected.tv_sec <= current_time.tv_sec &&
                (tracker->events[i].next_expected.tv_sec < current_time.tv_sec ||
                 tracker->events[i].next_expected.tv_nsec <= current_time.tv_nsec)) {
                
                pthread_mutex_unlock(&tracker->timing_mutex);
                return 1; // Due
            }
        }
    }
    
    pthread_mutex_unlock(&tracker->timing_mutex);
    return 0; // Not due
}

/**
 * @brief Get time until next timing event
 */
int get_time_until_next_event(event_timing_tracker_t *tracker, timing_event_type_t event_type, int card_id)
{
    if (!tracker) {
        return -1;
    }
    
    struct timespec current_time = get_current_time();
    long min_time_ms = -1;
    
    pthread_mutex_lock(&tracker->timing_mutex);
    
    for (int i = 0; i < tracker->num_events; i++) {
        if (tracker->events[i].event_type == event_type && 
            (card_id == -1 || tracker->events[i].card_id == card_id)) {
            
            long time_ms = get_time_diff_ms(&current_time, &tracker->events[i].next_expected);
            if (time_ms >= 0 && (min_time_ms == -1 || time_ms < min_time_ms)) {
                min_time_ms = time_ms;
            }
        }
    }
    
    pthread_mutex_unlock(&tracker->timing_mutex);
    return min_time_ms;
}

// Event-based frequency test and background scan functions removed
// due to missing dependencies - can be re-added when needed

/**
 * @brief Get current time as timespec (monotonic - not affected by NTP)
 */
struct timespec get_current_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

/**
 * @brief Get current real time as timespec (affected by NTP - for compatibility)
 */
struct timespec get_current_realtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts;
}

/**
 * @brief Calculate time difference in milliseconds
 * @param start Start time
 * @param end End time
 * @return Time difference in milliseconds, or 0 if either pointer is NULL
 */
long get_time_diff_ms(const struct timespec *start, const struct timespec *end)
{
    if (!start || !end) {
        return 0;
    }
    
    long diff_sec = end->tv_sec - start->tv_sec;
    long diff_nsec = end->tv_nsec - start->tv_nsec;
    
    if (diff_nsec < 0) {
        diff_sec--;
        diff_nsec += 1000000000L;
    }
    
    return diff_sec * 1000 + diff_nsec / 1000000;
}

/**
 * @brief Add milliseconds to timespec
 */
void add_ms_to_timespec(struct timespec *ts, long ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000;
    
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/**
 * @brief Handle clock backward jumps due to NTP corrections
 */
int handle_clock_backward_jump(event_timing_tracker_t *tracker, const struct timespec *current_time)
{
    if (!tracker || !current_time) {
        return 0;
    }
    
    pthread_mutex_lock(&tracker->timing_mutex);
    
    // Reset all timing events to current time + their intervals
    for (int i = 0; i < tracker->num_events; i++) {
        timing_event_t *event = &tracker->events[i];
        
        // Reset the next expected time to current time + interval
        event->next_expected = *current_time;
        add_ms_to_timespec(&event->next_expected, event->interval_ms);
        
        log_message(log_module, MSG_DEBUG, "Reset timing event %d for card %d due to clock jump", 
                   event->event_type, event->card_id);
    }
    
    pthread_mutex_unlock(&tracker->timing_mutex);
    
    return 1; // Clock jumped backward
}

// usleep replacement functions
int event_sleep_us(unsigned int usec)
{
    if (usec == 0) {
        return 0;
    }
    
    // Convert microseconds to milliseconds
    unsigned int msec = (usec + 999) / 1000; // Round up
    
    // Use existing timing system if available
    extern event_timing_tracker_t *global_unified_timing_tracker;
    if (global_unified_timing_tracker) {
        timing_event_t event;
        int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                              TIMING_EVENT_POLL_INTERVAL, 
                                              msec, &event);
        return (wait_result >= 0) ? 0 : -1;
    }
    
    // Fallback to interruptible sleep
    return event_sleep_interruptible(usec);
}

int event_sleep_ms(unsigned int msec)
{
    if (msec == 0) {
        return 0;
    }
    
    // Use existing timing system if available
    extern event_timing_tracker_t *global_unified_timing_tracker;
    if (global_unified_timing_tracker) {
        timing_event_t event;
        int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                              TIMING_EVENT_POLL_INTERVAL, 
                                              msec, &event);
        return (wait_result >= 0) ? 0 : -1;
    }
    
    // Fallback to interruptible sleep
    return event_sleep_interruptible(MS_TO_US(msec));
}

int event_sleep_interruptible(unsigned int usec)
{
    if (usec == 0) {
        return 0;
    }
    
    // Check for interrupt first
    if (get_interrupted()) {
        return -1; // Interrupted
    }
    
    // Convert microseconds to milliseconds
    unsigned int msec = (usec + 999) / 1000; // Round up
    
    // Use existing timing system if available
    extern event_timing_tracker_t *global_unified_timing_tracker;
    if (global_unified_timing_tracker) {
        timing_event_t event;
        int wait_result = wait_for_timing_event(global_unified_timing_tracker, 
                                              TIMING_EVENT_POLL_INTERVAL, 
                                              msec, &event);
        
        // Check for interrupt after waiting
        if (get_interrupted()) {
            return -1; // Interrupted
        }
        
        return (wait_result >= 0) ? 0 : -2; // Error
    }
    
    // Fallback to poll-based sleep with interrupt checking
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    unsigned int remaining_us = usec;
    while (remaining_us > 0) {
        if (get_interrupted()) {
            return -1; // Interrupted
        }
        
        // Use poll() with short timeout instead of usleep
        unsigned int chunk_us = (remaining_us > 50000) ? 50000 : remaining_us; // Max 50ms chunks
        struct pollfd pfd = {0, 0, 0}; // No file descriptor, just timeout
        int poll_timeout_ms = (chunk_us + 999) / 1000; // Convert to ms, round up
        
        int poll_result = poll(&pfd, 1, poll_timeout_ms);
        
        if (poll_result < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, check if we should exit
                if (get_interrupted()) {
                    return -1;
                }
                continue; // Retry
            }
            return -1; // Error
        }
        
        // Check for interrupt after poll
        if (get_interrupted()) {
            return -1;
        }
        
        remaining_us -= chunk_us;
        
        // Check elapsed time
        clock_gettime(CLOCK_MONOTONIC, &current);
        long elapsed_us = (current.tv_sec - start.tv_sec) * 1000000 + 
                         (current.tv_nsec - start.tv_nsec) / 1000;
        
        if (elapsed_us >= (long)usec) {
            break; // Time reached
        }
        
        remaining_us = usec - elapsed_us;
    }
    
    return 0;
}

// Tuning timeout functions
/**
 * @brief Start tuning timeout for a specific card
 */
int start_tuning_timeout(event_timing_tracker_t *tracker, int card_id, int timeout_seconds)
{
    if (!tracker || card_id < 0 || timeout_seconds <= 0) {
        return -1;
    }
    
    // Remove any existing tuning timeout for this card
    cancel_tuning_timeout(tracker, card_id);
    
    // Add new tuning timeout event
    int timeout_ms = timeout_seconds * 1000;
    int result = schedule_timing_event(tracker, TIMING_EVENT_TUNING_TIMEOUT, card_id, 0.0, timeout_ms, NULL);
    
    if (result == 0) {
        log_message(log_module, MSG_DEBUG, "Started tuning timeout for card %d (%d seconds)", card_id, timeout_seconds);
    }
    
    return result;
}

/**
 * @brief Cancel tuning timeout for a specific card
 */
int cancel_tuning_timeout(event_timing_tracker_t *tracker, int card_id)
{
    if (!tracker || card_id < 0) {
        return -1;
    }
    
    // Mark the tuning timeout event as inactive by setting its interval to 0
    pthread_mutex_lock(&tracker->timing_mutex);
    
    for (int i = 0; i < tracker->num_events; i++) {
        if (tracker->events[i].event_type == TIMING_EVENT_TUNING_TIMEOUT && 
            tracker->events[i].card_id == card_id) {
            tracker->events[i].interval_ms = 0; // Mark as inactive
            log_message(log_module, MSG_DEBUG, "Cancelled tuning timeout for card %d", card_id);
            pthread_mutex_unlock(&tracker->timing_mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&tracker->timing_mutex);
    return -1; // Not found
}

/**
 * @brief Check if tuning timeout is active for a card
 */
int is_tuning_timeout_active(event_timing_tracker_t *tracker, int card_id)
{
    if (!tracker || card_id < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&tracker->timing_mutex);
    
    for (int i = 0; i < tracker->num_events; i++) {
        if (tracker->events[i].event_type == TIMING_EVENT_TUNING_TIMEOUT && 
            tracker->events[i].card_id == card_id) {
            pthread_mutex_unlock(&tracker->timing_mutex);
            return 1; // Active
        }
    }
    
    pthread_mutex_unlock(&tracker->timing_mutex);
    return 0; // Not active
}

/**
 * @brief Handle tuning timeout event
 */
static void handle_tuning_timeout(timing_event_t *event)
{
    if (!event) {
        return;
    }
    
    int card_id = event->card_id;
    
    // Check if card is actually tuned
    extern int *card_tuned;
    if (card_tuned && !*card_tuned) {
        // Check if this is a parallel operation - parallel system doesn't update global card_tuned
        // We need to be more careful about setting interrupts for parallel operations
        extern int is_parallel_operation_active(void);
        if (is_parallel_operation_active()) {
            log_message(log_module, MSG_DEBUG, "Card %d tuning timeout occurred during parallel operation - not setting interrupt", card_id);
            return; // Don't interrupt parallel operations
        }
        
        log_message(log_module, MSG_ERROR, "Card %d not tuned after timeout - setting interrupt", card_id);
        set_interrupted(ERROR_TUNE);
    } else {
        log_message(log_module, MSG_DEBUG, "Card %d tuning timeout occurred but card is tuned", card_id);
    }
}
