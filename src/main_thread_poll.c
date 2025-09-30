/**
 * @file main_thread_poll.c
 * @brief Main thread polling functions to replace usleep calls with poll()
 * 
 * This module provides polling functions for the main thread to monitor
 * active file descriptors instead of using blocking usleep calls.
 */

#include "main_thread_poll.h"
#include "log.h"
#include "mumudvb.h"
#include "unicast_http.h"
#include <sys/poll.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

static char *log_module = "Main-Poll: ";

// Global references for main thread polling (used by other modules)
fds_t *global_main_fds = NULL;
unicast_parameters_t *global_unicast_params = NULL;

/**
 * @brief Poll available file descriptors with timeout
 * @param fds DVB file descriptors structure
 * @param unic_p Unicast parameters with network sockets
 * @param timeout_ms Timeout in milliseconds
 * @return Number of file descriptors ready, -1 on error, 0 on timeout
 */
int main_thread_poll_with_timeout(fds_t *fds, unicast_parameters_t *unic_p, int timeout_ms)
{
    if (!fds || !unic_p) {
        return -1;
    }
    
    struct pollfd *all_pfds = NULL;
    int total_fds = 0;
    int poll_result;
    
    // Calculate total number of file descriptors
    int dvb_fds = (fds && fds->pfds) ? fds->pfdsnum : 0;
    int unicast_fds = (unic_p && unic_p->pfds) ? unic_p->pfdsnum : 0;
    total_fds = dvb_fds + unicast_fds;
    
    if (total_fds == 0) {
        // No file descriptors to poll, use simple timeout
        struct pollfd dummy = {0, 0, 0}; // No file descriptor, just timeout
        return poll(&dummy, 1, timeout_ms);
    }
    
    // Allocate combined pollfd array
    all_pfds = malloc((total_fds + 1) * sizeof(struct pollfd));
    if (!all_pfds) {
        log_message(log_module, MSG_ERROR, "Failed to allocate pollfd array");
        return -1;
    }
    
    int fd_index = 0;
    
    // Add DVB file descriptors
    if (fds && fds->pfds) {
        for (int i = 0; i < fds->pfdsnum; i++) {
            all_pfds[fd_index] = fds->pfds[i];
            fd_index++;
        }
    }
    
    // Add unicast file descriptors
    if (unic_p && unic_p->pfds) {
        for (int i = 0; i < unic_p->pfdsnum; i++) {
            all_pfds[fd_index] = unic_p->pfds[i];
            fd_index++;
        }
    }
    
    // Add terminator (required by some poll implementations)
    all_pfds[total_fds].fd = 0;
    all_pfds[total_fds].events = 0;
    all_pfds[total_fds].revents = 0;
    
    // Perform the poll
    poll_result = poll(all_pfds, total_fds, timeout_ms);
    
    if (poll_result < 0) {
        if (errno == EINTR) {
            // Interrupted by signal, this is normal
            poll_result = 0;
        } else {
            log_message(log_module, MSG_ERROR, "Poll error: %s", strerror(errno));
        }
    }
    
    // Copy back revents to original structures
    fd_index = 0;
    
    if (fds && fds->pfds) {
        for (int i = 0; i < fds->pfdsnum; i++) {
            fds->pfds[i].revents = all_pfds[fd_index].revents;
            fd_index++;
        }
    }
    
    if (unic_p && unic_p->pfds) {
        for (int i = 0; i < unic_p->pfdsnum; i++) {
            unic_p->pfds[i].revents = all_pfds[fd_index].revents;
            fd_index++;
        }
    }
    
    free(all_pfds);
    return poll_result;
}

/**
 * @brief Poll only available file descriptors (non-blocking check)
 * @param fds DVB file descriptors structure
 * @param unic_p Unicast parameters with network sockets
 * @param timeout_ms Timeout in milliseconds
 * @return Number of file descriptors ready, -1 on error, 0 on timeout
 */
int main_thread_poll_available_fds(fds_t *fds, unicast_parameters_t *unic_p, int timeout_ms)
{
    // First check if we have any file descriptors to poll
    int dvb_fds = (fds && fds->pfds) ? fds->pfdsnum : 0;
    int unicast_fds = (unic_p && unic_p->pfds) ? unic_p->pfdsnum : 0;
    
    if (dvb_fds == 0 && unicast_fds == 0) {
        // No file descriptors available, use simple timeout
        struct pollfd dummy = {0, 0, 0};
        return poll(&dummy, 1, timeout_ms);
    }
    
    // Use the main polling function
    return main_thread_poll_with_timeout(fds, unic_p, timeout_ms);
}

/**
 * @brief Main thread sleep replacement using poll()
 * @param fds DVB file descriptors structure
 * @param unic_p Unicast parameters with network sockets
 * @param sleep_ms Sleep duration in milliseconds
 * @return 0 on success, -1 on error
 */
int main_thread_sleep_with_poll(fds_t *fds, unicast_parameters_t *unic_p, int sleep_ms)
{
    int poll_result = main_thread_poll_available_fds(fds, unic_p, sleep_ms);
    
    if (poll_result < 0) {
        return -1; // Error
    }
    
    // poll_result >= 0 means we either got events or timed out
    // Both are acceptable for a sleep operation
    return 0;
}
