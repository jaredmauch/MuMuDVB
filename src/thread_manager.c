/**
 * @file thread_manager.c
 * @brief Global thread management system implementation
 */

#define _GNU_SOURCE
#include "thread_manager.h"
#include "mumudvb.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static char *log_module = "Thread-Manager: ";

static thread_manager_t *global_thread_manager = NULL;

/**
 * @brief Expand the thread array if needed
 * @return 0 on success, -1 on error
 */
static int expand_thread_array_if_needed(void)
{
    if (!global_thread_manager) {
        return -1;
    }
    
    // Check if we need to expand
    if (global_thread_manager->num_threads >= global_thread_manager->allocated_threads) {
        // Check if we're at the limit
        if (global_thread_manager->max_threads > 0 && 
            global_thread_manager->num_threads >= global_thread_manager->max_threads) {
            return -1; // At hard limit
        }
        
        // Expand by doubling the size, with a minimum increase of 10
        int new_size = global_thread_manager->allocated_threads * 2;
        if (new_size < global_thread_manager->allocated_threads + 10) {
            new_size = global_thread_manager->allocated_threads + 10;
        }
        
        // Respect max_threads if set
        if (global_thread_manager->max_threads > 0 && new_size > global_thread_manager->max_threads) {
            new_size = global_thread_manager->max_threads;
        }
        
        tracked_thread_t *new_threads = realloc(global_thread_manager->threads, 
                                               new_size * sizeof(tracked_thread_t));
        if (!new_threads) {
            log_message(log_module, MSG_ERROR, "Failed to expand thread array from %d to %d", 
                       global_thread_manager->allocated_threads, new_size);
            return -1;
        }
        
        // Zero out the new space
        memset(new_threads + global_thread_manager->allocated_threads, 0, 
               (new_size - global_thread_manager->allocated_threads) * sizeof(tracked_thread_t));
        
        global_thread_manager->threads = new_threads;
        global_thread_manager->allocated_threads = new_size;
        
        log_message(log_module, MSG_DEBUG, "Expanded thread array from %d to %d slots", 
                   global_thread_manager->allocated_threads / 2, new_size);
    }
    
    return 0;
}

int init_thread_manager(int max_threads)
{
    if (global_thread_manager) {
        return 0; // Already initialized
    }
    
    global_thread_manager = malloc(sizeof(thread_manager_t));
    if (!global_thread_manager) {
        return -1;
    }
    
    // Set default values
    if (max_threads == -1) {
        max_threads = 0; // Default to unlimited
    }
    
    // Start with a reasonable initial allocation
    int initial_allocation = (max_threads > 0) ? max_threads : 50;
    
    global_thread_manager->threads = calloc(initial_allocation, sizeof(tracked_thread_t));
    if (!global_thread_manager->threads) {
        free(global_thread_manager);
        global_thread_manager = NULL;
        return -1;
    }
    
    global_thread_manager->max_threads = max_threads;
    global_thread_manager->num_threads = 0;
    global_thread_manager->allocated_threads = initial_allocation;
    
    if (pthread_mutex_init(&global_thread_manager->mutex, NULL) != 0) {
        free(global_thread_manager->threads);
        free(global_thread_manager);
        global_thread_manager = NULL;
        return -1;
    }
    
    const char *limit_str = (max_threads == 0) ? "unlimited" : "limited";
    log_message(log_module, MSG_DEBUG, "Thread manager initialized with %s thread limit (initial allocation: %d)", 
                limit_str, initial_allocation);
    return 0;
}

void cleanup_thread_manager(void)
{
    if (!global_thread_manager) {
        return;
    }
    
    log_message(log_module, MSG_DEBUG, "Cleaning up thread manager with %d tracked threads", 
                global_thread_manager->num_threads);
    
    // Signal all threads to shutdown
    signal_all_threads_shutdown();
    
    // Wait for all threads to complete
    wait_for_all_threads(5); // 5 second timeout
    
    // Cleanup resources
    if (global_thread_manager->threads) {
        free(global_thread_manager->threads);
    }
    
    pthread_mutex_destroy(&global_thread_manager->mutex);
    free(global_thread_manager);
    global_thread_manager = NULL;
    
    log_message(log_module, MSG_DEBUG, "Thread manager cleanup completed");
}

