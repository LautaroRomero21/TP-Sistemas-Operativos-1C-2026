#include "../../utils/src/utils/conexiones.h"
#include "../../utils/src/utils/protocolos.h"
#include "contexto.h"
#include "instrucciones.h"
#include "segmentacion.h"
#include "swap_manager.h"
#include <arpa/inet.h>
#include <commons/collections/list.h>
#include <commons/config.h>
#include <commons/log.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

t_log *logger_km = NULL;

static int socket_servidor = -1;
static int socket_swap = -1;
static int segmentacion_lista = 0;
static char *path_scripts = NULL;
static int tam_max_segmento = 64;
static int retardo_compactacion_ms = 500;
static int instruction_delay_ms = 0;
static t_algoritmo_ajuste algoritmo_ajuste = AJUSTE_BEST_FIT;

// ─── Lista de Memory Sticks (multi-MS support) ───
t_list *lista_ms = NULL;
static pthread_mutex_t mutex_ms = PTHREAD_MUTEX_INITIALIZER;

// Obtener el MS que corresponde a una direccion fisica
t_memory_stick *obtener_ms_para_dir(uint32_t dir_fisica) {
  if (!lista_ms)
    return NULL;
  for (int i = 0; i < list_size(lista_ms); i++) {
    t_memory_stick *ms = list_get(lista_ms, i);
    if (!ms->conectado)
      continue;
    if (dir_fisica >= ms->base_addr &&
        dir_fisica < ms->base_addr + (uint32_t)ms->tam_memoria)
      return ms;
  }
  // Fallback: primer MS conectado
  for (int i = 0; i < list_size(lista_ms); i++) {
    t_memory_stick *ms = list_get(lista_ms, i);
    if (ms->conectado)
      return ms;
  }
  return NULL;
}

// Obtener el socket MS que corresponde a una direccion fisica
static int socket_ms_para_dir(uint32_t dir_fisica) {
  if (!lista_ms)
    return -1;
  for (int i = 0; i < list_size(lista_ms); i++) {
    t_memory_stick *ms = list_get(lista_ms, i);
    if (!ms->conectado)
      continue;
    if (dir_fisica >= ms->base_addr &&
        dir_fisica < ms->base_addr + (uint32_t)ms->tam_memoria)
      return ms->socket;
  }
  // Fallback: primer MS conectado
  for (int i = 0; i < list_size(lista_ms); i++) {
    t_memory_stick *ms = list_get(lista_ms, i);
    if (ms->conectado)
      return ms->socket;
  }
  return -1;
}

// Alias de compatibilidad para el resto del codigo
#define socket_memory_stick (socket_ms_para_dir(0))

static pthread_mutex_t mutex_memoria = PTHREAD_MUTEX_INITIALIZER;
static int socket_ks_notificaciones = -1;
static int swap_block_size = 0;

// Responder OK
static void responder_ok(int socket) {
  t_paquete *resp = crear_paquete(KM_RESPUESTA_OK);
  enviar_paquete(resp, socket);
  eliminar_paquete(resp);
}

// Responder ERROR
static void responder_error(int socket) {
  t_paquete *resp = crear_paquete(KM_RESPUESTA_ERROR);
  enviar_paquete(resp, socket);
  eliminar_paquete(resp);
}

// Responder con necesidad de compactacion
static void responder_compactar(int socket) {
  t_paquete *resp = crear_paquete(KM_RESPUESTA_COMPACTAR);
  enviar_paquete(resp, socket);
  eliminar_paquete(resp);
}

// Argumento para el hilo de monitoreo de Memory Stick
typedef struct {
  int socket;
  int ms_idx;
} t_ms_hilo_arg;

static void *hilo_ms_monitor(void *arg) {
  t_ms_hilo_arg *a = arg;
  int sock = a->socket;
  int idx = a->ms_idx;
  free(a);

  // Bloquear hasta que el socket se cierre
  char buf[1];
  while (recv(sock, buf, 1, MSG_PEEK) > 0)
    usleep(100000);

  // Marcar MS como desconectado
  pthread_mutex_lock(&mutex_ms);
  for (int i = 0; i < list_size(lista_ms); i++) {
    t_memory_stick *m = list_get(lista_ms, i);
    if (m->socket == sock) {
      m->conectado = 0;
      log_warning(logger_km, "## Memory Stick %d desconectado (base=0x%X)", idx,
                  (unsigned)m->base_addr);
      break;
    }
  }
  pthread_mutex_unlock(&mutex_ms);

  if (socket_ks_notificaciones != -1) {
    enviar_operacion(socket_ks_notificaciones, KM_BSOD);
    log_info(logger_km, "Notificando BSOD al KS");
  }

  close(sock);
  return NULL;
}

