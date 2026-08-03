#ifndef SEGMENTACION_H_
#define SEGMENTACION_H_

#include <commons/collections/list.h>
#include <commons/log.h>
#include <stdint.h>

typedef enum { AJUSTE_BEST_FIT, AJUSTE_WORST_FIT } t_algoritmo_ajuste;

// Segmento asignado a un proceso
typedef struct {
  int pid;
  int id_segmento;
  uint32_t base;
  uint32_t tamanio;
} t_segmento;

// Hueco libre en memoria
typedef struct {
  uint32_t base;
  uint32_t tamanio;
} t_hueco;

// ─── Lista de Memory Sticks (multi-MS support) ───
typedef struct {
  int socket;
  int tam_memoria; // Bytes que maneja este MS
  uint32_t
      base_addr; // Direccion base que le corresponde (asignada secuencialmente)
  int conectado; // 0 si se desconecto
  char ip[32];
  char puerto[16];
} t_memory_stick;

// Obtener el MS que corresponde a una direccion fisica
t_memory_stick *obtener_ms_para_dir(uint32_t dir_fisica);

extern t_list *lista_ms;

// Inicializar la tabla de segmentos global
void segmentacion_inicializar(int tam_memoria_total,
                              t_algoritmo_ajuste algoritmo);

// Asignar un segmento a un proceso
// Retorna 0 si OK, -1 si no hay espacio, -2 si necesita compactacion
int segmento_asignar(int pid, int id_segmento, uint32_t tamanio);

// Liberar un segmento de un proceso
void segmento_liberar(int pid, int id_segmento);

// Liberar todos los segmentos de un proceso
void segmentos_liberar_proceso(int pid);

// Obtener tabla de segmentos de un proceso
t_list *segmentos_obtener_tabla(int pid);

// Buscar un segmento especifico
t_segmento *segmento_buscar(int pid, int id_segmento);

// Compactar memoria
void segmentos_compactar(int socket_memory_stick);

// Expandir espacio total (cuando se agrega un nuevo Memory Stick)
void segmentacion_expandir(int bytes_adicionales);

// Obtener lista de huecos libres
t_list *segmentos_obtener_huecos(void);

// Obtener memoria total usada
uint32_t segmentos_memoria_usada(void);

// Obtener la lista global de segmentos
t_list *segmentos_obtener_todos(void);

// Verifica si una lista de tamanos de segmentos pueden asignarse sin compactar
int segmentos_pueden_asignarse_sin_compactar(t_list *tamanios);

#endif
