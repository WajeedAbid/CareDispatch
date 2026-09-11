#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 5050

static int write_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t bytes = write(fd, data + sent, length - sent);
        if (bytes <= 0) return -1;
        sent += (size_t)bytes;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int fd;
    struct sockaddr_in server;
    char request[256];
    char response[128];
    ssize_t bytes;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s FALL|MEDICINE|SERVICE NAME MESSAGE\n", argv[0]);
        return EXIT_FAILURE;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) == -1) {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }
    snprintf(request, sizeof(request), "%s|%s|%s\n", argv[1], argv[2], argv[3]);
    if (write_all(fd, request, strlen(request)) == -1) {
        perror("write");
        close(fd);
        return EXIT_FAILURE;
    }
    bytes = read(fd, response, sizeof(response) - 1);
    if (bytes > 0) {
        response[bytes] = '\0';
        printf("%s", response);
    }
    close(fd);
    return EXIT_SUCCESS;
}
