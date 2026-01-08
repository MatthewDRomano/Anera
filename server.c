#include "config.h"

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
#include <time.h>

#include "anera_net.h"
#include "log.h"

#define MIN_PORT 1024
#define MAX_PORT 49151
#define CLIENT_STACK (1024 * 1024)  // 1 MB

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
	atomic_bool running;
	int socket_fd; // for listening only
	int max_players;
	struct sockaddr_in server;

} settings_t;

static settings_t settings;
// Volatile ignores compiler opitmizations due to updating in async signal handler
static volatile sig_atomic_t shutdown_requested = 0;

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

			settings.max_players = (int)n;
		}

		else {
			fprintf(stderr, "Invalid argument \"%s\"\n", argv[i]);
			return -1;
		}
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
				// Avoids deadlock in other threads if join stalls
				pthread_mutex_unlock(&clients_mutex);
				pthread_join(ct->thread, NULL);
				pthread_mutex_lock(&clients_mutex);
				
				*pp = ct->next;
				atomic_fetch_sub(&settings.connected_players, 1);
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

// Assumes clients mutex unlocked
int send_by_type(int sock_fd, message_type_t msg_type) {
	user_data_t msg[MAX_CONNECTIONS] = {0};
	const size_t n = sizeof(user_data_t);
	int i = 0;

	client_thread_t *c = clients;
	pthread_mutex_lock(&clients_mutex);
	while (c != NULL && i < MAX_CONNECTIONS) {
		memcpy(msg + (i++), &c->net_msg, n);
		c = c->next;
	}
	pthread_mutex_unlock(&clients_mutex);
	
	msg[0].type = (uint8_t)msg_type;
	return full_write(sock_fd, msg, MAX_CONNECTIONS);
}

// Safely marks thread ready to be reaped
void mark_thread_finished(client_thread_t *ct) {
	pthread_mutex_lock(&clients_mutex);
	ct->finished = true;
	pthread_mutex_unlock(&clients_mutex);
}

void* client_io_thread(void* arg) {	
	client_thread_t *ct = (client_thread_t*)arg;
	
	struct pollfd pfd;
	
	pthread_mutex_lock(&clients_mutex);
	int locked = true;
	while (!ct->finished) {
		pfd.fd = ct->client_fd;
        	pfd.events = POLLIN | POLLOUT;
		
		pthread_mutex_unlock(&clients_mutex);
		locked = false;

		
		int ready = poll(&pfd, 1, 100); // Waits 100 ms

		if (ready > 0) {
			
			if (pfd.revents & (POLLHUP | POLLERR)) {
				send_by_type(ct->client_fd, LOGOUT);
				mark_thread_finished(ct);
				break;
                        }
			
			// Reads from client	
			if (pfd.revents & POLLIN) {
				int result = 0;
				user_data_t buf;
				if ((result = full_read(ct->client_fd, &buf, 1)) != 0) {
					if (result == EOF)
						fprintf(stderr, "EOF reached on %s\n", buf.username);
					else {
						char err_buf[128];
                                        	char *msg = strerror_r(result, err_buf, sizeof(err_buf));
						fprintf(stderr, "Client read error: %s\n", msg);
					}
					
					mark_thread_finished(ct);
					break;
				}
				
				pthread_mutex_lock(&clients_mutex);
				memcpy(&ct->net_msg, &buf, sizeof(user_data_t));
				pthread_mutex_unlock(&clients_mutex);
				

				if ((message_type_t)buf.type == LOGIN) {
					fprintf(stdout, "%s logged in\n", buf.username);
					fflush(stdout);
				}
			}
		
			// Writes to client
			if (pfd.revents & POLLOUT) {
				int result = 0;
				if ((result = send_by_type(ct->client_fd, CLIENT_UPDATE)) != 0) {
					char err_buf[128];
					char *msg = strerror_r(result, err_buf, sizeof(err_buf));
					fprintf(stderr, "Client write error: %s\n", msg);
					mark_thread_finished(ct);
					break;
				}
			}

				
			
		}
		
		// Error occurred during poll()
		else if (ready < 0)
			if (errno != EINTR) {
				perror("Poll failed");
				mark_thread_finished(ct);
				break;
			}
	
		pthread_mutex_lock(&clients_mutex);
		locked = true;
	}
	
	if (!locked) 
		pthread_mutex_lock(&clients_mutex);

	pthread_cond_broadcast(&clients_cond);
	pthread_mutex_unlock(&clients_mutex);	

	return NULL;
}

