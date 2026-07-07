//Launcher principal: muestra el estado de las ventanas y permite controlarlas.
//Coordina el servidor, crea ventanas y resume la informacion al usuario.

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
#include "config.h"

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

// Busca el registro de una ventana/proceso por su PID.
static ProcessInfo *find_process(pid_t pid) {
	// Recorre la tabla de procesos activos para ubicar el slot correspondiente.
    for (int index = 0; index < MAX_WINDOWS; ++index) {
        if (processes[index].active && processes[index].pid == pid) {
            return &processes[index];
        }
    }
    return NULL;
}

// Carga el texto persistido de una ventana desde su archivo local.
static void load_text_from_file(pid_t pid, char *destination, size_t capacity) {
	// Construye la ruta por PID y copia el contenido con terminacion segura en '\0'.
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

// Busca en results.log el ultimo resultado asociado a un PID.
static void load_result_from_log(pid_t pid, char *destination, size_t capacity) {
	// Lee linea por linea y conserva la coincidencia mas reciente para esa ventana.
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

// Sincroniza un registro en memoria con los archivos persistidos en disco.
static void sync_process_from_disk(ProcessInfo *process) {
	// Recupera texto y resultado cuando el launcher se reinicia o pierde estado local.
    if (process == NULL) {
        return;
    }

    //Si el launcher se reinicia, reconstruye el estado desde los archivos locales.
    if (process->text[0] == '\0') {
        load_text_from_file(process->pid, process->text, sizeof(process->text));
    }

    if (process->result[0] == '\0' && process->terminated) {
        load_result_from_log(process->pid, process->result, sizeof(process->result));
    }
}

// Asegura que exista un slot activo para una ventana recien creada.
static void ensure_window_slot(pid_t pid) {
	// Evita duplicados y usa el primer espacio libre cuando el PID es nuevo.
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

// Actualiza el resultado final de una ventana y la marca como terminada.
static void update_result(pid_t pid, const char *result) {
	// Protege la escritura compartida para evitar condiciones de carrera.
    pthread_mutex_lock(&processes_mutex);
    ProcessInfo *process = find_process(pid);
    if (process != NULL) {
        snprintf(process->result, sizeof(process->result), "%s", result);
        process->terminated = 1;
    }
    pthread_mutex_unlock(&processes_mutex);
}

// Muestra en pantalla el resumen de estado de todas las ventanas conocidas.
static void print_processes(const char *filter) {
	// Recalcula contadores y refresca datos persistidos antes de imprimir la tabla.
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

// Hilo receptor: escucha eventos TEXT/RESULT enviados por el servidor.
static void *listen_results(void *arg) {
	// Parsea cada datagrama y actualiza el estado interno del proceso correspondiente.
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

// Recolecta hijos terminados para evitar zombies y marcar estado en la tabla.
static void refresh_children(void) {
	// Hace waitpid no bloqueante hasta vaciar todos los eventos pendientes.
    int status = 0;
    pid_t child_pid = 0;
    // Revisa hijos terminados para evitar procesos zombies.
    while ((child_pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&processes_mutex);
        ProcessInfo *process = find_process(child_pid);
        if (process != NULL) {
            process->terminated = 1;
        }
        pthread_mutex_unlock(&processes_mutex);
    }
}

// Crea el proceso servidor principal del sistema.
static pid_t launch_server(void) {
	// El hijo ejecuta el binario server y el padre recibe su PID.
    pid_t pid = fork();
    if (pid == 0) {
        execl("./server", "./server", LAUNCHER_SOCKET_PATH, NULL);
        _exit(1);
    }
    return pid;
}

// Crea una nueva ventana cliente que se conecta al servidor.
static pid_t launch_window(void) {
	// El proceso hijo reemplaza su imagen por el ejecutable window.
    pid_t pid = fork();
    if (pid == 0) {
        execl("./window", "./window", SERVER_SOCKET_PATH, NULL);
        _exit(1);
    }
    return pid;
}

// Termina todas las ventanas activas que aun no finalizaron.
static void kill_all_windows(void) {
	// Recorre la tabla y envia SIGTERM solo a procesos en ejecucion.
    pthread_mutex_lock(&processes_mutex);
    // Cierra primero las ventanas activas para liberar recursos.
    for (int index = 0; index < MAX_WINDOWS; ++index) {
        if (processes[index].active && !processes[index].terminated) {
            kill(processes[index].pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&processes_mutex);
}

// Muestra informacion detallada de una ventana segun PID ingresado por el usuario.
static void show_details(void) {
	// Valida el PID solicitado, sincroniza desde disco e imprime texto y resultado.
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

// Solicita un PID por consola y envia la senal de termino a ese proceso.
static void terminate_process_menu(void) {
	// Se usa SIGTERM para permitir finalizacion controlada del proceso objetivo.
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

// Punto de entrada: inicializa launcher, server e interfaz de control por consola.
int main(void) {
	// Configura sockets e hilo receptor, luego atiende el menu interactivo.
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
    // Ciclo interactivo del launcher.
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
