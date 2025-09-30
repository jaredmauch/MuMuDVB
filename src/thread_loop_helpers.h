/**
 * @file thread_loop_helpers.h
 * @brief Helper macros and functions for thread loops with proper shutdown checking
 * 
 * This module provides convenient macros and functions that threads can use
 * to ensure they properly check for shutdown signals and coordinate with
 * the main thread.
 */

#ifndef _THREAD_LOOP_HELPERS_H
#define _THREAD_LOOP_HELPERS_H

#include "thread_shutdown_monitor.h"
#include "log.h"
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize thread monitoring for a new thread
 * @param thread_name Name of the thread for logging
 * @param checkin_interval_ms How often to checkin (milliseconds)
 * @param timeout_ms Timeout before considering thread dead (milliseconds)
 * @param cleanup_func Optional cleanup function
 * @param cleanup_data Data for cleanup function
 * @return 0 on success, -1 on error
 */
int init_thread_monitoring(const char *thread_name, int checkin_interval_ms, 
                          int timeout_ms, void *(*cleanup_func)(void *), void *cleanup_data);

/**
 * @brief Cleanup thread monitoring when thread exits
 */
void cleanup_thread_monitoring(void);

/**
 * @brief Check if current thread should shutdown
 * @return 1 if should shutdown, 0 if should continue
 */
int should_current_thread_shutdown(void);

/**
 * @brief Checkin for current thread (call regularly in loops)
 * @return 0 on success, -1 on error
 */
int current_thread_checkin(void);

/**
 * @brief Standard thread loop with proper shutdown checking
 * @param loop_body Code to execute in each loop iteration (as a macro parameter)
 * @param sleep_ms Sleep time between iterations (milliseconds)
 */
#define THREAD_LOOP_WITH_SHUTDOWN(loop_body, sleep_ms) do { \
    while (!should_current_thread_shutdown()) { \
        current_thread_checkin(); \
        loop_body; \
        if (sleep_ms > 0) { \
            struct pollfd pfd = {0, 0, 0}; \
            int poll_result = poll(&pfd, 1, sleep_ms); \
            if (poll_result < 0 && errno != EINTR) { \
                log_message(log_module, MSG_DEBUG, "Thread poll error: %s", strerror(errno)); \
            } \
        } \
    } \
} while(0)

/**
 * @brief Thread loop with custom shutdown check
 * @param loop_body Code to execute in each loop iteration
 * @param custom_check Custom shutdown check function
 * @param sleep_ms Sleep time between iterations (milliseconds)
 */
#define THREAD_LOOP_WITH_CUSTOM_CHECK(loop_body, custom_check, sleep_ms) do { \
    while (!should_current_thread_shutdown() && !(custom_check)) { \
        current_thread_checkin(); \
        loop_body; \
        if (sleep_ms > 0) { \
            struct pollfd pfd = {0, 0, 0}; \
            int poll_result = poll(&pfd, 1, sleep_ms); \
            if (poll_result < 0 && errno != EINTR) { \
                log_message(log_module, MSG_DEBUG, "Thread poll error: %s", strerror(errno)); \
            } \
        } \
    } \
} while(0)

/**
 * @brief Thread loop with event-based timing
 * @param loop_body Code to execute in each loop iteration
 * @param event_type Event type to wait for
 * @param timeout_ms Timeout for event wait (milliseconds)
 */
#define THREAD_LOOP_WITH_EVENTS(loop_body, event_type, timeout_ms) do { \
    while (!should_current_thread_shutdown()) { \
        current_thread_checkin(); \
        loop_body; \
        \
        extern event_timing_tracker_t *global_unified_timing_tracker; \
        if (global_unified_timing_tracker) { \
            timing_event_t event; \
            int wait_result = wait_for_timing_event(global_unified_timing_tracker, \
                                                  event_type, timeout_ms, &event); \
            if (wait_result < 0 && wait_result != -1) { \
                log_message(log_module, MSG_DEBUG, "Thread event wait failed: %d", wait_result); \
            } \
        } else { \
            struct pollfd pfd = {0, 0, 0}; \
            int poll_result = poll(&pfd, 1, timeout_ms); \
            if (poll_result < 0 && errno != EINTR) { \
                log_message(log_module, MSG_DEBUG, "Thread poll error: %s", strerror(errno)); \
            } \
        } \
    } \
} while(0)

/**
 * @brief Main thread function to check if it should stop
 * @return 1 if should stop, 0 if should continue
 */
int should_main_thread_stop(void);

/**
 * @brief Main thread function to wait for all threads to be ready for shutdown
 * @param timeout_sec Timeout in seconds (0 = wait forever)
 * @return 0 if all threads ready, -1 on timeout
 */
int wait_for_threads_ready_for_shutdown(int timeout_sec);

/**
 * @brief Main thread function to initiate graceful shutdown
 * @return 0 on success, -1 on error
 */
int initiate_graceful_shutdown(void);

/**
 * @brief Main thread function to check thread health
 * @return Number of unhealthy threads found
 */
int check_thread_health(void);

#ifdef __cplusplus
}
#endif

#endif // _THREAD_LOOP_HELPERS_H
