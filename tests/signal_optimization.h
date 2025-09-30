/**
 * @file signal_optimization.h
 * @brief Optimized signal handling for faster Ctrl-C response
 * 
 * This module provides fast signal response by reducing thread yielding intervals
 * and implementing immediate signal propagation to all threads.
 */

#ifndef _SIGNAL_OPTIMIZATION_H
#define _SIGNAL_OPTIMIZATION_H

#include "mumudvb.h"
#include <pthread.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Optimized timing constants for fast signal response */
#define FAST_SIGNAL_RESPONSE_MS 10        // 10ms for critical threads
#define NORMAL_SIGNAL_RESPONSE_MS 50      // 50ms for normal threads  
#define BACKGROUND_SIGNAL_RESPONSE_MS 100 // 100ms for background threads

/** @brief Signal propagation structure */
typedef struct {
    pthread_cond_t signal_cond;           // Condition variable for signal events
    pthread_mutex_t signal_mutex;         // Mutex for signal synchronization
    volatile int signal_received;         // Signal received flag
    volatile int shutdown_requested;      // Shutdown requested flag
    pthread_t signal_thread;              // Signal propagation thread
} signal_propagator_t;

/** @brief Thread signal context */
typedef struct {
    pthread_cond_t *signal_cond;          // Pointer to signal condition
    pthread_mutex_t *signal_mutex;        // Pointer to signal mutex
    volatile int *shutdown_requested;     // Pointer to shutdown flag
    int thread_id;                        // Thread identifier
    int priority;                         // Thread priority (0=critical, 1=normal, 2=background)
} thread_signal_context_t;

/**
 * @brief Initialize optimized signal handling
 * @return 0 on success, -1 on error
 */
int init_fast_signal_handling(void);

/**
 * @brief Cleanup optimized signal handling
 */
void cleanup_fast_signal_handling(void);

/**
 * @brief Register a thread for fast signal response
 * @param thread_id Unique thread identifier
 * @param priority Thread priority (0=critical, 1=normal, 2=background)
 * @param context Output context structure
 * @return 0 on success, -1 on error
 */
int register_thread_for_signals(int thread_id, int priority, thread_signal_context_t *context);

/**
 * @brief Wait for signal or timeout with optimized response time
 * @param context Thread signal context
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return 0 if signal received, -1 on timeout, -2 on error
 */
int wait_for_signal_or_timeout(thread_signal_context_t *context, int timeout_ms);

/**
 * @brief Check if signal was received (non-blocking)
 * @param context Thread signal context
 * @return 1 if signal received, 0 if not
 */
int check_signal_received(thread_signal_context_t *context);

/**
 * @brief Signal all registered threads immediately
 * @return 0 on success, -1 on error
 */
int signal_all_threads(void);

/**
 * @brief Get recommended yielding interval for thread priority
 * @param priority Thread priority (0=critical, 1=normal, 2=background)
 * @return Recommended yielding interval in milliseconds
 */
int get_recommended_yield_interval(int priority);

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_OPTIMIZATION_H */
