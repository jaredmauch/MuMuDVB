/**
 * @file main_thread_poll.h
 * @brief Main thread polling functions to replace usleep calls with poll()
 * 
 * This module provides polling functions for the main thread to monitor
 * active file descriptors instead of using blocking usleep calls.
 */

#ifndef _MAIN_THREAD_POLL_H
#define _MAIN_THREAD_POLL_H

#include "mumudvb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Poll available file descriptors with timeout
 * @param fds DVB file descriptors structure
 * @param unic_p Unicast parameters with network sockets
 * @param timeout_ms Timeout in milliseconds
 * @return Number of file descriptors ready, -1 on error, 0 on timeout
 */
int main_thread_poll_with_timeout(fds_t *fds, unicast_parameters_t *unic_p, int timeout_ms);

/**
 * @brief Poll only available file descriptors (non-blocking check)
 * @param fds DVB file descriptors structure
 * @param unic_p Unicast parameters with network sockets
 * @param timeout_ms Timeout in milliseconds
 * @return Number of file descriptors ready, -1 on error, 0 on timeout
 */
int main_thread_poll_available_fds(fds_t *fds, unicast_parameters_t *unic_p, int timeout_ms);

/**
 * @brief Main thread sleep replacement using poll()
 * @param fds DVB file descriptors structure
 * @param unic_p Unicast parameters with network sockets
 * @param sleep_ms Sleep duration in milliseconds
 * @return 0 on success, -1 on error
 */
int main_thread_sleep_with_poll(fds_t *fds, unicast_parameters_t *unic_p, int sleep_ms);

#ifdef __cplusplus
}
#endif

#endif /* _MAIN_THREAD_POLL_H */
