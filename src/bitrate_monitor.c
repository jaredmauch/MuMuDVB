/**
 * @file bitrate_monitor.c
 * @brief Bitrate monitoring and client synchronization implementation
 * 
 * This module provides bitrate monitoring for input streams and ensures
 * client threads don't fall too far behind the stream bitrate.
 */

#include "bitrate_monitor.h"
#include "event_timing.h"
#include "log.h"
#include "errors.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <math.h>

static char *log_module = "Bitrate-Monitor: ";

/** @brief Default bitrate monitor configuration */
const bitrate_monitor_config_t default_bitrate_config = {
    .window_size_ms = 5000,              // 5 second window
    .max_lag_threshold_ms = 2000,        // 2 second max lag
    .sync_check_interval_ms = 100,       // Check every 100ms
    .queue_size_threshold = 100,         // 100 packets threshold
    .bitrate_smoothing_factor = 80,      // 80% smoothing
    .adaptive_buffering = 1,             // Enable adaptive buffering
    .max_drop_rate_percent = 10          // Max 10% drop rate
};

/**
 * @brief Background monitoring thread function
 */
static void *monitor_thread_function(void *arg)
{
    bitrate_monitor_t *monitor = (bitrate_monitor_t *)arg;
    struct timespec current_time;
    int i, j;
    
    log_message(log_module, MSG_INFO, "Bitrate monitoring thread started");
    
    while (monitor->active) {
        current_time = get_current_time();
        
        pthread_mutex_lock(&monitor->monitor_mutex);
        
        // Check all clients for synchronization
        for (i = 0; i < monitor->num_clients; i++) {
            client_sync_status_t *client = &monitor->clients[i];
            
            // Skip inactive clients
            if (client->socket_fd < 0) {
                continue;
            }
            
            // Calculate time since last sync check
            long time_since_check = get_time_diff_ms(&client->last_sync_check, &current_time);
            
            if (time_since_check >= monitor->config.sync_check_interval_ms) {
                // Update sync check time
                client->last_sync_check = current_time;
                
                // Find associated stream
                stream_bitrate_info_t *stream = NULL;
                for (j = 0; j < monitor->num_streams; j++) {
                    if (monitor->streams[j].card_id == client->client_id) {
                        stream = &monitor->streams[j];
                        break;
                    }
                }
                
                if (stream) {
                    // Calculate client lag behind stream
                    long time_since_last_send = get_time_diff_ms(&client->last_send_time, &current_time);
                    client->lag_behind_stream_ms = time_since_last_send;
                    
                    // Update target bitrate from stream
                    client->target_bitrate = stream->current_bitrate;
                    
                    // Calculate send efficiency
                    if (client->target_bitrate > 0) {
                        client->send_efficiency = (client->current_bitrate / client->target_bitrate);
                        if (client->send_efficiency > 1.0) {
                            client->send_efficiency = 1.0;
                        }
                    } else {
                        client->send_efficiency = 1.0;
                    }
                    
                    // Determine if client needs throttling
                    if (client->lag_behind_stream_ms > monitor->config.max_lag_threshold_ms ||
                        client->queue_packets > monitor->config.queue_size_threshold ||
                        client->consecutive_errors > 3) {
                        client->throttled = 1;
                    } else {
                        client->throttled = 0;
                    }
                }
            }
        }
        
        // Update global check time
        monitor->last_global_check = current_time;
        
        pthread_mutex_unlock(&monitor->monitor_mutex);
        
        // Poll with timeout instead of usleep
        struct pollfd pfd = {0, 0, 0}; // No file descriptor, just timeout
        int poll_result = poll(&pfd, 1, monitor->config.sync_check_interval_ms);
        if (poll_result < 0 && errno != EINTR) {
            log_message(log_module, MSG_ERROR, "Bitrate monitor poll error: %s", strerror(errno));
        }
    }
    
    log_message(log_module, MSG_INFO, "Bitrate monitoring thread stopped");
    return NULL;
}

/**
 * @brief Create a bitrate monitor instance
 */
