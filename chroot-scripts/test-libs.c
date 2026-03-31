#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <hybris/common/binding.h>

static const char *libs[] = {
    "/apex/com.android.runtime/lib64/bionic/libc.so",
    "/apex/com.android.runtime/lib64/bionic/libm.so",
    "/vendor/lib64/libc++.so",
    "/system/lib64/liblog.so",
    "/vendor/lib64/libcutils.so",
    "/system/lib64/libnativewindow.so",
    "/system/lib64/libEGL.so",
    NULL
};

void handler(int sig) {
    const char msg[] = "SIGSEGV caught during load\n";
    write(2, msg, sizeof(msg)-1);
    _exit(1);
}

int main() {
    signal(SIGSEGV, handler);
    signal(SIGABRT, handler);

    for (int i = 0; libs[i]; i++) {
        printf("%d: %s... ", i+1, libs[i]);
        fflush(stdout);
        void *h = android_dlopen(libs[i], 1);
        if (h) {
            printf("OK (%p)\n", h);
        } else {
            printf("FAIL: %s\n", android_dlerror());
        }
        fflush(stdout);
    }

    printf("Done\n");
    return 0;
}
