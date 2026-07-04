#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int main() {
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned char buf[16];
    ssize_t n = read(fd, buf, sizeof(buf));
    for (ssize_t i = 0; i < n; i++) {
        printf("%02x", buf[i]);
    }
    printf("\n");
    close(fd);
    return 0;
}