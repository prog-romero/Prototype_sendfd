#include <stdio.h>
#include <stdlib.h>

int main() {
    // Disable any buffering instantly
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);

    char input_buffer[256];
    int num1 = 0;
    int num2 = 0;

    // Safely grab the data string passed by the watchdog
    if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
        if (sscanf(input_buffer, "%d %d", &num1, &num2) == 2) {
            int sum = num1 + num2;
            printf("{\n  \"status\": \"success\",\n  \"result\": %d\n}\n", sum);
            return 0;
        }
    }

    printf("{\n  \"status\": \"error\",\n  \"message\": \"Invalid input format.\"\n}\n");
    return 0;
}
