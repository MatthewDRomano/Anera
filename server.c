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
#include <poll.h>
#include <stdatomic.h>

#define DEFAULT_PORT 5555
#define MIN_PORT 1024
#define MAX_PORT 49151
#define MAX_CONNECTIONS 15

// Message type identifier
typedef enum {
	LOGIN,
	LOGOUT,
	SERVER_UPDATE,
	CLIENT_UPDATE
} message_type_t;

// Network safe player info struct
// NOTE: Struct variables may be misaligned on arrival on different architectures
typedef struct __attribute__((packed)) {
	uint8_t type; // Safe for network transfer; portibility on varying systems; can cast to enum for readability
        char username[32];
        uint16_t pos_x, pos_y;
} user_data_t;

typedef struct client_thread {
	pthread_t thread;
	int client_fd;
	user_data_t net_msg;
	bool finished;
	
	struct client_thread *next;
} client_thread_t;

static client_thread_t *clients = NULL;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clients_cond = PTHREAD_COND_INITIALIZER;


typedef struct {
	atomic_int connected_players;
	int socket_fd; // for listening only
	atomic_bool running;
	uint8_t max_players;
	struct sockaddr_in server;

} settings_t;

static settings_t settings;

void init_def_settings() {
	settings.server.sin_family = AF_INET;
	settings.server.sin_port = htons(DEFAULT_PORT);
	settings.server.sin_addr.s_addr = INADDR_ANY;
	settings.socket_fd = 0;
	settings.connected_players = ATOMIC_VAR_INIT(0);
	settings.running = ATOMIC_VAR_INIT(false);
	settings.max_players = MAX_CONNECTIONS;
}


int parse_args(int argc, char *argv[]) {
	
	for (int i = 1; i < argc; i++) {
		// Usage menu
		if (strcmp("-h", argv[i]) == 0 || strcmp("--help", argv[i]) == 0) {
			fprintf(stdout, "usage: ./server [-h] [--port PORT] [--max-players PLAYER_COUNT]\n");
			exit(0);
		}

		else if	(strcmp("--port", argv[i]) == 0) {
			// Missing arg
			if (++i == argc) {
				fprintf(stderr, "Missing port\n");
				return -1;
			}
			
			char *end;
			long int port = strtol(argv[i], &end, 10);
			
			// Invalid Port
			if (port < MIN_PORT || port > MAX_PORT || *end != '\0') {
				fprintf(stderr, "Invalid Port (Must be %d-%d)\n", MIN_PORT, MAX_PORT);
				return -1;
			}

			settings.server.sin_port = htons((uint16_t)port);
		}

		else if (strcmp("--max-players", argv[i]) == 0) {
			// Missing arg
			if (++i == argc) {
				fprintf(stderr, "Missing max player count\n");
				return -1;
			}

			char *end;
			long int n = strtol(argv[i], &end, 10);
			
			// Invalid player count
			if (n < 1 || n > MAX_CONNECTIONS || *end != '\0') {
				fprintf(stderr, "Invalid max player count (Must be %d-%d)\n", 1, MAX_CONNECTIONS);
				return -1;
			}

			settings.max_players = (uint8_t)n;
		}

		else {
			fprintf(stderr, "Invalid argument \"%s\"\n", argv[i]);
			return -1;
		}
	}

	return 0;
}

// Ensures full read from client
int read_from_client(int socket_fd, user_data_t *user_info) {
	size_t const n = sizeof(user_data_t);
	size_t bytes_read = 0;
	ssize_t result;

	while (bytes_read < n) {

		result = read(socket_fd, (char*)user_info + bytes_read, n - bytes_read);
		
		// Error occured
		if (result < 0) {
			if (errno == EINTR)
				continue;
			perror("Error reading from client");
			return -1;
		}

		// EOF; client disconnected
		else if (result == 0) { 
			fprintf(stderr, "EOF reached reading from client\n");
			return -1;
		}

		bytes_read += result;
	}
	
	return 0;
}

// Ensures all user info writes to client --Assumes dynamically allocated users_info---
int write_to_client(int socket_fd, user_data_t *users_info, int users) {
	const size_t n = users * sizeof(user_data_t);
	size_t bytes_written = 0;
	ssize_t result;

	while (bytes_written < n) {
		result = write(socket_fd, (char*)users_info + bytes_written, n - bytes_written);

		// Error writing to client -1
		if (result < 0) {
			if (errno == EINTR)
				continue;
			perror("Error writing to client");
			return -1;
		}

		bytes_written += result;
	}	

	return 0;
}


