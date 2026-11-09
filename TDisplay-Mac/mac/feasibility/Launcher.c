// The executable inside TDisplayCaptureTest.app. It only starts .build/release/tdisplay-feasibility with the same
// arguments and waits for it. macOS ties the Screen Recording permission to the app's signature, and an ad-hoc
// signature changes with every build; the tool started from here inherits the app's permission, so the tool can be
// rebuilt freely while this launcher, and the permission given to it, stay the same.
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
    // macOS only adds the app to the Screen Recording list when the app itself asks, not the tool on its behalf.
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
        fprintf(stderr, "no Screen Recording permission: turn on TDisplay Capture Test under System Settings > Privacy & Security > "
                        "Screen & System Audio Recording, then run it again\n");
        return 3;
    }
    char self[4096];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0) return 1;
    // Contents/MacOS/launcher in build/TDisplayCaptureTest.app, four levels below mac/feasibility.
    char tool[4096];
    snprintf(tool, sizeof tool, "%s/../../../../.build/release/tdisplay-feasibility", dirname(self));
    char resolved[4096];
    if (!realpath(tool, resolved)) {
        fprintf(stderr, "no tool at %s; run swift build -c release\n", tool);
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