// Helper para leer cruzando boundaries de Memory Sticks
static int leer_de_ms(uint32_t dir_fisica, void *datos, int tamanio) {
  int offset = 0;
  while (offset < tamanio) {
    t_memory_stick *ms = obtener_ms_para_dir(dir_fisica + offset);
    if (!ms || !ms->conectado)
      return -1;

    uint32_t base_local = (dir_fisica + offset) - ms->base_addr;
    int tam_disponible = ms->tam_memoria - base_local;
    int chunk = (tamanio - offset < tam_disponible) ? (tamanio - offset)
                                                    : tam_disponible;

    t_paquete *paq = crear_paquete(MS_LEER);
    agregar_a_paquete(paq, &base_local, sizeof(uint32_t));
    agregar_a_paquete(paq, &chunk, sizeof(int));
    enviar_paquete(paq, ms->socket);
    eliminar_paquete(paq);

    int op = recibir_operacion(ms->socket);
    if (op != MS_RESPUESTA_LEER)
      return -1;

    t_paquete *resp = recibir_paquete(ms->socket);
    if (!resp)
      return -1;
    buffer_read((char *)datos + offset, resp->buffer, chunk);
    eliminar_paquete(resp);

    offset += chunk;
  }
  return 0;
}

// Helper para escribir cruzando boundaries de Memory Sticks
static int escribir_en_ms(uint32_t dir_fisica, void *datos, int tamanio) {
  int offset = 0;
  while (offset < tamanio) {
    t_memory_stick *ms = obtener_ms_para_dir(dir_fisica + offset);
    if (!ms || !ms->conectado)
      return -1;

    uint32_t base_local = (dir_fisica + offset) - ms->base_addr;
    int tam_disponible = ms->tam_memoria - base_local;
    int chunk = (tamanio - offset < tam_disponible) ? (tamanio - offset)
                                                    : tam_disponible;

    t_paquete *paq = crear_paquete(MS_ESCRIBIR);
    agregar_a_paquete(paq, &base_local, sizeof(uint32_t));
    agregar_a_paquete(paq, &chunk, sizeof(int));
    agregar_a_paquete(paq, (char *)datos + offset, chunk);
    enviar_paquete(paq, ms->socket);
    eliminar_paquete(paq);

    int op = recibir_operacion(ms->socket);
    if (op != MS_RESPUESTA_OK)
      return -1;

    offset += chunk;
  }
  return 0;
}

