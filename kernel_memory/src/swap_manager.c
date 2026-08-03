#include "swap_manager.h"
#include <commons/bitarray.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

static t_list *procesos_en_swap = NULL;
static t_bitarray *bitmap_bloques = NULL;
static void *bitmap_mem = NULL;
static int cant_bloques = 0;
static int _block_size = 0;
static pthread_mutex_t mutex_swap_manager = PTHREAD_MUTEX_INITIALIZER;

void swap_manager_inicializar(int block_size, int swap_size) {
  _block_size = block_size;
  cant_bloques = swap_size / block_size;

  int bitmap_bytes = ceil((double)cant_bloques / 8.0);
  bitmap_mem = calloc(1, bitmap_bytes);
  bitmap_bloques = bitarray_create_with_mode(bitmap_mem, bitmap_bytes, LSB_FIRST);

  procesos_en_swap = list_create();
}

void swap_manager_destruir(void) {
  if (bitmap_bloques) {
    bitarray_destroy(bitmap_bloques);
    free(bitmap_mem);
  }
  if (procesos_en_swap) {
    for (int i = 0; i < list_size(procesos_en_swap); i++) {
      t_swap_mapping_proceso *p = list_get(procesos_en_swap, i);
      swap_manager_liberar_proceso(p->pid); // Esto limpia la lista iterativamente, pero es ineficiente en destruccion.
    }
    list_destroy(procesos_en_swap);
  }
}

static t_swap_mapping_proceso *buscar_proceso(int pid) {
  for (int i = 0; i < list_size(procesos_en_swap); i++) {
    t_swap_mapping_proceso *p = list_get(procesos_en_swap, i);
    if (p->pid == pid) {
      return p;
    }
  }
  return NULL;
}

int swap_manager_reservar_proceso(int pid, t_list *segmentos_info) {
  pthread_mutex_lock(&mutex_swap_manager);

  if (buscar_proceso(pid) != NULL) {
    pthread_mutex_unlock(&mutex_swap_manager);
    return -1; // Ya existe
  }

  // Calcular cant de bloques totales necesarios
  int bloques_necesarios = 0;
  for (int i = 0; i < list_size(segmentos_info); i++) {
    t_swap_mapping_segmento *seg = list_get(segmentos_info, i);
    bloques_necesarios += ceil((double)seg->tamanio / (double)_block_size);
  }

  // Verificar si hay suficientes bloques libres
  int libres = 0;
  for (int i = 0; i < cant_bloques; i++) {
    if (!bitarray_test_bit(bitmap_bloques, i)) libres++;
  }

  if (libres < bloques_necesarios) {
    pthread_mutex_unlock(&mutex_swap_manager);
    return -1; // No hay espacio
  }

  t_swap_mapping_proceso *nuevo = malloc(sizeof(t_swap_mapping_proceso));
  nuevo->pid = pid;
  nuevo->segmentos = list_create();

  int idx_bloque = 0;
  for (int i = 0; i < list_size(segmentos_info); i++) {
    t_swap_mapping_segmento *seg_info = list_get(segmentos_info, i);
    
    t_swap_mapping_segmento *seg_map = malloc(sizeof(t_swap_mapping_segmento));
    seg_map->id_segmento = seg_info->id_segmento;
    seg_map->tamanio = seg_info->tamanio;
    seg_map->bloques = list_create();

    int bloques_seg = ceil((double)seg_info->tamanio / (double)_block_size);
    for (int b = 0; b < bloques_seg; b++) {
      // Buscar siguiente bloque libre
      while (bitarray_test_bit(bitmap_bloques, idx_bloque)) {
        idx_bloque++;
      }
      bitarray_set_bit(bitmap_bloques, idx_bloque);

      int *num = malloc(sizeof(int));
      *num = idx_bloque;
      list_add(seg_map->bloques, num);
    }
    list_add(nuevo->segmentos, seg_map);
  }

  list_add(procesos_en_swap, nuevo);
  pthread_mutex_unlock(&mutex_swap_manager);
  return 0;
}

t_list *swap_manager_obtener_bloques(int pid) {
  pthread_mutex_lock(&mutex_swap_manager);
  t_swap_mapping_proceso *p = buscar_proceso(pid);
  t_list *res = p ? p->segmentos : NULL;
  pthread_mutex_unlock(&mutex_swap_manager);
  return res;
}

void swap_manager_liberar_proceso(int pid) {
  pthread_mutex_lock(&mutex_swap_manager);
  for (int i = 0; i < list_size(procesos_en_swap); i++) {
    t_swap_mapping_proceso *p = list_get(procesos_en_swap, i);
    if (p->pid == pid) {
      list_remove(procesos_en_swap, i);
      
      for (int s = 0; s < list_size(p->segmentos); s++) {
        t_swap_mapping_segmento *seg = list_get(p->segmentos, s);
        for (int b = 0; b < list_size(seg->bloques); b++) {
          int *num = list_get(seg->bloques, b);
          bitarray_clean_bit(bitmap_bloques, *num);
          free(num);
        }
        list_destroy(seg->bloques);
        free(seg);
      }
      list_destroy(p->segmentos);
      free(p);
      break;
    }
  }
  pthread_mutex_unlock(&mutex_swap_manager);
}
