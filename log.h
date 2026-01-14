#ifndef LOG_H
#define LOG_H


// Creates new log specific to server instance
// Must call before multithreading
int init_log(char* log_name);

// Closes file descriptor to log upon shutdown
// Must call after joining threads
void end_log();

/* 
 * Logs an error entry
 * thread   -> thread name
 * call     -> function that produced error
 * fd 	    -> file descriptor num
 * errnum   -> errno value
 * err_desc -> string describing error
 * client   -> client name
*/
int errlog(const char* thread, 
	   const char* call, 
	   int fd, 
	   int errnum,
	   const char* err_desc,
	   const char* client);

#endif
