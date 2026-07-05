//launcher, ventana principal donde se interactuara con el sistema
//Debe ser una consola interactiva donde cumpla con todo lo indicado más abajo

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_WINDOWS 64
#define MAX_TEXT 2048

static const char *SERVER_SOCKET_PATH = "/tmp/proyecto_so_server.sock";
static const char *LAUNCHER_SOCKET_PATH = "/tmp/proyecto_so_launcher.sock";
static const char *RUNTIME_DIR = ".";

typedef struct {
    pid_t pid;
    int active;
    int terminated;
    char text[MAX_TEXT];
    char result[MAX_TEXT];
} ProcessInfo;

static ProcessInfo processes[MAX_WINDOWS];
static pthread_mutex_t processes_mutex = PTHREAD_MUTEX_INITIALIZER;
static int launcher_socket_fd = -1;
static pid_t server_pid = -1;

static ProcessInfo *find_process(pid_t pid) {
    for (int index = 0; index < MAX_WINDOWS; ++index) {
        if (processes[index].active && processes[index].pid == pid) {
            return &processes[index];
        }
    }
    return NULL;
}

static void load_text_from_file(pid_t pid, char *destination, size_t capacity) {
    char path[256];
    snprintf(path, sizeof(path), "%s/window_%d.txt", RUNTIME_DIR, (int)pid);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    size_t total = fread(destination, 1, capacity - 1, file);
    destination[total] = '\0';
    fclose(file);
}

static void load_result_from_log(pid_t pid, char *destination, size_t capacity) {
    FILE *file = fopen("./results.log", "r");
    if (file == NULL) {
        return;
    }

    char line[MAX_TEXT + 256];
    char matched[MAX_TEXT] = {0};
    while (fgets(line, sizeof(line), file) != NULL) {
        int logged_pid = 0;
        if (sscanf(line, "WINDOW %d", &logged_pid) == 1 && logged_pid == (int)pid) {
            char *payload = strstr(line, "RESULT ");
            if (payload != NULL) {
                payload += strlen("RESULT ");
                payload[strcspn(payload, "\r\n")] = '\0';
                snprintf(matched, sizeof(matched), "%s", payload);
            }
        }
    }

    fclose(file);
    if (matched[0] != '\0') {
        snprintf(destination, capacity, "%s", matched);
    }
}

static void sync_process_from_disk(ProcessInfo *process) {
    if (process == NULL) {
        return;
    }

    if (process->text[0] == '\0') {
        load_text_from_file(process->pid, process->text, sizeof(process->text));
    }

    if (process->result[0] == '\0' && process->terminated) {
        load_result_from_log(process->pid, process->result, sizeof(process->result));
    }
}

static void ensure_window_slot(pid_t pid) {
    pthread_mutex_lock(&processes_mutex);
    for (int index = 0; index < MAX_WINDOWS; ++index) {
        if (processes[index].active && processes[index].pid == pid) {
            pthread_mutex_unlock(&processes_mutex);
            return;
        }
    }

    for (int index = 0; index < MAX_WINDOWS; ++index) {
        if (!processes[index].active) {
            processes[index].active = 1;
            processes[index].terminated = 0;
            processes[index].pid = pid;
            processes[index].text[0] = '\0';
            processes[index].result[0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&processes_mutex);
}

static void update_result(pid_t pid, const char *result) {
    pthread_mutex_lock(&processes_mutex);
    ProcessInfo *process = find_process(pid);
    if (process != NULL) {
        snprintf(process->result, sizeof(process->result), "%s", result);
        process->terminated = 1;
    }
    pthread_mutex_unlock(&processes_mutex);
}

static void print_processes(const char *filter) {
    pthread_mutex_lock(&processes_mutex);
    int running = 0;
    int terminated = 0;

    (void)filter;
    printf("\nPID\tEstado\t\tResultado\n");
    printf("-----------------------------------------------\n");
    for (int index = 0; index < MAX_WINDOWS; ++index) {
        if (!processes[index].active) {
            continue;
        }

        if (processes[index].terminated) {
            ++terminated;
        } else {
            ++running;
        }

        sync_process_from_disk(&processes[index]);

        printf("%d\t%s\t", processes[index].pid,
               processes[index].terminated ? "terminated" : "running");
        printf("%s\n", processes[index].terminated ? processes[index].result : "-");
    }
    printf("-----------------------------------------------\n");
    printf("Running: %d | Terminated: %d\n", running, terminated);
    pthread_mutex_unlock(&processes_mutex);
}

static void *listen_results(void *arg) {
    (void)arg;
    while (1) {
        char buffer[MAX_TEXT + 64];
        struct sockaddr_un sender;
        socklen_t sender_len = sizeof(sender);
        ssize_t received = recvfrom(launcher_socket_fd, buffer, sizeof(buffer) - 1, 0,
                                    (struct sockaddr *)&sender, &sender_len);
        if (received <= 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        buffer[received] = '\0';
        char *type = strtok(buffer, "|");
        char *pid_text = strtok(NULL, "|");
        char *payload = strtok(NULL, "");
        if (type == NULL || pid_text == NULL || payload == NULL) {
            continue;
        }

        pid_t pid = (pid_t)atoi(pid_text);
        if (strcmp(type, "TEXT") == 0) {
            pthread_mutex_lock(&processes_mutex);
            ProcessInfo *process = find_process(pid);
            if (process != NULL) {
                snprintf(process->text, sizeof(process->text), "%s", payload);
            }
            pthread_mutex_unlock(&processes_mutex);
        } else if (strcmp(type, "RESULT") == 0) {
            update_result(pid, payload);
        }
    }
    return NULL;
}

static void refresh_children(void) {
    int status = 0;
    pid_t child_pid = 0;
    while ((child_pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&processes_mutex);
        ProcessInfo *process = find_process(child_pid);
        if (process != NULL) {
            process->terminated = 1;
        }
        pthread_mutex_unlock(&processes_mutex);
    }
}

static pid_t launch_server(void) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("./server", "./server", LAUNCHER_SOCKET_PATH, NULL);
        _exit(1);
    }
    return pid;
}

static pid_t launch_window(void) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("./window", "./window", SERVER_SOCKET_PATH, NULL);
        _exit(1);
    }
    return pid;
}

