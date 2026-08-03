#ifndef SWAP_MANAGER_H_
#define SWAP_MANAGER_H_

#include <commons/collections/list.h>
#include <stdint.h>

typedef struct {
  int id_segmento;
  int tamanio;
  t_list *bloques; // Lista de numeros de bloque (int*)
} t_swap_mapping_segmento;

typedef struct {
  int pid;
  t_list *segmentos; // Lista de t_swap_mapping_segmento*
} t_swap_mapping_proceso;

void swap_manager_inicializar(int block_size, int swap_size);
void swap_manager_destruir(void);

// Reservar bloques para un proceso segun los tamanios de sus segmentos
// Retorna 0 si ok, -1 si no hay espacio
int swap_manager_reservar_proceso(int pid, t_list *tamanio_segmentos);

// Obtener los bloques asignados a un proceso.
// Retorna la lista de t_swap_mapping_segmento*, o NULL si no existe
t_list *swap_manager_obtener_bloques(int pid);

// Liberar todos los bloques de un proceso
void swap_manager_liberar_proceso(int pid);

#endif
