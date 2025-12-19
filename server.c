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

#define DEFAULT_PORT 5555
#define MIN_PORT 1024
#define MAX_PORT 49151


typedef struct {
	int max_players;
	// user data dynamic allocation	
	int socket_fd;
	struct sockaddr_in server;

} settings_t;

static settings_t settings = {0};

void init_def_server_settings() {
	settings.server.sin_family = AF_INET;
	settings.server.sin_port = htons(DEFAULT_PORT);
	settings.server.sin_addr.s_addr = INADDR_ANY;
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
				fprintf(stderr, "Missing max player count arg\n");
				return -1;
			}

			char *end;
			long int max_players = strtol(argv[i], &end, 10);
			
			// Invalid player count
			if (max_players < 0x1 || max_players > 0xF || *end != '\0') {
				fprintf(stderr, "Invalid max player count (Must be %d-%d)\n", 0x1, 0xF);
				return -1;
			}

			settings.max_players = (int)max_players;
		}

		else {
			fprintf(stderr, "Invalid argument \"%s\"\n", argv[i]);
			return -1;
		}
	}

	return 0;
}

int main (int argc, char *argv[]) {

	init_def_server_settings();

	if (parse_args(argc, argv) == -1) 
		return -1;

	// IPv4 TCP default protocol
	settings.socket_fd = socket(AF_INET, SOCK_STREAM, 0);

	if (bind(settings.socket_fd, (struct sockaddr*)(&settings.server), sizeof(settings.server)) == -1) {
		fprintf(stderr, "Failed to bind socket to port %d\n", ntohs(settings.server.sin_port));
		return -1;
	}	

	fprintf(stdout, "Server Listening on port: %d\n", ntohs(settings.server.sin_port));
	fflush(stdout);



	return 0;
}
