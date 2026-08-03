#include "swap_archivo.h"
#include <commons/collections/list.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern t_log *logger_swap;

static FILE *archivo_swap = NULL;
static int tam_archivo_total = 0;
static int tam_bloque = 64;
static int cant_bloques = 0;

int swap_inicializar(const char *path, int tam_archivo, int tam_blq) {
  tam_archivo_total = tam_archivo;
  tam_bloque = tam_blq;
  cant_bloques = (tam_blq > 0) ? tam_archivo / tam_blq : 0;

  archivo_swap = fopen(path, "w+b");
  if (!archivo_swap) {
    log_error(logger_swap, "No se pudo crear archivo de SWAP: %s", path);
    return -1;
  }

  // Inicializar con ceros
  void *ceros = calloc(1, (size_t)tam_archivo);
  fwrite(ceros, 1, (size_t)tam_archivo, archivo_swap);
  fflush(archivo_swap);
  free(ceros);

  log_info(logger_swap, "SWAP inicializado: tam=%d, bloque=%d, bloques=%d",
           tam_archivo, tam_bloque, cant_bloques);
  return 0;
}

int swap_escribir_bloque(int numero_bloque, void *datos, int tamanio) {
  if (!archivo_swap || numero_bloque < 0 || numero_bloque >= cant_bloques)
    return -1;

  long offset = (long)numero_bloque * (long)tam_bloque;
  int tam_a_escribir = tamanio < tam_bloque ? tamanio : tam_bloque;

  if (fseek(archivo_swap, offset, SEEK_SET) != 0)
    return -1;
  size_t written = fwrite(datos, 1, (size_t)tam_a_escribir, archivo_swap);
  fflush(archivo_swap);

  return (written == (size_t)tam_a_escribir) ? 0 : -1;
}

void *swap_leer_bloque(int numero_bloque, int *tamanio_out) {
  if (!archivo_swap || numero_bloque < 0 || numero_bloque >= cant_bloques) {
    if (tamanio_out)
      *tamanio_out = 0;
    return NULL;
  }

  long offset = (long)numero_bloque * (long)tam_bloque;
  void *datos = malloc((size_t)tam_bloque);

  if (fseek(archivo_swap, offset, SEEK_SET) != 0) {
    free(datos);
    if (tamanio_out)
      *tamanio_out = 0;
    return NULL;
  }

  size_t leido = fread(datos, 1, (size_t)tam_bloque, archivo_swap);
  if (tamanio_out)
    *tamanio_out = (int)leido;
  return datos;
}

void swap_liberar_bloque(int numero_bloque) {
  if (!archivo_swap || numero_bloque < 0 || numero_bloque >= cant_bloques)
    return;

  // Limpiar el bloque con ceros
  void *ceros = calloc(1, (size_t)tam_bloque);
  long offset = (long)numero_bloque * (long)tam_bloque;
  fseek(archivo_swap, offset, SEEK_SET);
  fwrite(ceros, 1, (size_t)tam_bloque, archivo_swap);
  fflush(archivo_swap);
  free(ceros);
}

void swap_destruir(void) {
  if (archivo_swap) {
    fclose(archivo_swap);
    archivo_swap = NULL;
  }
}
