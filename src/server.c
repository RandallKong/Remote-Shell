#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void      setup_signal_handler(void);
static void      sigint_handler(int signum);
static void      parse_arguments(int argc, char *argv[], char **ip_address, char **port);
static void      handle_arguments(const char *ip_address, const char *port_str, in_port_t *port);
static in_port_t parse_in_port_t(const char *port_str);
static void      convert_address(const char *address, struct sockaddr_storage *addr);
static int       socket_create(int domain, int type, int protocol);
static void      socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port);
static void      start_listening(int server_fd, int backlog);
static int       socket_accept_connection(int server_fd, struct sockaddr_storage *client_addr, socklen_t *client_addr_len);
static void      socket_close(int sockfd);

static void *handle_client(void *arg);
static void  start_server(struct sockaddr_storage addr, in_port_t port);

static void execute_command(char *buffer, int client_socket);
static void parse_command(char *buffer, char *args[]);

pid_t create_child_process(void);
void  await_child_process(void);

void redirect_output(int fd);
void restore_output(void);

#define BASE_TEN 10
#define MAX_CLIENTS 32
#define BUFFER_SIZE 10000
//#define UINT16_MAX 65535
#define MAX_ARGS 100

struct ClientInfo
{
    int client_socket;
    int client_index;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int original_stdout;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int original_stdin;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int original_stderr;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
// static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int clients[MAX_CLIENTS] = {0};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile sig_atomic_t exit_flag = 0;

int main(int argc, char *argv[])
{
    in_port_t               port;
    char                   *address;
    char                   *port_str;
    struct sockaddr_storage addr;

    address  = NULL;
    port_str = NULL;

    parse_arguments(argc, argv, &address, &port_str);
    handle_arguments(address, port_str, &port);
    convert_address(address, &addr);

    start_server(addr, port);

    return 0;
}

void *handle_client(void *arg)
{
    char               buffer[BUFFER_SIZE];
    struct ClientInfo *client_info   = (struct ClientInfo *)arg;
    int                client_socket = client_info->client_socket;
    int                client_index  = client_info->client_index;

    while(!exit_flag)
    {
        char   *newline_pos;
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if(bytes_received <= 0)
        {
            clients[client_index] = 0;
            close(client_socket);
            clients[client_index] = 0;
            free(client_info);
            pthread_exit(NULL);
        }

        //        pthread_mutex_lock(&mutex);

        newline_pos = strchr(buffer, '\n');
        if(newline_pos != NULL)
        {
            *newline_pos = '\0';
        }

        restore_output();
        printf("Received from Client %d: %s\n", client_socket, buffer);

        redirect_output(client_socket);
        execute_command(buffer, client_socket);
        restore_output();

        //        pthread_mutex_unlock(&mutex);
    }

    return NULL;
}

void redirect_output(int fd)
{
    // Save the original file descriptors
    original_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC);
    original_stdin = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC);
    original_stderr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC);

    // Duplicate the file descriptor for stdout to fd
    if(dup2(fd, STDOUT_FILENO) == -1)
    {
        perror("dup2");
        return;
    }

    if(dup2(fd, STDIN_FILENO) == -1)
    {
        perror("dup2");
        return;
    }

    // Duplicate the file descriptor for stderr to fd
    if(dup2(fd, STDERR_FILENO) == -1)
    {
        perror("dup2");
        return;
    }
}

void restore_output(void)
{
    // Duplicate the original file descriptors back to stdout and stderr
    if(dup2(original_stdout, STDOUT_FILENO) == -1)
    {
        perror("dup2");
        return;
    }

    if(dup2(original_stderr, STDERR_FILENO) == -1)
    {
        perror("dup2");
        return;
    }

    if(dup2(original_stdin, STDIN_FILENO) == -1)
    {
        perror("dup2");
        return;
    }

    //    // Close the duplicated file descriptors
    //    close(original_stdout);
    //    close(original_stderr);
}

static void execute_command(char *buffer, int client_socket)
{
    pid_t childPid;
    char *args[MAX_ARGS];

    parse_command(buffer, args);

    childPid = create_child_process();

    (void)client_socket;

    if(childPid == 0)
    {
        // Construct the full path to the command
        char *path = getenv("PATH");
        char *saveptr;
        char *token = strtok_r(path, ":", &saveptr);
        char  full_path[BUFFER_SIZE];    // Adjust the size as needed

        while(token != NULL)
        {
            snprintf(full_path, sizeof(full_path), "%s/%s", token, args[0]);
            if(access(full_path, X_OK) == 0)
            {
                // Execute the command with execv
                execv(full_path, args);
                // If execv returns, an error occurred
                perror("execv");
                exit(EXIT_FAILURE);
            }
            token = strtok_r(NULL, ":", &saveptr);
        }

        // If the command is not found
        fprintf(stderr, "Command not found: %s\n", args[0]);
        exit(EXIT_FAILURE);
    }
    else
    {
        await_child_process();
    }
}

