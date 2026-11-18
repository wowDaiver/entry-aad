// Standalone, bounded diagnostic source check. Never linked into the plugin.
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "egs_reader.hpp"
static double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main() {
    telemetry::EgsReader r;
    double begin = now();
    r.start(begin);
    while (now() - begin < 9) {
        r.poll(now());
        double v = 0;
        bool ok = false;
        if (r.take(v, ok)) {
            printf("EGS check: valid=%d temperature_C=%.1f\n", ok, v);
            r.stop();
            return ok ? 0 : 1;
        }
        usleep(50000);
    }
    r.stop();
    puts("EGS check: absolute timeout");
    return 2;
}
