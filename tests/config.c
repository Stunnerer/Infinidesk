#define _POSIX_C_SOURCE 200809L
#include "infinidesk/config.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char config_path[512];
static void write_config(const char *text) {
    FILE *file = fopen(config_path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

int main(void) {
    char directory[] = "/tmp/infinidesk-config-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(setenv("XDG_CONFIG_HOME", directory, 1) == 0);
    snprintf(config_path, sizeof(config_path), "%s/infinidesk", directory);
    assert(mkdir(config_path, 0700) == 0);
    snprintf(config_path, sizeof(config_path), "%s/infinidesk/infinidesk.toml",
             directory);
    struct infinidesk_config config;
    assert(config_load(&config));
    assert(config.keybind_count == 10);
    config_free(&config);

    const char *invalid[] = {"nan", "inf", "0", "-1", "1e30", "1.5junk"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char text[128];
        snprintf(text, sizeof(text), "scale = %s\n", invalid[i]);
        write_config(text);
        assert(config_load(&config));
        assert(config.scale == 1.0f);
        assert(config.keybind_count == 10);
        config_free(&config);
    }
    write_config("scale = 1.5 # valid\nstartup = [\"one\", \"two\",]\n"
                 "[scroll] # comment\nwheel_speed = 2\n"
                 "[snapping] # comment\nwindow_edges = 0\n"
                 "[keybinds]\n\"super + q\" = \"close_window\"\n");
    assert(config_load(&config));
    assert(config.scale == 1.5f && config.wheel_speed == 2 &&
           config.snap_window_px == 0);
    assert(config.startup_command_count == 2);
    assert(strcmp(config.startup_commands[1], "two") == 0);
    assert(config.keybind_count == 1);
    config_free(&config);

    const char *malformed[] = {
        "startup = [\"unterminated\n",       "startup = [\"one\"\n",
        "startup = [\"one\" \"two\"]\n",     "startup = [42]\n",
        "startup = [\"one\", \"bad\\z\"]\n",
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        write_config(malformed[i]);
        assert(!config_load(&config));
        assert(config.startup_command_count == 0 && config.keybind_count == 10);
        config_free(&config);
    }
    assert(unlink(config_path) == 0);
    assert(mkdir(config_path, 0700) == 0);
    assert(!config_load(&config));
    assert(config.keybind_count == 10);
    config_free(&config);
    assert(rmdir(config_path) == 0);
    snprintf(config_path, sizeof(config_path), "%s/infinidesk", directory);
    assert(rmdir(config_path) == 0);
    assert(rmdir(directory) == 0);

    /* Shell children must not inherit Wayland's blocked termination signals. */
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    assert(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
    config_run_command("kill -TERM $$; exit 42");
    int status;
    assert(wait(&status) > 0);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
    return 0;
}
