
#include "segmentacion.h"
#include "../../utils/src/utils/protocolos.h"
#include <stdlib.h>
#include <string.h>

extern t_log *logger_km;

static t_list *lista_segmentos = NULL; // Lista de t_segmento*
static t_list *lista_huecos = NULL;    // Lista de t_hueco*
static int tam_total = 0;
static t_algoritmo_ajuste alg_ajuste = AJUSTE_BEST_FIT;

void segmentacion_inicializar(int tam_memoria_total,
                              t_algoritmo_ajuste algoritmo) {
  tam_total = tam_memoria_total;
  alg_ajuste = algoritmo;

  lista_segmentos = list_create();
  lista_huecos = list_create();

  // Toda la memoria es un hueco libre
  t_hueco *hueco = malloc(sizeof(t_hueco));
  hueco->base = 0;
  hueco->tamanio = (uint32_t)tam_total;
  list_add(lista_huecos, hueco);
}

void segmentacion_expandir(int bytes_adicionales) {
  if (bytes_adicionales <= 0)
    return;
  // Agregar un hueco libre al final del espacio actual
  t_hueco *nuevo = malloc(sizeof(t_hueco));
  nuevo->base = (uint32_t)tam_total;
  nuevo->tamanio = (uint32_t)bytes_adicionales;
  list_add(lista_huecos, nuevo);
  tam_total += bytes_adicionales;

  // Fusionar huecos adyacentes para evitar fragmentacion inicial
  for (int j = 0; j < list_size(lista_huecos) - 1;) {
    t_hueco *h1 = list_get(lista_huecos, j);
    t_hueco *h2 = list_get(lista_huecos, j + 1);
    if (h1->base + h1->tamanio == h2->base) {
      h1->tamanio += h2->tamanio;
      list_remove(lista_huecos, j + 1);
      free(h2);
    } else {
      j++;
    }
  }
  log_info(logger_km, "Segmentacion expandida: +%d bytes (total=%d)",
           bytes_adicionales, tam_total);
}

static t_hueco *buscar_hueco_best_fit(uint32_t tamanio) {
  t_hueco *mejor = NULL;
  for (int i = 0; i < list_size(lista_huecos); i++) {
    t_hueco *h = list_get(lista_huecos, i);
    if (h->tamanio >= tamanio) {
      if (!mejor || h->tamanio < mejor->tamanio) {
        mejor = h;
      }
    }
  }
  return mejor;
}

static t_hueco *buscar_hueco_worst_fit(uint32_t tamanio) {
  t_hueco *peor = NULL;
  for (int i = 0; i < list_size(lista_huecos); i++) {
    t_hueco *h = list_get(lista_huecos, i);
    if (h->tamanio >= tamanio) {
      if (!peor || h->tamanio > peor->tamanio) {
        peor = h;
      }
    }
  }
  return peor;
}

static int hay_espacio_total_libre(uint32_t tamanio) {
  uint32_t total_libre = 0;
  for (int i = 0; i < list_size(lista_huecos); i++) {
    t_hueco *h = list_get(lista_huecos, i);
    total_libre += h->tamanio;
  }
  return total_libre >= tamanio;
}

int segmento_asignar(int pid, int id_segmento, uint32_t tamanio) {
  t_hueco *hueco = NULL;
  if (alg_ajuste == AJUSTE_BEST_FIT)
    hueco = buscar_hueco_best_fit(tamanio);
  else
    hueco = buscar_hueco_worst_fit(tamanio);

  if (!hueco) {
    // Verificar si compactando habria espacio
    if (hay_espacio_total_libre(tamanio)) {
      return -2; // Necesita compactacion
    }
    return -1; // No hay espacio
  }

  // Crear segmento
  t_segmento *seg = malloc(sizeof(t_segmento));
  seg->pid = pid;
  seg->id_segmento = id_segmento;
  seg->base = hueco->base;
  seg->tamanio = tamanio;
  list_add(lista_segmentos, seg);

  log_info(logger_km, "## PID: %d - Segmento Creado %d - Tamaño: %u", pid,
           id_segmento, tamanio);

  // Actualizar hueco
  if (hueco->tamanio == tamanio) {
    // El hueco se lleno completamente, removerlo
    for (int i = 0; i < list_size(lista_huecos); i++) {
      if (list_get(lista_huecos, i) == hueco) {
        list_remove(lista_huecos, i);
        free(hueco);
        break;
      }
    }
  } else {
    hueco->base += tamanio;
    hueco->tamanio -= tamanio;
  }

  return 0;
}

