/**
 * @file dvb_events.c
 * @brief Event-based DVB frontend monitoring and tuning implementation
 * 
 * This module provides event-driven alternatives to polling loops
 * for DVB frontend status monitoring, signal lock detection, and tuning.
 */

#include "dvb_events.h"
#include "log.h"
#include "errors.h"
#include "tune.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

static char *log_module = "DVB-Events: ";

/** @brief Default timeout for tuning operations (6 seconds) */
#define DEFAULT_TUNE_TIMEOUT_MS 6000

/** @brief Default timeout for lock detection (2 seconds) */
#define DEFAULT_LOCK_TIMEOUT_MS 2000

/** @brief Polling interval for epoll (100ms) */
#define EPOLL_POLL_INTERVAL_MS 100

/** @brief Maximum number of events per epoll_wait call */
#define MAX_EPOLL_EVENTS 10

/**
 * @brief Create a multi-frontend event monitor
 */
int create_multi_fe_event_monitor(int max_frontends, multi_fe_event_monitor_t *monitor)
{
    if (!monitor || max_frontends <= 0) {
        return -1;
    }
    
    memset(monitor, 0, sizeof(multi_fe_event_monitor_t));
    
    // Allocate frontend data array
    monitor->frontends = calloc(max_frontends, sizeof(fe_monitor_data_t));
    if (!monitor->frontends) {
        log_message(log_module, MSG_ERROR, "Cannot allocate frontend data array");
        return -1;
    }
    
    monitor->max_frontends = max_frontends;
    monitor->num_frontends = 0;
    
    // Create epoll instance
    monitor->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (monitor->epoll_fd < 0) {
        log_message(log_module, MSG_ERROR, "Cannot create epoll (errno: %d)", errno);
        free(monitor->frontends);
        return -1;
    }
    
    // Initialize mutex
    if (pthread_mutex_init(&monitor->event_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot initialize mutex");
        close(monitor->epoll_fd);
        free(monitor->frontends);
        return -1;
    }
    
    monitor->monitoring = 0;
    
    log_message(log_module, MSG_DEBUG, "Created multi-frontend event monitor (max: %d)", max_frontends);
    return 0;
}

/**
 * @brief Destroy a multi-frontend event monitor
 */
void destroy_multi_fe_event_monitor(multi_fe_event_monitor_t *monitor)
{
    if (!monitor) {
        return;
    }
    
    // Stop monitoring
    stop_multi_fe_monitoring(monitor);
    
    // Close all frontend file descriptors
    for (int i = 0; i < monitor->num_frontends; i++) {
        if (monitor->frontends[i].fd_frontend >= 0) {
            close(monitor->frontends[i].fd_frontend);
            monitor->frontends[i].fd_frontend = -1;
        }
    }
    
    // Clean up resources
    if (monitor->epoll_fd >= 0) {
        close(monitor->epoll_fd);
        monitor->epoll_fd = -1;
    }
    
    if (monitor->frontends) {
        free(monitor->frontends);
        monitor->frontends = NULL;
    }
    
    // Destroy mutex
    pthread_mutex_destroy(&monitor->event_mutex);
    
    log_message(log_module, MSG_DEBUG, "Destroyed multi-frontend event monitor");
}

/**
 * @brief Add a frontend to the multi-frontend monitor
 */
int add_frontend_to_monitor(multi_fe_event_monitor_t *monitor, int card_id, 
                           int has_clients, int needs_lock_monitoring)
{
    if (!monitor || card_id < 0) {
        return -1;
    }
    
    // Check if we already have this card
    for (int i = 0; i < monitor->num_frontends; i++) {
        if (monitor->frontends[i].card_id == card_id) {
            log_message(log_module, MSG_DEBUG, "card-%d already in monitor", card_id);
            return 0; // Already exists
        }
    }
    
    // Check if we have space
    if (monitor->num_frontends >= monitor->max_frontends) {
        log_message(log_module, MSG_ERROR, "Cannot add card-%d: monitor at capacity (%d/%d)", 
                    card_id, monitor->num_frontends, monitor->max_frontends);
        return -1;
    }
    
    // Open frontend device
    char frontend_path[256];
    snprintf(frontend_path, sizeof(frontend_path), "/dev/dvb/adapter%d/frontend0", card_id);
    
    int fd_frontend = open(frontend_path, O_RDWR | O_NONBLOCK);
    if (fd_frontend < 0) {
        log_message(log_module, MSG_ERROR, "Cannot open frontend %s for card-%d (errno: %d)", 
                    frontend_path, card_id, errno);
        return -1;
    }
    
    // Add to frontend array
    int idx = monitor->num_frontends++;
    monitor->frontends[idx].card_id = card_id;
    monitor->frontends[idx].fd_frontend = fd_frontend;
    monitor->frontends[idx].has_clients = has_clients;
    monitor->frontends[idx].needs_lock_monitoring = needs_lock_monitoring;
    monitor->frontends[idx].last_activity = time(NULL);
    memset(&monitor->frontends[idx].last_event, 0, sizeof(fe_event_t));
    
    // Add to epoll
    struct epoll_event epoll_ev;
    epoll_ev.events = EPOLLIN | EPOLLPRI | EPOLLERR;
    epoll_ev.data.u32 = card_id; // Store card_id in data
    
    if (epoll_ctl(monitor->epoll_fd, EPOLL_CTL_ADD, fd_frontend, &epoll_ev) < 0) {
        log_message(log_module, MSG_ERROR, "Cannot add frontend to epoll for card-%d (errno: %d)", 
                    card_id, errno);
        close(fd_frontend);
        monitor->num_frontends--; // Rollback
        return -1;
    }
    
    log_message(log_module, MSG_DEBUG, "Added card-%d to monitor (clients: %d, lock_monitoring: %d)", 
                card_id, has_clients, needs_lock_monitoring);
    return 0;
}

/**
 * @brief Remove a frontend from the multi-frontend monitor
 */
int remove_frontend_from_monitor(multi_fe_event_monitor_t *monitor, int card_id)
{
    if (!monitor || card_id < 0) {
        return -1;
    }
    
    // Find the frontend
    int idx = -1;
    for (int i = 0; i < monitor->num_frontends; i++) {
        if (monitor->frontends[i].card_id == card_id) {
            idx = i;
            break;
        }
    }
    
    if (idx == -1) {
        log_message(log_module, MSG_DEBUG, "card-%d not found in monitor", card_id);
        return -1;
    }
    
    // Remove from epoll
    if (epoll_ctl(monitor->epoll_fd, EPOLL_CTL_DEL, monitor->frontends[idx].fd_frontend, NULL) < 0) {
        log_message(log_module, MSG_WARN, "Cannot remove frontend from epoll for card-%d (errno: %d)", 
                    card_id, errno);
    }
    
    // Close frontend
    close(monitor->frontends[idx].fd_frontend);
    
    // Remove from array (shift remaining elements)
    for (int i = idx; i < monitor->num_frontends - 1; i++) {
        monitor->frontends[i] = monitor->frontends[i + 1];
    }
    monitor->num_frontends--;
    
    log_message(log_module, MSG_DEBUG, "Removed card-%d from monitor", card_id);
    return 0;
}

/**
 * @brief Update frontend client status
 */
int update_frontend_client_status(multi_fe_event_monitor_t *monitor, int card_id, int has_clients)
{
    if (!monitor || card_id < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->event_mutex);
    
    // Find the frontend
    for (int i = 0; i < monitor->num_frontends; i++) {
        if (monitor->frontends[i].card_id == card_id) {
            monitor->frontends[i].has_clients = has_clients;
            monitor->frontends[i].last_activity = time(NULL);
            log_message(log_module, MSG_DEBUG, "Updated card-%d client status: %d", card_id, has_clients);
            pthread_mutex_unlock(&monitor->event_mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&monitor->event_mutex);
    log_message(log_module, MSG_DEBUG, "card-%d not found in monitor for client status update", card_id);
    return -1;
}

/**
 * @brief Background monitoring thread function
 */
static void *multi_fe_monitor_thread(void *arg)
{
    multi_fe_event_monitor_t *monitor = (multi_fe_event_monitor_t *)arg;
    struct epoll_event events[MAX_EPOLL_EVENTS];
    
    log_message(log_module, MSG_DEBUG, "Multi-frontend monitor thread started");
    
    while (monitor->monitoring) {
        // Check for interrupt
        if (get_interrupted()) {
            log_message(log_module, MSG_DEBUG, "Monitor thread interrupted");
            break;
        }
        
        // Wait for events
        int nfds = epoll_wait(monitor->epoll_fd, events, MAX_EPOLL_EVENTS, EPOLL_POLL_INTERVAL_MS);
        
        if (nfds < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, retry
            }
            log_message(log_module, MSG_ERROR, "Epoll wait failed (errno: %d)", errno);
            break;
        }
        
        if (nfds == 0) {
            continue; // Timeout, check again
        }
        
        // Process events
        for (int i = 0; i < nfds; i++) {
            int card_id = events[i].data.u32;
            
            // Find the frontend data
            fe_monitor_data_t *fe_data = NULL;
            for (int j = 0; j < monitor->num_frontends; j++) {
                if (monitor->frontends[j].card_id == card_id) {
                    fe_data = &monitor->frontends[j];
                    break;
                }
            }
            
            if (!fe_data) {
                continue; // Frontend not found
            }
            
            // Read frontend status
            fe_status_t fe_status;
            int res = ioctl(fe_data->fd_frontend, FE_READ_STATUS, &fe_status);
            
            if (res < 0) {
                log_message(log_module, MSG_ERROR, "Cannot read frontend status for card-%d (errno: %d)", 
                            card_id, errno);
                continue;
            }
            
            // Update event data
            pthread_mutex_lock(&monitor->event_mutex);
            fe_data->last_event.fd_frontend = fe_data->fd_frontend;
            fe_data->last_event.status = fe_status;
            fe_data->last_event.error_code = 0;
            
            // Read signal quality
            ioctl(fe_data->fd_frontend, FE_READ_SIGNAL_STRENGTH, &fe_data->last_event.signal_strength);
            ioctl(fe_data->fd_frontend, FE_READ_SNR, &fe_data->last_event.snr);
            ioctl(fe_data->fd_frontend, FE_READ_BER, &fe_data->last_event.ber);
            
            // Determine event type
            fe_data->last_event.event_type = 0;
            if (fe_status & FE_HAS_SIGNAL) {
                fe_data->last_event.event_type |= FE_EVENT_SIGNAL;
            }
            if (fe_status & FE_HAS_CARRIER) {
                fe_data->last_event.event_type |= FE_EVENT_CARRIER;
            }
            if (fe_status & FE_HAS_LOCK) {
                fe_data->last_event.event_type |= FE_EVENT_LOCK;
            }
            if (events[i].events & EPOLLERR) {
                fe_data->last_event.event_type |= FE_EVENT_ERROR;
            }
            
            fe_data->last_activity = time(NULL);
            
            // Call callback if set
            if (monitor->event_callback) {
                monitor->event_callback(card_id, &fe_data->last_event, monitor->callback_user_data);
            }
            
            pthread_mutex_unlock(&monitor->event_mutex);
            
            // Log significant events
            if (fe_data->has_clients && (fe_data->last_event.event_type & FE_EVENT_LOCK)) {
                log_message(log_module, MSG_INFO, "card-%d with clients achieved lock (strength: %d, SNR: %d)", 
                            card_id, fe_data->last_event.signal_strength, fe_data->last_event.snr);
            }
        }
    }
    
    log_message(log_module, MSG_DEBUG, "Multi-frontend monitor thread stopped");
    return NULL;
}

/**
 * @brief Start continuous multi-frontend monitoring
 */
int start_multi_fe_monitoring(multi_fe_event_monitor_t *monitor, 
                             void (*callback)(int card_id, const fe_event_t *event, void *user_data),
                             void *user_data)
{
    if (!monitor) {
        return -1;
    }
    
    if (monitor->monitoring) {
        log_message(log_module, MSG_DEBUG, "Monitor already running");
        return 0;
    }
    
    monitor->event_callback = callback;
    monitor->callback_user_data = user_data;
    monitor->monitoring = 1;
    
    // Create monitoring thread
    if (pthread_create(&monitor->monitor_thread, NULL, multi_fe_monitor_thread, monitor) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot create monitor thread");
        monitor->monitoring = 0;
        return -1;
    }
    
    log_message(log_module, MSG_DEBUG, "Started multi-frontend monitoring");
    return 0;
}

/**
 * @brief Stop continuous multi-frontend monitoring
 */
void stop_multi_fe_monitoring(multi_fe_event_monitor_t *monitor)
{
    if (!monitor || !monitor->monitoring) {
        return;
    }
    
    monitor->monitoring = 0;
    
    // Wait for thread to finish
    if (monitor->monitor_thread) {
        pthread_join(monitor->monitor_thread, NULL);
        monitor->monitor_thread = 0;
    }
    
    log_message(log_module, MSG_DEBUG, "Stopped multi-frontend monitoring");
}

/**
 * @brief Get current status for a specific frontend
 */
int get_frontend_status(multi_fe_event_monitor_t *monitor, int card_id, fe_event_t *event)
{
    if (!monitor || !event || card_id < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->event_mutex);
    
    // Find the frontend
    for (int i = 0; i < monitor->num_frontends; i++) {
        if (monitor->frontends[i].card_id == card_id) {
            *event = monitor->frontends[i].last_event;
            pthread_mutex_unlock(&monitor->event_mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&monitor->event_mutex);
    return -1; // Frontend not found
}

/**
 * @brief Wait for a specific frontend event with timeout
 */
int wait_for_frontend_event(multi_fe_event_monitor_t *monitor, int card_id, 
                           fe_event_type_t event_types, int timeout_ms, fe_event_t *event)
{
    if (!monitor || !event) {
        return -2;
    }
    
    time_t start_time = time(NULL);
    int timeout_remaining = timeout_ms;
    
    while (1) {
        // Check for interrupt
        if (get_interrupted()) {
            log_message(log_module, MSG_DEBUG, "Event wait interrupted");
            return -2;
        }
        
        // Calculate remaining timeout
        if (timeout_ms > 0) {
            time_t current_time = time(NULL);
            timeout_remaining = timeout_ms - (int)((current_time - start_time) * 1000);
            if (timeout_remaining <= 0) {
                event->event_type = FE_EVENT_TIMEOUT;
                return -1; // Timeout
            }
        }
        
        // Get current status
        if (get_frontend_status(monitor, card_id, event) == 0) {
            // Check if this is an event we're waiting for
            if (event->event_type & event_types) {
                log_message(log_module, MSG_DEBUG, "card-%d received event 0x%02x (status: 0x%08x)", 
                            card_id, event->event_type, event->status);
                return 0; // Success
            }
        }
        
        // Sleep briefly before checking again
        if (event_sleep_interruptible(50000) < 0) { // 50ms
            return -1; // Interrupted
        }
    }
}

/**
 * @brief Event-based tuning with lock detection using multi-frontend monitor
 */
int event_based_tune(int card_id, tune_p_t *tune_params, int timeout_ms, tuning_result_t *result)
{
    if (!tune_params || !result) {
        return -1;
    }
    
    // Use default timeout if not specified
    if (timeout_ms <= 0) {
        timeout_ms = DEFAULT_TUNE_TIMEOUT_MS;
    }
    
    // Initialize result
    memset(result, 0, sizeof(tuning_result_t));
    result->timeout_ms = timeout_ms;
    
    // Create temporary multi-frontend monitor for this tuning operation
    multi_fe_event_monitor_t monitor;
    if (create_multi_fe_event_monitor(1, &monitor) < 0) {
        result->error_code = errno;
        return -1;
    }
    
    // Add this frontend to the monitor
    if (add_frontend_to_monitor(&monitor, card_id, 0, 1) < 0) {
        result->error_code = errno;
        destroy_multi_fe_event_monitor(&monitor);
        return -1;
    }
    
    // Find the frontend data
    fe_monitor_data_t *fe_data = NULL;
    for (int i = 0; i < monitor.num_frontends; i++) {
        if (monitor.frontends[i].card_id == card_id) {
            fe_data = &monitor.frontends[i];
            break;
        }
    }
    
    if (!fe_data) {
        result->error_code = -1;
        destroy_multi_fe_event_monitor(&monitor);
        return -1;
    }
    
    // Perform tuning
    int tune_result = tune_it(fe_data->fd_frontend, tune_params);
    if (tune_result != 0) {
        log_message(log_module, MSG_ERROR, "card-%d tuning failed (result: %d)", card_id, tune_result);
        result->error_code = tune_result;
        destroy_multi_fe_event_monitor(&monitor);
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "card-%d tuning initiated, waiting for lock...", card_id);
    
    // Wait for lock event
    fe_event_t event;
    int wait_result = wait_for_frontend_event(&monitor, card_id, FE_EVENT_LOCK, timeout_ms, &event);
    
    if (wait_result == 0) {
        // Success - we got a lock event
        result->success = 1;
        result->final_event = event.event_type;
        result->final_status = event.status;
        result->signal_strength = event.signal_strength;
        result->snr = event.snr;
        result->ber = event.ber;
        
        log_message(log_module, MSG_INFO, "card-%d achieved lock (strength: %d, SNR: %d)", 
                    card_id, event.signal_strength, event.snr);
    } else if (wait_result == -1) {
        // Timeout
        result->success = 0;
        result->final_event = FE_EVENT_TIMEOUT;
        log_message(log_module, MSG_INFO, "card-%d tuning timeout after %d ms", card_id, timeout_ms);
    } else {
        // Error
        result->success = 0;
        result->final_event = FE_EVENT_ERROR;
        result->error_code = event.error_code;
        log_message(log_module, MSG_ERROR, "card-%d tuning error (code: %d)", card_id, event.error_code);
    }
    
    destroy_multi_fe_event_monitor(&monitor);
    return (result->success) ? 0 : -1;
}

/**
 * @brief Event-based signal lock detection using multi-frontend monitor
 */
int event_based_check_lock(int card_id, double frequency, int timeout_ms, tuning_result_t *result)
{
    if (!result) {
        return -1;
    }
    
    // Use default timeout if not specified
    if (timeout_ms <= 0) {
        timeout_ms = DEFAULT_LOCK_TIMEOUT_MS;
    }
    
    // Initialize result
    memset(result, 0, sizeof(tuning_result_t));
    result->timeout_ms = timeout_ms;
    
    // Create temporary multi-frontend monitor for this check
    multi_fe_event_monitor_t monitor;
    if (create_multi_fe_event_monitor(1, &monitor) < 0) {
        result->error_code = errno;
        return -1;
    }
    
    // Add this frontend to the monitor
    if (add_frontend_to_monitor(&monitor, card_id, 0, 1) < 0) {
        result->error_code = errno;
        destroy_multi_fe_event_monitor(&monitor);
        return -1;
    }
    
    log_message(log_module, MSG_DEBUG, "card-%d checking for lock on frequency %.0f Hz", card_id, frequency);
    
    // Wait for any signal-related event
    fe_event_t event;
    int wait_result = wait_for_frontend_event(&monitor, card_id,
                                             FE_EVENT_SIGNAL | FE_EVENT_CARRIER | FE_EVENT_LOCK, 
                                             timeout_ms, &event);
    
    if (wait_result == 0) {
        // We got an event
        result->success = (event.event_type & FE_EVENT_LOCK) ? 1 : 0;
        result->final_event = event.event_type;
        result->final_status = event.status;
        result->signal_strength = event.signal_strength;
        result->snr = event.snr;
        result->ber = event.ber;
        
        log_message(log_module, MSG_DEBUG, "card-%d lock check result: %s (event: 0x%02x)", 
                    card_id, result->success ? "LOCKED" : "NO LOCK", event.event_type);
    } else if (wait_result == -1) {
        // Timeout
        result->success = 0;
        result->final_event = FE_EVENT_TIMEOUT;
        log_message(log_module, MSG_DEBUG, "card-%d lock check timeout after %d ms", card_id, timeout_ms);
    } else {
        // Error
        result->success = 0;
        result->final_event = FE_EVENT_ERROR;
        result->error_code = event.error_code;
        log_message(log_module, MSG_ERROR, "card-%d lock check error (code: %d)", card_id, event.error_code);
    }
    
    destroy_multi_fe_event_monitor(&monitor);
    return (result->success) ? 0 : -1;
}
