#ifndef CONFIG_H
#define CONFIG_H

// Socket paths
#define SERVER_SOCKET_PATH "/tmp/proyecto_so_server.sock"
#define LAUNCHER_SOCKET_PATH "/tmp/proyecto_so_launcher.sock"

// Buffer sizes
#define MAX_WINDOWS 64
#define MAX_TEXT 4096
#define MAX_BUFFER 4096

// AI Model configuration
#define MAX_TYPES 3
#define MAX_WORDS 32
#define MAX_WORD_LENGTH 64
#define AI_THRESHOLD 0

// Runtime directory
#define RUNTIME_DIR "."

#endif
