/**
 * @file bitrate_monitor.h
 * @brief Bitrate monitoring and client synchronization system
 * 
 * This module provides bitrate monitoring for input streams and ensures
 * client threads don't fall too far behind the stream bitrate.
 */

#ifndef _BITRATE_MONITOR_H
#define _BITRATE_MONITOR_H

#include "mumudvb.h"
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bitrate monitoring configuration */
typedef struct {
    int window_size_ms;           // Time window for bitrate calculation (ms)
    int max_lag_threshold_ms;     // Maximum allowed lag behind stream (ms)
    int sync_check_interval_ms;   // How often to check client sync (ms)
    int queue_size_threshold;     // Queue size threshold for throttling
    int bitrate_smoothing_factor; // Smoothing factor for bitrate calculation (0-100)
    int adaptive_buffering;       // Enable adaptive buffering (0/1)
    int max_drop_rate_percent;    // Maximum packet drop rate (0-100)
} bitrate_monitor_config_t;

/** @brief Client synchronization status */
typedef struct {
    int client_id;                    // Client identifier
    int socket_fd;                    // Client socket file descriptor
    char client_ip[IPV6_CHAR_LEN];    // Client IP address (IPv4 or IPv6)
    double current_bitrate;           // Current bitrate being sent to client
    double target_bitrate;            // Target bitrate (stream bitrate)
    long lag_behind_stream_ms;        // Lag behind stream in milliseconds
    int queue_packets;                // Number of packets in client queue
    int queue_bytes;                  // Number of bytes in client queue
    int consecutive_errors;           // Consecutive send errors
    int throttled;                    // Is client currently throttled
    int dropped_packets;              // Number of packets dropped for this client
    struct timespec last_send_time;   // Last successful send time
    struct timespec last_sync_check;  // Last sync check time
    double send_efficiency;           // Send efficiency (0.0-1.0)
} client_sync_status_t;

/** @brief Stream bitrate information */
typedef struct {
    double frequency;                 // Stream frequency
    int card_id;                     // Card ID
    double current_bitrate;           // Current bitrate (bits per second)
    double average_bitrate;           // Average bitrate over time window
    double peak_bitrate;              // Peak bitrate observed
    long packets_per_second;          // Packets per second
    long bytes_per_second;            // Bytes per second
    struct timespec last_update;      // Last bitrate update
    int is_active;                    // Is stream currently active
    int quality_score;                // Stream quality score (0-100)
} stream_bitrate_info_t;

/** @brief Bitrate monitor instance */
typedef struct {
    stream_bitrate_info_t *streams;   // Array of stream bitrate info
    client_sync_status_t *clients;    // Array of client sync status
    int max_streams;                  // Maximum number of streams
    int max_clients;                  // Maximum number of clients
    int num_streams;                  // Current number of streams
    int num_clients;                  // Current number of clients
    pthread_mutex_t monitor_mutex;    // Mutex for thread safety
    bitrate_monitor_config_t config;  // Configuration
    volatile int active;              // Is monitor active
    pthread_t monitor_thread;         // Background monitoring thread
    struct timespec last_global_check; // Last global sync check
} bitrate_monitor_t;

/** @brief Default bitrate monitor configuration */
extern const bitrate_monitor_config_t default_bitrate_config;

/**
 * @brief Create a bitrate monitor instance
 * @param max_streams Maximum number of streams to monitor
 * @param max_clients Maximum number of clients to monitor
 * @param config Configuration parameters (NULL for defaults)
 * @return Bitrate monitor instance or NULL on error
 */
bitrate_monitor_t *create_bitrate_monitor(int max_streams, int max_clients, 
                                        const bitrate_monitor_config_t *config);

/**
 * @brief Destroy a bitrate monitor instance
 * @param monitor Monitor instance to destroy
 */
void destroy_bitrate_monitor(bitrate_monitor_t *monitor);

/**
 * @brief Start bitrate monitoring
 * @param monitor Monitor instance
 * @return 0 on success, -1 on error
 */
int start_bitrate_monitoring(bitrate_monitor_t *monitor);

/**
 * @brief Stop bitrate monitoring
 * @param monitor Monitor instance
 */
void stop_bitrate_monitoring(bitrate_monitor_t *monitor);

