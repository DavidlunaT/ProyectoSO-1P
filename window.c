//Ventana cliente: captura teclas y las reenvia al servidor.
//Si el servidor no responde, guarda los mensajes pendientes en disco.

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h> //
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "config.h" //

static char server_socket_path[108] = {0};
static char queue_file_path[256] = {0};
static volatile sig_atomic_t keep_running = 1;
static Atom wm_delete_window = None;

// Inicializa o valida la ruta de trabajo donde se guarda la cola local.
static void ensure_runtime_dir(void) {
    // El proyecto trabaja sobre el directorio actual, sin creacion adicional.
    (void)RUNTIME_DIR;
}

// Encola un mensaje cuando no se puede enviar al servidor en ese momento.
static void append_queue(const char *message) {
    // Guarda el mensaje al final del archivo para reintento posterior.
    FILE *file = fopen(queue_file_path, "a");
    if (file == NULL) {
        return;
    }
    fprintf(file, "%s\n", message);
    fclose(file);
}

// Reintenta el envio de la cola pendiente y conserva solo fallos restantes.
static void flush_queue(int socket_fd, struct sockaddr_un *address) {
    // Reescribe el archivo de cola con los mensajes que no pudieron enviarse.
    FILE *file = fopen(queue_file_path, "r");
    if (file == NULL) {
        return;
    }

    char line[MAX_BUFFER];
    char remaining_path[512];
    snprintf(remaining_path, sizeof(remaining_path), "%s.tmp", queue_file_path);
    FILE *remaining = fopen(remaining_path, "w");
    if (remaining == NULL) {
        fclose(file);
        return;
    }

    //Reintenta los mensajes guardados y conserva solo los que sigan fallando.
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (sendto(socket_fd, line, strlen(line), 0, (struct sockaddr *)address, sizeof(*address)) < 0) {
            fprintf(remaining, "%s\n", line);
        }
    }

    fclose(file);
    fclose(remaining);
    rename(remaining_path, queue_file_path);
}

// Envia un mensaje al servidor o lo persiste en cola si falla el envio.
static void send_message(int socket_fd, struct sockaddr_un *address, const char *message) {
    // El mecanismo de cola protege ante caidas temporales del servidor.
    if (sendto(socket_fd, message, strlen(message), 0, (struct sockaddr *)address, sizeof(*address)) < 0) {
        append_queue(message);
    }
}

// Handler de senales para detener el bucle principal de la ventana.
static void handle_signal(int signal_number) {
    // Solo marca bandera de salida para finalizar en forma ordenada.
    (void)signal_number;
    keep_running = 0;
}

// Traduce una tecla de X11 al token de protocolo esperado por el servidor.
static const char *translate_key(KeySym keysym, char *buffer, size_t buffer_size) {
    // Acepta letras y teclas especiales conocidas, descartando el resto.
    const char *name = XKeysymToString(keysym);
    if (name == NULL) {
        return NULL;
    }

    if (strlen(name) == 1 || strcmp(name, "Space") == 0 || strcmp(name, "Return") == 0 || strcmp(name, "BackSpace") == 0) {
        snprintf(buffer, buffer_size, "%s", name);
        return buffer;
    }

    return NULL;
}

// Cierra la ventana notificando al servidor y liberando recursos locales.
static void shutdown_window(Display *display, Window window, int socket_fd, struct sockaddr_un *address) {
    // Antes de cerrar, intenta vaciar cola y envia evento END del proceso.
    flush_queue(socket_fd, address);
    char end_message[MAX_BUFFER];
    snprintf(end_message, sizeof(end_message), "END|%d|window", (int)getpid());
    send_message(socket_fd, address, end_message);

    if (window != None) {
        XDestroyWindow(display, window);
    }
    XCloseDisplay(display);
    close(socket_fd);
}

// Punto de entrada: crea ventana X11, captura teclas y reporta eventos al servidor.
int main(int argc, char **argv) {
    // Configura socket, bucle de eventos y notificaciones OPEN/KEY/END.
    if (argc > 1) {
        snprintf(server_socket_path, sizeof(server_socket_path), "%s", argv[1]);
    } else {
        snprintf(server_socket_path, sizeof(server_socket_path), "%s", SERVER_SOCKET_PATH);
    }

    ensure_runtime_dir();
    snprintf(queue_file_path, sizeof(queue_file_path), "%s/window_%d.queue", RUNTIME_DIR, (int)getpid());

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        fprintf(stderr, "No se pudo crear el socket\n");
        XCloseDisplay(display);
        return 1;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", server_socket_path);

    int screen = DefaultScreen(display);
    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        10, 10, 400, 200,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );

    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);
    wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    if (wm_delete_window != None) {
        XSetWMProtocols(display, window, &wm_delete_window, 1);
    }

    char open_message[MAX_BUFFER];
    snprintf(open_message, sizeof(open_message), "OPEN|%d|window", (int)getpid());
    send_message(socket_fd, &address, open_message);

    XEvent event;
    // Bucle de eventos de X11 sin bloquear la interfaz.
    while (keep_running) {
        while (XPending(display) > 0) {
            XNextEvent(display, &event);

            if (event.type == KeyPress) {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                char key_buffer[64];
                const char *translated = translate_key(keysym, key_buffer, sizeof(key_buffer));
                if (translated != NULL) {
                    flush_queue(socket_fd, &address);
                    char message[MAX_BUFFER];
                    snprintf(message, sizeof(message), "KEY|%d|%s", (int)getpid(), translated);
                    send_message(socket_fd, &address, message);
                }

                if (keysym == XK_Escape) {
                    keep_running = 0;
                    break;
                }
            } else if (event.type == ClientMessage) {
                if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
                    keep_running = 0;
                    break;
                }
            }
        }

        usleep(10000);
    }

    shutdown_window(display, window, socket_fd, &address);
    return 0;
}