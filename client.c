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
#include <math.h>
#include <stdatomic.h>

#include <raylib.h>
#include "maps.h"

#define MAX_CONNECTIONS 15
#define DEFAULT_RAYS 180
#define FOV 90
#define MAX_PORT 65535




// Message type identifier
typedef enum {
        LOGIN,
        LOGOUT,
        SERVER_UPDATE,
        CLIENT_UPDATE
} message_type_t;

// Server safe player info struct
typedef struct __attribute__((packed)) {
	uint8_t type; // Constant size across architectures; Cast to message_type_t before using
	char username[32];
	uint16_t pos_x, pos_y;
} user_data_t;


static user_data_t players[MAX_CONNECTIONS] = {0};


typedef struct settings {
        user_data_t client_info;
        uint16_t ray_multiplier;
        struct sockaddr_in server;
        int socket_fd;
        atomic_bool connected;

} settings_t;

static pthread_mutex_t settings_mutex = PTHREAD_MUTEX_INITIALIZER;
static settings_t settings = {0};

// Default tcp server connection Port / Ip
void init_def_settings() {
        settings.server.sin_family = AF_INET;
        settings.server.sin_port = htons(8080);
        inet_pton(AF_INET, "127.0.0.1", &settings.server.sin_addr);
	
	atomic_init(&settings.connected, false);
        settings.ray_multiplier = 1;
}

// Parses in-line client arguments
int parse_args(int argc, char* argv[]) {
		
	bool ip_set = false;
	bool domain_set = false;
	bool username_set = false;
	for (int i = 1; i < argc; i++) {
		
		// Usage menu
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			fprintf(stdout, "usage: ./client [-h] [-u USERNAME] [--port PORT] [--ip IP]\n");
			exit(0);
		}
		
		// Username
		else if (strcmp(argv[i], "-u") == 0) {
			// Missing arg
			if (++i == argc) {
				fprintf(stderr, "Missing username\n");
				return -1;
			}
			
			strncpy(settings.client_info.username, argv[i], sizeof(settings.client_info.username));
			settings.client_info.username[31] = '\0';
			username_set = true;
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
			if (port < 0 || port > MAX_PORT || *end != '\0') {
				fprintf(stderr, "Invalid port \"%s\" (must be 0-%d)\n", argv[i], MAX_PORT);
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
		
			if (domain_set) {
				fprintf(stderr, "Cannot specify BOTH domain and IP\n");
				return -1;
			}

			struct hostent* h_ent = gethostbyname(argv[i]);
			if (h_ent == NULL) {
				fprintf(stderr, "Invalid IP\n");
				return -1;
			}
			
			ip_set = true;
			memcpy(&settings.server.sin_addr, h_ent->h_addr_list[0], h_ent->h_length);
		}

		else if (strcmp(argv[i], "--domain") == 0) {
			// Missing arg
			if (++i == argc) {
                                fprintf(stderr, "Missing Domain\n");
                                return -1;
                        }	
				
			if (ip_set) {
				fprintf(stderr, "Cannot specify BOTH domain and IP\n");
                                return -1;
			}

                        struct hostent* h_ent = gethostbyname(argv[i]);
                        if (h_ent == NULL) {
                                fprintf(stderr, "Invalid Domain\n");
                                return -1;
                        }
			
			domain_set = true;
                        memcpy(&settings.server.sin_addr, h_ent->h_addr_list[0], h_ent->h_length);
		}

		// Invalid argument
		else {
			fprintf(stderr, "Invalid argument \"%s\"\n", argv[i]);
			return -1;
		}

	}

	if (!username_set) {
		fprintf(stderr, "Must specify username (-h for more details)\n");
		return -1;
	}

	return 0;
}

// Ensures full read from server --Assumes dynamically allocated users-info---
int read_from_server(int socket_fd, user_data_t* users_info) {
	const size_t n = MAX_CONNECTIONS * sizeof(user_data_t);
	size_t bytes_read = 0;
	ssize_t result;
	
	// Read data for all users
	while (bytes_read < n) {
	
		result = read(socket_fd, 
			(char*)users_info + bytes_read, 
			n - bytes_read);
	
		// Error while reading
		if (result < 0) {
			if (errno == EINTR)
				continue;
			perror("Error reading from server");
			return -1;
		}
		
		// EOF signaled; server connection closed
		else if (result == 0)
			return -1;
			

		bytes_read += result;
	}

	return 0;
}