static void parse_command(char *buffer, char *args[])
{
    char *saveptr;
    char *token;
    int   counter = 0;

    token = strtok_r(buffer, " ", &saveptr);

    while(token != NULL && counter < MAX_ARGS - 1)
    {
        args[counter] = token;
        counter++;
        token = strtok_r(NULL, " ", &saveptr);
    }

    args[counter] = NULL;    // Null-terminate the array
}

pid_t create_child_process(void)
{
    pid_t childPid = fork();

    if(childPid == -1)
    {
        perror("Error creating child process");
        exit(EXIT_FAILURE);
    }

    return childPid;
}

void await_child_process(void)
{
    int child;

    wait(&child);
}

static void start_server(struct sockaddr_storage addr, in_port_t port)
{
    int                     server_socket;
    struct sockaddr_storage client_addr;
    socklen_t               client_addr_len;
    pthread_t               tid;

    server_socket = socket_create(addr.ss_family, SOCK_STREAM, 0);
    socket_bind(server_socket, &addr, port);
    start_listening(server_socket, BASE_TEN);
    setup_signal_handler();

    while(!exit_flag)
    {
        int    max_sd;
        int    activity;
        fd_set readfds;
        memset(&readfds, 0, sizeof(readfds));
        FD_SET(server_socket, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        max_sd = server_socket;
        for(int i = 0; i < MAX_CLIENTS; ++i)
        {
            if(clients[i] > 0)
            {
                FD_SET(clients[i], &readfds);
                if(clients[i] > max_sd)
                {
                    max_sd = clients[i];
                }
            }
        }

        // Wait for activity on one of the sockets
        activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);
        if(activity == -1)
        {
            //            perror("select");
            continue;    // Keep listening for connections
        }

        // New connection
        if(FD_ISSET(server_socket, &readfds))
        {
            int                client_socket;
            int                client_index = -1;
            struct ClientInfo *client_info;
            client_addr_len = sizeof(client_addr);
            client_socket   = socket_accept_connection(server_socket, &client_addr, &client_addr_len);
            if(client_socket == -1)
            {
                continue;    // Continue listening for connections
            }
            for(int i = 0; i < MAX_CLIENTS; ++i)
            {
                if(clients[i] == 0)
                {
                    client_index = i;
                    break;
                }
            }

            if(client_index == -1)
            {
                fprintf(stderr, "Too many clients. Connection rejected.\n");
                close(client_socket);
                continue;    // Continue listening for connections
            }

            printf("New connection from %s:%d, assigned to Client %d\n", inet_ntoa(((struct sockaddr_in *)&client_addr)->sin_addr), ntohs(((struct sockaddr_in *)&client_addr)->sin_port), client_index);

            clients[client_index] = client_socket;

            // Create a structure to hold client information
            client_info = malloc(sizeof(struct ClientInfo));
            if(client_info == NULL)
            {
                perror("Memory allocation failed");
                close(client_socket);
                continue;    // Continue listening for connections
            }

            client_info->client_socket = client_socket;
            client_info->client_index  = client_index;

            // Create a new thread to handle the client
            if(pthread_create(&tid, NULL, handle_client, (void *)client_info) != 0)
            {
                perror("Thread creation failed");
                close(client_socket);
                free(client_info);
                continue;
            }

            pthread_detach(tid);
        }

        // Check if there is input from the server's console
        //        if(FD_ISSET(STDIN_FILENO, &readfds))
        //        {
        //            char server_buffer[BUFFER_SIZE];
        //            fgets(server_buffer, sizeof(server_buffer), stdin);
        //
        //            // Broadcast the server's message to all connected clients
        //            for(int i = 0; i < MAX_CLIENTS; ++i)
        //            {
        //                if(clients[i] != 0)
        //                {
        //                    send(clients[i], server_buffer, strlen(server_buffer), 0);
        //                    printf("%d <-------- %s", clients[i], server_buffer);
        //                }
        //            }
        //        }
    }

    // Close server socket
    shutdown(server_socket, SHUT_RDWR);
    socket_close(server_socket);
}

static void parse_arguments(int argc, char *argv[], char **ip_address, char **port)
{
    if(argc == 3)
    {
        *ip_address = argv[1];
        *port       = argv[2];
    }
    else
    {
        printf("invalid num args\n");
        printf("usage: ./server [ip addr] [port]\n");
        exit(EXIT_FAILURE);
    }
}

static void handle_arguments(const char *ip_address, const char *port_str, in_port_t *port)
{
    if(ip_address == NULL)
    {
        printf("ip is null\n");
        exit(EXIT_FAILURE);
    }

    if(port_str == NULL)
    {
        printf("port str is null\n");
        exit(EXIT_FAILURE);
    }

    *port = parse_in_port_t(port_str);
}

