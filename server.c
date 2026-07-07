//Servidor: recibe eventos del cliente, los guarda y coordina el analisis final.
//Cuando una ventana termina, deriva su texto al modelo y reenvia el resultado al launcher.

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "config.h"

static char launcher_socket_path[108] = {0};

typedef struct {
	pid_t pid;
	int active;
	char text[MAX_TEXT];
} WindowData;

static WindowData windows[MAX_WINDOWS];
static int server_socket_fd = -1;

// Inicializa o valida el directorio de trabajo en tiempo de ejecucion.
static void ensure_runtime_dir(void) {
	// Actualmente se usa el directorio actual, por eso no requiere creacion explicita.
	(void)RUNTIME_DIR;
}

// Busca una ventana por PID y opcionalmente crea un slot nuevo.
static WindowData *get_window(pid_t pid, int create) {
	// Primero intenta encontrar un registro existente para reutilizarlo.
	for (int index = 0; index < MAX_WINDOWS; ++index) {
		if (windows[index].active && windows[index].pid == pid) {
			return &windows[index];
		}
	}

	if (!create) {
		return NULL;
	}

	for (int index = 0; index < MAX_WINDOWS; ++index) {
		if (!windows[index].active) {
			windows[index].active = 1;
			windows[index].pid = pid;
			windows[index].text[0] = '\0';
			return &windows[index];
		}
	}
	return NULL;
}

// Agrega una linea al final de un archivo de log.
static void append_to_file(const char *path, const char *line) {
	// Abre en modo append para no perder entradas previas.
	FILE *file = fopen(path, "a");
	if (file == NULL) {
		return;
	}
	fprintf(file, "%s\n", line);
	fclose(file);
}

// Sobrescribe un archivo de texto con el estado actual de una ventana.
static void write_text_file(const char *path, const char *text) {
	// Se usa modo write para mantener solo la ultima version del contenido.
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		return;
	}
	fprintf(file, "%s", text);
	fclose(file);
}

// Garantiza que el resultado final no quede vacio o solo con espacios.
static void normalize_result(char *result, size_t capacity) {
	// Si la salida del modelo no trae contenido visible, aplica un mensaje fallback.
	const char *fallback = "informacion insuficiente para determinar el tipo de documento";
	int has_visible_content = 0;

	for (size_t index = 0; result[index] != '\0'; ++index) {
		if (!isspace((unsigned char)result[index])) {
			has_visible_content = 1;
			break;
		}
	}

	if (!has_visible_content) {
		snprintf(result, capacity, "%s", fallback);
	}
}

// Envia mensajes asincronos al launcher por socket UNIX datagrama.
static void send_to_launcher(const char *type, pid_t pid, const char *payload) {
	// Si no existe ruta configurada para launcher, no se intenta enviar nada.
	if (launcher_socket_path[0] == '\0') {
		return;
	}

	int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (socket_fd < 0) {
		return;
	}

	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", launcher_socket_path);

	char buffer[MAX_TEXT + 64];
	snprintf(buffer, sizeof(buffer), "%s|%d|%s", type, (int)pid, payload);
	sendto(socket_fd, buffer, strlen(buffer), 0, (struct sockaddr *)&address, sizeof(address));
	close(socket_fd);
}

// Aplica una tecla recibida sobre el buffer de texto de una ventana.
static void update_text(WindowData *window, const char *key) {
	// Maneja teclas especiales y caracteres simples respetando limites de buffer.
	if (strcmp(key, "BackSpace") == 0) {
		size_t length = strlen(window->text);
		if (length > 0) {
			window->text[length - 1] = '\0';
		}
	} else if (strcmp(key, "Return") == 0) {
		size_t length = strlen(window->text);
		if (length + 1 < sizeof(window->text)) {
			window->text[length] = '\n';
			window->text[length + 1] = '\0';
		}
	} else if (strcmp(key, "Space") == 0) {
		size_t length = strlen(window->text);
		if (length + 1 < sizeof(window->text)) {
			window->text[length] = ' '; 
			window->text[length + 1] = '\0';
		}
	} else if (strlen(key) == 1) {
		size_t length = strlen(window->text);
		if (length + 1 < sizeof(window->text)) {
			window->text[length] = key[0];
			window->text[length + 1] = '\0';
		}
	}
}