bitrate_monitor_t *create_bitrate_monitor(int max_streams, int max_clients, 
                                        const bitrate_monitor_config_t *config)
{
    bitrate_monitor_t *monitor;
    
    if (max_streams <= 0 || max_clients <= 0) {
        log_message(log_module, MSG_ERROR, "Invalid max_streams or max_clients");
        return NULL;
    }
    
    monitor = calloc(1, sizeof(bitrate_monitor_t));
    if (!monitor) {
        log_message(log_module, MSG_ERROR, "Cannot allocate bitrate monitor");
        return NULL;
    }
    
    // Allocate streams array
    monitor->streams = calloc(max_streams, sizeof(stream_bitrate_info_t));
    if (!monitor->streams) {
        log_message(log_module, MSG_ERROR, "Cannot allocate streams array");
        free(monitor);
        return NULL;
    }
    
    // Allocate clients array
    monitor->clients = calloc(max_clients, sizeof(client_sync_status_t));
    if (!monitor->clients) {
        log_message(log_module, MSG_ERROR, "Cannot allocate clients array");
        free(monitor->streams);
        free(monitor);
        return NULL;
    }
    
    monitor->max_streams = max_streams;
    monitor->max_clients = max_clients;
    monitor->num_streams = 0;
    monitor->num_clients = 0;
    monitor->active = 0;
    
    // Set configuration
    if (config) {
        monitor->config = *config;
    } else {
        monitor->config = default_bitrate_config;
    }
    
    // Initialize mutex
    if (pthread_mutex_init(&monitor->monitor_mutex, NULL) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot initialize monitor mutex");
        free(monitor->streams);
        free(monitor->clients);
        free(monitor);
        return NULL;
    }
    
    // Initialize timespec
    monitor->last_global_check = get_current_time();
    
    log_message(log_module, MSG_INFO, "Bitrate monitor created (max_streams=%d, max_clients=%d)", 
                max_streams, max_clients);
    
    return monitor;
}

/**
 * @brief Destroy a bitrate monitor instance
 */
void destroy_bitrate_monitor(bitrate_monitor_t *monitor)
{
    if (!monitor) {
        return;
    }
    
    // Stop monitoring
    stop_bitrate_monitoring(monitor);
    
    // Clean up
    if (monitor->streams) {
        free(monitor->streams);
    }
    if (monitor->clients) {
        free(monitor->clients);
    }
    
    pthread_mutex_destroy(&monitor->monitor_mutex);
    free(monitor);
    
    log_message(log_module, MSG_INFO, "Bitrate monitor destroyed");
}

/**
 * @brief Start bitrate monitoring
 */
int start_bitrate_monitoring(bitrate_monitor_t *monitor)
{
    if (!monitor) {
        return -1;
    }
    
    if (monitor->active) {
        log_message(log_module, MSG_WARN, "Bitrate monitoring already active");
        return 0;
    }
    
    monitor->active = 1;
    
    // Start monitoring thread
    if (pthread_create(&monitor->monitor_thread, NULL, monitor_thread_function, monitor) != 0) {
        log_message(log_module, MSG_ERROR, "Cannot create monitoring thread");
        monitor->active = 0;
        return -1;
    }
    
    log_message(log_module, MSG_INFO, "Bitrate monitoring started");
    return 0;
}

/**
 * @brief Stop bitrate monitoring
 */
void stop_bitrate_monitoring(bitrate_monitor_t *monitor)
{
    if (!monitor || !monitor->active) {
        return;
    }
    
    monitor->active = 0;
    
    // Wait for monitoring thread to finish
    if (monitor->monitor_thread) {
        pthread_join(monitor->monitor_thread, NULL);
        monitor->monitor_thread = 0;
    }
    
    log_message(log_module, MSG_INFO, "Bitrate monitoring stopped");
}

/**
 * @brief Register a stream for bitrate monitoring
 */
