#define _WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <direct.h>
#include "win32.h"

#if _MSC_VER < 1800
#undef va_copy
#define va_copy(dst, src) (dst = src)
#endif

#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
#define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64
#else
#define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL
#endif

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag = 0;

    if (NULL != tv) {
        GetSystemTimeAsFileTime(&ft);

        tmpres |= ft.dwHighDateTime;
        tmpres <<= 32;
        tmpres |= ft.dwLowDateTime;

        tmpres /= 10;  /*convert into microseconds*/
        /*converting file time to unix epoch*/
        tmpres -= DELTA_EPOCH_IN_MICROSECS;
        tv->tv_sec = (long)(tmpres / 1000000UL);
        tv->tv_usec = (long)(tmpres % 1000000UL);
    }

    return 0;
}

int vasprintf(char **strp, const char *fmt, va_list ap)
{
    va_list ap_copy;
    int formattedLength, actualLength;
    size_t requiredSize;

    // be paranoid
    *strp = NULL;

    // copy va_list, as it is used twice
    va_copy(ap_copy, ap);

    // compute length of formatted string, without NULL terminator
    formattedLength = _vscprintf(fmt, ap_copy);
    va_end(ap_copy);

    // bail out on error
    if (formattedLength < 0) {
        return -1;
    }

    // allocate buffer, with NULL terminator
    requiredSize = ((size_t)formattedLength) + 1;
    *strp = (char *)malloc(requiredSize);

    // bail out on failed memory allocation
    if (*strp == NULL) {
        errno = ENOMEM;
        return -1;
    }

    // write formatted string to buffer, use security hardened _s function
    actualLength = vsnprintf_s(*strp, requiredSize, requiredSize - 1, fmt, ap);

    // again, be paranoid
    if (actualLength != formattedLength) {
        free(*strp);
        *strp = NULL;
        errno = EOTHER;
        return -1;
    }

    return formattedLength;
}

int asprintf(char **strp, const char *fmt, ...)
{
    int result;

    va_list ap;
    va_start(ap, fmt);
    result = vasprintf(strp, fmt, ap);
    va_end(ap);

    return result;
}

void usleep(unsigned int usec)
{
    HANDLE timer;
    LARGE_INTEGER ft;

    ft.QuadPart = -(10 * (__int64)usec);

    timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (timer) {
        SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
        WaitForSingleObject(timer, INFINITE);
        CloseHandle(timer);
    }
}

// Better Windows sleep function using high-resolution timer
void win32_sleep_us(unsigned int usec)
{
    if (usec == 0) return;
    
    // Use high-resolution timer for better accuracy
    LARGE_INTEGER frequency, start, end, elapsed;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    
    // Calculate target elapsed time in ticks
    LONGLONG target_ticks = (LONGLONG)((double)usec * frequency.QuadPart / 1000000.0);
    
    do {
        // Use Sleep() for the bulk of the time to avoid busy waiting
        if (usec > 1000) {
            Sleep(usec / 1000);
            usec = usec % 1000;
        }
        
        // Use high-resolution timer for the remainder
        QueryPerformanceCounter(&end);
        elapsed.QuadPart = end.QuadPart - start.QuadPart;
        
        if (elapsed.QuadPart >= target_ticks) {
            break;
        }
        
        // Yield CPU to other threads
        Sleep(0);
        
    } while (elapsed.QuadPart < target_ticks);
}

// Windows equivalent of nanosleep() - most precise sleep
int win32_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req) return -1;
    
    // Convert to microseconds
    unsigned int usec = (unsigned int)(req->tv_sec * 1000000 + req->tv_nsec / 1000);
    
    win32_sleep_us(usec);
    
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    
    return 0;
}

