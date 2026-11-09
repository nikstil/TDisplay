// The executable inside the development build/TDisplay.app. It only starts .build/release/TDisplay with the same
// arguments and waits for it. macOS ties the Screen Recording permission to the app's signature, and an ad-hoc
// signature changes with every build; the program started from here inherits this launcher's permission, so it can be
// rebuilt freely while the launcher, and the permission given to it, stay the same. Step 5 replaces all this with a
// properly signed app.
#include <CoreGraphics/CoreGraphics.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

int main(int argc, char **argv) {
    // macOS only adds an app to the Screen Recording list when the app itself asks, not a program on its behalf.
    if (!CGPreflightScreenCaptureAccess()) CGRequestScreenCaptureAccess();
    char self[4096];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0) return 1;
    // Contents/MacOS/launcher in build/TDisplay.app, four levels below mac/app.
    char program[4096];
    snprintf(program, sizeof program, "%s/../../../../.build/release/TDisplay", dirname(self));
    char resolved[4096];
    if (!realpath(program, resolved)) {
        fprintf(stderr, "no program at %s; run ./build.sh\n", program);
        return 1;
    }
    argv[0] = resolved;
    pid_t pid;
    int error = posix_spawn(&pid, resolved, NULL, NULL, argv, environ);
    if (error) {
        fprintf(stderr, "could not start %s: %s\n", resolved, strerror(error));
        return 1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
