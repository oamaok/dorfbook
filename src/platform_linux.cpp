
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <errno.h>

typedef timespec os_timer_mark;

os_timer_mark os_get_timer()
{
	timespec value;
	clock_gettime(CLOCK_MONOTONIC, &value);
	return value;
}

float os_timer_delta_ms(os_timer_mark begin, os_timer_mark end)
{
	time_t sec_diff = end.tv_sec - begin.tv_sec;
	int nano_diff = end.tv_nsec - begin.tv_nsec;
	float ms = sec_diff * 1000.0f + nano_diff / 1000000.0f;
}

typedef int os_socket;

enum Close_Mode
{
	Close_Read = SHUT_RD,
	Close_Write = SHUT_WR,
	Close_ReadWrite = SHUT_RDWR,
};

inline bool os_valid_socket(os_socket sock)
{
	return sock != -1;
}

void os_socket_format_last_error(char *buffer, U32 buffer_length)
{
	// TODO: Text errors for linux
	snprintf(buffer, buffer_length, "%d", errno);
}

void os_socket_close(os_socket sock, Close_Mode mode)
{
	shutdown(sock, (int)mode);
}

typedef pthread_mutex_t os_mutex;

inline void os_mutex_init(os_mutex *mutex)
{
	pthread_mutex_init(mutex, 0);
}

inline void os_mutex_lock(os_mutex *mutex)
{
	pthread_mutex_lock(mutex);
}

inline void os_mutex_unlock(os_mutex *mutex)
{
	pthread_mutex_unlock(mutex);
}

inline void os_sleep_seconds(int seconds)
{
	sleep(seconds);
}

typedef volatile U32 os_atomic_uint32;

inline void os_atomic_increment(os_atomic_uint32 *value)
{
	__sync_fetch_and_add(value, 1);
}

inline void os_atomic_decrement(os_atomic_uint32 *value)
{
	__sync_fetch_and_sub(value, 1);
}

#define OS_THREAD_ENTRY(function, param) void* function(void *param)
#define OS_THREAD_RETURN return 0

typedef pthread_t os_thread;
typedef void* (*os_thread_func)(void*);

inline os_thread os_thread_do(os_thread_func func, void *param)
{
	os_thread result;
	pthread_create(&result, 0, func, param);
	return result;
}

inline void os_startup()
{
}

inline void os_cleanup()
{
}