// Hilo que atiende a un cliente (KS o CPU)
static void *hilo_atender_cliente(void *arg) {
  int socket_cliente = *(int *)arg;
  free(arg);

  while (1) {
    int op = recibir_operacion(socket_cliente);
    if (op < 0) {
      log_warning(logger_km, "Cliente desconectado");
      close(socket_cliente);
      return NULL;
    }

    t_paquete *paquete = recibir_paquete(socket_cliente);
    if (!paquete)
      continue;

    pthread_mutex_lock(&mutex_memoria);

    switch (op) {
    case KS_INIT_PROCESO: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      char *path = buffer_read_string(paquete->buffer);

      log_info(logger_km, "## PID: %d - Proceso Creado", pid);

      // Verificar que la segmentacion este inicializada (necesita Memory Stick)
      if (!segmentacion_lista) {
        log_error(logger_km,
                  "Segmentacion no inicializada (Memory Stick no conectado)");
        free(path);
        responder_error(socket_cliente);
        break;
      }

      // Cargar instrucciones
      int resultado = instrucciones_cargar(pid, path_scripts, path);
      free(path);

      if (resultado == 0) {
        responder_ok(socket_cliente);
      } else {
        responder_error(socket_cliente);
      }
      break;
    }

    case KS_VERIFICAR_ESPACIO: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));

      t_list *map_segs = swap_manager_obtener_bloques(pid);
      if (!map_segs || list_is_empty(map_segs)) {
        // Si no tiene segmentos en swap, no necesita espacio adicional
        responder_ok(socket_cliente);
        break;
      }

      t_list *tamanios = list_create();
      for (int i = 0; i < list_size(map_segs); i++) {
        t_swap_mapping_segmento *map_seg = list_get(map_segs, i);
        uint32_t *t = malloc(sizeof(uint32_t));
        *t = (uint32_t)map_seg->tamanio;
        list_add(tamanios, t);
      }

      int cabe = segmentos_pueden_asignarse_sin_compactar(tamanios);

      for (int i = 0; i < list_size(tamanios); i++) {
        free(list_get(tamanios, i));
      }
      list_destroy(tamanios);

      if (cabe) {
        responder_ok(socket_cliente);
      } else {
        responder_error(socket_cliente);
      }
      break;
    }

    case KS_FIN_PROCESO: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));

      log_info(logger_km, "## Finalizacion de Proceso: PID %d", pid);

      segmentos_liberar_proceso(pid);
      instrucciones_liberar(pid);
      if (socket_swap >= 0) {
        t_paquete *paq_swap = crear_paquete(SWAP_LIBERAR);
        agregar_a_paquete(paq_swap, &pid, sizeof(int));
        enviar_paquete(paq_swap, socket_swap);
        eliminar_paquete(paq_swap);

        int op_swap = recibir_operacion(socket_swap);
        t_paquete *resp_swap = recibir_paquete(socket_swap);
        (void)op_swap;
        if (resp_swap)
          eliminar_paquete(resp_swap);
      }

      responder_ok(socket_cliente);
      break;
    }

    case KS_SUSPENDER_PROCESO: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));

      log_info(logger_km, "Suspendiendo proceso PID %d (moviendo a SWAP)", pid);

      // Obtener segmentos del proceso
      t_list *tabla = segmentos_obtener_tabla(pid);

      // Reservar espacio en SWAP
      if (socket_swap >= 0 && swap_block_size > 0) {
        t_list *info_segs = list_create();
        for (int i = 0; i < list_size(tabla); i++) {
          t_segmento *seg = list_get(tabla, i);
          t_swap_mapping_segmento *ms = malloc(sizeof(t_swap_mapping_segmento));
          ms->id_segmento = seg->id_segmento;
          ms->tamanio = seg->tamanio;
          ms->bloques = NULL;
          list_add(info_segs, ms);
        }

        if (swap_manager_reservar_proceso(pid, info_segs) < 0) {
          log_error(logger_km, "No hay espacio en SWAP para suspender PID %d",
                    pid);
          responder_error(socket_cliente);
          // Liberar info_segs
          for (int i = 0; i < list_size(info_segs); i++)
            free(list_get(info_segs, i));
          list_destroy(info_segs);
          break;
        }

        // Liberar info_segs, swap_manager ya copio lo necesario
        for (int i = 0; i < list_size(info_segs); i++)
          free(list_get(info_segs, i));
        list_destroy(info_segs);

        t_list *map_segs = swap_manager_obtener_bloques(pid);

        // Para cada segmento, leer de Memory Stick y escribir en SWAP por
        // bloques
        for (int i = 0; i < list_size(tabla); i++) {
          t_segmento *seg = list_get(tabla, i);

          // Buscar el mapping correspondiente
          t_swap_mapping_segmento *map_seg = NULL;
          for (int j = 0; j < list_size(map_segs); j++) {
            t_swap_mapping_segmento *m = list_get(map_segs, j);
            if (m->id_segmento == seg->id_segmento) {
              map_seg = m;
              break;
            }
          }

          int tam = (int)seg->tamanio;
          void *datos = malloc(tam);

          // Leer de Memory Stick cruzando boundaries si es necesario
          int status = leer_de_ms(seg->base, datos, tam);

          if (status == 0) {
            // Escribir en SWAP por bloques
            int offset = 0;
            for (int b = 0; b < list_size(map_seg->bloques); b++) {
              int *num_bloque = list_get(map_seg->bloques, b);
              int bytes_chunk = swap_block_size;
              if (offset + bytes_chunk > tam) {
                bytes_chunk = tam - offset;
              }

              t_paquete *paq_swap_esc = crear_paquete(SWAP_ESCRIBIR_BLOQUE);
              agregar_a_paquete(paq_swap_esc, num_bloque, sizeof(int));
              agregar_a_paquete(paq_swap_esc, &bytes_chunk, sizeof(int));
              agregar_a_paquete(paq_swap_esc, (char *)datos + offset,
                                bytes_chunk);
              enviar_paquete(paq_swap_esc, socket_swap);
              eliminar_paquete(paq_swap_esc);

              int op_swap = recibir_operacion(socket_swap);
              t_paquete *resp_swap = recibir_paquete(socket_swap);
              (void)op_swap;
              if (resp_swap)
                eliminar_paquete(resp_swap);

              offset += bytes_chunk;
            }
          }

          free(datos);
        }
      }

      // Liberar segmentos de memoria principal
      segmentos_liberar_proceso(pid);
      list_destroy(tabla);

      responder_ok(socket_cliente);
      break;
    }

    case KS_REANUDAR_PROCESO: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));

      log_info(logger_km, "Reanudando proceso PID %d (trayendo de SWAP)", pid);

      // Leer datos de SWAP y re-asignar segmentos
      if (socket_swap >= 0 && swap_block_size > 0) {
        t_list *map_segs = swap_manager_obtener_bloques(pid);
        if (map_segs != NULL) {
          for (int i = 0; i < list_size(map_segs); i++) {
            t_swap_mapping_segmento *map_seg = list_get(map_segs, i);
            int id_seg = map_seg->id_segmento;
            int tam = map_seg->tamanio;

            int bloques_totales = list_size(map_seg->bloques);
            int tam_asignado = (bloques_totales * swap_block_size > tam)
                                   ? bloques_totales * swap_block_size
                                   : tam;
            if (tam_asignado == 0)
              tam_asignado = 1; // Prevenir malloc(0)
            void *datos = malloc(tam_asignado);
            int offset = 0;

            // Leer de SWAP por bloques
            for (int b = 0; b < list_size(map_seg->bloques); b++) {
              int *num_bloque = list_get(map_seg->bloques, b);

              t_paquete *paq_leer = crear_paquete(SWAP_LEER_BLOQUE);
              agregar_a_paquete(paq_leer, num_bloque, sizeof(int));
              enviar_paquete(paq_leer, socket_swap);
              eliminar_paquete(paq_leer);

              int op_swap = recibir_operacion(socket_swap);
              if (op_swap == SWAP_RESPUESTA_DATOS) {
                t_paquete *resp_swap = recibir_paquete(socket_swap);
                if (resp_swap) {
                  int chunk_tam = 0;
                  buffer_read(&chunk_tam, resp_swap->buffer, sizeof(int));
                  if (chunk_tam > 0) {
                    buffer_read((char *)datos + offset, resp_swap->buffer,
                                chunk_tam);
                    offset += chunk_tam;
                  }
                  eliminar_paquete(resp_swap);
                }
              }
            }

            // Re-asignar segmento en memoria
            int res = segmento_asignar(pid, id_seg, (uint32_t)tam);
            if (res == -2) {
              // Compactar y reintentar
              segmentos_compactar(socket_memory_stick);
              usleep((useconds_t)retardo_compactacion_ms * 1000);
              res = segmento_asignar(pid, id_seg, (uint32_t)tam);
            }

            if (res == 0) {
              // Escribir datos en Memory Stick cruzando boundaries si es
              // necesario
              t_segmento *seg = segmento_buscar(pid, id_seg);
              if (seg) {
                escribir_en_ms(seg->base, datos, tam);
              }
            }
            free(datos);
          }
        }
        swap_manager_liberar_proceso(pid);
      }

      responder_ok(socket_cliente);
      break;
    }

    case KS_MEM_ALLOC: {
      int pid = 0, id_segmento = 0, tamanio = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      buffer_read(&id_segmento, paquete->buffer, sizeof(int));
      buffer_read(&tamanio, paquete->buffer, sizeof(int));

      int resultado = segmento_asignar(pid, id_segmento, (uint32_t)tamanio);
      if (resultado == 0) {
        t_segmento *seg = segmento_buscar(pid, id_segmento);
        uint32_t base = seg ? seg->base : 0;
        log_info(logger_km, "MEM_ALLOC: PID=%d seg=%d base=%u tam=%d", pid,
                 id_segmento, base, tamanio);
        responder_ok(socket_cliente);
      } else if (resultado == -2) {
        log_info(logger_km,
                 "MEM_ALLOC: PID=%d seg=%d tam=%d (Requiere compactacion)", pid,
                 id_segmento, tamanio);
        responder_compactar(socket_cliente);
      } else {
        log_info(logger_km,
                 "MEM_ALLOC: PID=%d seg=%d tam=%d (Error: Sin espacio)", pid,
                 id_segmento, tamanio);
        responder_error(socket_cliente);
      }
      break;
    }

    case KS_MEM_FREE: {
      int pid = 0, id_segmento = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      buffer_read(&id_segmento, paquete->buffer, sizeof(int));

      log_info(logger_km, "MEM_FREE: PID=%d seg=%d", pid, id_segmento);
      segmento_liberar(pid, id_segmento);
      responder_ok(socket_cliente);
      break;
    }

    case KS_STDIN_ESCRIBIR: {
      int pid = 0, tamanio = 0;
      uint32_t dir_logica = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      buffer_read(&dir_logica, paquete->buffer, sizeof(uint32_t));
      buffer_read(&tamanio, paquete->buffer, sizeof(int));
      char *datos = buffer_read_string(paquete->buffer);

      uint32_t dir_fisica = 0;
      int error_trad = 1;
      t_list *tabla = segmentos_obtener_tabla(pid);
      if (tabla) {
        int num_segmento = dir_logica / tam_max_segmento;
        uint32_t desplazamiento = dir_logica % tam_max_segmento;
        for (int i = 0; i < list_size(tabla); i++) {
          t_segmento *seg = list_get(tabla, i);
          if (seg->id_segmento == num_segmento) {
            if (desplazamiento + tamanio <= seg->tamanio) {
              dir_fisica = seg->base + desplazamiento;
              error_trad = 0;
            }
            break;
          }
        }
        list_destroy(tabla);
      }

      if (error_trad) {
        log_error(logger_km,
                  "Error traduciendo dir_logica %u para PID %d en STDIN",
                  dir_logica, pid);
        free(datos);
        responder_error(socket_cliente);
        break;
      }

      log_info(logger_km,
               "## PID: %d - Escritura - Dir. Fisica: %u - Tamaño: %d", pid,
               dir_fisica, tamanio);

      if (datos) {
        escribir_en_ms(dir_fisica, datos, tamanio);
      }
      free(datos);
      responder_ok(socket_cliente);
      break;
    }

    case KS_STDOUT_LEER: {
      int pid = 0, tamanio = 0;
      uint32_t dir_logica = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      buffer_read(&dir_logica, paquete->buffer, sizeof(uint32_t));
      buffer_read(&tamanio, paquete->buffer, sizeof(int));

      uint32_t dir_fisica = 0;
      int error_trad = 1;
      t_list *tabla = segmentos_obtener_tabla(pid);
      if (tabla) {
        int num_segmento = dir_logica / tam_max_segmento;
        uint32_t desplazamiento = dir_logica % tam_max_segmento;
        for (int i = 0; i < list_size(tabla); i++) {
          t_segmento *seg = list_get(tabla, i);
          if (seg->id_segmento == num_segmento) {
            if (desplazamiento + tamanio <= seg->tamanio) {
              dir_fisica = seg->base + desplazamiento;
              error_trad = 0;
            }
            break;
          }
        }
        list_destroy(tabla);
      }

      if (error_trad) {
        log_error(logger_km,
                  "Error traduciendo dir_logica %u para PID %d en STDOUT",
                  dir_logica, pid);
        responder_error(socket_cliente);
        break;
      }

      log_info(logger_km, "## PID: %d - Lectura - Dir. Fisica: %u - Tamaño: %d",
               pid, dir_fisica, tamanio);

      int tam_a_leer = tamanio;
      void *datos = malloc(tam_a_leer + 1);
      int status = leer_de_ms(dir_fisica, datos, tam_a_leer);

      if (status == 0) {
        ((char *)datos)[tam_a_leer] = '\0';

        // Responder con los datos
        t_paquete *resp = crear_paquete(KM_RESPUESTA_DATOS);
        agregar_string_a_paquete(resp, (char *)datos);
        enviar_paquete(resp, socket_cliente);
        eliminar_paquete(resp);
      } else {
        responder_error(socket_cliente);
      }
      free(datos);
      break;
    }

    case KS_GET_TABLA: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));

      t_list *tabla = segmentos_obtener_tabla(pid);
      if (tabla) {
        t_paquete *resp = crear_paquete(KM_RESPUESTA_TABLA);
        int cant = list_size(tabla);
        agregar_a_paquete(resp, &cant, sizeof(int));
        for (int i = 0; i < cant; i++) {
          t_segmento *seg = list_get(tabla, i);
          agregar_a_paquete(resp, &seg->id_segmento, sizeof(int));
          agregar_a_paquete(resp, &seg->base, sizeof(uint32_t));
          agregar_a_paquete(resp, &seg->tamanio, sizeof(uint32_t));
        }
        enviar_paquete(resp, socket_cliente);
        eliminar_paquete(resp);
        list_destroy(tabla);
      } else {
        responder_error(socket_cliente);
      }
      break;
    }

    case CPU_SET_CONTEXTO: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      uint32_t pc = 0;
      buffer_read(&pc, paquete->buffer, sizeof(uint32_t));
      uint8_t ax = 0, bx = 0, cx = 0, dx = 0;
      buffer_read(&ax, paquete->buffer, sizeof(uint8_t));
      buffer_read(&bx, paquete->buffer, sizeof(uint8_t));
      buffer_read(&cx, paquete->buffer, sizeof(uint8_t));
      buffer_read(&dx, paquete->buffer, sizeof(uint8_t));
      uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0, si = 0, di = 0;
      buffer_read(&eax, paquete->buffer, sizeof(uint32_t));
      buffer_read(&ebx, paquete->buffer, sizeof(uint32_t));
      buffer_read(&ecx, paquete->buffer, sizeof(uint32_t));
      buffer_read(&edx, paquete->buffer, sizeof(uint32_t));
      buffer_read(&si, paquete->buffer, sizeof(uint32_t));
      buffer_read(&di, paquete->buffer, sizeof(uint32_t));

      contexto_guardar(pid, pc, ax, bx, cx, dx, eax, ebx, ecx, edx, si, di);
      responder_ok(socket_cliente);
      break;
    }

    case CPU_GET_CONTEXTO: {
      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));

      t_contexto_km *ctx = contexto_obtener(pid);
      if (ctx) {
        t_paquete *resp = crear_paquete(KM_CONTEXTO);
        agregar_a_paquete(resp, &ctx->pc, sizeof(uint32_t));
        agregar_a_paquete(resp, &ctx->ax, sizeof(uint8_t));
        agregar_a_paquete(resp, &ctx->bx, sizeof(uint8_t));
        agregar_a_paquete(resp, &ctx->cx, sizeof(uint8_t));
        agregar_a_paquete(resp, &ctx->dx, sizeof(uint8_t));
        agregar_a_paquete(resp, &ctx->eax, sizeof(uint32_t));
        agregar_a_paquete(resp, &ctx->ebx, sizeof(uint32_t));
        agregar_a_paquete(resp, &ctx->ecx, sizeof(uint32_t));
        agregar_a_paquete(resp, &ctx->edx, sizeof(uint32_t));
        agregar_a_paquete(resp, &ctx->si, sizeof(uint32_t));
        agregar_a_paquete(resp, &ctx->di, sizeof(uint32_t));
        enviar_paquete(resp, socket_cliente);
        eliminar_paquete(resp);
        log_info(logger_km, "## Contexto enviado: PID=%d PC=%u", pid, ctx->pc);
      } else {
        responder_error(socket_cliente);
      }
      break;
    }

    case CPU_GET_INSTRUCCION: {
      int pid = 0, pc = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      buffer_read(&pc, paquete->buffer, sizeof(int));

      // Aplicar retardo de instruccion configurable
      if (instruction_delay_ms > 0)
        usleep((useconds_t)instruction_delay_ms * 1000);

      char *instruccion = instrucciones_obtener(pid, pc);
      if (instruccion) {
        log_info(logger_km,
                 "## PID: %d - Obtener instruccion: %d - Instruccion: %s", pid,
                 pc, instruccion);
        t_paquete *resp = crear_paquete(KM_INSTRUCCION);
        agregar_string_a_paquete(resp, instruccion);
        enviar_paquete(resp, socket_cliente);
        eliminar_paquete(resp);
        free(instruccion);
      } else {
        responder_error(socket_cliente);
      }
      break;
    }

    case CPU_GET_MS_LIST: {
      t_paquete *resp = crear_paquete(KM_MS_LIST);
      int count = 0;
      for (int i = 0; i < list_size(lista_ms); i++) {
        t_memory_stick *ms = list_get(lista_ms, i);
        if (ms->conectado)
          count++;
      }
      agregar_a_paquete(resp, &count, sizeof(int));

      for (int i = 0; i < list_size(lista_ms); i++) {
        t_memory_stick *ms = list_get(lista_ms, i);
        if (ms->conectado) {
          agregar_a_paquete(resp, &ms->base_addr, sizeof(uint32_t));
          agregar_a_paquete(resp, &ms->tam_memoria, sizeof(uint32_t));
          agregar_string_a_paquete(resp, ms->ip);
          agregar_string_a_paquete(resp, ms->puerto);
        }
      }
      enviar_paquete(resp, socket_cliente);
      eliminar_paquete(resp);
      break;
    }

    case KS_COMPACTAR: {
      log_info(logger_km,
               "Iniciando compactacion a pedido del Kernel Scheduler");
      segmentos_compactar(socket_memory_stick);
      // Retardo de compactacion configurable
      usleep((useconds_t)retardo_compactacion_ms * 1000);
      responder_ok(socket_cliente);
      break;
    }

    default:
      log_warning(logger_km, "Operacion desconocida: %d", op);
      responder_error(socket_cliente);
      break;
    }

    pthread_mutex_unlock(&mutex_memoria);
    eliminar_paquete(paquete);
  }

  return NULL;
}