// Ensures full write to server
int write_to_server(int socket_fd, user_data_t* msg) {
	const size_t n = sizeof(user_data_t);
	size_t bytes_written = 0;
	
	ssize_t result;
	while (bytes_written < n) {
		result = write(socket_fd, (char*)msg + bytes_written, n - bytes_written);
		
		// Error while writing
		if (result < 0) {
			if (errno == EINTR)
				continue;
			perror("Error while writing to server");
			return -1;
		}

		
		else if (result == 0) {
			perror("Connection closed while writing");
			return -1;
		}

		bytes_written += result;
	}

	return 0;
}

void* recv_thread(void *arg) {
	while (atomic_load(&settings.connected)) {
		if (read_from_server(settings.socket_fd, players) == -1) {
			atomic_store(&settings.connected, false);	
			break;
		}
		
		message_type_t type = (message_type_t)players[0].type;
		switch (type) {
			case LOGOUT:
				atomic_store(&settings.connected, false);
				break;
			case CLIENT_UPDATE:
				break;
			// Types LOGIN and SERVER_UPDATE are invalid and cannot be sent
			default:
				fprintf(stderr, "Invalid inbound message type: \"%d\"\n", type);
				break;
		}
	}	
	return NULL;
}

void* send_thread(void *arg) {
	while (atomic_load(&settings.connected)) {
		pthread_mutex_lock(&settings_mutex);
		
		settings.client_info.type = (uint8_t)SERVER_UPDATE;
		if (write_to_server(settings.socket_fd, &settings.client_info) == -1)
			atomic_store(&settings.connected, false);
		
		pthread_mutex_unlock(&settings_mutex);
	}
	return NULL;
}

// Graceful shutdown on SIGINT SIGTERM SIGHUP
void shutdown_handler(int signum) {
	atomic_store(&settings.connected, false);
}


int main(int argc, char* argv[]) {
	// Set signal handler to shutdown gracefully (Default flags)
	struct sigaction shutdown = {0};
	shutdown.sa_handler = shutdown_handler;
	sigaction(SIGINT, &shutdown, NULL);
	sigaction(SIGTERM, &shutdown, NULL);
	sigaction(SIGHUP, &shutdown, NULL);
	
	// Parse in-line arguments and set default settings
	init_def_settings(); 
	
	if (parse_args(argc, argv))
		return -1;
		
	// IPv4 TCP default protocol
	settings.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (settings.socket_fd == -1) {
		perror("Socket creation failed");
		return -1;
	}
	
	// Connects socket to server
	while (connect(settings.socket_fd, (struct sockaddr*)(&settings.server), sizeof(settings.server)) == -1) {
		if (errno == EINTR)                                                                                                  		continue;
		fprintf(stderr, "Failed to connect socket to server");
		perror(" ");
		return -1;
	}
	
	// Connection successful
	atomic_store(&settings.connected, true);

	// Thread #1: send to server
	pthread_t send_tid;
	pthread_create(&send_tid, NULL, send_thread, NULL);
	
	// Thread #2: Receive from server
	pthread_t recv_tid;
        pthread_create(&recv_tid, NULL, recv_thread, NULL);
	// Thread Main: Game loop

	pthread_mutex_lock(&settings_mutex);
	settings.client_info.type = (uint8_t)LOGIN;
	write_to_server(settings.socket_fd, &settings.client_info);
	pthread_mutex_unlock(&settings_mutex);

	while (atomic_load(&settings.connected)) {
		
	}
	/*
	point_t start_pos;
	start_pos.x = 50;
	start_pos.y = 50;

	double view_angle = M_PI / 4;
	double ray_displacement = fov / (default_rays * settings.ray_multiplier);

	
	InitWindow(1000, 500, "Anera: Alpha");
	while (!WindowShouldClose()) {
		BeginDrawing();
		ClearBackground(BLACK);

		// Handles each ray iteratively
		for (int i = 0; i < default_rays * settings.ray_multiplier; i++) {
			// Find where it hits
			
		}



		EndDrawing();
		
	}
	CloseWindow(); */
		
	// Sends logout
	pthread_mutex_lock(&settings_mutex);
	settings.client_info.type = (uint8_t)LOGOUT;
	write_to_server(settings.socket_fd, &settings.client_info);
	close(settings.socket_fd);
	pthread_mutex_unlock(&settings_mutex);

	pthread_join(send_tid, NULL);
        pthread_join(recv_tid, NULL);
	pthread_mutex_destroy(&settings_mutex);
	
	return 0;
}
