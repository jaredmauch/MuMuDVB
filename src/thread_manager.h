/**
 * @file thread_manager.h
 * @brief Global thread management system for MuMuDVB
 * 
 * This module provides centralized thread tracking and cleanup for all
 * background threads created by MuMuDVB, ensuring proper shutdown on Ctrl+C.
 */

#ifndef _THREAD_MANAGER_H
#define _THREAD_MANAGER_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Thread information structure */
typedef struct {
    pthread_t thread;           // Thread handle
    char name[64];             // Thread name for logging
    volatile int shutdown;     // Shutdown flag
    void *(*cleanup_func)(void *); // Optional cleanup function
    void *cleanup_data;        // Data for cleanup function
} tracked_thread_t;

/** @brief Global thread manager */
typedef struct {
    tracked_thread_t *threads;  // Array of tracked threads
    int max_threads;           // Maximum number of threads (0 = unlimited)
    int num_threads;           // Current number of threads
    int allocated_threads;     // Currently allocated array size
    pthread_mutex_t mutex;     // Mutex for thread safety
} thread_manager_t;

/**
 * @brief Initialize the global thread manager
 * @param max_threads Maximum number of threads to track (0 = unlimited, -1 = default)
 * @return 0 on success, -1 on error
 */
int init_thread_manager(int max_threads);

/**
 * @brief Cleanup the global thread manager
 */
void cleanup_thread_manager(void);

/**
 * @brief Register a thread for tracking
 * @param thread Thread handle
 * @param name Thread name for logging
 * @param cleanup_func Optional cleanup function
 * @param cleanup_data Data for cleanup function
 * @return 0 on success, -1 on error
 */
int register_thread(pthread_t thread, const char *name, 
                   void *(*cleanup_func)(void *), void *cleanup_data);

/**
 * @brief Unregister a thread from tracking
 * @param thread Thread handle to unregister
 * @return 0 on success, -1 on error
 */
int unregister_thread(pthread_t thread);

/**
 * @brief Signal all tracked threads to shutdown
 */
void signal_all_threads_shutdown(void);

/**
 * @brief Wait for all tracked threads to complete
 * @param timeout_sec Timeout in seconds (0 = no timeout)
 * @return 0 on success, -1 on timeout
 */
int wait_for_all_threads(int timeout_sec);

/**
 * @brief Get the global thread manager instance
 * @return Pointer to thread manager, or NULL if not initialized
 */
thread_manager_t *get_thread_manager(void);

/**
 * @brief Get current thread count
 * @return Number of currently tracked threads
 */
int get_thread_count(void);

/**
 * @brief Get maximum thread limit
 * @return Maximum thread limit (0 = unlimited)
 */
int get_max_thread_limit(void);

#ifdef __cplusplus
}
#endif

#endif /* _THREAD_MANAGER_H */
