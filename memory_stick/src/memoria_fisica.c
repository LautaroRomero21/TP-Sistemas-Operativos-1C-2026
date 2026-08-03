#include "memoria_fisica.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern t_log *logger_ms;

static void *espacio_memoria = NULL;
static int tam_memoria = 0;
static int retardo_acceso_ms = 0;

void memoria_fisica_inicializar(int tamanio, int retardo_ms) {
  tam_memoria = tamanio;
  retardo_acceso_ms = retardo_ms;
  espacio_memoria = calloc(1, (size_t)tamanio);
  log_info(logger_ms, "Memoria fisica inicializada: %d bytes, retardo=%d ms",
           tamanio, retardo_ms);
}

int memoria_fisica_leer(uint32_t direccion, void *destino, int tamanio) {
  if ((int)(direccion + (uint32_t)tamanio) > tam_memoria) {
    log_error(logger_ms, "Lectura fuera de rango: dir=%u tam=%d (max=%d)",
              direccion, tamanio, tam_memoria);
    return -1;
  }

  // Aplicar retardo de acceso
  if (retardo_acceso_ms > 0) {
    usleep((useconds_t)retardo_acceso_ms * 1000);
  }

  memcpy(destino, (char *)espacio_memoria + direccion, (size_t)tamanio);
  log_info(logger_ms, "## Lectura de %d bytes", tamanio);
  return 0;
}

int memoria_fisica_escribir(uint32_t direccion, void *origen, int tamanio) {
  if ((int)(direccion + (uint32_t)tamanio) > tam_memoria) {
    log_error(logger_ms, "Escritura fuera de rango: dir=%u tam=%d (max=%d)",
              direccion, tamanio, tam_memoria);
    return -1;
  }

  // Aplicar retardo de acceso
  if (retardo_acceso_ms > 0) {
    usleep((useconds_t)retardo_acceso_ms * 1000);
  }

  memcpy((char *)espacio_memoria + direccion, origen, (size_t)tamanio);
  log_info(logger_ms, "## Escritura de %d bytes", tamanio);
  return 0;
}

int memoria_fisica_tamanio(void) { return tam_memoria; }

void memoria_fisica_destruir(void) {
  free(espacio_memoria);
  espacio_memoria = NULL;
  tam_memoria = 0;
}