// Checks for finished clients to remove / assumes mutex locked
bool has_dead_clients() {
	client_thread_t *ct = clients;
	while (ct != NULL) {
		if (ct->finished)
			return true;
		ct = ct->next;
	}
	return false;
}

void* reaper_thread(void *arg) {
	
	while(atomic_load(&settings.running)) {
		pthread_mutex_lock(&clients_mutex);
	
		// Ignores spurious wake ups	
		while (!has_dead_clients()) {
			pthread_cond_wait(&clients_cond, &clients_mutex);
			// Ensures reaper does not infinitely sleep upon shutdown with no clients
			if (!atomic_load(&settings.running) && atomic_load(&settings.connected_players) == 0)
				break;
		}


		client_thread_t **pp = &clients;
		while (*pp != NULL) {
			client_thread_t *ct = *pp;
			
			if (ct->finished) {
				pthread_join(ct->thread, NULL);
				*pp = ct->next;
				atomic_fetch_sub_explicit(&settings.connected_players, 1, memory_order_relaxed);
				close(ct->client_fd);

				// Prints user disconnected and frees associated memory
				fprintf(stdout, "%s disconnected\n", ct->net_msg.username);
                                fflush(stdout);
				free(ct);	
			}
				
			else
				pp = &ct->next;

		}
		
		pthread_mutex_unlock(&clients_mutex);
	}	

	return NULL;
}

// Assumes clients mutex locked
int send_by_type(int sock_fd, uint8_t msg_type) {
	user_data_t msg[MAX_CONNECTIONS] = {0};
	const size_t n = sizeof(user_data_t);
	int i = 0;
	
	client_thread_t *c = clients;
	while (c != NULL && i < MAX_CONNECTIONS) {
		memcpy(msg + (i++), &c->net_msg, n);
		c = c->next;
	}
	
	msg[0].type = msg_type;
	if (write_to_client(sock_fd, msg, MAX_CONNECTIONS) == -1)
		return -1;
	
	return 0;
}

void* client_thread(void* arg) {	
	client_thread_t *ct = (client_thread_t*)arg;
	
	struct pollfd pfd;

	while (!ct->finished) {	
	//pthread_mutex_lock(&clients_mutex);
		pfd.fd = ct->client_fd;
        	pfd.events = POLLIN;
	
		//if (pending write)
		pfd.events |= POLLOUT;
		
		int ready = poll(&pfd, 1, 100); // Waits 100 ms

		pthread_mutex_lock(&clients_mutex);
		if (ready > 0) {
			
			if (pfd.revents & (POLLHUP | POLLERR)) {
				send_by_type(ct->client_fd, (uint8_t)LOGOUT);
                                ct->finished = true;
				pthread_mutex_unlock(&clients_mutex);
				break;
                        }
		
			if (pfd.revents & POLLIN) {
				// Reads from client
				if (read_from_client(ct->client_fd, &ct->net_msg) == -1) {
					// ALL -1 returns on blocking read() are catastrophic (Except EINTR)
					ct->finished = true;
					pthread_mutex_unlock(&clients_mutex);
					break;
				}
				
				// handle message types accordingly
				bool terminate_loop = false;
				switch ((message_type_t)ct->net_msg.type) {
					case LOGIN:
						fprintf(stdout, "%s logged in\n", ct->net_msg.username);
						fflush(stdout);
						break;
					case LOGOUT:
                                		ct->finished = true;
						terminate_loop = true;
						break;
					case SERVER_UPDATE:
						break;
					default: // Invalid message type
						break;
				}

				// Skips write if logout message was received
				if (terminate_loop) {
                                	pthread_mutex_unlock(&clients_mutex);
					break;
				}
			}
		
	
			if (pfd.revents & POLLOUT) 
				if (send_by_type(ct->client_fd, (uint8_t)CLIENT_UPDATE) == -1)
					ct->finished = true;
				
			
		}
		pthread_mutex_unlock(&clients_mutex);	
	}
	
	pthread_cond_broadcast(&clients_cond);
	
	return NULL;
}

// Allows graceful shutdown for SIGINT / SIGTERM
void shutdown_handler(int signum) {
	atomic_store(&settings.running, false);

	const char msg[] = "Shutdown Signaled\n";
	write(1, msg, sizeof(msg) - 1);
}

