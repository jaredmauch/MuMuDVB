/**
 * @file thread_shutdown_monitor.c
 * @brief Centralized thread shutdown monitoring system implementation
 */

#include "thread_shutdown_monitor.h"
#include "log.h"
#include "mumudvb.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

static char *log_module = "Thread-Monitor: ";

// Global thread shutdown monitor
static thread_shutdown_monitor_t *global_monitor = NULL;

/**
 * @brief Initialize the thread shutdown monitor
 */
int init_thread_shutdown_monitor(int max_threads)
{
    if (global_monitor) {
        log_message(log_module, MSG_WARN, "Thread shutdown monitor already initialized");
        return 0;
    }
    
    global_monitor = malloc(sizeof(thread_shutdown_monitor_t));
    if (!global_monitor) {
        log_message(log_module, MSG_ERROR, "Cannot allocate thread shutdown monitor");
        return -1;
    }
    
    // Initialize structure
    global_monitor->max_threads = (max_threads <= 0) ? 100 : max_threads;
    global_monitor->num_threads = 0;
    global_monitor->allocated_threads = 0;
    global_monitor->global_shutdown = 0;
    global_monitor->shutdown_initiated = 0;
    global_monitor->threads = NULL;
    
    // Initialize mutex
    if (pthread_mutex_init(&global_monitor->mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot initialize monitor mutex");
        free(global_monitor);
        global_monitor = NULL;
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Thread shutdown monitor initialized (max_threads=%d)", 
                global_monitor->max_threads);
    return 0;
}

/**
 * @brief Cleanup the thread shutdown monitor
 */
void cleanup_thread_shutdown_monitor(void)
{
    if (!global_monitor) {
        return;
    }
    
    log_message(log_module, MSG_DEBUG, "Cleaning up thread shutdown monitor with %d threads", 
                global_monitor->num_threads);
    
    // Signal all threads to shutdown
    signal_all_monitored_threads_shutdown();
    
    // Wait for all threads to complete
    wait_for_all_threads_shutdown(5); // 5 second timeout
    
    // Cleanup resources
    if (global_monitor->threads) {
        free(global_monitor->threads);
    }
    
    pthread_mutex_destroy(&global_monitor->mutex);
    free(global_monitor);
    global_monitor = NULL;
    
    log_message(log_module, MSG_DEBUG, "Thread shutdown monitor cleanup completed");
}

/**
 * @brief Register a thread for monitoring
 */
int register_thread_for_monitoring(pthread_t thread, const char *name,
                                  int checkin_interval_ms, int timeout_ms,
                                  void *(*cleanup_func)(void *), void *cleanup_data)
{
    if (!global_monitor) {
        log_message(log_module, MSG_ERROR, "Thread shutdown monitor not initialized");
        return -1;
    }
    
    if (!name) {
        log_message(log_module, MSG_ERROR, "Thread name cannot be NULL");
        return -1;
    }
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    // Check if we're at the limit
    if (global_monitor->max_threads > 0 && global_monitor->num_threads >= global_monitor->max_threads) {
        log_message(log_module, MSG_ERROR, "Maximum number of threads (%d) reached", 
                    global_monitor->max_threads);
        pthread_mutex_unlock(&global_monitor->mutex);
        return -1;
    }
    
    // Allocate more space if needed
    if (global_monitor->num_threads >= global_monitor->allocated_threads) {
        int new_size = global_monitor->allocated_threads + 10;
        monitored_thread_t *new_threads = realloc(global_monitor->threads, 
                                                 new_size * sizeof(monitored_thread_t));
        if (!new_threads) {
            log_message(log_module, MSG_ERROR, "Cannot allocate space for thread monitoring");
            pthread_mutex_unlock(&global_monitor->mutex);
            return -1;
        }
        global_monitor->threads = new_threads;
        global_monitor->allocated_threads = new_size;
    }
    
    // Add thread to monitoring
    int idx = global_monitor->num_threads;
    global_monitor->threads[idx].thread = thread;
    strncpy(global_monitor->threads[idx].name, name, sizeof(global_monitor->threads[idx].name) - 1);
    global_monitor->threads[idx].name[sizeof(global_monitor->threads[idx].name) - 1] = '\0';
    global_monitor->threads[idx].shutdown_requested = 0;
    global_monitor->threads[idx].status = THREAD_STATUS_RUNNING;
    global_monitor->threads[idx].last_checkin = time(NULL);
    global_monitor->threads[idx].checkin_interval_ms = checkin_interval_ms;
    global_monitor->threads[idx].timeout_ms = timeout_ms;
    global_monitor->threads[idx].cleanup_func = cleanup_func;
    global_monitor->threads[idx].cleanup_data = cleanup_data;
    
    global_monitor->num_threads++;
    
    pthread_mutex_unlock(&global_monitor->mutex);
    
    log_message(log_module, MSG_DEBUG, "Registered thread for monitoring: %s (checkin=%dms, timeout=%dms)", 
                name, checkin_interval_ms, timeout_ms);
    return 0;
}

/**
 * @brief Unregister a thread from monitoring
 */
int unregister_thread_from_monitoring(pthread_t thread)
{
    if (!global_monitor) {
        return -1;
    }
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    for (int i = 0; i < global_monitor->num_threads; i++) {
        if (pthread_equal(global_monitor->threads[i].thread, thread)) {
            // Move last thread to this position
            if (i < global_monitor->num_threads - 1) {
                global_monitor->threads[i] = global_monitor->threads[global_monitor->num_threads - 1];
            }
            global_monitor->num_threads--;
            pthread_mutex_unlock(&global_monitor->mutex);
            log_message(log_module, MSG_DEBUG, "Unregistered thread from monitoring: %s", 
                       global_monitor->threads[i].name);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return -1;
}

/**
 * @brief Check if a thread should shutdown
 */
int should_thread_shutdown(pthread_t thread)
{
    if (!global_monitor) {
        return 0;
    }
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    for (int i = 0; i < global_monitor->num_threads; i++) {
        if (pthread_equal(global_monitor->threads[i].thread, thread)) {
            int should_shutdown = global_monitor->threads[i].shutdown_requested || 
                                 global_monitor->global_shutdown || 
                                 get_interrupted();
            pthread_mutex_unlock(&global_monitor->mutex);
            return should_shutdown;
        }
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return 0;
}

/**
 * @brief Thread checkin - call this regularly in thread loops
 */
int thread_checkin(pthread_t thread)
{
    if (!global_monitor) {
        return -1;
    }
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    for (int i = 0; i < global_monitor->num_threads; i++) {
        if (pthread_equal(global_monitor->threads[i].thread, thread)) {
            global_monitor->threads[i].last_checkin = time(NULL);
            pthread_mutex_unlock(&global_monitor->mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return -1;
}

/**
 * @brief Signal all monitored threads to shutdown
 */
int signal_all_monitored_threads_shutdown(void)
{
    if (!global_monitor) {
        return -1;
    }
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    log_message(log_module, MSG_INFO, "Signaling %d monitored threads to shutdown", 
                global_monitor->num_threads);
    
    global_monitor->global_shutdown = 1;
    global_monitor->shutdown_initiated = time(NULL);
    
    for (int i = 0; i < global_monitor->num_threads; i++) {
        global_monitor->threads[i].shutdown_requested = 1;
        global_monitor->threads[i].status = THREAD_STATUS_SHUTTING_DOWN;
        log_message(log_module, MSG_DEBUG, "Signaled thread to shutdown: %s", 
                   global_monitor->threads[i].name);
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return 0;
}

/**
 * @brief Wait for all threads to shutdown
 */
int wait_for_all_threads_shutdown(int timeout_sec)
{
    if (!global_monitor) {
        return -1;
    }
    
    time_t start_time = time(NULL);
    time_t timeout_time = (timeout_sec > 0) ? start_time + timeout_sec : 0;
    
    log_message(log_module, MSG_INFO, "Waiting for %d threads to shutdown...", 
                global_monitor->num_threads);
    
    while (1) {
        pthread_mutex_lock(&global_monitor->mutex);
        
        int running_threads = 0;
        for (int i = 0; i < global_monitor->num_threads; i++) {
            if (global_monitor->threads[i].status == THREAD_STATUS_RUNNING ||
                global_monitor->threads[i].status == THREAD_STATUS_SHUTTING_DOWN) {
                running_threads++;
            }
        }
        
        pthread_mutex_unlock(&global_monitor->mutex);
        
        if (running_threads == 0) {
            log_message(log_module, MSG_INFO, "All threads have shutdown successfully");
            return 0;
        }
        
        // Check timeout
        if (timeout_sec > 0 && time(NULL) >= timeout_time) {
            log_message(log_module, MSG_WARN, "Timeout waiting for %d threads to shutdown", 
                        running_threads);
            return -1;
        }
        
        // Poll with timeout instead of usleep
        struct pollfd pfd = {0, 0, 0}; // No file descriptor, just timeout
        int poll_result = poll(&pfd, 1, 100); // 100ms timeout
        if (poll_result < 0 && errno != EINTR) {
            log_message(log_module, MSG_ERROR, "Thread shutdown monitor poll error: %s", strerror(errno));
        }
    }
}

/**
 * @brief Check if any threads are still running
 */
int are_threads_still_running(void)
{
    if (!global_monitor) {
        return 0;
    }
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    for (int i = 0; i < global_monitor->num_threads; i++) {
        if (global_monitor->threads[i].status == THREAD_STATUS_RUNNING ||
            global_monitor->threads[i].status == THREAD_STATUS_SHUTTING_DOWN) {
            pthread_mutex_unlock(&global_monitor->mutex);
            return 1;
        }
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return 0;
}

/**
 * @brief Get status of all monitored threads
 */
int get_thread_statuses(monitored_thread_t *status_array, int max_threads)
{
    if (!global_monitor || !status_array) {
        return 0;
    }
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    int count = (global_monitor->num_threads < max_threads) ? 
                global_monitor->num_threads : max_threads;
    
    for (int i = 0; i < count; i++) {
        status_array[i] = global_monitor->threads[i];
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return count;
}

/**
 * @brief Check for dead or unresponsive threads
 */
int check_for_dead_threads(void)
{
    if (!global_monitor) {
        return 0;
    }
    
    time_t now = time(NULL);
    int dead_threads = 0;
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    for (int i = 0; i < global_monitor->num_threads; i++) {
        if (global_monitor->threads[i].status == THREAD_STATUS_RUNNING) {
            time_t time_since_checkin = now - global_monitor->threads[i].last_checkin;
            int timeout_sec = global_monitor->threads[i].timeout_ms / 1000;
            
            if (time_since_checkin > timeout_sec) {
                log_message(log_module, MSG_WARN, "Thread %s appears dead (no checkin for %ld seconds)", 
                           global_monitor->threads[i].name, time_since_checkin);
                global_monitor->threads[i].status = THREAD_STATUS_ERROR;
                dead_threads++;
            }
        }
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return dead_threads;
}

/**
 * @brief Force shutdown of unresponsive threads
 */
int force_shutdown_unresponsive_threads(void)
{
    if (!global_monitor) {
        return 0;
    }
    
    int force_shutdown_count = 0;
    
    pthread_mutex_lock(&global_monitor->mutex);
    
    for (int i = 0; i < global_monitor->num_threads; i++) {
        if (global_monitor->threads[i].status == THREAD_STATUS_ERROR) {
            log_message(log_module, MSG_WARN, "Force-shutting down unresponsive thread: %s", 
                       global_monitor->threads[i].name);
            
            // Note: We can't actually force-kill threads in a portable way
            // This is more of a status update
            global_monitor->threads[i].status = THREAD_STATUS_STOPPED;
            force_shutdown_count++;
        }
    }
    
    pthread_mutex_unlock(&global_monitor->mutex);
    return force_shutdown_count;
}
