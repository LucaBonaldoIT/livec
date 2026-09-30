#include <cstdio>
#include <unistd.h>
#include <ctime>
#include <iostream>

#include <livec/reflection.hpp>

#define SQRT_ITERATIONS 8

#define nameof(x) #x

float my_sqrt(float x) {
    if (x < 0.0f) return 0.0f; // or NaN

    float guess = x > 1.0f ? x : 1.0f;

    for (int i = 0; i < SQRT_ITERATIONS; i++) {
        guess = 0.5f * (guess + x / guess);
    }

    return guess;
}


auto func() {
    return 3.f;
}

int main() {


    for (;;) {
        // const int value = 2;
        // std::printf("value = %d\n", value);

        // std::clock_t start = std::clock();

        // const float s = my_sqrt(value);

        // std::clock_t end = std::clock();

        // double elapsed = static_cast<double>(end - start) / CLOCKS_PER_SEC;


        // std::printf("value = %f\n", elapsed);
        //printf("sono lucaaasda\n");
        //printf("sono aaalucaaasda\n");


        // auto addFunction = livec::function(nameof(my_sqrt));

        // std::cout << " function: " << addFunction.name << " " << addFunction.signature << '\n';


        auto funcInfo = livec::function(nameof(func));


        std::cout << func() << "(1): " << funcInfo.signature << std::endl;
        std::cout << func() << "(2): " << funcInfo.signature << std::endl;
        std::cout << func() << "(3): " << funcInfo.signature << std::endl;

        std::fflush(stdout);

        // if (value == 2) break;

        sleep(1);
    }
}
