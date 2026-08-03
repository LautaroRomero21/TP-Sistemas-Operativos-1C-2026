#ifndef MEMORIA_FISICA_H_
#define MEMORIA_FISICA_H_

#include <commons/log.h>
#include <stdint.h>

// Inicializar la memoria fisica
void memoria_fisica_inicializar(int tamanio, int retardo_ms);

// Lee bytes de la memoria fisica y retorna 0 si esta OK, -1 si hay error
int memoria_fisica_leer(uint32_t direccion, void *destino, int tamanio);

// Escribir bytes en la memoria fisica y retorna 0 si esta OK, -1 si hay error

int memoria_fisica_escribir(uint32_t direccion, void *origen, int tamanio);

// Obtiene el tamaño de la memoria
int memoria_fisica_tamanio(void);

// Liberar la memoria fisica
void memoria_fisica_destruir(void);

#endif