in_port_t parse_in_port_t(const char *str)
{
    char     *endptr;
    uintmax_t parsed_value;

    errno        = 0;
    parsed_value = strtoumax(str, &endptr, BASE_TEN);

    if(errno != 0)
    {
        perror("Error parsing in_port_t\n");
        exit(EXIT_FAILURE);
    }

    if(*endptr != '\0')
    {
        printf("non-numerics inside port\n");
        exit(EXIT_FAILURE);
    }

    if(parsed_value > UINT16_MAX)
    {
        printf("port out of range\n");
        exit(EXIT_FAILURE);
    }

    return (in_port_t)parsed_value;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

static void sigint_handler(int signum)
{
    exit_flag = 1;
}

#pragma GCC diagnostic pop

static void convert_address(const char *address, struct sockaddr_storage *addr)
{
    memset(addr, 0, sizeof(*addr));

    if(inet_pton(AF_INET, address, &(((struct sockaddr_in *)addr)->sin_addr)) == 1)
    {
        addr->ss_family = AF_INET;
    }
    else if(inet_pton(AF_INET6, address, &(((struct sockaddr_in6 *)addr)->sin6_addr)) == 1)
    {
        addr->ss_family = AF_INET6;
    }
    else
    {
        fprintf(stderr, "%s is not an IPv4 or an IPv6 address\n", address);
        exit(EXIT_FAILURE);
    }
}

static int socket_create(int domain, int type, int protocol)
{
    int sockfd;
    int opt = 1;

    sockfd = socket(domain, type, protocol);

    if(sockfd == -1)
    {
        perror("Socket creation failed\n");
        exit(EXIT_FAILURE);
    }

    if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

static void socket_bind(int sockfd, struct sockaddr_storage *addr, in_port_t port)
{
    char      addr_str[INET6_ADDRSTRLEN];
    socklen_t addr_len;
    void     *vaddr;
    in_port_t net_port;

    net_port = htons(port);

    if(addr->ss_family == AF_INET)
    {
        struct sockaddr_in *ipv4_addr;

        ipv4_addr           = (struct sockaddr_in *)addr;
        addr_len            = sizeof(*ipv4_addr);
        ipv4_addr->sin_port = net_port;
        vaddr               = (void *)&(((struct sockaddr_in *)addr)->sin_addr);
    }
    else if(addr->ss_family == AF_INET6)
    {
        struct sockaddr_in6 *ipv6_addr;

        ipv6_addr            = (struct sockaddr_in6 *)addr;
        addr_len             = sizeof(*ipv6_addr);
        ipv6_addr->sin6_port = net_port;
        vaddr                = (void *)&(((struct sockaddr_in6 *)addr)->sin6_addr);
    }
    else
    {
        fprintf(stderr,
                "Internal error: addr->ss_family must be AF_INET or AF_INET6, was: "
                "%d\n",
                addr->ss_family);
        exit(EXIT_FAILURE);
    }

    if(inet_ntop(addr->ss_family, vaddr, addr_str, sizeof(addr_str)) == NULL)
    {
        perror("inet_ntop\n");
        exit(EXIT_FAILURE);
    }

    printf("Binding to: %s:%u\n", addr_str, port);

    if(bind(sockfd, (struct sockaddr *)addr, addr_len) == -1)
    {
        perror("Binding failed");
        fprintf(stderr, "Error code: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    printf("Bound to socket: %s:%u\n", addr_str, port);
}

static void start_listening(int server_fd, int backlog)
{
    if(listen(server_fd, backlog) == -1)
    {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Listening for incoming connections...\n");
}

static int socket_accept_connection(int server_fd, struct sockaddr_storage *client_addr, socklen_t *client_addr_len)
{
    int  client_fd;
    char client_host[NI_MAXHOST];
    char client_service[NI_MAXSERV];

    errno     = 0;
    client_fd = accept(server_fd, (struct sockaddr *)client_addr, client_addr_len);

    if(client_fd == -1)
    {
        if(errno != EINTR)
        {
            perror("accept failed\n");
        }

        return -1;
    }

    if(getnameinfo((struct sockaddr *)client_addr, *client_addr_len, client_host, NI_MAXHOST, client_service, NI_MAXSERV, 0) == 0)
    {
        // printf("Received new request from -> %s:%s\n\n", client_host, client_service);
    }
    else
    {
        printf("Unable to get client information\n");
    }

    return client_fd;
}

static void setup_signal_handler(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
    sa.sa_handler = sigint_handler;
#if defined(__clang__)
    #pragma clang diagnostic pop
#endif

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if(sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static void socket_close(int sockfd)
{
    if(close(sockfd) == -1)
    {
        perror("Error closing socket\n");
        exit(EXIT_FAILURE);
    }
}
