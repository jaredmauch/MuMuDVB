/**
 * @file thread_monitoring_example.c
 * @brief Example of how to update existing threads to use the new monitoring system
 * 
 * This file shows how to convert existing thread functions to use the new
 * thread shutdown monitoring system for better coordination and health checking.
 */

#include "thread_loop_helpers.h"
#include "log.h"
#include "mumudvb.h"

static char *log_module = "Thread-Example: ";

/**
 * @brief Example: Updated card worker thread with proper monitoring
 * This shows how to convert an existing thread function
 */
void *card_worker_thread_with_monitoring(void *arg)
{
    int *card_id_ptr = (int *)arg;
    int card_id = *card_id_ptr;
    free(arg); // Free the allocated card_id
    
    // Initialize thread monitoring
    if (init_thread_monitoring("Card-Worker", 1000, 5000, NULL, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize monitoring for card-%d", card_id);
        return NULL;
    }
    
    log_message(log_module, MSG_INFO, "card-%d worker thread started with monitoring", card_id);
    
    // Use the new thread loop macro with proper shutdown checking
    THREAD_LOOP_WITH_SHUTDOWN({
        // Your existing thread logic here
        log_message(log_module, MSG_DEBUG, "card-%d processing...", card_id);
        
        // Simulate some work
        struct pollfd pfd = {0, 0, 0};
        int poll_result = poll(&pfd, 1, 100); // 100ms timeout
        if (poll_result < 0 && errno != EINTR) {
            log_message(log_module, MSG_DEBUG, "Thread poll error: %s", strerror(errno));
        }
        
        // The macro automatically handles:
        // - Checking for shutdown signals
        // - Thread checkin
        // - Proper sleep timing
    }, 100); // 100ms sleep between iterations
    
    // Cleanup when thread exits
    cleanup_thread_monitoring();
    log_message(log_module, MSG_INFO, "card-%d worker thread exiting", card_id);
    return NULL;
}

/**
 * @brief Example: Updated background scanner with event-based timing
 */
void *background_scanner_with_monitoring(void *arg)
{
    // Initialize thread monitoring
    if (init_thread_monitoring("Background-Scanner", 2000, 10000, NULL, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize monitoring for background scanner");
        return NULL;
    }
    
    log_message(log_module, MSG_INFO, "Background scanner started with monitoring");
    
    // Use event-based timing for better responsiveness
    THREAD_LOOP_WITH_EVENTS({
        // Your existing scanning logic here
        log_message(log_module, MSG_DEBUG, "Background scanner processing...");
        
        // The macro automatically handles:
        // - Checking for shutdown signals
        // - Thread checkin
        // - Event-based timing
    }, TIMING_EVENT_BACKGROUND_SCAN, 1000); // 1 second timeout
    
    // Cleanup when thread exits
    cleanup_thread_monitoring();
    log_message(log_module, MSG_INFO, "Background scanner exiting");
    return NULL;
}

/**
 * @brief Example: Updated main thread with proper thread coordination
 */
int main_thread_with_monitoring(void)
{
    log_message(log_module, MSG_INFO, "Main thread started with monitoring");
    
    // Initialize thread shutdown monitor
    if (init_thread_shutdown_monitor(50) != 0) { // Monitor up to 50 threads
        log_message(log_module, MSG_ERROR, "Failed to initialize thread shutdown monitor");
        return -1;
    }
    
    // Main loop with proper thread coordination
    while (!should_main_thread_stop()) {
        // Check thread health periodically
        int unhealthy_threads = check_thread_health();
        if (unhealthy_threads > 0) {
            log_message(log_module, MSG_WARN, "Found %d unhealthy threads", unhealthy_threads);
        }
        
        // Your main thread logic here
        log_message(log_module, MSG_DEBUG, "Main thread processing...");
        
        // Poll with timeout instead of usleep
        struct pollfd pfd = {0, 0, 0};
        int poll_result = poll(&pfd, 1, 100); // 100ms timeout
        if (poll_result < 0 && errno != EINTR) {
            log_message(log_module, MSG_DEBUG, "Main thread poll error: %s", strerror(errno));
        }
    }
    
    // Initiate graceful shutdown
    log_message(log_module, MSG_INFO, "Main thread initiating graceful shutdown");
    if (initiate_graceful_shutdown() != 0) {
        log_message(log_module, MSG_ERROR, "Graceful shutdown failed");
    }
    
    // Cleanup
    cleanup_thread_shutdown_monitor();
    log_message(log_module, MSG_INFO, "Main thread exiting");
    return 0;
}

/**
 * @brief Example: How to update existing thread functions
 * 
 * OLD WAY:
 * ```c
 * void *old_thread_function(void *arg) {
 *     while (!get_interrupted()) {
 *         // do work
 *         usleep(100000);
 *     }
 *     return NULL;
 * }
 * ```
 * 
 * NEW WAY:
 * ```c
 * void *new_thread_function(void *arg) {
 *     init_thread_monitoring("Thread-Name", 1000, 5000, NULL, NULL);
 *     
 *     THREAD_LOOP_WITH_SHUTDOWN({
 *         // do work
 *     }, 100);
 *     
 *     cleanup_thread_monitoring();
 *     return NULL;
 * }
 * ```
 */