// Windows equivalent of poll() for file descriptors
int win32_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
    if (!fds || nfds <= 0) {
        // No file descriptors, just sleep for timeout
        if (timeout_ms > 0) {
            Sleep(timeout_ms);
        }
        return 0;
    }

    HANDLE *handles = (HANDLE*)malloc(nfds * sizeof(HANDLE));
    if (!handles) {
        return -1;
    }

    int valid_handles = 0;
    for (int i = 0; i < nfds; i++) {
        if (fds[i].fd >= 0) {
            // Convert file descriptor to Windows handle
            handles[valid_handles] = (HANDLE)(intptr_t)fds[i].fd;
            valid_handles++;
        }
    }

    if (valid_handles == 0) {
        // No valid file descriptors, just sleep for timeout
        free(handles);
        if (timeout_ms > 0) {
            Sleep(timeout_ms);
        }
        return 0;
    }

    // Wait for any of the handles to become ready
    DWORD wait_result = WaitForMultipleObjects(valid_handles, handles, FALSE, 
                                             timeout_ms > 0 ? timeout_ms : INFINITE);

    free(handles);

    if (wait_result == WAIT_FAILED) {
        return -1;
    } else if (wait_result == WAIT_TIMEOUT) {
        return 0; // Timeout
    } else if (wait_result >= WAIT_OBJECT_0 && wait_result < WAIT_OBJECT_0 + valid_handles) {
        // At least one handle is ready
        int ready_count = 0;
        for (int i = 0; i < nfds; i++) {
            if (fds[i].fd >= 0) {
                fds[i].revents = fds[i].events; // Assume all requested events are ready
                ready_count++;
            } else {
                fds[i].revents = 0;
            }
        }
        return ready_count;
    }

    return 0;
}

// Windows equivalent of poll() for sockets (more accurate)
int win32_poll_sockets(struct pollfd *fds, int nfds, int timeout_ms)
{
    if (!fds || nfds <= 0) {
        if (timeout_ms > 0) {
            Sleep(timeout_ms);
        }
        return 0;
    }

    // Use WSAEventSelect for socket polling
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return -1;
    }

    HANDLE *events = (HANDLE*)malloc(nfds * sizeof(HANDLE));
    if (!events) {
        WSACleanup();
        return -1;
    }

    int valid_events = 0;
    for (int i = 0; i < nfds; i++) {
        if (fds[i].fd >= 0) {
            events[valid_events] = WSACreateEvent();
            if (events[valid_events] != WSA_INVALID_EVENT) {
                SOCKET sock = (SOCKET)fds[i].fd;
                long event_mask = 0;
                if (fds[i].events & POLLIN) event_mask |= FD_READ | FD_ACCEPT | FD_CLOSE;
                if (fds[i].events & POLLOUT) event_mask |= FD_WRITE | FD_CONNECT;
                if (fds[i].events & POLLPRI) event_mask |= FD_OOB;
                
                WSAEventSelect(sock, events[valid_events], event_mask);
                valid_events++;
            }
        }
    }

    if (valid_events == 0) {
        free(events);
        WSACleanup();
        if (timeout_ms > 0) {
            Sleep(timeout_ms);
        }
        return 0;
    }

    DWORD wait_result = WaitForMultipleObjects(valid_events, events, FALSE,
                                             timeout_ms > 0 ? timeout_ms : INFINITE);

    // Clean up events
    for (int i = 0; i < valid_events; i++) {
        WSACloseEvent(events[i]);
    }
    free(events);
    WSACleanup();

    if (wait_result == WAIT_FAILED) {
        return -1;
    } else if (wait_result == WAIT_TIMEOUT) {
        return 0; // Timeout
    } else if (wait_result >= WAIT_OBJECT_0 && wait_result < WAIT_OBJECT_0 + valid_events) {
        // At least one socket is ready
        int ready_count = 0;
        int event_index = 0;
        for (int i = 0; i < nfds; i++) {
            if (fds[i].fd >= 0) {
                fds[i].revents = fds[i].events; // Assume all requested events are ready
                ready_count++;
                event_index++;
            } else {
                fds[i].revents = 0;
            }
        }
        return ready_count;
    }

    return 0;
}

void sleep(unsigned int sec)
{
    usleep(sec * 1000000);
}

int mkpath(char *file_path, unsigned int mode)
{
    char *p = file_path;
    (void)mode;

    while (*p != '\0') {
        p++;

        while (*p != '\0' && *p != '/')
            p++;

        char v = *p;
        *p = '\0';

        if (_mkdir(file_path) == -1 && errno != EEXIST) {
            *p = v;
            return -1;
        }
        *p = v;
    }

    return 0;
}
