#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>




const int default_rays = 180;
const uint16_t max_port = 65535;


typedef struct settings {
	char username[32];
	uint16_t ray_multiplier; 
	struct sockaddr_in server;
	int socket_fd;
	bool connected;

} settings_t;

static settings_t settings = {0};

void init_def_server_settings() {
	settings.server.sin_family = AF_INET;
	settings.server.sin_port = htons(8080);
	inet_pton(AF_INET, "127.0.0.1", &settings.server.sin_addr);
}

typedef enum {
	LOGIN,
	LOGOUT,
	SEND_CLIENT_UPDATE,
	RECV_SERVER_UPDATE,
	DISCONNECT
} message_type_t;


// Server safe message struct
typedef struct __attribute__((packed)) {
	

} message_t;



// Parses in-line client arguments
int parse_args(int argc, char* argv[]) {
	for (int i = 1; i < argc; i++) {
		
		// Usage menu
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
			fprintf(stdout, "usage: ./client [-h] [-u USERNAME] [--port PORT] [--ip IP]\n");
		
		// Username
		else if (strcmp(argv[i], "-u") == 0) {
			// Missing arg
			if (i++ == argc) {
				fprintf(stderr, "Missing username\n");
				return -1;
			}

			strncpy(settings.username, argv[i], sizeof(settings.username));
			settings.username[31] = '\0';
		}

		// Port
		else if (strcmp(argv[i], "--port") == 0) {
			// Missing arg
			if (++i == argc) {
				fprintf(stderr, "Missing port\n");
				return -1;
			}	
			
			char* end = NULL;
			long int port = strtol(argv[i], &end, 10);
			if (port < 0 || port > max_port || *end != '\0') {
				fprintf(stderr, "Invalid port \"%s\" (must be 0-%d)\n", argv[i], max_port);
				return -1;
			}

			settings.server.sin_port = htons((uint16_t)port);
		}

		// Ip
		else if (strcmp(argv[i], "--ip") == 0) {
			// Missing arg
			if (++i == argc) {
				fprintf(stderr, "Missing IP\n");
				return -1;
			}

			struct hostent* h_ent = gethostbyname(argv[i]);
			if (h_ent == NULL) {
				fprintf(stderr, "Invalid IP\n");
				return -1;
			}

			memcpy(&settings.server.sin_addr, h_ent->h_addr_list[0], h_ent->h_length);
		}

		// Invalid argument
		else {
			fprintf(stderr, "Invalid argument \"%s\"\n", argv[i]);
			return -1;
		}

	}

	return 0;
}

// Ensures full read from server
int full_read(settings_t* settings, message_t* msg) {
	size_t n = sizeof(*msg);
	size_t bytes_read = 0;

	ssize_t result;
	while (bytes_read < n) {
		result = read(settings->socket_fd, (char*)msg + bytes_read, n - bytes_read);
	
		// Error while reading
		if (result < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "Error reading from server\n");
			return -1;
		}
		
		// EOF signaled; server connection closed
		else if (result == 0) {
			settings->connected = false;
			return -1;
		}

		bytes_read += result;
	}

	return 0;
}


// Ensures full write to server
int full_write(settings_t* settings, message_t* msg) {
	size_t n = sizeof(*msg);
	size_t bytes_written = 0;
	
	ssize_t result;
	while (bytes_written < n) {
		result = write(settings->socket_fd, (char*)msg + bytes_written, n - bytes_written);
		
		// Error while writing
		if (result < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "Error while writing to server\n");
			return -1;
		}

		// EOF signaled; server connection closed
		else if (result == 0) {
			settings->connected = false;
			return -1;
		}

		bytes_written += result;
	}

	return 0;
}


int main(int argc, char* argv[]) {

	// Parse in-line arguments and set default settings
	
	init_def_server_settings();
	settings.ray_multiplier = 1; 
	
	if (parse_args(argc, argv))
		return -1;

	// Thread #1: send to server
	// Thread #2: Receive from server
	// Thread Main: Game loop
	

	
	return 0;
}
