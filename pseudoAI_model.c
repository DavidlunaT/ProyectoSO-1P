//Analiza el texto recibido y decide que tipo de documento describe.
//Primero calcula puntajes por tipo y luego aplica las reglas definidas en el JSON.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"

typedef struct {
	char name[32];
	char words[MAX_WORDS][MAX_WORD_LENGTH];
	int word_count;
} DocumentType;

typedef struct {
	char name[64];
	char types[2][32];
	int type_count;
} ResultRule;

// Carga un archivo completo en memoria y devuelve un buffer terminado en '\0'.
static char *load_file(const char *path) {
	// Abre el archivo en modo lectura para obtener su contenido bruto.
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		return NULL;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);

	char *content = malloc((size_t)size + 1);
	if (content == NULL) {
		fclose(file);
		return NULL;
	}

	size_t bytes_read = fread(content, 1, (size_t)size, file);
	if (bytes_read != (size_t)size && ferror(file)) {
		free(content);
		fclose(file);
		return NULL;
	}
	content[size] = '\0';
	fclose(file);
	return content;
}

// Copia una cadena convirtiendo todos los caracteres alfabeticos a minuscula.
static void lowercase_copy(char *destination, const char *source, size_t capacity) {
	// Recorre la fuente y corta cuando se agota la capacidad del destino.
	size_t index = 0;
	for (; source[index] != '\0' && index + 1 < capacity; ++index) {
		destination[index] = (char)tolower((unsigned char)source[index]);
	}
	destination[index] = '\0';
}

// Cuenta apariciones no solapadas de una palabra dentro de un texto.
static int count_occurrences(const char *text, const char *word) {
	// Si la palabra esta vacia, no hay coincidencias validas para contar.
	int count = 0;
	size_t word_length = strlen(word);
	if (word_length == 0) {
		return 0;
	}

	for (const char *cursor = text; *cursor != '\0'; ++cursor) {
		if (strncasecmp(cursor, word, word_length) == 0) {
			++count;
			cursor += word_length - 1;
		}
	}
	return count;
}

// Extrae del JSON la lista de palabras asociadas a un tipo documental.
static void extract_words(const char *json, const char *type_name, DocumentType *type) {
	// Localiza la seccion del tipo y carga cada token entre comillas.
	// Search for "tipo": pattern safely without snprintf
	size_t type_len = strlen(type_name);
	if (type_len == 0 || type_len > MAX_WORD_LENGTH) {
		return;
	}

	// Manually construct search without snprintf to avoid truncation warning
	const char *cursor = json;
	const char *type_section = NULL;
	while ((cursor = strchr(cursor, '"')) != NULL) {
		if (strncmp(cursor + 1, type_name, type_len) == 0 && cursor[1 + type_len] == '"') {
			type_section = cursor;
			break;
		}
		cursor++;
	}

	if (type_section == NULL) {
		return;
	}

	const char *open_bracket = strchr(type_section, '[');
	const char *close_bracket = strchr(type_section, ']');
	if (open_bracket == NULL || close_bracket == NULL || close_bracket < open_bracket) {
		return;
	}

	type->word_count = 0;
	const char *word_cursor = open_bracket;
	while (word_cursor < close_bracket && type->word_count < MAX_WORDS) {
		const char *start = strchr(word_cursor, '"');
		if (start == NULL || start >= close_bracket) {
			break;
		}
		const char *end = strchr(start + 1, '"');
		if (end == NULL || end > close_bracket) {
			break;
		}

		size_t length = (size_t)(end - start - 1);
		if (length > 0 && length < MAX_WORD_LENGTH) {
			memcpy(type->words[type->word_count], start + 1, length);
			type->words[type->word_count][length] = '\0';
			++type->word_count;
		}
		word_cursor = end + 1;
	}
}