int register_thread(pthread_t thread, const char *name, 
                   void *(*cleanup_func)(void *), void *cleanup_data)
{
    if (!global_thread_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_thread_manager->mutex);
    
    // Check if we're at the hard limit
    if (global_thread_manager->max_threads > 0 && 
        global_thread_manager->num_threads >= global_thread_manager->max_threads) {
        pthread_mutex_unlock(&global_thread_manager->mutex);
        log_message(log_module, MSG_ERROR, "Thread manager: maximum threads reached (%d)", 
                   global_thread_manager->max_threads);
        return -1;
    }
    
    // Expand array if needed
    if (expand_thread_array_if_needed() != 0) {
        pthread_mutex_unlock(&global_thread_manager->mutex);
        log_message(log_module, MSG_ERROR, "Thread manager: failed to expand thread array");
        return -1;
    }
    
    int idx = global_thread_manager->num_threads++;
    tracked_thread_t *t = &global_thread_manager->threads[idx];
    
    t->thread = thread;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    t->shutdown = 0;
    t->cleanup_func = cleanup_func;
    t->cleanup_data = cleanup_data;
    
    pthread_mutex_unlock(&global_thread_manager->mutex);
    
    log_message(log_module, MSG_DEBUG, "Registered thread: %s (total: %d)", name, 
               global_thread_manager->num_threads);
    return 0;
}

int unregister_thread(pthread_t thread)
{
    if (!global_thread_manager) {
        return -1;
    }
    
    pthread_mutex_lock(&global_thread_manager->mutex);
    
    for (int i = 0; i < global_thread_manager->num_threads; i++) {
        if (pthread_equal(global_thread_manager->threads[i].thread, thread)) {
            // Move last thread to this position
            if (i < global_thread_manager->num_threads - 1) {
                global_thread_manager->threads[i] = 
                    global_thread_manager->threads[global_thread_manager->num_threads - 1];
            }
            global_thread_manager->num_threads--;
            pthread_mutex_unlock(&global_thread_manager->mutex);
            log_message(log_module, MSG_DEBUG, "Unregistered thread: %s", 
                       global_thread_manager->threads[i].name);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&global_thread_manager->mutex);
    return -1;
}

void signal_all_threads_shutdown(void)
{
    if (!global_thread_manager) {
        return;
    }
    
    pthread_mutex_lock(&global_thread_manager->mutex);
    
    log_message(log_module, MSG_INFO, "Signaling %d tracked threads to shutdown", 
                global_thread_manager->num_threads);
    
    for (int i = 0; i < global_thread_manager->num_threads; i++) {
        global_thread_manager->threads[i].shutdown = 1;
        log_message(log_module, MSG_DEBUG, "Signaled thread to shutdown: %s", 
                   global_thread_manager->threads[i].name);
    }
    
    pthread_mutex_unlock(&global_thread_manager->mutex);
}

int wait_for_all_threads(int timeout_sec)
{
    if (!global_thread_manager) {
        return 0;
    }
    
    pthread_mutex_lock(&global_thread_manager->mutex);
    
    int num_threads = global_thread_manager->num_threads;
    tracked_thread_t *threads = malloc(num_threads * sizeof(tracked_thread_t));
    if (!threads) {
        pthread_mutex_unlock(&global_thread_manager->mutex);
        return -1;
    }
    
    // Copy thread info for joining
    memcpy(threads, global_thread_manager->threads, num_threads * sizeof(tracked_thread_t));
    
    pthread_mutex_unlock(&global_thread_manager->mutex);
    
    log_message(log_module, MSG_INFO, "Waiting for %d threads to complete...", num_threads);
    
    int completed = 0;
    for (int i = 0; i < num_threads; i++) {
        if (!pthread_equal(threads[i].thread, pthread_self())) {
            log_message(log_module, MSG_DEBUG, "Waiting for thread: %s", threads[i].name);
            
            if (timeout_sec > 0) {
                // Use timed join
                struct timespec timeout;
                clock_gettime(CLOCK_REALTIME, &timeout);
                timeout.tv_sec += timeout_sec;
                
                int ret = pthread_timedjoin_np(threads[i].thread, NULL, &timeout);
                if (ret == 0) {
                    completed++;
                    log_message(log_module, MSG_DEBUG, "Thread completed: %s", threads[i].name);
                } else if (ret == ETIMEDOUT) {
                    log_message(log_module, MSG_WARN, "Thread timeout: %s", threads[i].name);
                } else {
                    log_message(log_module, MSG_WARN, "Thread join failed: %s (%s)", 
                               threads[i].name, strerror(ret));
                }
            } else {
                // Use regular join
                int ret = pthread_join(threads[i].thread, NULL);
                if (ret == 0) {
                    completed++;
                    log_message(log_module, MSG_DEBUG, "Thread completed: %s", threads[i].name);
                } else {
                    log_message(log_module, MSG_WARN, "Thread join failed: %s (%s)", 
                               threads[i].name, strerror(ret));
                }
            }
        } else {
            completed++;
        }
    }
    
    free(threads);
    
    log_message(log_module, MSG_INFO, "Thread cleanup completed: %d/%d threads", 
                completed, num_threads);
    
    return (completed == num_threads) ? 0 : -1;
}

thread_manager_t *get_thread_manager(void)
{
    return global_thread_manager;
}
