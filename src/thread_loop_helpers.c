/**
 * @file thread_loop_helpers.c
 * @brief Helper functions for thread loops with proper shutdown checking
 */

#include "thread_loop_helpers.h"
#include "thread_shutdown_monitor.h"
#include "mumudvb.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

static char *log_module = "Thread-Helpers: ";

// Thread-local storage for current thread handle
static __thread pthread_t current_thread = 0;

/**
 * @brief Initialize thread monitoring for a new thread
 */
int init_thread_monitoring(const char *thread_name, int checkin_interval_ms, 
                          int timeout_ms, void *(*cleanup_func)(void *), void *cleanup_data)
{
    if (current_thread == 0) {
        current_thread = pthread_self();
    }
    
    return register_thread_for_monitoring(current_thread, thread_name,
                                        checkin_interval_ms, timeout_ms,
                                        cleanup_func, cleanup_data);
}

/**
 * @brief Cleanup thread monitoring when thread exits
 */
void cleanup_thread_monitoring(void)
{
    if (current_thread != 0) {
        unregister_thread_from_monitoring(current_thread);
        current_thread = 0;
    }
}

/**
 * @brief Check if current thread should shutdown
 */
int should_current_thread_shutdown(void)
{
    if (current_thread == 0) {
        current_thread = pthread_self();
    }
    
    return should_thread_shutdown(current_thread);
}

/**
 * @brief Checkin for current thread (call regularly in loops)
 */
int current_thread_checkin(void)
{
    if (current_thread == 0) {
        current_thread = pthread_self();
    }
    
    return thread_checkin(current_thread);
}

/**
 * @brief Main thread function to check if it should stop
 */
int should_main_thread_stop(void)
{
    // Check for interrupt signal
    if (get_interrupted()) {
        log_message(log_module, MSG_DEBUG, "Main thread should stop due to interrupt signal");
        return 1;
    }
    
    // Check if all other threads are ready for shutdown
    if (are_threads_still_running()) {
        // Check for dead threads
        int dead_threads = check_for_dead_threads();
        if (dead_threads > 0) {
            log_message(log_module, MSG_WARN, "Found %d dead threads, main thread should stop", dead_threads);
            return 1;
        }
        
        // Check if shutdown was initiated and threads are taking too long
        // This is a safety mechanism to prevent hanging
        return 0; // Continue running
    } else {
        log_message(log_module, MSG_DEBUG, "All threads have stopped, main thread should stop");
        return 1;
    }
}

/**
 * @brief Main thread function to wait for all threads to be ready for shutdown
 */
int wait_for_threads_ready_for_shutdown(int timeout_sec)
{
    log_message(log_module, MSG_INFO, "Waiting for threads to be ready for shutdown...");
    
    // First, signal all threads to shutdown
    if (signal_all_monitored_threads_shutdown() != 0) {
        log_message(log_module, MSG_ERROR, "Failed to signal threads for shutdown");
        return -1;
    }
    
    // Wait for all threads to actually shutdown
    if (wait_for_all_threads_shutdown(timeout_sec) != 0) {
        log_message(log_module, MSG_WARN, "Timeout waiting for threads to shutdown");
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "All threads are ready for shutdown");
    return 0;
}

/**
 * @brief Main thread function to initiate graceful shutdown
 */
int initiate_graceful_shutdown(void)
{
    log_message(log_module, MSG_INFO, "Initiating graceful shutdown...");
    
    // Set interrupt flag to signal all threads
    set_interrupted(SIGTERM);
    
    // Signal all monitored threads
    if (signal_all_monitored_threads_shutdown() != 0) {
        log_message(log_module, MSG_ERROR, "Failed to signal threads for shutdown");
        return -1;
    }
    
    // Wait for threads to shutdown gracefully
    if (wait_for_all_threads_shutdown(10) != 0) { // 10 second timeout
        log_message(log_module, MSG_WARN, "Some threads did not shutdown gracefully");
        
        // Force shutdown unresponsive threads
        int force_count = force_shutdown_unresponsive_threads();
        if (force_count > 0) {
            log_message(log_module, MSG_WARN, "Force-shutdown %d unresponsive threads", force_count);
        }
    }
    
    log_message(log_module, MSG_INFO, "Graceful shutdown completed");
    return 0;
}

/**
 * @brief Main thread function to check thread health
 */
int check_thread_health(void)
{
    int dead_threads = check_for_dead_threads();
    
    if (dead_threads > 0) {
        log_message(log_module, MSG_WARN, "Found %d dead or unresponsive threads", dead_threads);
        
        // Get detailed status of all threads
        monitored_thread_t statuses[100];
        int num_statuses = get_thread_statuses(statuses, 100);
        
        for (int i = 0; i < num_statuses; i++) {
            if (statuses[i].status == THREAD_STATUS_ERROR) {
                log_message(log_module, MSG_WARN, "Thread %s is in error state (last checkin: %ld seconds ago)", 
                           statuses[i].name, time(NULL) - statuses[i].last_checkin);
            }
        }
    }
    
    return dead_threads;
}