// Extrae del JSON las reglas finales que combinan uno o dos tipos.
static void extract_rules(const char *json, ResultRule *rules, int *rule_count) {
	// Recorre el bloque "resultados" y guarda nombre y tipos de cada regla.
	const char *cursor = strstr(json, "\"resultados\"");
	if (cursor == NULL) {
		*rule_count = 0;
		return;
	}

	const char *section_start = strchr(cursor, '{');
	const char *section_end = strrchr(cursor, '}');
	if (section_start == NULL || section_end == NULL || section_end <= section_start) {
		*rule_count = 0;
		return;
	}

	*rule_count = 0;
	cursor = section_start + 1;
	while (cursor < section_end && *rule_count < 8) {
		const char *name_start = strchr(cursor, '"');
		if (name_start == NULL) {
			break;
		}
		const char *name_end = strchr(name_start + 1, '"');
		if (name_end == NULL || name_end > section_end) {
			break;
		}

		const char *colon = strchr(name_end, ':');
		const char *open_bracket = colon != NULL ? strchr(colon, '[') : NULL;
		const char *close_bracket = open_bracket != NULL ? strchr(open_bracket, ']') : NULL;
		if (colon == NULL || open_bracket == NULL || close_bracket == NULL || close_bracket > section_end) {
			cursor = name_end + 1;
			continue;
		}

		size_t name_length = (size_t)(name_end - name_start - 1);
		if (name_length == 0 || name_length >= sizeof(rules[*rule_count].name)) {
			cursor = close_bracket + 1;
			continue;
		}

		memcpy(rules[*rule_count].name, name_start + 1, name_length);
		rules[*rule_count].name[name_length] = '\0';
		rules[*rule_count].type_count = 0;

		const char *type_cursor = open_bracket;
		while (type_cursor < close_bracket && rules[*rule_count].type_count < 2) {
			const char *type_start = strchr(type_cursor, '"');
			if (type_start == NULL || type_start >= close_bracket) {
				break;
			}
			const char *type_end = strchr(type_start + 1, '"');
			if (type_end == NULL || type_end > close_bracket) {
				break;
			}

			size_t type_length = (size_t)(type_end - type_start - 1);
			if (type_length > 0 && type_length < sizeof(rules[*rule_count].types[0])) {
				memcpy(rules[*rule_count].types[rules[*rule_count].type_count], type_start + 1, type_length);
				rules[*rule_count].types[rules[*rule_count].type_count][type_length] = '\0';
				++rules[*rule_count].type_count;
			}
			type_cursor = type_end + 1;
		}

		++(*rule_count);
		cursor = close_bracket + 1;
	}
}

// Verifica si una regla coincide exactamente con el conjunto de tipos detectados.
static int match_rule(const ResultRule *rule, const char *types[], int type_count) {
	// Requiere misma cantidad de tipos y coincidencia por nombre sin importar orden.
	if (rule->type_count != type_count) {
		return 0;
	}

	for (int index = 0; index < type_count; ++index) {
		int found = 0;
		for (int match = 0; match < type_count; ++match) {
			if (strcmp(rule->types[index], types[match]) == 0) {
				found = 1;
				break;
			}
		}
		if (!found) {
			return 0;
		}
	}
	return 1;
}

// Punto de entrada: clasifica el texto segun diccionarios y reglas declaradas.
int main(int argc, char **argv) {
	// Valida argumentos, carga insumos y ejecuta el flujo completo de clasificacion.
	if (argc < 2) {
		return 1;
	}

	char *json = load_file("diccionarios.json");
	char *text = load_file(argv[1]);
	if (json == NULL || text == NULL) {
		free(json);
		free(text);
		return 1;
	}

	DocumentType document_types[MAX_TYPES] = {
		{.name = "correo"},
		{.name = "articulo"},
		{.name = "reporte"}
	};
	for (int index = 0; index < MAX_TYPES; ++index) {
		extract_words(json, document_types[index].name, &document_types[index]);
	}

	ResultRule rules[8];
	int rule_count = 0;
	extract_rules(json, rules, &rule_count);

	char text_lower[MAX_TEXT * 2];
	lowercase_copy(text_lower, text, sizeof(text_lower));

	int scores[MAX_TYPES] = {0, 0, 0};
	const int threshold = AI_THRESHOLD;
	int qualifying = 0;

	for (int type_index = 0; type_index < MAX_TYPES; ++type_index) {
		for (int word_index = 0; word_index < document_types[type_index].word_count; ++word_index) {
			scores[type_index] += count_occurrences(text_lower, document_types[type_index].words[word_index]);
		}
		if (scores[type_index] > threshold) {
			++qualifying;
		}
	}

	if (qualifying == 0) {
		printf("informacion insuficiente para determinar el tipo de documento");
		free(json);
		free(text);
		return 0;
	}

	int order[MAX_TYPES] = {0, 1, 2};
	for (int outer = 0; outer < MAX_TYPES - 1; ++outer) {
		for (int inner = outer + 1; inner < MAX_TYPES; ++inner) {
			if (scores[order[inner]] > scores[order[outer]]) {
				int swap = order[outer];
				order[outer] = order[inner];
				order[inner] = swap;
			}
		}
	}

	const char *selected_types[2] = {NULL, NULL};
	int selected_count = 0;
	for (int index = 0; index < MAX_TYPES && selected_count < 2; ++index) {
		if (scores[order[index]] > threshold) {
			selected_types[selected_count++] = document_types[order[index]].name;
		}
	}

	if (qualifying == 3 && selected_count > 2) {
		selected_count = 2;
	}

	const char *matched_result = NULL;
	for (int rule_index = 0; rule_index < rule_count; ++rule_index) {
		if (match_rule(&rules[rule_index], selected_types, selected_count)) {
			matched_result = rules[rule_index].name;
			break;
		}
	}

	if (matched_result == NULL) {
		printf("no se pudo determinar el resultado con la informacion recibida");
	} else {
		printf("Resultado: %s\n", matched_result);
		printf("Tipos detectados: ");
		for (int index = 0; index < selected_count; ++index) {
			printf("%s%s", selected_types[index], index + 1 < selected_count ? ", " : "");
		}
	}

	free(json);
	free(text);
	return 0;
}