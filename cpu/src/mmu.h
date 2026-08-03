#ifndef MMU_H_
#define MMU_H_

#include <commons/collections/list.h>
#include <commons/log.h>
#include <stdint.h>

// Traduccion de direccion logica a fisica
// dir_logica -> (num_segmento, desplazamiento)
// num_segmento = dir_logica / tam_max_segmento
// desplazamiento = dir_logica % tam_max_segmento

typedef struct {
  int num_segmento;
  uint32_t desplazamiento;
} t_dir_traducida;

typedef struct {
  int id_segmento;
  uint32_t base;
  uint32_t tamanio;
} t_segmento_mmu;

// Configurar el tamanio maximo de segmento (se obtiene de Kernel Memory al
// conectar)
void mmu_configurar(int tam_max_segmento);

// Traducir una direccion logica a una direccion fisica
// Retorna la direccion fisica o -1 si hay segmentation fault
int mmu_traducir(uint32_t dir_logica, uint32_t tamanio, t_list *tabla_segmentos,
                 uint32_t *dir_fisica);

// Descomponer una direccion logica en segmento y desplazamiento
t_dir_traducida mmu_descomponer(uint32_t dir_logica);

#endif