int register_stream_for_monitoring(bitrate_monitor_t *monitor, int card_id, double frequency)
{
    if (!monitor || monitor->num_streams >= monitor->max_streams) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    stream_bitrate_info_t *stream = &monitor->streams[monitor->num_streams];
    stream->card_id = card_id;
    stream->frequency = frequency;
    stream->current_bitrate = 0.0;
    stream->average_bitrate = 0.0;
    stream->peak_bitrate = 0.0;
    stream->packets_per_second = 0;
    stream->bytes_per_second = 0;
    stream->last_update = get_current_time();
    stream->is_active = 1;
    stream->quality_score = 100;
    
    int stream_id = monitor->num_streams;
    monitor->num_streams++;
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    log_message(log_module, MSG_INFO, "Registered stream %d (card=%d, freq=%.0f)", 
                stream_id, card_id, frequency);
    
    return stream_id;
}

/**
 * @brief Unregister a stream from monitoring
 */
int unregister_stream_from_monitoring(bitrate_monitor_t *monitor, int stream_id)
{
    if (!monitor || stream_id < 0 || stream_id >= monitor->num_streams) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    // Mark stream as inactive
    monitor->streams[stream_id].is_active = 0;
    
    // Remove from active streams (move last stream to this position)
    if (stream_id < monitor->num_streams - 1) {
        monitor->streams[stream_id] = monitor->streams[monitor->num_streams - 1];
    }
    monitor->num_streams--;
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    log_message(log_module, MSG_INFO, "Unregistered stream %d", stream_id);
    return 0;
}

/**
 * @brief Register a client for synchronization monitoring
 */
