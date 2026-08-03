#include "interfaz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

t_tipo_interfaz interfaz_parsear_tipo(const char *tipo) {
  if (strcasecmp(tipo, "STDIN") == 0)
    return INTERFAZ_STDIN;
  if (strcasecmp(tipo, "STDOUT") == 0)
    return INTERFAZ_STDOUT;
  if (strcasecmp(tipo, "SLEEP") == 0)
    return INTERFAZ_SLEEP;
  return INTERFAZ_SLEEP; // Default
}

char *interfaz_stdin(int tamanio, t_log *logger) {
  log_info(logger, "Esperando entrada del usuario (max %d caracteres)...",
           tamanio);

  char *buffer = calloc(1, (size_t)(tamanio + 1));
  printf("[STDIN] Ingrese datos (max %d chars): ", tamanio);
  fflush(stdout);

  if (fgets(buffer, tamanio + 1, stdin) != NULL) {
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
      buffer[len - 1] = '\0';
      len--;
    } else {
      int c;
      while ((c = getchar()) != '\n' && c != EOF) {
      }
    }
    if (len > 0 && buffer[len - 1] == '\r') {
      buffer[len - 1] = '\0';
      len--;
    }
    log_info(logger, "Entrada recibida: '%s' (%zu bytes)", buffer, len);
  } else {
    buffer[0] = '\0';
  }

  return buffer;
}

void interfaz_stdout(const char *datos, int tamanio, t_log *logger) {
  log_info(logger, "STDOUT (%d bytes): %s", tamanio, datos ? datos : "(null)");
  if (datos) {
    printf("[STDOUT] %.*s\n", tamanio, datos);
    fflush(stdout);
  }
}

void interfaz_sleep(int tiempo_ms, t_log *logger) {
  log_info(logger, "SLEEP %d ms", tiempo_ms);
  usleep((unsigned int)tiempo_ms * 1000);
  log_info(logger, "SLEEP finalizado");
}