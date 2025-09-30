/**
 * @file http_thread.h
 * @brief HTTP server thread implementation
 * 
 * This module provides a dedicated thread for the HTTP server to prevent
 * blocking during DVB operations and scanning.
 */

#ifndef HTTP_THREAD_H
#define HTTP_THREAD_H

#include "mumudvb.h"
#include "unicast_http.h"
#include "tune.h"
#include "rewrite.h"
#include "dvb.h"

// HTTP thread state
typedef enum {
    HTTP_THREAD_STOPPED = 0,
    HTTP_THREAD_RUNNING = 1,
    HTTP_THREAD_STOPPING = 2
} http_thread_state_t;

// HTTP thread parameters
typedef struct {
    // Thread control
    pthread_t thread;
    http_thread_state_t state;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    
    // HTTP server parameters
    unicast_parameters_t *unicast_params;
    
    // Shared data (read-only for HTTP thread)
    mumudvb_channel_t *channels;
    int number_of_channels;
    strength_parameters_t *strength_params;
    auto_p_t *auto_params;
    void *cam_params;
    void *scam_vars;
    eit_packet_t *eit_packets;
    
    // Data mutexes for thread-safe access
    pthread_mutex_t channels_mutex;
    pthread_mutex_t strength_mutex;
    pthread_mutex_t auto_mutex;
    pthread_mutex_t cam_mutex;
    pthread_mutex_t scam_mutex;
    pthread_mutex_t eit_mutex;
    
    // Shutdown flag
    volatile int shutdown_requested;
} http_thread_params_t;

// Global HTTP thread instance
extern http_thread_params_t *global_http_thread;

// Function declarations
int init_http_thread(unicast_parameters_t *unicast_params);
void cleanup_http_thread(void);
int start_http_thread(void);
int stop_http_thread(void);
void *http_thread_main(void *arg);

// Thread-safe data access functions
int http_thread_update_channels(mumudvb_channel_t *channels, int number_of_channels);
int http_thread_update_strength_params(strength_parameters_t *strength_params);
int http_thread_update_auto_params(auto_p_t *auto_params);
int http_thread_update_cam_params(void *cam_params);
int http_thread_update_scam_vars(void *scam_vars);
int http_thread_update_eit_packets(eit_packet_t *eit_packets);

// Status functions
int is_http_thread_running(void);
int is_http_thread_healthy(void);

// Client request processing (integrated into HTTP thread)
void process_client_requests_in_http_thread(void);

#endif // HTTP_THREAD_H
