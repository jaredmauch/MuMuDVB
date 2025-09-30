/**
 * @file dvb_events.h
 * @brief Event-based DVB frontend monitoring and tuning
 * 
 * This module provides event-driven alternatives to polling loops
 * for DVB frontend status monitoring, signal lock detection, and tuning.
 */

#ifndef _DVB_EVENTS_H
#define _DVB_EVENTS_H

#include "mumudvb.h"
#include "tune.h"
#include <sys/poll.h>
#include <sys/epoll.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Frontend event types */
typedef enum {
    FE_EVENT_SIGNAL = 0x01,      // Signal detected
    FE_EVENT_CARRIER = 0x02,     // Carrier detected  
    FE_EVENT_LOCK = 0x04,        // Lock achieved
    FE_EVENT_TIMEOUT = 0x08,     // Timeout occurred
    FE_EVENT_ERROR = 0x10        // Error occurred
} fe_event_type_t;

/** @brief Frontend event structure */
typedef struct {
    int fd_frontend;              // Frontend file descriptor
    fe_status_t status;           // Current frontend status
    fe_event_type_t event_type;   // Type of event
    uint16_t signal_strength;     // Signal strength (0-65535)
    uint16_t snr;                 // Signal-to-noise ratio (0-65535)
    uint32_t ber;                 // Bit error rate
    int error_code;               // Error code if FE_EVENT_ERROR
} fe_event_t;

/** @brief Single frontend event data */
typedef struct {
    int card_id;                  // Card ID
    int fd_frontend;              // Frontend file descriptor
    fe_event_t last_event;        // Last received event
    int has_clients;              // Whether this frontend has active clients
    int needs_lock_monitoring;    // Whether this frontend needs lock monitoring
    time_t last_activity;         // Last activity timestamp
} fe_monitor_data_t;

/** @brief Multi-frontend event monitor */
typedef struct {
    int epoll_fd;                 // Epoll file descriptor
    fe_monitor_data_t *frontends; // Array of frontend data
    int max_frontends;            // Maximum number of frontends
    int num_frontends;            // Current number of active frontends
    pthread_mutex_t event_mutex;  // Mutex for thread safety
    volatile int monitoring;      // Monitoring state
    pthread_t monitor_thread;     // Background monitoring thread
    void (*event_callback)(int card_id, const fe_event_t *event, void *user_data);
    void *callback_user_data;     // User data for callback
} multi_fe_event_monitor_t;

/** @brief Tuning result structure */
typedef struct {
    int success;                  // 1 if successful, 0 if failed
    fe_event_type_t final_event;  // Final event type
    fe_status_t final_status;     // Final frontend status
    uint16_t signal_strength;     // Final signal strength
    uint16_t snr;                 // Final SNR
    uint32_t ber;                 // Final BER
    int timeout_ms;               // Timeout used
    int error_code;               // Error code if failed
} tuning_result_t;

/**
 * @brief Create a multi-frontend event monitor
 * @param max_frontends Maximum number of frontends to monitor
 * @param monitor Output monitor structure
 * @return 0 on success, -1 on error
 */
int create_multi_fe_event_monitor(int max_frontends, multi_fe_event_monitor_t *monitor);

/**
 * @brief Destroy a multi-frontend event monitor
 * @param monitor The monitor to destroy
 */
void destroy_multi_fe_event_monitor(multi_fe_event_monitor_t *monitor);

/**
 * @brief Add a frontend to the multi-frontend monitor
 * @param monitor The multi-frontend monitor
 * @param card_id The DVB card ID
 * @param has_clients Whether this frontend has active clients
 * @param needs_lock_monitoring Whether this frontend needs lock monitoring
 * @return 0 on success, -1 on error
 */
int add_frontend_to_monitor(multi_fe_event_monitor_t *monitor, int card_id, 
                           int has_clients, int needs_lock_monitoring);

/**
 * @brief Remove a frontend from the multi-frontend monitor
 * @param monitor The multi-frontend monitor
 * @param card_id The DVB card ID to remove
 * @return 0 on success, -1 on error
 */
int remove_frontend_from_monitor(multi_fe_event_monitor_t *monitor, int card_id);

/**
 * @brief Update frontend client status
 * @param monitor The multi-frontend monitor
 * @param card_id The DVB card ID
 * @param has_clients Whether this frontend has active clients
 * @return 0 on success, -1 on error
 */
int update_frontend_client_status(multi_fe_event_monitor_t *monitor, int card_id, int has_clients);

/**
 * @brief Start continuous multi-frontend monitoring
 * @param monitor The multi-frontend monitor
 * @param callback Function to call on events (NULL for no callback)
 * @param user_data User data passed to callback
 * @return 0 on success, -1 on error
 */
int start_multi_fe_monitoring(multi_fe_event_monitor_t *monitor, 
                             void (*callback)(int card_id, const fe_event_t *event, void *user_data),
                             void *user_data);

/**
 * @brief Stop continuous multi-frontend monitoring
 * @param monitor The multi-frontend monitor
 */
void stop_multi_fe_monitoring(multi_fe_event_monitor_t *monitor);

/**
 * @brief Get current status for a specific frontend
 * @param monitor The multi-frontend monitor
 * @param card_id The card ID to get status for
 * @param event Output event structure
 * @return 0 on success, -1 on error
 */
int get_frontend_status(multi_fe_event_monitor_t *monitor, int card_id, fe_event_t *event);

/**
 * @brief Wait for a specific frontend event with timeout
 * @param monitor The multi-frontend monitor
 * @param card_id The card ID to wait for events on
 * @param event_types Bitmask of event types to wait for
 * @param timeout_ms Timeout in milliseconds (0 = no timeout, -1 = infinite)
 * @param event Output event structure
 * @return 0 on success, -1 on timeout, -2 on error
 */
int wait_for_frontend_event(multi_fe_event_monitor_t *monitor, int card_id, 
                           fe_event_type_t event_types, int timeout_ms, fe_event_t *event);

/**
 * @brief Event-based tuning with lock detection
 * @param card_id The DVB card ID
 * @param tune_params Tuning parameters
 * @param timeout_ms Maximum time to wait for lock (0 = use default)
 * @param result Output tuning result
 * @return 0 on success, -1 on error
 */
int event_based_tune(int card_id, tune_p_t *tune_params, int timeout_ms, tuning_result_t *result);

/**
 * @brief Event-based signal lock detection
 * @param card_id The DVB card ID
 * @param frequency The frequency to check
 * @param timeout_ms Maximum time to wait for lock
 * @param result Output tuning result
 * @return 0 on success, -1 on error
 */
int event_based_check_lock(int card_id, double frequency, int timeout_ms, tuning_result_t *result);

/**
 * @brief Start continuous signal monitoring
 * @param monitor The frontend monitor
 * @param callback Function to call on events (NULL for no callback)
 * @param user_data User data passed to callback
 * @return 0 on success, -1 on error
 */
int start_fe_monitoring(multi_fe_event_monitor_t *monitor, 
                       void (*callback)(const fe_event_t *event, void *user_data),
                       void *user_data);

/**
 * @brief Stop continuous signal monitoring
 * @param monitor The frontend monitor
 */
void stop_fe_monitoring(multi_fe_event_monitor_t *monitor);

/**
 * @brief Get current frontend status without blocking
 * @param monitor The frontend monitor
 * @param event Output event structure
 * @return 0 on success, -1 on error
 */
int get_fe_status_now(multi_fe_event_monitor_t *monitor, fe_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* _DVB_EVENTS_H */
