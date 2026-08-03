#include "mmu.h"

static int tam_max_seg = 64; // Valor por defecto

void mmu_configurar(int tam_max_segmento) { tam_max_seg = tam_max_segmento; }

t_dir_traducida mmu_descomponer(uint32_t dir_logica) {
  t_dir_traducida resultado;
  resultado.num_segmento = (int)(dir_logica / (uint32_t)tam_max_seg);
  resultado.desplazamiento = dir_logica % (uint32_t)tam_max_seg;
  return resultado;
}

int mmu_traducir(uint32_t dir_logica, uint32_t tamanio, t_list *tabla_segmentos,
                 uint32_t *dir_fisica) {
  t_dir_traducida desc = mmu_descomponer(dir_logica);

  // Buscar el segmento en la tabla
  for (int i = 0; i < list_size(tabla_segmentos); i++) {
    t_segmento_mmu *seg = list_get(tabla_segmentos, i);
    if (seg->id_segmento == desc.num_segmento) {
      // Verificar que el desplazamiento + tamanio no exceda el tamanio del
      // segmento
      if (desc.desplazamiento + tamanio > seg->tamanio) {
        // Segmentation Fault
        return -1;
      }
      *dir_fisica = seg->base + desc.desplazamiento;
      return 0;
    }
  }

  // Segmento no encontrado
  return -1;
}
