/**
 * @file signal_optimization.c
 * @brief Optimized signal handling implementation for faster Ctrl-C response
 */

#include "signal_optimization.h"
#include "log.h"
#include "errors.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

static char *log_module = "Signal-Opt: ";

// Global signal propagator
static signal_propagator_t *global_signal_propagator = NULL;

/**
 * @brief Signal propagation thread function
 */
static void *signal_propagation_thread(void *arg)
{
    signal_propagator_t *propagator = (signal_propagator_t *)arg;
    
    log_message(log_module, MSG_DEBUG, "Signal propagation thread started");
    
    while (!propagator->shutdown_requested) {
        // Wait for signal events
        pthread_mutex_lock(&propagator->signal_mutex);
        
        // Wait for signal or timeout (10ms)
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 10000000; // 10ms
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        
        int ret = pthread_cond_timedwait(&propagator->signal_cond, 
                                        &propagator->signal_mutex, &timeout);
        
        if (ret == 0) {
            // Signal received, propagate to all threads
            log_message(log_module, MSG_DEBUG, "Signal propagation triggered");
            propagator->signal_received = 1;
        }
        
        pthread_mutex_unlock(&propagator->signal_mutex);
    }
    
    log_message(log_module, MSG_DEBUG, "Signal propagation thread stopped");
    return NULL;
}

/**
 * @brief Initialize optimized signal handling
 */
int init_fast_signal_handling(void)
{
    if (global_signal_propagator) {
        log_message(log_module, MSG_WARN, "Signal handling already initialized");
        return 0;
    }
    
    global_signal_propagator = malloc(sizeof(signal_propagator_t));
    if (!global_signal_propagator) {
        log_message(log_module, MSG_ERROR, "Cannot allocate signal propagator");
        return -1;
    }
    
    // Initialize condition variable and mutex
    if (pthread_cond_init(&global_signal_propagator->signal_cond, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot initialize signal condition");
        free(global_signal_propagator);
        return -1;
    }
    
    if (pthread_mutex_init(&global_signal_propagator->signal_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot initialize signal mutex");
        pthread_cond_destroy(&global_signal_propagator->signal_cond);
        free(global_signal_propagator);
        return -1;
    }
    
    global_signal_propagator->signal_received = 0;
    global_signal_propagator->shutdown_requested = 0;
    
    // Create signal propagation thread
    if (pthread_create(&global_signal_propagator->signal_thread, NULL, 
                      signal_propagation_thread, global_signal_propagator) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot create signal propagation thread");
        pthread_cond_destroy(&global_signal_propagator->signal_cond);
        pthread_mutex_destroy(&global_signal_propagator->signal_mutex);
        free(global_signal_propagator);
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Fast signal handling initialized");
    return 0;
}

/**
 * @brief Cleanup optimized signal handling
 */
void cleanup_fast_signal_handling(void)
{
    if (!global_signal_propagator) {
        return;
    }
    
    // Signal shutdown
    global_signal_propagator->shutdown_requested = 1;
    pthread_cond_signal(&global_signal_propagator->signal_cond);
    
    // Wait for thread to finish
    pthread_join(global_signal_propagator->signal_thread, NULL);
    
    // Cleanup resources
    pthread_cond_destroy(&global_signal_propagator->signal_cond);
    pthread_mutex_destroy(&global_signal_propagator->signal_mutex);
    free(global_signal_propagator);
    global_signal_propagator = NULL;
    
    log_message(log_module, MSG_INFO, "Fast signal handling cleaned up");
}

/**
 * @brief Register a thread for fast signal response
 */
int register_thread_for_signals(int thread_id, int priority, thread_signal_context_t *context)
{
    if (!global_signal_propagator || !context) {
        return -1;
    }
    
    context->signal_cond = &global_signal_propagator->signal_cond;
    context->signal_mutex = &global_signal_propagator->signal_mutex;
    context->shutdown_requested = &global_signal_propagator->shutdown_requested;
    context->thread_id = thread_id;
    context->priority = priority;
    
    log_message(log_module, MSG_DEBUG, "Registered thread %d with priority %d", thread_id, priority);
    return 0;
}

/**
 * @brief Wait for signal or timeout with optimized response time
 */
int wait_for_signal_or_timeout(thread_signal_context_t *context, int timeout_ms)
{
    if (!context || !global_signal_propagator) {
        return -2;
    }
    
    // Use different timeouts based on thread priority
    int effective_timeout = timeout_ms;
    if (effective_timeout <= 0) {
        effective_timeout = get_recommended_yield_interval(context->priority);
    }
    
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += effective_timeout * 1000000; // Convert ms to ns
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_sec++;
        timeout.tv_nsec -= 1000000000;
    }
    
    pthread_mutex_lock(context->signal_mutex);
    
    int ret = pthread_cond_timedwait(context->signal_cond, context->signal_mutex, &timeout);
    
    int signal_received = global_signal_propagator->signal_received;
    pthread_mutex_unlock(context->signal_mutex);
    
    if (ret == 0) {
        // Condition was signaled
        return 0;
    } else if (ret == ETIMEDOUT) {
        // Timeout
        return -1;
    } else {
        // Error
        return -2;
    }
}

/**
 * @brief Check if signal was received (non-blocking)
 */
int check_signal_received(thread_signal_context_t *context)
{
    if (!context || !global_signal_propagator) {
        return 0;
    }
    
    pthread_mutex_lock(context->signal_mutex);
    int signal_received = global_signal_propagator->signal_received;
    pthread_mutex_unlock(context->signal_mutex);
    
    return signal_received;
}

/**
 * @brief Signal all registered threads immediately
 */
int signal_all_threads(void)
{
    if (!global_signal_propagator) {
        return -1;
    }
    
    pthread_mutex_lock(&global_signal_propagator->signal_mutex);
    global_signal_propagator->signal_received = 1;
    pthread_cond_broadcast(&global_signal_propagator->signal_cond);
    pthread_mutex_unlock(&global_signal_propagator->signal_mutex);
    
    log_message(log_module, MSG_DEBUG, "Signaled all registered threads");
    return 0;
}

/**
 * @brief Get recommended yielding interval for thread priority
 */
int get_recommended_yield_interval(int priority)
{
    switch (priority) {
        case 0: // Critical threads (main loop, signal handlers)
            return FAST_SIGNAL_RESPONSE_MS;
        case 1: // Normal threads (card workers, DVB threads)
            return NORMAL_SIGNAL_RESPONSE_MS;
        case 2: // Background threads (scanner, monitor)
            return BACKGROUND_SIGNAL_RESPONSE_MS;
        default:
            return NORMAL_SIGNAL_RESPONSE_MS;
    }
}
