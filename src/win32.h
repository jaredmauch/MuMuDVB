#ifndef _WIN32_H
#define _WIN32_H

#include <winsock2.h>
#include <ws2tcpip.h>

struct timezone
{
    int  tz_minuteswest; /* minutes W of Greenwich */
    int  tz_dsttime;     /* type of dst correction */
};

// pollfd structure for Windows compatibility
struct pollfd {
    int fd;        // File descriptor
    short events;  // Events to watch for
    short revents; // Events that occurred
};

// Poll event flags
#define POLLIN    0x0001  // Data available for reading
#define POLLOUT   0x0004  // Writing will not block
#define POLLPRI   0x0002  // Urgent data available
#define POLLERR   0x0008  // Error condition
#define POLLHUP   0x0010  // Hang up
#define POLLNVAL  0x0020  // Invalid file descriptor

int gettimeofday(struct timeval *tv, struct timezone *tz);
int asprintf(char **strp, const char *fmt, ...);
void usleep(unsigned int usec);
void sleep(unsigned int sec);
int mkpath(char *file_path, unsigned int mode);

// Better Windows sleep functions - non-busy waiting
void win32_sleep_us(unsigned int usec);
int win32_nanosleep(const struct timespec *req, struct timespec *rem);

// Windows equivalents of poll() - non-busy waiting
int win32_poll(struct pollfd *fds, int nfds, int timeout_ms);
int win32_poll_sockets(struct pollfd *fds, int nfds, int timeout_ms);

// Cross-platform poll() wrapper
#define poll(fds, nfds, timeout_ms) win32_poll(fds, nfds, timeout_ms)

#endif /* _WIN32_H */