void segmento_liberar(int pid, int id_segmento) {
  for (int i = 0; i < list_size(lista_segmentos); i++) {
    t_segmento *seg = list_get(lista_segmentos, i);
    if (seg->pid == pid && seg->id_segmento == id_segmento) {
      // Crear hueco libre
      t_hueco *nuevo_hueco = malloc(sizeof(t_hueco));
      nuevo_hueco->base = seg->base;
      nuevo_hueco->tamanio = seg->tamanio;

      log_info(logger_km, "Segmento liberado: PID=%d seg=%d base=%u tam=%u",
               pid, id_segmento, seg->base, seg->tamanio);

      list_remove(lista_segmentos, i);
      free(seg);

      // Insertar hueco ordenadamente y fusionar con adyacentes
      int insertado = 0;
      for (int j = 0; j < list_size(lista_huecos); j++) {
        t_hueco *h = list_get(lista_huecos, j);
        if (nuevo_hueco->base < h->base) {
          list_add_in_index(lista_huecos, j, nuevo_hueco);
          insertado = 1;
          break;
        }
      }
      if (!insertado) {
        list_add(lista_huecos, nuevo_hueco);
      }

      // Fusionar huecos adyacentes
      for (int j = 0; j < list_size(lista_huecos) - 1;) {
        t_hueco *h1 = list_get(lista_huecos, j);
        t_hueco *h2 = list_get(lista_huecos, j + 1);
        if (h1->base + h1->tamanio == h2->base) {
          h1->tamanio += h2->tamanio;
          list_remove(lista_huecos, j + 1);
          free(h2);
        } else {
          j++;
        }
      }

      return;
    }
  }
}

void segmentos_liberar_proceso(int pid) {
  for (int i = list_size(lista_segmentos) - 1; i >= 0; i--) {
    t_segmento *seg = list_get(lista_segmentos, i);
    if (seg->pid == pid) {
      segmento_liberar(pid, seg->id_segmento);
      // Re-iterar desde el final porque la lista cambio
      i = list_size(lista_segmentos);
    }
  }
}

t_list *segmentos_obtener_tabla(int pid) {
  t_list *tabla = list_create();
  for (int i = 0; i < list_size(lista_segmentos); i++) {
    t_segmento *seg = list_get(lista_segmentos, i);
    if (seg->pid == pid) {
      list_add(tabla, seg);
    }
  }
  return tabla;
}

t_segmento *segmento_buscar(int pid, int id_segmento) {
  for (int i = 0; i < list_size(lista_segmentos); i++) {
    t_segmento *seg = list_get(lista_segmentos, i);
    if (seg->pid == pid && seg->id_segmento == id_segmento) {
      return seg;
    }
  }
  return NULL;
}

int km_leer_global(uint32_t dir_fisica, void *destino, int tamanio) {
  int offset = 0;
  while (tamanio > 0) {
    t_memory_stick *ms_dest = NULL;
    for (int i = 0; i < list_size(lista_ms); i++) {
      t_memory_stick *m = list_get(lista_ms, i);
      if (dir_fisica >= m->base_addr &&
          dir_fisica < m->base_addr + m->tam_memoria) {
        ms_dest = m;
        break;
      }
    }
    if (!ms_dest || !ms_dest->conectado)
      return -1;

    uint32_t base_relativa = dir_fisica - ms_dest->base_addr;
    int chunk = tamanio;
    if (base_relativa + chunk > ms_dest->tam_memoria) {
      chunk = ms_dest->tam_memoria - base_relativa;
    }

    t_paquete *paq = crear_paquete(MS_LEER);
    agregar_a_paquete(paq, &base_relativa, sizeof(uint32_t));
    agregar_a_paquete(paq, &chunk, sizeof(int));
    enviar_paquete(paq, ms_dest->socket);
    eliminar_paquete(paq);

    int op = recibir_operacion(ms_dest->socket);
    if (op == MS_RESPUESTA_LEER) {
      t_paquete *resp = recibir_paquete(ms_dest->socket);
      if (resp) {
        buffer_read(destino + offset, resp->buffer, chunk);
        eliminar_paquete(resp);
      }
    } else {
      return -1;
    }

    dir_fisica += chunk;
    offset += chunk;
    tamanio -= chunk;
  }
  return 0;
}

int km_escribir_global(uint32_t dir_fisica, void *origen, int tamanio) {
  int offset = 0;
  while (tamanio > 0) {
    t_memory_stick *ms_dest = NULL;
    for (int i = 0; i < list_size(lista_ms); i++) {
      t_memory_stick *m = list_get(lista_ms, i);
      if (dir_fisica >= m->base_addr &&
          dir_fisica < m->base_addr + m->tam_memoria) {
        ms_dest = m;
        break;
      }
    }
    if (!ms_dest || !ms_dest->conectado)
      return -1;

    uint32_t base_relativa = dir_fisica - ms_dest->base_addr;
    int chunk = tamanio;
    if (base_relativa + chunk > ms_dest->tam_memoria) {
      chunk = ms_dest->tam_memoria - base_relativa;
    }

    t_paquete *paq = crear_paquete(MS_ESCRIBIR);
    agregar_a_paquete(paq, &base_relativa, sizeof(uint32_t));
    agregar_a_paquete(paq, &chunk, sizeof(int));
    agregar_a_paquete(paq, origen + offset, chunk);
    enviar_paquete(paq, ms_dest->socket);
    eliminar_paquete(paq);

    int op = recibir_operacion(ms_dest->socket);
    if (op != MS_RESPUESTA_OK) {
      return -1;
    }

    dir_fisica += chunk;
    offset += chunk;
    tamanio -= chunk;
  }
  return 0;
}

