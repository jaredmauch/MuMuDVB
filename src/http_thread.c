/**
 * @file http_thread.c
 * @brief HTTP server thread implementation
 * 
 * This module provides a dedicated thread for the HTTP server to prevent
 * blocking during DVB operations and scanning.
 */

#include "http_thread.h"
#include "log.h"
#include "thread_manager.h"
#include "dvb.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

static char *log_module = "HTTP-Thread: ";

// Global HTTP thread instance
http_thread_params_t *global_http_thread = NULL;

/**
 * @brief Initialize HTTP thread parameters
 * @param unicast_params Unicast parameters for HTTP server
 * @return 0 on success, -1 on error
 */
int init_http_thread(unicast_parameters_t *unicast_params)
{
    if (global_http_thread) {
        log_message(log_module, MSG_WARN, "HTTP thread already initialized");
        return 0;
    }
    
    global_http_thread = malloc(sizeof(http_thread_params_t));
    if (!global_http_thread) {
        log_message(log_module, MSG_ERROR, "Failed to allocate HTTP thread parameters");
        return -1;
    }
    
    memset(global_http_thread, 0, sizeof(http_thread_params_t));
    
    // Initialize mutexes
    if (pthread_mutex_init(&global_http_thread->state_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize state mutex");
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    if (pthread_cond_init(&global_http_thread->state_cond, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize state condition");
        pthread_mutex_destroy(&global_http_thread->state_mutex);
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    if (pthread_mutex_init(&global_http_thread->channels_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize channels mutex");
        pthread_cond_destroy(&global_http_thread->state_cond);
        pthread_mutex_destroy(&global_http_thread->state_mutex);
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    if (pthread_mutex_init(&global_http_thread->strength_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize strength mutex");
        pthread_mutex_destroy(&global_http_thread->channels_mutex);
        pthread_cond_destroy(&global_http_thread->state_cond);
        pthread_mutex_destroy(&global_http_thread->state_mutex);
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    if (pthread_mutex_init(&global_http_thread->auto_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize auto mutex");
        pthread_mutex_destroy(&global_http_thread->strength_mutex);
        pthread_mutex_destroy(&global_http_thread->channels_mutex);
        pthread_cond_destroy(&global_http_thread->state_cond);
        pthread_mutex_destroy(&global_http_thread->state_mutex);
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    if (pthread_mutex_init(&global_http_thread->cam_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize cam mutex");
        pthread_mutex_destroy(&global_http_thread->auto_mutex);
        pthread_mutex_destroy(&global_http_thread->strength_mutex);
        pthread_mutex_destroy(&global_http_thread->channels_mutex);
        pthread_cond_destroy(&global_http_thread->state_cond);
        pthread_mutex_destroy(&global_http_thread->state_mutex);
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    if (pthread_mutex_init(&global_http_thread->scam_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize scam mutex");
        pthread_mutex_destroy(&global_http_thread->cam_mutex);
        pthread_mutex_destroy(&global_http_thread->auto_mutex);
        pthread_mutex_destroy(&global_http_thread->strength_mutex);
        pthread_mutex_destroy(&global_http_thread->channels_mutex);
        pthread_cond_destroy(&global_http_thread->state_cond);
        pthread_mutex_destroy(&global_http_thread->state_mutex);
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    if (pthread_mutex_init(&global_http_thread->eit_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Failed to initialize eit mutex");
        pthread_mutex_destroy(&global_http_thread->scam_mutex);
        pthread_mutex_destroy(&global_http_thread->cam_mutex);
        pthread_mutex_destroy(&global_http_thread->auto_mutex);
        pthread_mutex_destroy(&global_http_thread->strength_mutex);
        pthread_mutex_destroy(&global_http_thread->channels_mutex);
        pthread_cond_destroy(&global_http_thread->state_cond);
        pthread_mutex_destroy(&global_http_thread->state_mutex);
        free(global_http_thread);
        global_http_thread = NULL;
        return -1;
    }
    
    // Set initial state
    global_http_thread->state = HTTP_THREAD_STOPPED;
    global_http_thread->unicast_params = unicast_params;
    global_http_thread->shutdown_requested = 0;
    
    log_message(log_module, MSG_INFO, "HTTP thread initialized successfully");
    return 0;
}

/**
 * @brief Cleanup HTTP thread resources
 */
void cleanup_http_thread(void)
{
    if (!global_http_thread) {
        return;
    }
    
    // Stop thread if running
    stop_http_thread();
    
    // Wait for thread to finish
    if (global_http_thread->thread != 0) {
        pthread_t thread_id = global_http_thread->thread;
        // Validate thread ID before attempting to join
        if ((unsigned long)thread_id >= 0x1000 && (unsigned long)thread_id <= 0x7fffffffffff) {
            int join_result = pthread_join(thread_id, NULL);
            if (join_result != 0) {
                log_message(log_module, MSG_WARN, "HTTP thread join failed: %s", strerror(join_result));
            }
        } else {
            log_message(log_module, MSG_WARN, "HTTP thread has invalid thread ID: %lu, skipping join", 
                       (unsigned long)thread_id);
        }
        global_http_thread->thread = 0;
    }
    
    // Destroy mutexes
    pthread_mutex_destroy(&global_http_thread->eit_mutex);
    pthread_mutex_destroy(&global_http_thread->scam_mutex);
    pthread_mutex_destroy(&global_http_thread->cam_mutex);
    pthread_mutex_destroy(&global_http_thread->auto_mutex);
    pthread_mutex_destroy(&global_http_thread->strength_mutex);
    pthread_mutex_destroy(&global_http_thread->channels_mutex);
    pthread_cond_destroy(&global_http_thread->state_cond);
    pthread_mutex_destroy(&global_http_thread->state_mutex);
    
    free(global_http_thread);
    global_http_thread = NULL;
    
    log_message(log_module, MSG_INFO, "HTTP thread cleaned up");
}

/**
 * @brief Start the HTTP thread
 * @return 0 on success, -1 on error
 */
int start_http_thread(void)
{
    if (!global_http_thread) {
        log_message(log_module, MSG_ERROR, "HTTP thread not initialized");
        return -1;
    }
    
    pthread_mutex_lock(&global_http_thread->state_mutex);
    
    if (global_http_thread->state == HTTP_THREAD_RUNNING) {
        pthread_mutex_unlock(&global_http_thread->state_mutex);
        log_message(log_module, MSG_WARN, "HTTP thread already running");
        return 0;
    }
    
    global_http_thread->state = HTTP_THREAD_RUNNING;
    global_http_thread->shutdown_requested = 0;
    
    int result = pthread_create(&global_http_thread->thread, NULL, http_thread_main, global_http_thread);
    if (result != 0) {
        global_http_thread->state = HTTP_THREAD_STOPPED;
        pthread_mutex_unlock(&global_http_thread->state_mutex);
        log_message(log_module, MSG_ERROR, "Failed to create HTTP thread: %s", strerror(result));
        return -1;
    }
    
    pthread_mutex_unlock(&global_http_thread->state_mutex);
    
    log_message(log_module, MSG_INFO, "HTTP thread started");
    return 0;
}

/**
 * @brief Stop the HTTP thread
 * @return 0 on success, -1 on error
 */
int stop_http_thread(void)
{
    if (!global_http_thread) {
        return 0;
    }
    
    pthread_mutex_lock(&global_http_thread->state_mutex);
    
    if (global_http_thread->state == HTTP_THREAD_STOPPED) {
        pthread_mutex_unlock(&global_http_thread->state_mutex);
        return 0;
    }
    
    global_http_thread->state = HTTP_THREAD_STOPPING;
    global_http_thread->shutdown_requested = 1;
    
    pthread_cond_signal(&global_http_thread->state_cond);
    pthread_mutex_unlock(&global_http_thread->state_mutex);
    
    log_message(log_module, MSG_INFO, "HTTP thread stop requested");
    return 0;
}

/**
 * @brief Main HTTP thread function
 * @param arg HTTP thread parameters
 * @return NULL
 */
void *http_thread_main(void *arg)
{
    http_thread_params_t *params = (http_thread_params_t *)arg;
    int select_ret;
    int iRet;
    
    log_message(log_module, MSG_INFO, "HTTP thread started");
    log_message(log_module, MSG_INFO, "Client request processor integrated into HTTP server thread");
    
    // Register this thread
    register_thread(pthread_self(), "HTTP-Server", NULL, NULL);
    
        // Log initial state
        if (params->unicast_params) {
            log_message(log_module, MSG_DEBUG, "HTTP thread: pfdsnum=%d, pfds=%p",
                       params->unicast_params->pfdsnum,
                       params->unicast_params->pfds);
        }
    
    // Performance monitoring
    int total_polls = 0;
    int events_processed = 0;
    
    while (!params->shutdown_requested && !get_interrupted()) {
        // Check if we have HTTP file descriptors to select
        if (params->unicast_params && params->unicast_params->pfdsnum > 0 && 
            params->unicast_params->pfds) {
            
            // Use select() with 1-second timeout
            fd_set readfds;
            int max_fd = -1;
            struct timeval timeout;
            
            FD_ZERO(&readfds);
            
            // Add all file descriptors to the read set
            for (int i = 0; i < params->unicast_params->pfdsnum; i++) {
                int fd = params->unicast_params->pfds[i].fd;
                if (fd >= 0) {
                    FD_SET(fd, &readfds);
                    if (fd > max_fd) {
                        max_fd = fd;
                    }
                }
            }
            
            // Set 1-second timeout
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
            
            select_ret = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
            total_polls++;
            
            if (select_ret < 0) {
                if (errno != EINTR) {
                    log_message(log_module, MSG_ERROR, "HTTP select error: %s", strerror(errno));
                    break;
                }
                continue;
            }
            
            if (select_ret > 0) {
                events_processed++;
                // Update pollfd revents based on select results
                for (int i = 0; i < params->unicast_params->pfdsnum; i++) {
                    int fd = params->unicast_params->pfds[i].fd;
                    if (fd >= 0 && FD_ISSET(fd, &readfds)) {
                        params->unicast_params->pfds[i].revents = POLLIN;
                    } else {
                        params->unicast_params->pfds[i].revents = 0;
                    }
                }
                
                // Handle HTTP events - thread-safe since HTTP operations are read-only
                // Data may be NULL during startup, but that's handled gracefully by unicast_handle_fd_event
                pthread_mutex_lock(&params->channels_mutex);
                pthread_mutex_lock(&params->strength_mutex);
                pthread_mutex_lock(&params->auto_mutex);
                pthread_mutex_lock(&params->cam_mutex);
                pthread_mutex_lock(&params->scam_mutex);
                pthread_mutex_lock(&params->eit_mutex);
                
                iRet = unicast_handle_fd_event(
                    params->unicast_params,
                    params->channels,
                    params->number_of_channels,
                    params->strength_params,
                    params->auto_params,
                    params->cam_params,
                    params->scam_vars,
                    params->eit_packets
                );
                
                pthread_mutex_unlock(&params->eit_mutex);
                pthread_mutex_unlock(&params->scam_mutex);
                pthread_mutex_unlock(&params->cam_mutex);
                pthread_mutex_unlock(&params->auto_mutex);
                pthread_mutex_unlock(&params->strength_mutex);
                pthread_mutex_unlock(&params->channels_mutex);
                
                if (iRet) {
                    log_message(log_module, MSG_ERROR, "HTTP fd error %d", iRet);
                    // Don't set interrupted here - let main thread handle it
                }
                
                // Process client requests for dynamic card assignment
                process_client_requests_in_http_thread();
            }
        } else {
            // No HTTP file descriptors, poll with timeout instead of usleep
            struct pollfd pfd = {0, 0, 0}; // No file descriptor, just timeout
            int poll_result = poll(&pfd, 1, 10); // 10ms timeout
            if (poll_result < 0 && errno != EINTR) {
                log_message(log_module, MSG_ERROR, "HTTP thread poll error: %s", strerror(errno));
            }
        }
        
        // Periodic performance reporting (every 1000 selects)
        if (total_polls % 1000 == 0 && total_polls > 0) {
            int current_clients = params->unicast_params ? params->unicast_params->client_number : 0;
            log_message(log_module, MSG_DEBUG, "HTTP thread performance: %d selects, %d events processed, %d clients", 
                       total_polls, events_processed, current_clients);
            
            // Reset counters to avoid overflow
            if (total_polls > 10000) {
                total_polls = 0;
                events_processed = 0;
            }
        }
    }
    
    log_message(log_module, MSG_INFO, "HTTP thread stopping");
    
    // Update state
    pthread_mutex_lock(&params->state_mutex);
    params->state = HTTP_THREAD_STOPPED;
    pthread_cond_signal(&params->state_cond);
    pthread_mutex_unlock(&params->state_mutex);
    
    return NULL;
}

/**
 * @brief Update channels data (thread-safe)
 * @param channels Channel array
 * @param number_of_channels Number of channels
 * @return 0 on success, -1 on error
 */
int http_thread_update_channels(mumudvb_channel_t *channels, int number_of_channels)
{
    if (!global_http_thread) {
        return -1;
    }
    
    pthread_mutex_lock(&global_http_thread->channels_mutex);
    global_http_thread->channels = channels;
    global_http_thread->number_of_channels = number_of_channels;
    pthread_mutex_unlock(&global_http_thread->channels_mutex);
    
    return 0;
}

/**
 * @brief Update strength parameters (thread-safe)
 * @param strength_params Strength parameters
 * @return 0 on success, -1 on error
 */
int http_thread_update_strength_params(strength_parameters_t *strength_params)
{
    if (!global_http_thread) {
        return -1;
    }
    
    pthread_mutex_lock(&global_http_thread->strength_mutex);
    global_http_thread->strength_params = strength_params;
    pthread_mutex_unlock(&global_http_thread->strength_mutex);
    
    return 0;
}

/**
 * @brief Update auto parameters (thread-safe)
 * @param auto_params Auto parameters
 * @return 0 on success, -1 on error
 */
int http_thread_update_auto_params(auto_p_t *auto_params)
{
    if (!global_http_thread) {
        return -1;
    }
    
    pthread_mutex_lock(&global_http_thread->auto_mutex);
    global_http_thread->auto_params = auto_params;
    pthread_mutex_unlock(&global_http_thread->auto_mutex);
    
    return 0;
}

/**
 * @brief Update CAM parameters (thread-safe)
 * @param cam_params CAM parameters
 * @return 0 on success, -1 on error
 */
int http_thread_update_cam_params(void *cam_params)
{
    if (!global_http_thread) {
        return -1;
    }
    
    pthread_mutex_lock(&global_http_thread->cam_mutex);
    global_http_thread->cam_params = cam_params;
    pthread_mutex_unlock(&global_http_thread->cam_mutex);
    
    return 0;
}

/**
 * @brief Update SCAM variables (thread-safe)
 * @param scam_vars SCAM variables
 * @return 0 on success, -1 on error
 */
int http_thread_update_scam_vars(void *scam_vars)
{
    if (!global_http_thread) {
        return -1;
    }
    
    pthread_mutex_lock(&global_http_thread->scam_mutex);
    global_http_thread->scam_vars = scam_vars;
    pthread_mutex_unlock(&global_http_thread->scam_mutex);
    
    return 0;
}

/**
 * @brief Update EIT packets (thread-safe)
 * @param eit_packets EIT packets
 * @return 0 on success, -1 on error
 */
int http_thread_update_eit_packets(eit_packet_t *eit_packets)
{
    if (!global_http_thread) {
        return -1;
    }
    
    pthread_mutex_lock(&global_http_thread->eit_mutex);
    global_http_thread->eit_packets = eit_packets;
    pthread_mutex_unlock(&global_http_thread->eit_mutex);
    
    return 0;
}

/**
 * @brief Check if HTTP thread is running
 * @return 1 if running, 0 if not
 */
int is_http_thread_running(void)
{
    if (!global_http_thread) {
        return 0;
    }
    
    pthread_mutex_lock(&global_http_thread->state_mutex);
    int running = (global_http_thread->state == HTTP_THREAD_RUNNING);
    pthread_mutex_unlock(&global_http_thread->state_mutex);
    
    return running;
}

/**
 * @brief Check if HTTP thread is healthy
 * @return 1 if healthy, 0 if not
 */
int is_http_thread_healthy(void)
{
    if (!global_http_thread) {
        return 0;
    }
    
    // Simple health check - thread exists and is not stopping
    pthread_mutex_lock(&global_http_thread->state_mutex);
    int healthy = (global_http_thread->state == HTTP_THREAD_RUNNING) && 
                  !global_http_thread->shutdown_requested;
    pthread_mutex_unlock(&global_http_thread->state_mutex);
    
    return healthy;
}

/**
 * @brief Process client requests for dynamic card assignment (called from HTTP thread)
 */
void process_client_requests_in_http_thread(void)
{
    // This function will be implemented to handle client requests
    // For now, it's a placeholder that can be called from the HTTP thread
    // The actual client request processing logic will be moved here from parallel_card_manager.c
    
    // Check if parallel manager is available
    // Forward declaration for parallel manager function
    extern int is_parallel_operation_active(void);
    if (!is_parallel_operation_active()) {
        return; // No parallel operations active, nothing to process
    }
    
    // TODO: Move client request processing logic here from client_request_processor()
    // This will integrate client request handling directly into the HTTP server thread
}