// Ejecuta pseudoAI_model para una ventana y captura su salida estandar.
static void run_pseudo_ai(pid_t pid) {
	// El analizador corre en un hijo y su salida se captura por un pipe.
	char input_path[256];
	snprintf(input_path, sizeof(input_path), "%s/window_%d.txt", RUNTIME_DIR, (int)pid);

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		return;
	}

	pid_t child = fork();
	if (child == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execl("./pseudoAI_model", "./pseudoAI_model", input_path, NULL);
		_exit(1);
	}

	close(pipefd[1]);
	char result[MAX_TEXT];
	size_t total = 0;
	ssize_t bytes = 0;
	while ((bytes = read(pipefd[0], result + total, sizeof(result) - 1 - total)) > 0) {
		total += (size_t)bytes;
		if (total + 1 >= sizeof(result)) {
			break;
		}
	}
	if (bytes < 0) {
		total = 0;
	}
	result[total] = '\0';
	normalize_result(result, sizeof(result));
	close(pipefd[0]);
	waitpid(child, NULL, 0);

	char result_log[5120];
	snprintf(result_log, sizeof(result_log), "WINDOW %d RESULT %s", (int)pid, result);
	append_to_file("./results.log", result_log);
	send_to_launcher("RESULT", pid, result);
}

// Interpreta y procesa un mensaje de protocolo recibido desde una ventana.
static void handle_message(const char *message) {
	// Formato esperado: TIPO|PID|PAYLOAD.
	char buffer[MAX_TEXT + 64];
	snprintf(buffer, sizeof(buffer), "%s", message);

	char *type = strtok(buffer, "|");
	char *pid_text = strtok(NULL, "|");
	char *payload = strtok(NULL, "");
	if (type == NULL || pid_text == NULL) {
		return;
	}

	pid_t pid = (pid_t)atoi(pid_text);
	WindowData *window = get_window(pid, 1);
	if (window == NULL) {
		return;
	}

	char log_line[5120];
	if (strcmp(type, "OPEN") == 0) {
		snprintf(log_line, sizeof(log_line), "WINDOW %d OPEN", (int)pid);
		append_to_file("./server.log", log_line);
		return;
	}

	if (strcmp(type, "KEY") == 0 && payload != NULL) {
		update_text(window, payload);
		snprintf(log_line, sizeof(log_line), "WINDOW %d KEY %s", (int)pid, payload);
		append_to_file("./server.log", log_line);

		char window_log_path[256];
		snprintf(window_log_path, sizeof(window_log_path), "%s/window_%d.txt", RUNTIME_DIR, (int)pid);
		write_text_file(window_log_path, window->text);
		send_to_launcher("TEXT", pid, window->text);
		return;
	}

	if (strcmp(type, "END") == 0) {
		snprintf(log_line, sizeof(log_line), "WINDOW %d END", (int)pid);
		append_to_file("./server.log", log_line);
		send_to_launcher("TEXT", pid, window->text);
		run_pseudo_ai(pid);
	}
}

// Punto de entrada: levanta el socket del servidor y atiende mensajes continuamente.
int main(int argc, char **argv) {
	// Inicializa dependencias del servidor y ejecuta el bucle principal de recepcion.
	ensure_runtime_dir();

	if (argc > 1) {
		snprintf(launcher_socket_path, sizeof(launcher_socket_path), "%s", argv[1]);
	}

	unlink(SERVER_SOCKET_PATH);
	server_socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (server_socket_fd < 0) {
		perror("socket");
		return 1;
	}

	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", SERVER_SOCKET_PATH);
	if (bind(server_socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("bind");
		close(server_socket_fd);
		unlink(SERVER_SOCKET_PATH);
		return 1;
	}

	//Bucle principal del servidor.
	while (1) {
		char message[MAX_TEXT + 64];
		ssize_t received = recvfrom(server_socket_fd, message, sizeof(message) - 1, 0, NULL, NULL);
		if (received < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		message[received] = '\0';
		handle_message(message);
	}

	close(server_socket_fd);
	unlink(SERVER_SOCKET_PATH);
	return 0;
}