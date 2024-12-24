#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024
#define MAX_USERNAME 32

typedef struct
{
    int socket;
    char username[MAX_USERNAME];
    struct sockaddr_in addr;
    int id;
} Client;

typedef struct
{
    Client *clients[MAX_CLIENTS];
    int count;
    pthread_mutex_t mutex;
    int next_id;
} ClientList;

ClientList client_list = {
    .count = 0,
    .next_id = 1,
    .mutex = PTHREAD_MUTEX_INITIALIZER};

// 로그 출력 함수
void log_message(const char *format, ...)
{
    time_t now = time(NULL);
    char timestamp[26];
    ctime_r(&now, timestamp);
    timestamp[24] = '\0';

    va_list args;
    va_start(args, format);
    printf("[%s] ", timestamp);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

// 클라이언트 목록에서 제거
void remove_client(Client* client) {
    pthread_mutex_lock(&client_list.mutex);

    for (int i = 0; i < client_list.count; i++) {
        if (client_list.clients[i] == client) {
            log_message("Removing client %d (%s)", client->id, client->username);
            close(client->socket);
            free(client);
            client_list.clients[i] = client_list.clients[client_list.count - 1];
            client_list.clients[client_list.count - 1] = NULL;
            client_list.count--;
            break;
        }
    }

    pthread_mutex_unlock(&client_list.mutex);
}


// 메시지 브로드캐스트
void broadcast_message(const char *message, int sender_socket)
{
    pthread_mutex_lock(&client_list.mutex);
    for (int i = 0; i < client_list.count; i++)
    {
        if (client_list.clients[i] != NULL && client_list.clients[i]->socket != sender_socket)
        {
            if (send(client_list.clients[i]->socket, message, strlen(message), MSG_NOSIGNAL) < 0)
            {
                log_message("Failed to send message to client %d: %s", client_list.clients[i]->id, strerror(errno));
                close(client_list.clients[i]->socket);
                free(client_list.clients[i]);
                client_list.clients[i] = NULL;
            }
        }
    }
    pthread_mutex_unlock(&client_list.mutex);
}

// 파일 전송 처리
void handle_file_transfer(int sender_socket, const char *filename)
{
    log_message("Starting file transfer: %s", filename);
    char buffer[BUFFER_SIZE];
    FILE *file = fopen(filename, "wb");

    if (!file)
    {
        log_message("Failed to open file: %s", strerror(errno));
        return;
    }

    size_t total_received = 0;
    ssize_t bytes;
    while ((bytes = recv(sender_socket, buffer, sizeof(buffer), 0)) > 0)
    {
        fwrite(buffer, 1, bytes, file);
        total_received += bytes;
    }

    fclose(file);
    log_message("File transfer completed: %s (%zu bytes)", filename, total_received);

    char notification[BUFFER_SIZE];
    snprintf(notification, sizeof(notification), "FILE:%s is available", filename);
    broadcast_message(notification, sender_socket);
}

// 클라이언트 처리
void* handle_client(void* arg) {
    Client* client = (Client*)arg;
    char buffer[BUFFER_SIZE];

    pthread_mutex_lock(&client_list.mutex);
    client->id = client_list.next_id++;
    pthread_mutex_unlock(&client_list.mutex);

    // 사용자 이름 받기
    ssize_t bytes = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {
        log_message("Failed to receive username from client %d", client->id);
        goto cleanup;
    }

    buffer[bytes] = '\0';
    strncpy(client->username, buffer, MAX_USERNAME - 1);
    log_message("Client %d connected with username: %s", client->id, client->username);

    snprintf(buffer, sizeof(buffer), "%s has joined the chat", client->username);
    broadcast_message(buffer, client->socket);

    while ((bytes = recv(client->socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';

        if (strncmp(buffer, "FILE:", 5) == 0) {
            handle_file_transfer(client->socket, buffer + 5);
        } else {
            char msg[BUFFER_SIZE];
            snprintf(msg, sizeof(msg), "%s: %s", client->username, buffer);
            broadcast_message(msg, client->socket);
            log_message("Message from %s: %s", client->username, buffer);
        }
    }

cleanup:
    snprintf(buffer, sizeof(buffer), "%s has left the chat", client->username);
    broadcast_message(buffer, client->socket);
    remove_client(client);
    return NULL;
}


// 메인 함수
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <port>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Socket creation failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(atoi(argv[1]))};

    bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_socket, 5);

    log_message("Server started on port %s", argv[1]);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

        Client *new_client = malloc(sizeof(Client));
        new_client->socket = client_socket;
        new_client->addr = client_addr;

        pthread_mutex_lock(&client_list.mutex);

        if (client_list.count < MAX_CLIENTS)
        {
            client_list.clients[client_list.count++] = new_client;
            log_message("Client connected. Total clients: %d", client_list.count);
        }
        else
        {
            log_message("Maximum clients reached. Connection rejected.");
            close(new_client->socket);
            free(new_client);
        }

        pthread_mutex_unlock(&client_list.mutex);

        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, new_client);
        pthread_detach(thread);
    }

    return 0;
}