int register_client_for_sync(bitrate_monitor_t *monitor, int client_id, int socket_fd, const char *client_ip, int stream_id)
{
    if (!monitor || monitor->num_clients >= monitor->max_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    client_sync_status_t *client = &monitor->clients[monitor->num_clients];
    client->client_id = client_id;
    client->socket_fd = socket_fd;
    if (client_ip) {
        strncpy(client->client_ip, client_ip, sizeof(client->client_ip) - 1);
        client->client_ip[sizeof(client->client_ip) - 1] = '\0';
    } else {
        client->client_ip[0] = '\0';
    }
    client->current_bitrate = 0.0;
    client->target_bitrate = 0.0;
    client->lag_behind_stream_ms = 0;
    client->queue_packets = 0;
    client->queue_bytes = 0;
    client->consecutive_errors = 0;
    client->throttled = 0;
    client->dropped_packets = 0;
    client->last_send_time = get_current_time();
    client->last_sync_check = get_current_time();
    client->send_efficiency = 1.0;
    
    int client_sync_id = monitor->num_clients;
    monitor->num_clients++;
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    log_message(log_module, MSG_INFO, "Registered client %d (socket=%d, stream=%d, ip=%s)", 
                client_sync_id, socket_fd, stream_id, client_ip ? client_ip : "unknown");
    
    return client_sync_id;
}

/**
 * @brief Unregister a client from synchronization monitoring
 */
int unregister_client_from_sync(bitrate_monitor_t *monitor, int client_sync_id)
{
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    // Store IP address before marking as inactive
    char client_ip[IPV6_CHAR_LEN];
    strncpy(client_ip, monitor->clients[client_sync_id].client_ip, sizeof(client_ip) - 1);
    client_ip[sizeof(client_ip) - 1] = '\0';
    
    // Mark client as inactive
    monitor->clients[client_sync_id].socket_fd = -1;
    
    // Remove from active clients (move last client to this position)
    if (client_sync_id < monitor->num_clients - 1) {
        monitor->clients[client_sync_id] = monitor->clients[monitor->num_clients - 1];
    }
    monitor->num_clients--;
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    log_message(log_module, MSG_INFO, "Unregistered client %d (ip=%s)", client_sync_id, client_ip[0] ? client_ip : "unknown");
    return 0;
}

/**
 * @brief Update stream bitrate information
 */
int update_stream_bitrate(bitrate_monitor_t *monitor, int stream_id, 
                         long bytes_sent, long packets_sent)
{
    if (!monitor || stream_id < 0 || stream_id >= monitor->num_streams) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    stream_bitrate_info_t *stream = &monitor->streams[stream_id];
    struct timespec current_time = get_current_time();
    
    // Calculate time difference
    long time_diff_ms = get_time_diff_ms(&stream->last_update, &current_time);
    
    if (time_diff_ms > 0) {
        // Calculate current bitrate
        double current_bitrate = (bytes_sent * 8.0 * 1000.0) / time_diff_ms; // bits per second
        
        // Apply smoothing
        if (stream->current_bitrate > 0) {
            double smoothing_factor = monitor->config.bitrate_smoothing_factor / 100.0;
            stream->current_bitrate = (smoothing_factor * stream->current_bitrate) + 
                                    ((1.0 - smoothing_factor) * current_bitrate);
        } else {
            stream->current_bitrate = current_bitrate;
        }
        
        // Update average bitrate
        if (stream->average_bitrate > 0) {
            stream->average_bitrate = (stream->average_bitrate + stream->current_bitrate) / 2.0;
        } else {
            stream->average_bitrate = stream->current_bitrate;
        }
        
        // Update peak bitrate
        if (stream->current_bitrate > stream->peak_bitrate) {
            stream->peak_bitrate = stream->current_bitrate;
        }
        
        // Update rates
        stream->packets_per_second = (packets_sent * 1000) / time_diff_ms;
        stream->bytes_per_second = (bytes_sent * 1000) / time_diff_ms;
        
        // Update quality score based on bitrate stability
        double bitrate_variance = fabs(stream->current_bitrate - stream->average_bitrate) / stream->average_bitrate;
        stream->quality_score = (int)(100.0 * (1.0 - bitrate_variance));
        if (stream->quality_score < 0) stream->quality_score = 0;
        if (stream->quality_score > 100) stream->quality_score = 100;
    }
    
    stream->last_update = current_time;
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return 0;
}

/**
 * @brief Update client send statistics
 */
int update_client_send_stats(bitrate_monitor_t *monitor, int client_sync_id,
                            long bytes_sent, long packets_sent, int send_errors)
{
    (void)packets_sent; // Suppress unused parameter warning - may be used in future
    
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    client_sync_status_t *client = &monitor->clients[client_sync_id];
    struct timespec current_time = get_current_time();
    
    // Calculate time difference
    long time_diff_ms = get_time_diff_ms(&client->last_send_time, &current_time);
    
    if (time_diff_ms > 0) {
        // Calculate current bitrate
        client->current_bitrate = (bytes_sent * 8.0 * 1000.0) / time_diff_ms; // bits per second
        
        // Update error count
        if (send_errors > 0) {
            client->consecutive_errors += send_errors;
        } else {
            client->consecutive_errors = 0;
        }
        
        // Update send time
        client->last_send_time = current_time;
    }
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return 0;
}

/**
 * @brief Check if client needs throttling
 */
int should_throttle_client(bitrate_monitor_t *monitor, int client_sync_id)
{
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    client_sync_status_t *client = &monitor->clients[client_sync_id];
    int should_throttle = client->throttled;
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return should_throttle;
}

/**
 * @brief Get client synchronization status
 */
int get_client_sync_status(bitrate_monitor_t *monitor, int client_sync_id, 
                          client_sync_status_t *status)
{
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients || !status) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    *status = monitor->clients[client_sync_id];
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return 0;
}

/**
 * @brief Get stream bitrate information
 */
int get_stream_bitrate_info(bitrate_monitor_t *monitor, int stream_id, 
                           stream_bitrate_info_t *info)
{
    if (!monitor || stream_id < 0 || stream_id >= monitor->num_streams || !info) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    *info = monitor->streams[stream_id];
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return 0;
}

/**
 * @brief Calculate optimal send rate for client
 */
