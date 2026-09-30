#include <cstdio>
#include <unistd.h>

/* Keep state in this stable translation unit so it survives tick.c reloads. */
int counter;

int tick();

int main() {
    int input;
    while ((input = std::getchar()) != EOF) {
        if (input == '\n') continue;
        std::printf("%d\n", tick());
        std::fflush(stdout);
    }
    return 0;
}