static void kill_all_windows(void) {
    pthread_mutex_lock(&processes_mutex);
    for (int index = 0; index < MAX_WINDOWS; ++index) {
        if (processes[index].active && !processes[index].terminated) {
            kill(processes[index].pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&processes_mutex);
}

static void show_details(void) {
    char input[32];
    printf("PID: ");
    if (fgets(input, sizeof(input), stdin) == NULL) {
        return;
    }

    pid_t pid = (pid_t)atoi(input);
    pthread_mutex_lock(&processes_mutex);
    ProcessInfo *process = find_process(pid);
    if (process == NULL) {
        pthread_mutex_unlock(&processes_mutex);
        printf("No existe esa window.\n");
        return;
    }

    sync_process_from_disk(process);

    printf("\nPID: %d\nEstado: %s\n", process->pid, process->terminated ? "terminated" : "running");
    printf("Texto guardado:\n%s\n", process->text[0] != '\0' ? process->text : "-");
    if (process->terminated) {
        printf("Resultado del pseudoAI_model:\n%s\n", process->result[0] != '\0' ? process->result : "-");
    } else {
        printf("Resultado del pseudoAI_model: no disponible mientras el proceso sigue running\n");
    }
    pthread_mutex_unlock(&processes_mutex);
}

static void terminate_process_menu(void) {
    char input[32];
    printf("PID a terminar: ");
    if (fgets(input, sizeof(input), stdin) == NULL) {
        return;
    }
    pid_t pid = (pid_t)atoi(input);
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
}

int main(void) {
    unlink(LAUNCHER_SOCKET_PATH);
    launcher_socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (launcher_socket_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un launcher_addr;
    memset(&launcher_addr, 0, sizeof(launcher_addr));
    launcher_addr.sun_family = AF_UNIX;
    snprintf(launcher_addr.sun_path, sizeof(launcher_addr.sun_path), "%s", LAUNCHER_SOCKET_PATH);
    if (bind(launcher_socket_fd, (struct sockaddr *)&launcher_addr, sizeof(launcher_addr)) < 0) {
        perror("bind");
        close(launcher_socket_fd);
        unlink(LAUNCHER_SOCKET_PATH);
        return 1;
    }

    server_pid = launch_server();
    if (server_pid < 0) {
        perror("fork");
        close(launcher_socket_fd);
        unlink(LAUNCHER_SOCKET_PATH);
        return 1;
    }

    pthread_t receiver_thread;
    if (pthread_create(&receiver_thread, NULL, listen_results, NULL) != 0) {
        perror("pthread_create");
        kill(server_pid, SIGTERM);
        close(launcher_socket_fd);
        unlink(LAUNCHER_SOCKET_PATH);
        return 1;
    }

    char option[32];
    while (1) {
        refresh_children();
        print_processes(NULL);
        printf("\n1) Crear window\n2) Ver detalles\n3) Terminar proceso\n4) Salir\nOpcion: ");
        if (fgets(option, sizeof(option), stdin) == NULL) {
            break;
        }

        switch (option[0]) {
            case '1': {
                pid_t pid = launch_window();
                if (pid > 0) {
                    ensure_window_slot(pid);
                }
                break;
            }
            case '2':
                show_details();
                break;
            case '3':
                terminate_process_menu();
                break;
            case '4':
                kill_all_windows();
                kill(server_pid, SIGTERM);
                close(launcher_socket_fd);
                unlink(LAUNCHER_SOCKET_PATH);
                return 0;
            default:
                break;
        }
    }

    kill_all_windows();
    kill(server_pid, SIGTERM);
    close(launcher_socket_fd);
    unlink(LAUNCHER_SOCKET_PATH);
    return 0;
}