int calculate_optimal_send_rate(bitrate_monitor_t *monitor, int client_sync_id)
{
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    client_sync_status_t *client = &monitor->clients[client_sync_id];
    int optimal_rate = 0;
    
    if (client->target_bitrate > 0) {
        // Calculate optimal rate based on target bitrate and efficiency
        double efficiency_factor = client->send_efficiency;
        if (efficiency_factor < 0.5) {
            efficiency_factor = 0.5; // Minimum 50% efficiency
        }
        
        // Calculate packets per second (assuming 188 bytes per TS packet)
        optimal_rate = (int)((client->target_bitrate * efficiency_factor) / (188.0 * 8.0));
        
        // Apply throttling if needed
        if (client->throttled) {
            optimal_rate = optimal_rate / 2; // Reduce by 50% when throttled
        }
    }
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return optimal_rate;
}

/**
 * @brief Check if client is falling behind stream
 */
int is_client_falling_behind(bitrate_monitor_t *monitor, int client_sync_id)
{
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    client_sync_status_t *client = &monitor->clients[client_sync_id];
    int falling_behind = (client->lag_behind_stream_ms > monitor->config.max_lag_threshold_ms) ||
                        (client->queue_packets > monitor->config.queue_size_threshold) ||
                        (client->consecutive_errors > 3);
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return falling_behind;
}

/**
 * @brief Get recommended action for client
 */
int get_client_recommended_action(bitrate_monitor_t *monitor, int client_sync_id)
{
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    client_sync_status_t *client = &monitor->clients[client_sync_id];
    int action = 0; // Normal
    
    if (client->consecutive_errors > 5) {
        action = 2; // Drop packets
    } else if (client->queue_packets > monitor->config.queue_size_threshold * 2) {
        action = 1; // Throttle
    } else if (client->send_efficiency < 0.7 && monitor->config.adaptive_buffering) {
        action = 3; // Increase buffer
    } else if (client->lag_behind_stream_ms > monitor->config.max_lag_threshold_ms) {
        action = 1; // Throttle
    }
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return action;
}

/**
 * @brief Apply adaptive buffering to client
 */
int apply_adaptive_buffering(bitrate_monitor_t *monitor, int client_sync_id, int queue_size)
{
    if (!monitor || client_sync_id < 0 || client_sync_id >= monitor->num_clients) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    client_sync_status_t *client = &monitor->clients[client_sync_id];
    int recommended_size = queue_size;
    
    if (monitor->config.adaptive_buffering) {
        if (client->send_efficiency < 0.5) {
            // Low efficiency, increase buffer
            recommended_size = queue_size * 2;
        } else if (client->send_efficiency > 0.9 && client->lag_behind_stream_ms < 500) {
            // High efficiency and low lag, decrease buffer
            recommended_size = queue_size / 2;
            if (recommended_size < 10) {
                recommended_size = 10; // Minimum buffer size
            }
        }
    }
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return recommended_size;
}

/**
 * @brief Get global bitrate monitor statistics
 */
int get_global_monitor_stats(bitrate_monitor_t *monitor, int *total_streams, int *total_clients,
                            double *avg_bitrate, int *clients_behind)
{
    if (!monitor || !total_streams || !total_clients || !avg_bitrate || !clients_behind) {
        return -1;
    }
    
    pthread_mutex_lock(&monitor->monitor_mutex);
    
    *total_streams = monitor->num_streams;
    *total_clients = monitor->num_clients;
    
    double total_bitrate = 0.0;
    int active_streams = 0;
    *clients_behind = 0;
    
    // Calculate average bitrate
    for (int i = 0; i < monitor->num_streams; i++) {
        if (monitor->streams[i].is_active) {
            total_bitrate += monitor->streams[i].current_bitrate;
            active_streams++;
        }
    }
    
    if (active_streams > 0) {
        *avg_bitrate = total_bitrate / active_streams;
    } else {
        *avg_bitrate = 0.0;
    }
    
    // Count clients falling behind
    for (int i = 0; i < monitor->num_clients; i++) {
        if (monitor->clients[i].socket_fd >= 0) {
            if (monitor->clients[i].lag_behind_stream_ms > monitor->config.max_lag_threshold_ms ||
                monitor->clients[i].queue_packets > monitor->config.queue_size_threshold) {
                (*clients_behind)++;
            }
        }
    }
    
    pthread_mutex_unlock(&monitor->monitor_mutex);
    
    return 0;
}
