/**
 * @file thread_shutdown_monitor.h
 * @brief Centralized thread shutdown monitoring system
 * 
 * This module provides a centralized way to monitor all threads and ensure
 * they properly check for shutdown signals, allowing the main thread to
 * detect when it needs to stop and coordinate graceful shutdown.
 */

#ifndef _THREAD_SHUTDOWN_MONITOR_H
#define _THREAD_SHUTDOWN_MONITOR_H

#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Thread shutdown status */
typedef enum {
    THREAD_STATUS_RUNNING = 0,    // Thread is running normally
    THREAD_STATUS_SHUTTING_DOWN,  // Thread is shutting down
    THREAD_STATUS_STOPPED,        // Thread has stopped
    THREAD_STATUS_ERROR           // Thread encountered an error
} thread_status_t;

/** @brief Thread monitoring information */
typedef struct {
    pthread_t thread;                    // Thread handle
    char name[64];                      // Thread name for logging
    volatile int shutdown_requested;    // Shutdown requested flag
    volatile thread_status_t status;    // Current thread status
    time_t last_checkin;               // Last time thread checked in
    int checkin_interval_ms;           // Expected checkin interval
    int timeout_ms;                    // Timeout before considering thread dead
    void *(*cleanup_func)(void *);     // Optional cleanup function
    void *cleanup_data;                // Data for cleanup function
} monitored_thread_t;

/** @brief Global thread shutdown monitor */
typedef struct {
    monitored_thread_t *threads;        // Array of monitored threads
    int max_threads;                   // Maximum number of threads
    int num_threads;                   // Current number of threads
    int allocated_threads;             // Currently allocated array size
    pthread_mutex_t mutex;             // Mutex for thread safety
    volatile int global_shutdown;      // Global shutdown flag
    time_t shutdown_initiated;         // When shutdown was initiated
} thread_shutdown_monitor_t;

/**
 * @brief Initialize the thread shutdown monitor
 * @param max_threads Maximum number of threads to monitor (0 = unlimited)
 * @return 0 on success, -1 on error
 */
int init_thread_shutdown_monitor(int max_threads);

/**
 * @brief Cleanup the thread shutdown monitor
 */
void cleanup_thread_shutdown_monitor(void);

/**
 * @brief Register a thread for monitoring
 * @param thread Thread handle
 * @param name Thread name for logging
 * @param checkin_interval_ms Expected checkin interval in milliseconds
 * @param timeout_ms Timeout before considering thread dead
 * @param cleanup_func Optional cleanup function
 * @param cleanup_data Data for cleanup function
 * @return 0 on success, -1 on error
 */
int register_thread_for_monitoring(pthread_t thread, const char *name,
                                  int checkin_interval_ms, int timeout_ms,
                                  void *(*cleanup_func)(void *), void *cleanup_data);

/**
 * @brief Unregister a thread from monitoring
 * @param thread Thread handle to unregister
 * @return 0 on success, -1 on error
 */
int unregister_thread_from_monitoring(pthread_t thread);

/**
 * @brief Check if a thread should shutdown (for use in thread loops)
 * @param thread Thread handle
 * @return 1 if should shutdown, 0 if should continue
 */
int should_thread_shutdown(pthread_t thread);

/**
 * @brief Thread checkin - call this regularly in thread loops
 * @param thread Thread handle
 * @return 0 on success, -1 on error
 */
int thread_checkin(pthread_t thread);

/**
 * @brief Signal all monitored threads to shutdown
 * @return 0 on success, -1 on error
 */
int signal_all_monitored_threads_shutdown(void);

/**
 * @brief Wait for all threads to shutdown
 * @param timeout_sec Timeout in seconds (0 = wait forever)
 * @return 0 if all threads stopped, -1 on timeout or error
 */
int wait_for_all_threads_shutdown(int timeout_sec);

/**
 * @brief Check if any threads are still running
 * @return 1 if threads are running, 0 if all stopped
 */
int are_threads_still_running(void);

/**
 * @brief Get status of all monitored threads
 * @param status_array Output array for thread statuses
 * @param max_threads Maximum number of statuses to return
 * @return Number of thread statuses returned
 */
int get_thread_statuses(monitored_thread_t *status_array, int max_threads);

/**
 * @brief Check for dead or unresponsive threads
 * @return Number of dead/unresponsive threads found
 */
int check_for_dead_threads(void);

/**
 * @brief Force shutdown of unresponsive threads
 * @return Number of threads force-shutdown
 */
int force_shutdown_unresponsive_threads(void);

#ifdef __cplusplus
}
#endif

#endif // _THREAD_SHUTDOWN_MONITOR_H