/**
 * @brief Register a stream for bitrate monitoring
 * @param monitor Monitor instance
 * @param card_id Card ID
 * @param frequency Stream frequency
 * @return Stream ID on success, -1 on error
 */
int register_stream_for_monitoring(bitrate_monitor_t *monitor, int card_id, double frequency);

/**
 * @brief Unregister a stream from monitoring
 * @param monitor Monitor instance
 * @param stream_id Stream ID
 * @return 0 on success, -1 on error
 */
int unregister_stream_from_monitoring(bitrate_monitor_t *monitor, int stream_id);

/**
 * @brief Register a client for synchronization monitoring
 * @param monitor Monitor instance
 * @param client_id Client identifier
 * @param socket_fd Client socket file descriptor
 * @param client_ip Client IP address string
 * @param stream_id Associated stream ID
 * @return Client sync ID on success, -1 on error
 */
int register_client_for_sync(bitrate_monitor_t *monitor, int client_id, int socket_fd, const char *client_ip, int stream_id);

/**
 * @brief Unregister a client from synchronization monitoring
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @return 0 on success, -1 on error
 */
int unregister_client_from_sync(bitrate_monitor_t *monitor, int client_sync_id);

/**
 * @brief Update stream bitrate information
 * @param monitor Monitor instance
 * @param stream_id Stream ID
 * @param bytes_sent Number of bytes sent
 * @param packets_sent Number of packets sent
 * @return 0 on success, -1 on error
 */
int update_stream_bitrate(bitrate_monitor_t *monitor, int stream_id, 
                         long bytes_sent, long packets_sent);

/**
 * @brief Update client send statistics
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @param bytes_sent Number of bytes sent to client
 * @param packets_sent Number of packets sent to client
 * @param send_errors Number of send errors
 * @return 0 on success, -1 on error
 */
int update_client_send_stats(bitrate_monitor_t *monitor, int client_sync_id,
                            long bytes_sent, long packets_sent, int send_errors);

/**
 * @brief Check if client needs throttling
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @return 1 if client should be throttled, 0 if not, -1 on error
 */
int should_throttle_client(bitrate_monitor_t *monitor, int client_sync_id);

/**
 * @brief Get client synchronization status
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @param status Output status structure
 * @return 0 on success, -1 on error
 */
int get_client_sync_status(bitrate_monitor_t *monitor, int client_sync_id, 
                          client_sync_status_t *status);

/**
 * @brief Get stream bitrate information
 * @param monitor Monitor instance
 * @param stream_id Stream ID
 * @param info Output bitrate info structure
 * @return 0 on success, -1 on error
 */
int get_stream_bitrate_info(bitrate_monitor_t *monitor, int stream_id, 
                           stream_bitrate_info_t *info);

/**
 * @brief Calculate optimal send rate for client
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @return Optimal send rate in packets per second, -1 on error
 */
int calculate_optimal_send_rate(bitrate_monitor_t *monitor, int client_sync_id);

/**
 * @brief Check if client is falling behind stream
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @return 1 if falling behind, 0 if keeping up, -1 on error
 */
int is_client_falling_behind(bitrate_monitor_t *monitor, int client_sync_id);

/**
 * @brief Get recommended action for client
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @return Action code: 0=normal, 1=throttle, 2=drop_packets, 3=increase_buffer, -1=error
 */
int get_client_recommended_action(bitrate_monitor_t *monitor, int client_sync_id);

/**
 * @brief Apply adaptive buffering to client
 * @param monitor Monitor instance
 * @param client_sync_id Client sync ID
 * @param queue_size Current queue size
 * @return Recommended new queue size, -1 on error
 */
int apply_adaptive_buffering(bitrate_monitor_t *monitor, int client_sync_id, int queue_size);

/**
 * @brief Get global bitrate monitor statistics
 * @param monitor Monitor instance
 * @param total_streams Output total streams
 * @param total_clients Output total clients
 * @param avg_bitrate Output average bitrate across all streams
 * @param clients_behind Output number of clients falling behind
 * @return 0 on success, -1 on error
 */
int get_global_monitor_stats(bitrate_monitor_t *monitor, int *total_streams, int *total_clients,
                            double *avg_bitrate, int *clients_behind);

#ifdef __cplusplus
}
#endif

#endif /* _BITRATE_MONITOR_H */