void segmentos_compactar(int socket_memory_stick_unused) {
  log_info(logger_km, "Iniciando compactacion de memoria...");

  // Ordenar segmentos por base
  for (int i = 0; i < list_size(lista_segmentos) - 1; i++) {
    for (int j = 0; j < list_size(lista_segmentos) - 1 - i; j++) {
      t_segmento *s1 = list_get(lista_segmentos, j);
      t_segmento *s2 = list_get(lista_segmentos, j + 1);
      if (s1->base > s2->base) {
        list_replace(lista_segmentos, j, s2);
        list_replace(lista_segmentos, j + 1, s1);
      }
    }
  }

  // Vaciar lista de huecos
  for (int i = list_size(lista_huecos) - 1; i >= 0; i--) {
    free(list_remove(lista_huecos, i));
  }

  // Mover segmentos globalmente
  uint32_t nueva_base = 0;

  for (int i = 0; i < list_size(lista_segmentos); i++) {
    t_segmento *seg = list_get(lista_segmentos, i);
    if (seg->base != nueva_base) {
      void *datos = malloc(seg->tamanio);
      if (km_leer_global(seg->base, datos, seg->tamanio) == 0) {
        if (km_escribir_global(nueva_base, datos, seg->tamanio) < 0) {
          log_error(logger_km,
                    "Error al escribir en Memory Stick durante compactacion");
        }
      } else {
        log_error(logger_km,
                  "Error al leer del Memory Stick durante compactacion");
      }
      free(datos);

      log_info(logger_km,
               "Compactacion: PID=%d seg=%d movido de base=%u a base=%u",
               seg->pid, seg->id_segmento, seg->base, nueva_base);
      seg->base = nueva_base;
    }
    nueva_base += seg->tamanio;
  }

  // Crear un unico hueco libre al final de la memoria global
  if (nueva_base < (uint32_t)tam_total) {
    t_hueco *h = malloc(sizeof(t_hueco));
    h->base = nueva_base;
    h->tamanio = (uint32_t)tam_total - nueva_base;
    list_add(lista_huecos, h);
  }

  log_info(
      logger_km,
      "Compactacion finalizada. Espacio libre total: %u bytes en %d huecos",
      (uint32_t)tam_total - nueva_base, list_size(lista_huecos));
}

t_list *segmentos_obtener_huecos(void) { return lista_huecos; }

uint32_t segmentos_memoria_usada(void) {
  uint32_t usada = 0;
  for (int i = 0; i < list_size(lista_segmentos); i++) {
    t_segmento *seg = list_get(lista_segmentos, i);
    usada += seg->tamanio;
  }
  return usada;
}

t_list *segmentos_obtener_todos(void) { return lista_segmentos; }

int segmentos_pueden_asignarse_sin_compactar(t_list *tamanios) {
  t_list *copia_huecos = list_create();
  for (int i = 0; i < list_size(lista_huecos); i++) {
    t_hueco *h = list_get(lista_huecos, i);
    t_hueco *hc = malloc(sizeof(t_hueco));
    hc->base = h->base;
    hc->tamanio = h->tamanio;
    list_add(copia_huecos, hc);
  }

  int exito = 1;

  for (int i = 0; i < list_size(tamanios); i++) {
    uint32_t *tam = list_get(tamanios, i);
    t_hueco *hueco = NULL;

    if (alg_ajuste == AJUSTE_BEST_FIT) {
      for (int j = 0; j < list_size(copia_huecos); j++) {
        t_hueco *h = list_get(copia_huecos, j);
        if (h->tamanio >= *tam) {
          if (!hueco || h->tamanio < hueco->tamanio)
            hueco = h;
        }
      }
    } else {
      for (int j = 0; j < list_size(copia_huecos); j++) {
        t_hueco *h = list_get(copia_huecos, j);
        if (h->tamanio >= *tam) {
          if (!hueco || h->tamanio > hueco->tamanio)
            hueco = h;
        }
      }
    }

    if (!hueco) {
      exito = 0;
      break;
    }

    if (hueco->tamanio == *tam) {
      for (int k = 0; k < list_size(copia_huecos); k++) {
        if (list_get(copia_huecos, k) == hueco) {
          list_remove(copia_huecos, k);
          free(hueco);
          break;
        }
      }
    } else {
      hueco->base += *tam;
      hueco->tamanio -= *tam;
    }
  }

  for (int i = 0; i < list_size(copia_huecos); i++) {
    free(list_get(copia_huecos, i));
  }
  list_destroy(copia_huecos);

  return exito;
}
