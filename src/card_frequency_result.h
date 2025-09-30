/**
 * @file card_frequency_result.h
 * @brief Card frequency result structure definition
 * 
 * This module defines the structure used to store card frequency test results.
 */

#ifndef _CARD_FREQUENCY_RESULT_H
#define _CARD_FREQUENCY_RESULT_H

#include <time.h>

// Card frequency result structure
typedef struct {
    int card_id;                    // Card ID
    double frequency;               // Frequency in Hz
    int channel_count;              // Number of channels found
    int signal_strength;            // Signal strength
    int snr;                        // Signal-to-noise ratio
    int ber;                        // Bit Error Rate
    int status;                     // 0=failed, 1=success, 2=timeout
    int fe_status_flags;            // Last FE_STATUS flags seen (FE_HAS_SIGNAL, etc.)
    char channel_names[128][64];     // Channel names
    int service_ids[128];            // Service IDs
    time_t test_time;               // Test timestamp
    
    // Comprehensive timing measurements (all in milliseconds)
    int tune_start_time_ms;         // Time when tuning started
    int tune_complete_time_ms;      // Time when tuning completed (ioctl returned)
    int signal_lock_time_ms;        // Time to reach FE_HAS_SIGNAL
    int carrier_lock_time_ms;       // Time to reach FE_HAS_CARRIER
    int viterbi_lock_time_ms;       // Time to reach FE_HAS_VITERBI
    int sync_lock_time_ms;          // Time to reach FE_HAS_SYNC
    int full_lock_time_ms;          // Time to reach FE_HAS_LOCK (complete lock)
    int lock_achieved_time_ms;      // Absolute time when FE_HAS_LOCK was achieved
    int ts_stabilization_time_ms;   // Time for transport stream stabilization
    int channel_acquisition_time_ms; // Time to complete channel acquisition
    int total_test_time_ms;         // Total time for entire test
    
    // Legacy field for backward compatibility
    int lock_time_ms;               // Time to reach FE_LOCK in milliseconds (deprecated, use full_lock_time_ms)
    
    int error_code;                 // Specific error code (errno or custom)
    char error_message[128];        // Human-readable error message
} card_frequency_result_t;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif // _CARD_FREQUENCY_RESULT_H