// Hilo que acepta conexiones
static void *hilo_servidor(void *arg) {
  (void)arg;

  while (1) {
    int cliente = esperar_cliente(socket_servidor, logger_km);
    if (cliente < 0)
      continue;

    int op = recibir_operacion(cliente);
    if (op < 0) {
      close(cliente);
      continue;
    }

    if (op == HANDSHAKE_KS || op == HANDSHAKE_CPU) {
      int cpu_id = -1;
      if (op == HANDSHAKE_CPU) {
        t_paquete *paq = recibir_paquete(cliente);
        if (paq) {
          if (paq->buffer->size >= sizeof(int)) {
            buffer_read(&cpu_id, paq->buffer, sizeof(int));
          }
          eliminar_paquete(paq);
        }
      }

      enviar_operacion(cliente, HANDSHAKE_OK);
      if (op == HANDSHAKE_KS) {
        log_info(logger_km, "## Kernel Scheduler Conectado - FD del socket: %d",
                 cliente);
      } else {
        log_info(logger_km, "## CPU %d Conectada", cpu_id);
      }

      int *sock_ptr = malloc(sizeof(int));
      *sock_ptr = cliente;
      pthread_t hilo;
      pthread_create(&hilo, NULL, hilo_atender_cliente, sock_ptr);
      pthread_detach(hilo);

    } else if (op == HANDSHAKE_KS_NOTIF) {
      enviar_operacion(cliente, HANDSHAKE_OK);
      socket_ks_notificaciones = cliente;
      log_info(logger_km, "Socket de notificaciones con KS establecido");
    } else if (op == HANDSHAKE_MS) {
      enviar_operacion(cliente, HANDSHAKE_OK);

      // Recibir tamanio de memoria y puerto del Memory Stick
      int tam_ms = 0;
      char *puerto_ms = NULL;
      int op_info = recibir_operacion(cliente);
      if (op_info == MS_INFO_CONFIG) {
        t_paquete *paq_tam = recibir_paquete(cliente);
        if (paq_tam) {
          buffer_read(&tam_ms, paq_tam->buffer, sizeof(int));
          puerto_ms = buffer_read_string(paq_tam->buffer);
          eliminar_paquete(paq_tam);
        }
      }

      // Obtener IP del Memory Stick
      char ip_ms[32] = "127.0.0.1";
      struct sockaddr_in addr;
      socklen_t addr_len = sizeof(addr);
      if (getpeername(cliente, (struct sockaddr *)&addr, &addr_len) == 0) {
        strncpy(ip_ms, inet_ntoa(addr.sin_addr), sizeof(ip_ms) - 1);
        ip_ms[sizeof(ip_ms) - 1] = '\0';
      }

      // Agregar a la lista de MS
      pthread_mutex_lock(&mutex_ms);
      t_memory_stick *nuevo_ms = malloc(sizeof(t_memory_stick));
      nuevo_ms->socket = cliente;
      nuevo_ms->tam_memoria = tam_ms;
      nuevo_ms->conectado = 1;
      strncpy(nuevo_ms->ip, ip_ms, sizeof(nuevo_ms->ip));
      if (puerto_ms) {
        strncpy(nuevo_ms->puerto, puerto_ms, sizeof(nuevo_ms->puerto));
        free(puerto_ms);
      } else {
        strcpy(nuevo_ms->puerto, "0");
      }
      // Calcular base: suma de tamanios de los MS ya conectados
      uint32_t base = 0;
      for (int i = 0; i < list_size(lista_ms); i++) {
        t_memory_stick *m = list_get(lista_ms, i);
        base += (uint32_t)m->tam_memoria;
      }
      nuevo_ms->base_addr = base;
      list_add(lista_ms, nuevo_ms);
      int ms_idx = list_size(lista_ms) - 1;
      int tam_total = (int)base + tam_ms;
      pthread_mutex_unlock(&mutex_ms);

      log_info(logger_km, "## Memory Stick de %d bytes Conectada", tam_ms);

      // Inicializar/expandir segmentacion
      pthread_mutex_lock(&mutex_memoria);
      if (!segmentacion_lista && tam_ms > 0) {
        segmentacion_inicializar(tam_total, algoritmo_ajuste);
        segmentacion_lista = 1;
        log_info(logger_km, "Segmentacion inicializada: %d bytes totales",
                 tam_total);
      } else if (segmentacion_lista && tam_ms > 0) {
        // MS adicional: expandir espacio disponible
        segmentacion_expandir(tam_ms);
        if (socket_ks_notificaciones != -1) {
          enviar_operacion(socket_ks_notificaciones,
                           KM_NOTIFICAR_NUEVA_MEMORIA);
          log_info(logger_km, "Notificando nueva memoria al KS");
        }
      }
      pthread_mutex_unlock(&mutex_memoria);

      // Hilo para detectar desconexion de este MS
      {
        t_ms_hilo_arg *ms_arg = malloc(sizeof(t_ms_hilo_arg));
        ms_arg->socket = cliente;
        ms_arg->ms_idx = ms_idx;
        pthread_t hilo_ms;
        pthread_create(&hilo_ms, NULL, hilo_ms_monitor, ms_arg);
        pthread_detach(hilo_ms);
      }

    } else if (op == HANDSHAKE_SWAP) {
      enviar_operacion(cliente, HANDSHAKE_OK);
      socket_swap = cliente;
      log_info(logger_km, "SWAP conectado");

      // Recibir info de SWAP (block_size y swap_size)
      int op_info = recibir_operacion(cliente);
      if (op_info == SWAP_INFO) {
        t_paquete *paq_info = recibir_paquete(cliente);
        if (paq_info) {
          int blk_sz = 0, swap_sz = 0;
          buffer_read(&blk_sz, paq_info->buffer, sizeof(int));
          buffer_read(&swap_sz, paq_info->buffer, sizeof(int));
          eliminar_paquete(paq_info);
          swap_block_size = blk_sz;
          swap_manager_inicializar(blk_sz, swap_sz);
          log_info(logger_km, "SWAP: block_size=%d swap_size=%d (bloques=%d)",
                   blk_sz, swap_sz, swap_sz / (blk_sz > 0 ? blk_sz : 1));
        }
      }

    } else {
      log_warning(logger_km, "Handshake desconocido: %d", op);
      close(cliente);
    }
  }

  return NULL;
}

