#ifndef SWAP_ARCHIVO_H_
#define SWAP_ARCHIVO_H_

#include <commons/log.h>
#include <stdint.h>

// Inicializar el archivo de SWAP
int swap_inicializar(const char *path, int tam_archivo, int tam_blq);

// Escribir un bloque (por numero de bloque, el KM gestiona que bloque usar)
int swap_escribir_bloque(int numero_bloque, void *datos, int tamanio);

// Leer un bloque. Retorna datos (caller libera) y tamanio real leido.
void *swap_leer_bloque(int numero_bloque, int *tamanio_out);

// Liberar/limpiar un bloque
void swap_liberar_bloque(int numero_bloque);

// Destruir SWAP
void swap_destruir(void);

#endif
