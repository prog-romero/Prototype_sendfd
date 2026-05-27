#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 5000
#define BUFFER_SIZE 16384

int main() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 10);

    while (1) {
        client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_socket < 0) continue;

        char buffer[BUFFER_SIZE] = {0};
        int total_read = 0;

        // STREAM READER LOOP: Keep reading until the complete HTTP chunked payload finishes
        while (1) {
            int bytes_read = read(client_socket, buffer + total_read, BUFFER_SIZE - total_read - 1);
            if (bytes_read <= 0) break;

            total_read += bytes_read;
            buffer[total_read] = '\0';

            // Check if we have received the complete chunked data trailing marker: "0\r\n\r\n"
            if (strstr(buffer, "0\r\n\r\n") != NULL) {
                break;
            }
            
            // Safety fallback: if it's not chunked but has a body, break when reading slows down
            if (strstr(buffer, "\r\n\r\n") != NULL && bytes_read < 10) {
                // Wait briefly for trailing packets before giving up
                usleep(1000); 
                break;
            }
        }

        // 1. Isolate the headers from the payload
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (body_start != NULL) {
            body_start += 4; // Advance past \r\n\r\n

            char *real_data = body_start;

            // 2. PARSE CHUNK METADATA: Step past the chunk-size line (e.g. "7\r\n")
            if (body_start[0] >= '0' && body_start[0] <= '9') {
                char *next_line = strstr(body_start, "\r\n");
                if (next_line != NULL) {
                    real_data = next_line + 2; // Jump directly to the raw payload data
                }
            }

            int num1 = 0, num2 = 0;
            char response_body[512];

            // 3. Compute our function payload numbers
            if (sscanf(real_data, "%d %d", &num1, &num2) >= 2) {
                int sum = num1 + num2;
                sprintf(response_body, "{\n  \"status\": \"success\",\n  \"result\": %d\n}\n", sum);
            } else {
                sprintf(response_body, 
                        "{\n  \"status\": \"error\",\n  \"message\": \"Parsing failed.\",\n  \"debug_raw_segment\": \"%s\"\n}\n", 
                        real_data);
            }

            // 4. Return standard HTTP/1.1 response packet
            char http_response[2048];
            sprintf(http_response,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %ld\r\n"
                    "Connection: close\r\n\r\n"
                    "%s",
                    strlen(response_body), response_body);

            write(client_socket, http_response, strlen(http_response));
        }
        close(client_socket);
    }
    return 0;
}