// Allows graceful shutdown for SIGINT / SIGTERM
void shutdown_handler(int signum) {
	shutdown_requested = 1; // Async safe sig_atomic_t
	close(settings.socket_fd); // Interrupts accept()
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

	// Allows rebind to same port after server termination
	// regardless if clients in TIME_WAIT or not
	int opt = 1;
	setsockopt(settings.socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	
	// Binds server to socket
	while (bind(settings.socket_fd, (struct sockaddr*)(&settings.server), sizeof(settings.server)) == -1) {
		if (errno == EINTR)
			continue;
		perror("Bind to port failed");
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
	fprintf(stdout, "Max players: %d\n", settings.max_players);
	fflush(stdout);

	// Spawns reaper thread to cleanup dead client threads
	pthread_t reaper;
	if (pthread_create(&reaper, NULL, reaper_thread, NULL) != 0) {
		perror("Failed to spawn reaper thread");
		return -1;
	}

	// Creates stack size attribute for client threads (1MB)
	pthread_attr_t client_attr;
	pthread_attr_init(&client_attr);
	if (pthread_attr_setstacksize(&client_attr, CLIENT_STACK) != 0) {
		fprintf(stderr, "Error setting client thread stack size\n");
		raise(SIGTERM);
	}

	// Error opening error log; treated as catastrophic
	if (init_log("Anera_server") != 0)
		raise(SIGTERM);
	
	
	// Server loop
	while (!shutdown_requested) {

		/*if (atomic_load(&settings.connected_players) >= settings.max_players) {
			sleep(1); // Avoids constant CPU use
			continue;
		}*/

		struct sockaddr_in new_client;
		socklen_t adderlen = sizeof(new_client);
		int client_fd;

		// Checks for accept() error / BLOCKING
		while (!shutdown_requested && ((client_fd = accept(settings.socket_fd, 
			(struct sockaddr*)&new_client, 
			&adderlen)) == -1)) {
			
			if (errno == EINTR)
				continue;

			// Depending on error, client either stays or is removed by kernel from accept queue
			// Retry accept() always since EMFILE or ENFILE not possible (if check)
		}
		
		// Exits server loop if terminated
		if (shutdown_requested)
			break;

		// Server is full
		if (atomic_load(&settings.connected_players) >= settings.max_players) {
			send_by_type(client_fd, LOGOUT);
			close(client_fd);
			continue;
		}

		// Create new client once accepted
		client_thread_t *ct = (client_thread_t*)calloc(1, sizeof(client_thread_t));
		if (!ct) {
			perror("Client thread calloc failed");
			break;
		}
		
	
		// Adds client to front of list / Ensures no race
		// Sets file descriptor; client_io_thread also has mutex protection
		pthread_mutex_lock(&clients_mutex);
		if (pthread_create(&ct->thread, &client_attr, client_io_thread, ct) == 0) {
                	ct->next = clients;
                	clients = ct;
			ct->client_fd = client_fd;

			atomic_fetch_add(&settings.connected_players, 1);
		}	
		
		// Failed to create thread			
		else {
			fprintf(stderr, "Error creating client thread\n");
			close(client_fd);
                        free(ct);
		}

		pthread_mutex_unlock(&clients_mutex);
	}
	
	// Shutdown requested. Updates running value to false for all threads
	atomic_store(&settings.running, false);
	//close(settings.socket_fd);	
	
	// Server is closing / finishes all threads for reaper to join
	// Sends logout message
	client_thread_t *c = clients;
	pthread_mutex_lock(&clients_mutex);
	while (c != NULL) {
		//send_by_type(c->client_fd, LOGOUT);
		c->finished = true;
		c = c->next;
	}

	// Wakes up reaper if sleeping; ready to join
	pthread_cond_broadcast(&clients_cond);
	pthread_mutex_unlock(&clients_mutex);

	pthread_join(reaper, NULL);

        pthread_cond_destroy(&clients_cond);
	pthread_attr_destroy(&client_attr);	
	
	end_log();
	return 0;
}
