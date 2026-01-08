#include "log.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#define MAX_PATH_LEN 128

// Server-instance specific log file path
static char log_path[MAX_PATH_LEN] = {0};
// Log file
static FILE* log_f = NULL;
// Human readable timestamp
static char time_s[32] = {0};
// Protects against multiple threads writing to log simultaneously
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_time() {
	struct timespec ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	strftime(time_s, sizeof(time_s), "%Y-%m-%d-%H:%M:%S", &tm);
}

int init_log(char* log_name) {
	// Log already initialized	
	if (log_f != NULL)
		return 0;

	char name[32] = {0};
	memcpy(name, log_name, sizeof(name) - 1);

	set_time();
	snprintf(log_path, sizeof(log_path), "./logs/%s_log_%s.txt", name, time_s);
	
	log_f = fopen(log_path, "w");
	if (log_f == NULL) {
		fprintf(stderr, "Error creating log file. Ironic\n");
		return -1;
	}

	fprintf(log_f, "==========SERVER START==========\n\n");
	return 0;
}

void end_log() {
	// Ignore end request; Already ended or never initialized
	if (log_f == NULL)
		return;

	fclose(log_f);
	log_f = NULL;
}

int errlog(const char* thread, const char* call, int fd, int errnum, const char* err_desc, const char* client) {
	pthread_mutex_lock(&log_mutex);
	set_time();
	if (fprintf(log_f, "[%s] | thread: %s | %s | fd=%d | Error: %s -> %s | Client name: %s\n", 
		    time_s, thread, call, fd, err_desc, strerror(errnum), client) == EOF) {
		fprintf(stderr, "Error writing to log. Ironic\n");
		pthread_mutex_unlock(&log_mutex);
		return -1;
	}
	fflush(log_f);
	pthread_mutex_unlock(&log_mutex);
	return 0;
}
