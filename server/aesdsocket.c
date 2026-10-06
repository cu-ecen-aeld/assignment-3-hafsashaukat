#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

static volatile sig_atomic_t caught_signal = 0;
static int server_fd = -1;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_data {
    pthread_t thread_id;
    int client_fd;
    char client_ip[INET6_ADDRSTRLEN];
    bool thread_complete;
    SLIST_ENTRY(thread_data) entries;
};

SLIST_HEAD(thread_list, thread_data);
static struct thread_list threads = SLIST_HEAD_INITIALIZER(threads);

// signal handler for SIGINT and SIGTERM
static void signal_handler(int signo)
{
    (void)signo;
    caught_signal = 1;

    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
    }
}

// send all bytes in buffer to the connected client
static int send_all(int client_fd, const char *buffer, size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t sent = send(client_fd,
                            buffer + total_sent,
                            length - total_sent,
                            0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR, "send failed: %s", strerror(errno));
            return -1;
        }

        total_sent += (size_t)sent;
    }

    return 0;
}

// send the entire contents of DATA_FILE back to the client
static int send_file_to_client(int client_fd)
{
    FILE *file = fopen(DATA_FILE, "r");

    if (file == NULL) {
        syslog(LOG_ERR, "Could not open %s: %s",
               DATA_FILE, strerror(errno));
        return -1;
    }

    char buffer[BUFFER_SIZE];

    while (!feof(file)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);

        if (bytes_read > 0) {
            if (send_all(client_fd, buffer, bytes_read) != 0) {
                fclose(file);
                return -1;
            }
        }

        if (ferror(file)) {
            syslog(LOG_ERR, "Error reading %s", DATA_FILE);
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    return 0;
}

// append one complete packet to DATA_FILE
static int append_packet(const char *packet, size_t length)
{
    FILE *file = fopen(DATA_FILE, "a");

    if (file == NULL) {
        syslog(LOG_ERR, "Could not open %s: %s",
               DATA_FILE, strerror(errno));
        return -1;
    }

    if (fwrite(packet, 1, length, file) != length) {
        syslog(LOG_ERR, "Error writing to %s", DATA_FILE);
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

// append a timestamp to DATA_FILE every 10 seconds
static void *timestamp_thread(void *arg)
{
    (void)arg;

    while (!caught_signal) {

        // wait 10 seconds between timestamp writes
        for (int i = 0; i < 10 && !caught_signal; i++) {
            sleep(1);
        }

        if (caught_signal) {
            break;
        }

        time_t current_time = time(NULL);

        if (current_time == (time_t)-1) {
            syslog(LOG_ERR, "time failed");
            continue;
        }

        struct tm time_info;

        if (localtime_r(&current_time, &time_info) == NULL) {
            syslog(LOG_ERR, "localtime_r failed");
            continue;
        }

        char timestamp[128];

        size_t length = strftime(timestamp,
                                 sizeof(timestamp),
                                 "timestamp:%a, %d %b %Y %H:%M:%S %z\n",
                                 &time_info);

        if (length == 0) {
            syslog(LOG_ERR, "strftime failed");
            continue;
        }

        // protect timestamp writes from client file access
        pthread_mutex_lock(&file_mutex);

        if (append_packet(timestamp, length) != 0) {
            syslog(LOG_ERR, "Could not append timestamp");
        }

        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}

// receive data from one client
static void *handle_client(void *arg)
{
    struct thread_data *data = (struct thread_data *)arg;
    int client_fd = data->client_fd;

    char recv_buffer[BUFFER_SIZE];

    char *packet = NULL;
    size_t packet_length = 0;

    while (!caught_signal) {
        ssize_t received = recv(client_fd,
                                recv_buffer,
                                sizeof(recv_buffer),
                                0);

        if (received == 0) {
            break;
        }

        if (received < 0) {
            if (errno == EINTR) {
                if (caught_signal) {
                    break;
                }
                continue;
            }

            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            goto cleanup;
        }

        for (ssize_t i = 0; i < received; i++) {

            char *new_packet = realloc(packet, packet_length + 1);

            if (new_packet == NULL) {
                syslog(LOG_ERR, "realloc failed");
                goto cleanup;
            }

            packet = new_packet;
            packet[packet_length++] = recv_buffer[i];

            if (recv_buffer[i] == '\n') {

    		// protect file access from other client threads
    		pthread_mutex_lock(&file_mutex);

    		if (append_packet(packet, packet_length) != 0) {
        		pthread_mutex_unlock(&file_mutex);
        		goto cleanup;
    		}

    		if (send_file_to_client(client_fd) != 0) {
        		pthread_mutex_unlock(&file_mutex);
        		goto cleanup;
    		}

    		pthread_mutex_unlock(&file_mutex);

    		free(packet);
    		packet = NULL;
    		packet_length = 0;
	   }
        }
    }

cleanup:
    free(packet);

    close(client_fd);
    data->client_fd = -1;

    syslog(LOG_DEBUG, "Closed connection from %s", data->client_ip);

    data->thread_complete = true;

    return NULL;
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;

    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
        return -1;
    }

    struct addrinfo hints;
    struct addrinfo *server_info = NULL;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // install SIGINT and SIGTERM handlers
    struct sigaction action;
    memset(&action, 0, sizeof(action));

    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {

        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    // obtain an address suitable for bind()
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status = getaddrinfo(NULL, PORT, &hints, &server_info);

    if (status != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s",
               gai_strerror(status));
        closelog();
        return -1;
    }

    // create the TCP socket
    server_fd = socket(server_info->ai_family,
                       server_info->ai_socktype,
                       server_info->ai_protocol);

    if (server_fd == -1) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        freeaddrinfo(server_info);
        closelog();
        return -1;
    }

    // allow the port to be reused shortly after restarting the server
    int reuse = 1;

    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) == -1) {

        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(server_fd);
        freeaddrinfo(server_info);
        closelog();
        return -1;
    }

    // bind the socket to TCP port 9000
    if (bind(server_fd,
             server_info->ai_addr,
             server_info->ai_addrlen) == -1) {

        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(server_fd);
        freeaddrinfo(server_info);
        closelog();
        return -1;
    }

    freeaddrinfo(server_info);

    // enter daemon mode only after successfully binding to port 9000
    if (daemon_mode) {
        pid_t pid = fork();

        if (pid < 0) {
            syslog(LOG_ERR, "fork failed: %s", strerror(errno));
            close(server_fd);
            closelog();
            return -1;
        }

        // parent exits successfully, child continues as the daemon
        if (pid > 0) {
            close(server_fd);
            closelog();
            return 0;
        }

        // create a new session and detach from the controlling terminal
        if (setsid() == -1) {
            syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
            close(server_fd);
            closelog();
            return -1;
        }

        // daemons should not depend on the directory from which they were started
        if (chdir("/") == -1) {
            syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
            close(server_fd);
            closelog();
            return -1;
        }

        // detach standard input/output/error from the terminal
        fclose(stdin);
        fclose(stdout);
        fclose(stderr);
    }

    // begin listening for connections
    if (listen(server_fd, 10) == -1) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    // create the thread which writes timestamps every 10 seconds
	pthread_t timestamp_thread_id;

	int timestamp_status =
    		pthread_create(&timestamp_thread_id,
                   NULL,
                   timestamp_thread,
                   NULL);

	if (timestamp_status != 0) {
    		syslog(LOG_ERR,
           	"pthread_create for timestamp failed: %s",
           	strerror(timestamp_status));

    		close(server_fd);
    		closelog();
    		return -1;
	}

    // continue accepting clients until SIGINT or SIGTERM
    while (!caught_signal) {

        // join and remove any client threads which have completed
	struct thread_data *thread = SLIST_FIRST(&threads);

	while (thread != NULL) {
    		struct thread_data *next = SLIST_NEXT(thread, entries);

    		if (thread->thread_complete) {
        		pthread_join(thread->thread_id, NULL);

        		SLIST_REMOVE(&threads,
                     		thread,
                     		thread_data,
                     		entries);

        		free(thread);
    		}

    		thread = next;
	}

        struct sockaddr_storage client_address;
        socklen_t client_length = sizeof(client_address);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_address,
                               &client_length);

        if (client_fd == -1) {
            if (caught_signal) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        // convert the client's address to printable form
        char client_ip[INET6_ADDRSTRLEN] = "unknown";

        if (client_address.ss_family == AF_INET) {
            struct sockaddr_in *address =
                (struct sockaddr_in *)&client_address;

            inet_ntop(AF_INET,
                      &address->sin_addr,
                      client_ip,
                      sizeof(client_ip));
        } else if (client_address.ss_family == AF_INET6) {
            struct sockaddr_in6 *address =
                (struct sockaddr_in6 *)&client_address;

            inet_ntop(AF_INET6,
                      &address->sin6_addr,
                      client_ip,
                      sizeof(client_ip));
        }

        syslog(LOG_DEBUG, "Accepted connection from %s", client_ip);

        // allocate information for the new client thread
        struct thread_data *new_thread =
            malloc(sizeof(struct thread_data));

        if (new_thread == NULL) {
            syslog(LOG_ERR, "malloc failed for client thread");
            close(client_fd);
            continue;
        }

        new_thread->client_fd = client_fd;
        new_thread->thread_complete = false;

        strncpy(new_thread->client_ip,
                client_ip,
                sizeof(new_thread->client_ip) - 1);

        new_thread->client_ip[
            sizeof(new_thread->client_ip) - 1] = '\0';

        // create a new thread to handle this client connection
        int thread_status =
            pthread_create(&new_thread->thread_id,
                           NULL,
                           handle_client,
                           new_thread);

        if (thread_status != 0) {
            syslog(LOG_ERR,
                   "pthread_create failed: %s",
                   strerror(thread_status));

            close(client_fd);
            free(new_thread);
            continue;
        }

        // add the new thread to the linked list
        SLIST_INSERT_HEAD(&threads, new_thread, entries);
    }

    syslog(LOG_DEBUG, "Caught signal, exiting");

    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }

    // request an exit from each active client thread by shutting down
    struct thread_data *thread;

    SLIST_FOREACH(thread, &threads, entries) {
        if (thread->client_fd != -1) {
            shutdown(thread->client_fd, SHUT_RDWR);
        }
    }

    // wait for all client threads to complete
    while (!SLIST_EMPTY(&threads)) {
        thread = SLIST_FIRST(&threads);

        pthread_join(thread->thread_id, NULL);

        SLIST_REMOVE_HEAD(&threads, entries);
        free(thread);
    }

    // wait for the timestamp thread to finish
    pthread_join(timestamp_thread_id, NULL);

    unlink(DATA_FILE);

    pthread_mutex_destroy(&file_mutex);

    closelog();

    return 0;
}