int main (int argc, char *argv[]) {
	
	// Handles termination	
	struct sigaction shutdown = {0};
	shutdown.sa_handler = shutdown_handler;
	
	sigaction(SIGINT, &shutdown, NULL);
	sigaction(SIGTERM, &shutdown, NULL);

	// Ensures Server stays open if terminal session closes
	struct sigaction sa = {0};
	sa.sa_handler = SIG_IGN;
	sigaction(SIGHUP, &sa, NULL);

	// Sets default settings
	init_def_settings();

	// Parses CLI arguments and sets user specified settings
	if (parse_args(argc, argv) == -1) 
		return -1;

	// IPv4 TCP default protocol
	settings.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (settings.socket_fd == -1) {
		perror("Socket creation failed");
		return -1;
	}
	
	// Binds server to socket
	while (bind(settings.socket_fd, (struct sockaddr*)(&settings.server), sizeof(settings.server)) == -1) {
		if (errno == EINTR)
			continue;
		fprintf(stderr, "Failed to bind socket to port %d", ntohs(settings.server.sin_port));
		perror(" ");
		return -1;
	}

	// Starts listening on socket
	while (listen(settings.socket_fd, settings.max_players) == -1) {
		if (errno == EINTR)
			continue;
		perror("Error while attmepting to listen on socket");
		return -1;
	}

	// Server is "running"
	atomic_store(&settings.running, true);
	fprintf(stdout, "Server Listening on port: %d\n", ntohs(settings.server.sin_port));
	fflush(stdout);

	// Spawns reaper thread to cleanup dead client threads
	pthread_t reaper;
	if (pthread_create(&reaper, NULL, reaper_thread, NULL) != 0) {
		perror("Error spawning reaper thread");
		return -1;
	}

	// Server loop
	while (atomic_load(&settings.running)) {
		if (atomic_load(&settings.connected_players) >= settings.max_players)
			continue;

		struct sockaddr_in new_client;
		socklen_t adderlen = sizeof(new_client);
		int client_fd;

		// Checks for accept() error / BLOCKING
		while (atomic_load(&settings.running) && ((client_fd = accept(settings.socket_fd, 
			(struct sockaddr*)&new_client, 
			&adderlen)) == -1)) {
			
			if (errno == EINTR)
				continue;

			// Depending on error, client either stays or is removed by kernel from accept queue
			// Retry accept() always since EMFILE or ENFILE not possible (if check)
			int cp = atomic_load(&settings.connected_players) + 1;
			fprintf(stderr, "Error trying to accept client #%s", cp);
			perror(" ");
		}

		// Exits server loop if terminated
		if (!atomic_load(&settings.running))
			break;
			
		// Create new client once accepted
		client_thread_t *ct = (client_thread_t*)malloc(sizeof(client_thread_t));
		if (!ct) {
			fprintf(stderr, "Error allocating space for new client thread\n");
			break;
		}
		ct->client_fd = client_fd;
		ct->finished = false;
		memset(&ct->net_msg, 0, sizeof(ct->net_msg));
		
	
		// Adds client to front of list / Ensures no race
		if (pthread_create(&ct->thread, NULL, client_thread, ct) == 0) {
			pthread_mutex_lock(&clients_mutex);
                	ct->next = clients;
                	clients = ct;
                	pthread_mutex_unlock(&clients_mutex);

			atomic_fetch_add_explicit(&settings.connected_players, 1, memory_order_relaxed);
		}	
		
		// Failed to create thread			
		else {
			fprintf(stderr, "Error creating client thread\n");
			close(client_fd);
                        free(ct);
		}
	}
	
	// Server is closing / finishes all threads for reaper to join
	// Sends logout message
	client_thread_t *c = clients;
	pthread_mutex_lock(&clients_mutex);
	while (c != NULL) {
		send_by_type(c->client_fd, (uint8_t)LOGOUT);
		c->finished = true;
		c = c->next;
	}
	pthread_mutex_unlock(&clients_mutex);

	// Wakes up reaper if sleeping; ready to join
	pthread_cond_broadcast(&clients_cond);

	pthread_join(reaper, NULL);
	pthread_mutex_destroy(&clients_mutex);
        pthread_cond_destroy(&clients_cond);	
	return 0;
}
