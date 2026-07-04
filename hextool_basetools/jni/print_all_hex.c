#include <unistd.h>

int main() {
    unsigned char bytes[256];
    for (int i = 0; i < 256; i++) {
        bytes[i] = (unsigned char)i;
    }
    write(1, bytes, 256);
    return 0;
}