int main(int argc, char **argv) {
  const char *config_path = "kernel_memory.config";
  if (argc > 1)
    config_path = argv[1];

  t_config *config = config_create((char *)config_path);
  if (!config) {
    fprintf(stderr, "No se pudo abrir el config: %s\n", config_path);
    return EXIT_FAILURE;
  }

  const char *puerto = config_get_string_value(config, "PUERTO_ESCUCHA");
  const char *alg = config_get_string_value(config, "ALLOCATION_STRATEGY");
  const char *log_level = config_get_string_value(config, "LOG_LEVEL");

  tam_max_segmento = config_get_int_value(config, "SEGMENT_MAX_SIZE");
  retardo_compactacion_ms = config_get_int_value(config, "COMPACTION_DELAY");
  if (config_has_property(config, "INSTRUCTION_DELAY"))
    instruction_delay_ms = config_get_int_value(config, "INSTRUCTION_DELAY");
  path_scripts = strdup(config_get_string_value(config, "SCRIPTS_BASEPATH"));

  // Crear logger
  logger_km = log_create("kernel_memory.log", "KM", 1,
                         log_level_from_string((char *)log_level));
  if (!logger_km) {
    fprintf(stderr, "No se pudo crear el logger\n");
    config_destroy(config);
    return EXIT_FAILURE;
  }

  log_info(logger_km, "Kernel Memory iniciando...");
  log_info(logger_km,
           "Tam max segmento: %d | Algoritmo: %s | Retardo compactacion: %d ms",
           tam_max_segmento, alg, retardo_compactacion_ms);

  // Inicializar subsistemas
  instrucciones_inicializar();
  contexto_inicializar();
  lista_ms = list_create();

  // Determinar algoritmo de ajuste
  algoritmo_ajuste = AJUSTE_BEST_FIT;
  if (alg) {
    if (strcmp(alg, "WORST_FIT") == 0 || strcmp(alg, "WORST") == 0)
      algoritmo_ajuste = AJUSTE_WORST_FIT;
    // BEST_FIT o BEST => default
  }

  // Iniciar servidor (puro servidor — espera conexiones de MS, SWAP, KS, CPU)
  socket_servidor = iniciar_servidor((char *)puerto, logger_km);
  if (socket_servidor < 0) {
    log_error(logger_km, "No se pudo iniciar servidor en puerto %s", puerto);
    return EXIT_FAILURE;
  }
  log_info(logger_km, "Servidor escuchando en puerto %s", puerto);
  log_info(logger_km,
           "Esperando conexiones de Memory Stick, SWAP, KS y CPU...");

  // Ejecutar servidor en este hilo (bloquea para siempre)
  hilo_servidor(NULL);

  // Limpieza (solo se llega si el servidor termina)
  close(socket_servidor);
  if (socket_memory_stick >= 0)
    close(socket_memory_stick);
  if (socket_swap >= 0)
    close(socket_swap);
  free(path_scripts);
  log_destroy(logger_km);
  config_destroy(config);

  return EXIT_SUCCESS;
}
