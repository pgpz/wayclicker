#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#define ERRBUF_SIZE 128
#define MAX_EINTR_RETRIES 100
#define MAX_INPUT_DEVICES 64

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t clicking = 0;

static int uinput_fd = -1;
static int uinput_created = 0;

static int input_fds[MAX_INPUT_DEVICES];
static int input_count = 0;

static const char *errno_message(int errnum, char *buf, size_t size)
{
    if (size == 0)
        return "unknown error";

    if (strerror_r(errnum, buf, size) == 0)
        return buf;

    snprintf(buf, size, "errno %d", errnum);
    return buf;
}

static int retry_ioctl0(int fd, unsigned long request)
{
    int retries = 0;

    for (;;) {
        int rc = ioctl(fd, request);

        if (rc >= 0 || errno != EINTR)
            return rc;

        if (++retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

static int retry_ioctl_ulong(int fd, unsigned long request,
                             unsigned long arg)
{
    int retries = 0;

    for (;;) {
        int rc = ioctl(fd, request, arg);

        if (rc >= 0 || errno != EINTR)
            return rc;

        if (++retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

static int retry_ioctl_ptr(int fd, unsigned long request, void *arg)
{
    int retries = 0;

    for (;;) {
        int rc = ioctl(fd, request, arg);

        if (rc >= 0 || errno != EINTR)
            return rc;

        if (++retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

static void cleanup_input_devices(void)
{
    for (int i = 0; i < input_count; i++) {
        if (input_fds[i] >= 0)
            close(input_fds[i]);

        input_fds[i] = -1;
    }

    input_count = 0;
}

static void cleanup_uinput(void)
{
    if (uinput_fd < 0)
        return;

    if (uinput_created) {
        retry_ioctl0(uinput_fd, UI_DEV_DESTROY);
        uinput_created = 0;
    }

    close(uinput_fd);
    uinput_fd = -1;
}

static void cleanup_all(void)
{
    cleanup_input_devices();
    cleanup_uinput();
}

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);

    cleanup_all();
    exit(EXIT_FAILURE);
}

static void die_errno(const char *message, int errnum)
{
    char errbuf[ERRBUF_SIZE];

    die("%s: %s",
        message,
        errno_message(errnum, errbuf, sizeof(errbuf)));
}

static void die_open_uinput(int errnum)
{
    char errbuf[ERRBUF_SIZE];

    die("Cannot open /dev/uinput: %s\n"
        "  → run as root, or add yourself to the 'input' group and\n"
        "    create /etc/udev/rules.d/99-uinput.rules containing:\n"
        "      KERNEL==\"uinput\", GROUP=\"input\", MODE=\"0660\"",
        errno_message(errnum, errbuf, sizeof(errbuf)));
}

static void handle_exit_signal(int sig)
{
    (void)sig;
    running = 0;
}

static long parse_long(const char *value, const char *name)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0')
        die("Invalid %s: '%s'", name, value);

    return parsed;
}

static int long_fits_time_t(long value)
{
    time_t converted = (time_t)value;

    return (long)converted == value;
}

static void parse_position(const char *value, long *x, long *y)
{
    char *end = NULL;

    errno = 0;
    *x = strtol(value, &end, 10);

    if (errno != 0 || end == value || *end != ',')
        die("Invalid position '%s' (expected X,Y)", value);

    value = end + 1;

    while (*value == ' ' || *value == '\t')
        value++;

    errno = 0;
    *y = strtol(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0')
        die("Invalid position '%s' (expected X,Y)", value);
}

static int get_cursor_position(long *x, long *y)
{
    char line[128];
    FILE *pipe;
    int status;
    char *newline;
    char *end = NULL;
    char *y_text;

    pipe = popen("hyprctl cursorpos", "r");

    if (pipe == NULL) {
        fprintf(stderr,
                "Cannot run hyprctl cursorpos: %s\n",
                strerror(errno));
        return -1;
    }

    if (fgets(line, sizeof(line), pipe) == NULL) {
        status = pclose(pipe);

        if (running)
            fprintf(stderr,
                    "hyprctl cursorpos returned no position "
                    "(status %d)\n",
                    status);

        return -1;
    }

    status = pclose(pipe);

    if (status == -1 ||
        !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {

        if (running)
            fprintf(stderr, "hyprctl cursorpos failed\n");

        return -1;
    }

    newline = strchr(line, '\n');

    if (newline != NULL)
        *newline = '\0';

    errno = 0;
    *x = strtol(line, &end, 10);

    if (errno != 0 || end == line || *end != ',') {
        fprintf(stderr,
                "Unexpected hyprctl cursorpos output: '%s'\n",
                line);
        return -1;
    }

    y_text = end + 1;

    while (*y_text == ' ' || *y_text == '\t')
        y_text++;

    errno = 0;
    *y = strtol(y_text, &end, 10);

    if (errno != 0 ||
        end == y_text ||
        (*end != '\0' && *end != '\r')) {

        fprintf(stderr,
                "Unexpected hyprctl cursorpos output: '%s'\n",
                line);
        return -1;
    }

    return 0;
}

static int move_cursor_to(long x, long y)
{
    char x_text[32];
    char y_text[32];

    pid_t pid;
    int status;

    snprintf(x_text, sizeof(x_text), "%ld", x);
    snprintf(y_text, sizeof(y_text), "%ld", y);

    pid = fork();

    if (pid < 0) {
        fprintf(stderr,
                "fork for hyprctl failed: %s\n",
                strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execlp("hyprctl",
               "hyprctl",
               "--quiet",
               "dispatch",
               "movecursor",
               x_text,
               y_text,
               (char *)NULL);

        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            if (!running) {
                kill(pid, SIGTERM);

                while (waitpid(pid, &status, 0) < 0 &&
                       errno == EINTR)
                    ;

                return -1;
            }

            continue;
        }

        fprintf(stderr,
                "waitpid for hyprctl failed: %s\n",
                strerror(errno));

        return -1;
    }

    if (!WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {

        if (running)
            fprintf(stderr,
                    "hyprctl could not move the cursor "
                    "to %ld,%ld\n",
                    x,
                    y);

        return -1;
    }

    return 0;
}

static int sleep_ms(long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };

    while (running && clicking &&
           nanosleep(&ts, &ts) < 0) {

        if (errno != EINTR)
            die_errno("nanosleep failed", errno);
    }

    return running;
}

static int wait_ms(long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };

    while (running && nanosleep(&ts, &ts) < 0) {
        if (errno != EINTR)
            die_errno("nanosleep failed", errno);
    }

    return running;
}

static struct timespec monotonic_now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        die_errno("clock_gettime failed", errno);

    return ts;
}

static int elapsed_at_least(const struct timespec *start,
                            long seconds)
{
    struct timespec now = monotonic_now();

    time_t elapsed_sec = now.tv_sec - start->tv_sec;
    long elapsed_nsec = now.tv_nsec - start->tv_nsec;
    time_t limit_sec = (time_t)seconds;

    if (elapsed_nsec < 0) {
        elapsed_sec--;
        elapsed_nsec += 1000000000L;
    }

    if (elapsed_sec < 0)
        return 0;

    if (elapsed_sec > limit_sec)
        return 1;

    if (elapsed_sec < limit_sec)
        return 0;

    return 1;
}

static void emit(int fd,
                 unsigned short type,
                 unsigned short code,
                 int value)
{
    struct input_event ev = {0};
    const char *buf = (const char *)&ev;
    size_t written = 0;

    ev.type = type;
    ev.code = code;
    ev.value = value;

    while (written < sizeof(ev)) {
        ssize_t n = write(fd,
                          buf + written,
                          sizeof(ev) - written);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            die_errno("write to uinput failed", errno);
        }

        if (n == 0)
            die("write to uinput failed: wrote 0 bytes");

        written += (size_t)n;
    }
}

static void click(unsigned short btn_code,
                  long hold_ms)
{
    emit(uinput_fd, EV_KEY, btn_code, 1);
    emit(uinput_fd, EV_SYN, SYN_REPORT, 0);

    if (hold_ms > 0)
        sleep_ms(hold_ms);

    emit(uinput_fd, EV_KEY, btn_code, 0);
    emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
}

static int open_uinput(void)
{
    int retries = 0;

    for (;;) {
        int fd = open("/dev/uinput",
                      O_WRONLY | O_NONBLOCK | O_CLOEXEC);

        if (fd >= 0 || errno != EINTR)
            return fd;

        if (++retries >= MAX_EINTR_RETRIES)
            return -1;
    }
}

static void setup_uinput(void)
{
    int name_len;
    int fd = open_uinput();

    if (fd < 0)
        die_open_uinput(errno);

    uinput_fd = fd;

    if (retry_ioctl_ulong(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        retry_ioctl_ulong(fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        retry_ioctl_ulong(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
        retry_ioctl_ulong(fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0) {

        die_errno("ioctl UI_SET_EVBIT/KEYBIT failed", errno);
    }

    if (retry_ioctl_ulong(fd, UI_SET_EVBIT, EV_SYN) < 0)
        die_errno("ioctl UI_SET_EVBIT EV_SYN failed", errno);

    struct uinput_setup usetup = {0};

    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;

    name_len = snprintf(usetup.name,
                        sizeof(usetup.name),
                        "%s",
                        "hypr-autoclicker");

    if (name_len < 0 ||
        (size_t)name_len >= sizeof(usetup.name)) {

        die("uinput device name is too long");
    }

    if (retry_ioctl_ptr(fd, UI_DEV_SETUP, &usetup) < 0)
        die_errno("UI_DEV_SETUP failed", errno);

    if (retry_ioctl0(fd, UI_DEV_CREATE) < 0)
        die_errno("UI_DEV_CREATE failed", errno);

    uinput_created = 1;

    wait_ms(100);
}

static int device_has_f6(int fd)
{
    unsigned long bits[KEY_MAX / (sizeof(unsigned long) * 8) + 1];

    memset(bits, 0, sizeof(bits));

    if (ioctl(fd,
              EVIOCGBIT(EV_KEY, sizeof(bits)),
              bits) < 0)
        return 0;

    return (bits[KEY_F6 / (sizeof(unsigned long) * 8)] &
            (1UL << (KEY_F6 % (sizeof(unsigned long) * 8)))) != 0;
}

static void setup_hotkey_devices(void)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir("/dev/input");

    if (dir == NULL)
        die_errno("Cannot open /dev/input", errno);

    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        snprintf(path,
                 sizeof(path),
                 "/dev/input/%s",
                 entry->d_name);

        fd = open(path,
                  O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        if (fd < 0)
            continue;

        if (!device_has_f6(fd)) {
            close(fd);
            continue;
        }

        if (input_count >= MAX_INPUT_DEVICES) {
            close(fd);
            break;
        }

        input_fds[input_count++] = fd;
    }

    closedir(dir);

    if (input_count == 0) {
        die("No keyboard input device with F6 access found.\n"
            "  → make sure your user can read /dev/input/event*");
    }
}

static void poll_hotkey(int timeout_ms)
{
    struct pollfd pfds[MAX_INPUT_DEVICES];

    if (input_count <= 0)
        return;

    for (int i = 0; i < input_count; i++) {
        pfds[i].fd = input_fds[i];
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    int rc;

    do {
        rc = poll(pfds,
                  (nfds_t)input_count,
                  timeout_ms);
    } while (rc < 0 && errno == EINTR && running);

    if (rc <= 0)
        return;

    for (int i = 0; i < input_count; i++) {
        if (!(pfds[i].revents & POLLIN))
            continue;

        for (;;) {
            struct input_event ev;
            ssize_t n = read(input_fds[i],
                             &ev,
                             sizeof(ev));

            if (n < 0) {
                if (errno == EINTR)
                    continue;

                break;
            }

            if (n != sizeof(ev))
                break;

            if (ev.type == EV_KEY &&
                ev.code == KEY_F6 &&
                ev.value == 1) {

                clicking = !clicking;

                if (clicking) {
                    fprintf(stderr,
                            "\n  STATUS   : ● CLICKING\n");
                } else {
                    fprintf(stderr,
                            "\n  STATUS   : ○ PAUSED\n");
                }

                fflush(stderr);
            }
        }
    }
}

static void usage(const char *prog, int status)
{
    FILE *stream =
        status == EXIT_SUCCESS ? stdout : stderr;

    fprintf(stream,
        "hypr-autoclicker — native Wayland/Hyprland autoclicker\n\n"

        "Usage: %s [OPTIONS]\n\n"

        "The program starts PAUSED.\n"
        "Press F6 to toggle clicking on/off.\n\n"

        "Options:\n"
        "  -i <ms>        Click interval in ms       (default: 100)\n"
        "  -b <button>    left | right | middle      (default: left)\n"
        "  -d <ms>        Hold duration in ms         (default: 10)\n"
        "  -c <count>     Stop after N clicks         (0 = infinite)\n"
        "  -t <seconds>   Stop after T seconds        (0 = infinite)\n"
        "  -p <seconds>   Capture cursor after countdown\n"
        "  -P <X,Y>       Lock clicks to X,Y\n"
        "  -g             Print current cursor position\n"
        "  -h             Show this help\n\n"

        "Controls:\n"
        "  F6             Toggle clicking ON/OFF\n"
        "  Ctrl-C         Exit\n\n"

        "Permissions:\n"
        "  The program needs access to /dev/uinput and\n"
        "  /dev/input/event*.\n"
        "  Add yourself to the 'input' group for both.\n",
        prog);

    exit(status);
}

int main(int argc, char *argv[])
{
    long interval_ms = 100;
    long hold_ms = 10;
    long max_clicks = 0;
    long max_secs = 0;
    long capture_secs = 0;

    long target_x = 0;
    long target_y = 0;

    int fixed_position = 0;
    int capture_requested = 0;
    int show_cursor_position = 0;

    unsigned short btn = BTN_LEFT;

    struct sigaction exit_sa = {0};

    static const struct option long_options[] = {
        {"capture", required_argument, NULL, 'p'},
        {"position", required_argument, NULL, 'P'},
        {"cursorpos", no_argument, NULL, 'g'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;

    if (atexit(cleanup_all) != 0)
        die("atexit cleanup registration failed");

    while ((opt = getopt_long(argc,
                              argv,
                              "i:b:d:c:t:p:P:gh",
                              long_options,
                              NULL)) != -1) {

        switch (opt) {
        case 'i':
            interval_ms = parse_long(optarg, "interval");
            break;

        case 'd':
            hold_ms = parse_long(optarg, "hold duration");
            break;

        case 'c':
            max_clicks = parse_long(optarg, "click count");
            break;

        case 't':
            max_secs = parse_long(optarg, "duration");
            break;

        case 'p':
            capture_secs = parse_long(optarg,
                                      "capture countdown");
            capture_requested = 1;
            break;

        case 'P':
            parse_position(optarg,
                           &target_x,
                           &target_y);
            fixed_position = 1;
            break;

        case 'g':
            show_cursor_position = 1;
            break;

        case 'b':
            if (strcmp(optarg, "left") == 0)
                btn = BTN_LEFT;
            else if (strcmp(optarg, "right") == 0)
                btn = BTN_RIGHT;
            else if (strcmp(optarg, "middle") == 0)
                btn = BTN_MIDDLE;
            else {
                fprintf(stderr,
                        "Unknown button '%s'\n",
                        optarg);
                usage(argv[0], EXIT_FAILURE);
            }
            break;

        case 'h':
            usage(argv[0], EXIT_SUCCESS);
            break;

        default:
            usage(argv[0], EXIT_FAILURE);
            break;
        }
    }

    if (optind < argc) {
        fprintf(stderr,
                "Unexpected argument: '%s'\n",
                argv[optind]);
        usage(argv[0], EXIT_FAILURE);
    }

    if (interval_ms <= 0)
        die("Interval must be > 0 ms");

    if (hold_ms < 0)
        die("Hold duration must be >= 0 ms");

    if (hold_ms > interval_ms)
        die("Hold duration must be <= interval");

    if (max_clicks < 0)
        die("Click count must be >= 0");

    if (max_secs < 0)
        die("Duration must be >= 0 seconds");

    if (capture_requested && capture_secs <= 0)
        die("Capture countdown must be > 0 seconds");

    if (!long_fits_time_t(max_secs))
        die("Duration is too large");

    exit_sa.sa_handler = handle_exit_signal;

    if (sigemptyset(&exit_sa.sa_mask) < 0)
        die_errno("sigemptyset failed", errno);

    if (sigaction(SIGINT, &exit_sa, NULL) < 0 ||
        sigaction(SIGTERM, &exit_sa, NULL) < 0)
        die_errno("sigaction failed", errno);

    if (show_cursor_position) {
        if (get_cursor_position(&target_x, &target_y) < 0)
            return EXIT_FAILURE;

        printf("%ld,%ld\n",
               target_x,
               target_y);

        return EXIT_SUCCESS;
    }

    if (capture_requested) {
        if (fixed_position)
            die("Use either --capture or --position, not both");

        fprintf(stderr,
                "Move the pointer to the target. "
                "Capturing in");

        for (long seconds = capture_secs;
             running && seconds > 0;
             seconds--) {

            fprintf(stderr,
                    " %ld",
                    seconds);

            fflush(stderr);

            wait_ms(1000);
        }

        fputc('\n', stderr);

        if (!running)
            return EXIT_SUCCESS;

        if (get_cursor_position(&target_x, &target_y) < 0)
            return EXIT_FAILURE;

        fixed_position = 1;

        fprintf(stderr,
                "Captured position: %ld,%ld\n",
                target_x,
                target_y);
    }

    setup_uinput();
    setup_hotkey_devices();

    const char *btn_name =
        (btn == BTN_LEFT)
            ? "left"
            : (btn == BTN_RIGHT)
                ? "right"
                : "middle";

    char click_limit[32];
    char duration_limit[32];

    if (max_clicks > 0)
        snprintf(click_limit,
                 sizeof(click_limit),
                 "%ld clicks",
                 max_clicks);
    else
        snprintf(click_limit,
                 sizeof(click_limit),
                 "unlimited");

    if (max_secs > 0)
        snprintf(duration_limit,
                 sizeof(duration_limit),
                 "%ld seconds",
                 max_secs);
    else
        snprintf(duration_limit,
                 sizeof(duration_limit),
                 "unlimited");

    fprintf(stderr,
        "\n"
        "╭──────────────────────────────────────╮\n"
        "│              WAYCLICKER              │\n"
        "╰──────────────────────────────────────╯\n\n"

        "  button   : %s\n"
        "  interval : %ld ms\n"
        "  hold     : %ld ms\n"
        "  position : %s\n"
        "  limit    : %s\n"
        "  duration : %s\n\n"

        "  STATUS   : ○ PAUSED\n\n"

        "  F6       → toggle clicking\n"
        "  Ctrl-C   → exit\n\n",

        btn_name,
        interval_ms,
        hold_ms,
        fixed_position ? "fixed" : "current cursor",
        click_limit,
        duration_limit);

    struct timespec start = monotonic_now();

    long count = 0;

    while (running) {
        if (!clicking) {
            poll_hotkey(100);
            continue;
        }

        if (max_secs > 0 &&
            elapsed_at_least(&start, max_secs)) {

            fprintf(stderr,
                    "\n  Duration limit reached.\n");

            break;
        }

        poll_hotkey(0);

        if (!running)
            break;

        if (!clicking)
            continue;

        if (fixed_position &&
            move_cursor_to(target_x, target_y) < 0) {

            if (!running)
                break;

            die("Cannot move cursor; ensure this runs "
                "inside your Hyprland session");
        }

        click(btn, hold_ms);

        if (count == LONG_MAX)
            die("Click counter overflow");

        count++;

        fprintf(stderr,
                "\r  clicks: %-10ld",
                count);

        fflush(stderr);

        if (max_clicks > 0 &&
            count >= max_clicks) {

            fprintf(stderr,
                    "\n  Click limit reached.\n");

            break;
        }

        long wait = interval_ms - hold_ms;

        if (wait > 0)
            poll_hotkey(wait);
    }

    clicking = 0;

    fprintf(stderr,
            "\n\nStopped after %ld click%s.\n",
            count,
            count == 1 ? "" : "s");

    cleanup_all();

    return EXIT_SUCCESS;
}