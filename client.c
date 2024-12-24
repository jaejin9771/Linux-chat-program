#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <libgen.h>

#define BUFFER_SIZE 1024

typedef struct
{
    GtkWidget *text_view;
    GtkWidget *entry;
    GtkTextBuffer *buffer;
    int socket;
    char username[32];
    gboolean is_connected;
} ChatClient;

ChatClient client;

// UI 업데이트를 위한 뮤텍스
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

void show_error_dialog(const char *message)
{
    GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_OK,
                                               "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

gboolean append_text_safe(gpointer data)
{
    char *text = (char *)data;

    pthread_mutex_lock(&ui_mutex);
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(client.buffer, &iter);
    gtk_text_buffer_insert(client.buffer, &iter, text, -1);
    gtk_text_buffer_insert(client.buffer, &iter, "\n", -1);

    // 자동 스크롤
    GtkTextMark *mark = gtk_text_buffer_get_insert(client.buffer);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(client.text_view), mark, 0.0, TRUE, 0.0, 1.0);
    pthread_mutex_unlock(&ui_mutex);

    g_free(text);
    return G_SOURCE_REMOVE;
}

void append_text(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char buffer[BUFFER_SIZE];
    vsnprintf(buffer, BUFFER_SIZE, format, args);
    va_end(args);

    gdk_threads_add_idle(append_text_safe, g_strdup(buffer));
}

void send_message(GtkWidget *widget, gpointer data)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(client.entry));
    if (strlen(text) > 0)
    {
        printf("Sending message: %s\n", text); // 디버그 출력
        ssize_t sent = send(client.socket, text, strlen(text), MSG_NOSIGNAL);
        if (sent < 0)
        {
            append_text("Error sending message: %s", strerror(errno));
            client.is_connected = FALSE;
            gtk_main_quit();
        }
        else
        {
            printf("Sent %zd bytes\n", sent); // 디버그 출력
            append_text("Me: %s", text);
        }
        gtk_entry_set_text(GTK_ENTRY(client.entry), "");
    }
}

void send_file(GtkWidget *widget, gpointer data)
{
    printf("Opening file chooser dialog\n"); // 디버그 출력

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Choose a file",
                                                    GTK_WINDOW(data),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        printf("Selected file: %s\n", filename); // 디버그 출력

        FILE *file = fopen(filename, "rb");
        if (file)
        {
            // Get file size
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            fseek(file, 0, SEEK_SET);

            printf("File size: %ld bytes\n", file_size); // 디버그 출력

            // Send file header
            char header[BUFFER_SIZE];
            sprintf(header, "FILE:%s:%ld", basename(filename), file_size);
            printf("Sending file header: %s\n", header); // 디버그 출력

            if (send(client.socket, header, strlen(header), 0) < 0)
            {
                printf("Error sending file header: %s\n", strerror(errno));
                append_text("Error sending file: %s", strerror(errno));
                fclose(file);
                g_free(filename);
                gtk_widget_destroy(dialog);
                return;
            }

            // Wait a bit for the server to prepare
            usleep(100000); // 100ms

            // Send file content
            char buffer[BUFFER_SIZE];
            size_t total_sent = 0;
            size_t bytes_read;

            while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0)
            {
                ssize_t bytes_sent = send(client.socket, buffer, bytes_read, 0);
                if (bytes_sent < 0)
                {
                    printf("Error sending file data: %s\n", strerror(errno));
                    append_text("Error sending file: %s", strerror(errno));
                    break;
                }
                total_sent += bytes_sent;
                printf("Sent %zd bytes, total: %zu/%ld\n", bytes_sent, total_sent, file_size);

                // Update progress in UI
                double progress = (double)total_sent / file_size * 100;
                append_text("Sending file: %.1f%%", progress);
            }

            fclose(file);
            append_text("File sent successfully: %s", basename(filename));
            printf("File transfer complete\n"); // 디버그 출력
        }
        else
        {
            printf("Error opening file: %s\n", strerror(errno)); // 디버그 출력
            append_text("Error opening file: %s", strerror(errno));
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void *receive_messages(void *arg)
{
    char buffer[BUFFER_SIZE];
    printf("Message receiving thread started\n");

    while (client.is_connected)
    {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client.socket, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';
            printf("Received data: %s\n", buffer); // 디버그 출력

            // Check if it's a file
            if (strncmp(buffer, "FILE:", 5) == 0)
            {
                char *filename = buffer + 5;
                char *filesize_str = strchr(filename, ':');

                if (filesize_str)
                {
                    *filesize_str = '\0';
                    filesize_str++;
                    long file_size = atol(filesize_str);

                    printf("Receiving file: %s (%ld bytes)\n", filename, file_size);
                    append_text("Receiving file: %s (%ld bytes)", filename, file_size);

                    // Open file for writing
                    FILE *file = fopen(filename, "wb");
                    if (file)
                    {
                        size_t total_received = 0;
                        while (total_received < file_size)
                        {
                            bytes_received = recv(client.socket, buffer,
                                                  MIN(BUFFER_SIZE, file_size - total_received), 0);
                            if (bytes_received <= 0)
                                break;

                            fwrite(buffer, 1, bytes_received, file);
                            total_received += bytes_received;

                            // Update progress
                            double progress = (double)total_received / file_size * 100;
                            append_text("Receiving file: %.1f%%", progress);
                        }
                        fclose(file);
                        append_text("File received successfully: %s", filename);
                    }
                    else
                    {
                        append_text("Error creating file: %s", strerror(errno));
                    }
                }
            }
            else
            {
                append_text("%s", buffer);
            }
        }
        else if (bytes_received == 0)
        {
            append_text("Server closed the connection.");
            client.is_connected = FALSE;
            gtk_main_quit();
            break;
        }
        else
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                append_text("Error receiving message: %s", strerror(errno));
                client.is_connected = FALSE;
                gtk_main_quit();
                break;
            }
        }
    }
    return NULL;
}

void activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Chat Client");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 600);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), box);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);

    client.text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(client.text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(client.text_view), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled), client.text_view);
    client.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(client.text_view));

    GtkWidget *input_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(box), input_box, FALSE, FALSE, 0);

    client.entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(input_box), client.entry, TRUE, TRUE, 0);
    g_signal_connect(client.entry, "activate", G_CALLBACK(send_message), NULL); // 엔터키로도 전송 가능

    GtkWidget *send_button = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(input_box), send_button, FALSE, FALSE, 0);
    g_signal_connect(send_button, "clicked", G_CALLBACK(send_message), NULL);

    GtkWidget *file_button = gtk_button_new_with_label("Send File");
    gtk_box_pack_start(GTK_BOX(input_box), file_button, FALSE, FALSE, 0);
    g_signal_connect(file_button, "clicked", G_CALLBACK(send_file), window);

    gtk_widget_show_all(window);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        printf("Usage: %s <username> <server_ip> <port>\n", argv[0]);
        return 1;
    }

    client.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client.socket < 0)
    {
        printf("Socket creation failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(atoi(argv[3]))};

    if (inet_pton(AF_INET, argv[2], &server_addr.sin_addr) <= 0)
    {
        printf("Invalid address: %s\n", strerror(errno));
        return 1;
    }

    printf("Connecting to server %s:%s...\n", argv[2], argv[3]);
    if (connect(client.socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        printf("Connection failed: %s\n", strerror(errno));
        return 1;
    }

    client.is_connected = TRUE;
    strncpy(client.username, argv[1], sizeof(client.username) - 1);
    printf("Connected to server. Sending username: %s\n", client.username);

    if (send(client.socket, client.username, strlen(client.username), 0) < 0)
    {
        printf("Failed to send username: %s\n", strerror(errno));
        return 1;
    }

    pthread_t receive_thread;
    if (pthread_create(&receive_thread, NULL, receive_messages, NULL) != 0)
    {
        printf("Failed to create receive thread: %s\n", strerror(errno));
        return 1;
    }

    GtkApplication *app = gtk_application_new("org.gtk.example", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    printf("Starting GTK application...\n");
    int status = g_application_run(G_APPLICATION(app), 0, NULL);

    client.is_connected = FALSE;
    close(client.socket);
    pthread_join(receive_thread, NULL);
    g_object_unref(app);

    return status;
}