#include <commons/config.h>
#include <commons/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../utils/src/utils/conexiones.h"
#include "../../utils/src/utils/protocolos.h"
#include "pcb.h"
#include "planificador.h"
#include "sincronizacion.h"

static int socket_servidor = -1;

// ─── Multi-CPU support ───
typedef struct {
  int socket_dispatch;
  int socket_interrupt;
  int ocupado;        // 1 si hay un proceso corriendo en esta CPU
  int pid_ejecutando; // PID del proceso en ejecucion
} t_cpu_conectado;

t_list *lista_cpus = NULL;
pthread_mutex_t mutex_cpus = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  int socket;
  char *nombre;
  char *tipo;
  int ocupado;
  t_pcb *pcb_atendiendo;
} t_io_conectado;

static t_list *lista_io = NULL;
static pthread_mutex_t mutex_io = PTHREAD_MUTEX_INITIALIZER;

static t_io_conectado *buscar_io_por_tipo(const char *tipo) {
  for (int i = 0; i < list_size(lista_io); i++) {
    t_io_conectado *io = list_get(lista_io, i);
    if (strcmp(io->tipo, tipo) == 0 && !io->ocupado)
      return io;
  }
  return NULL;
}

static t_io_conectado *buscar_io_por_socket(int socket) {
  for (int i = 0; i < list_size(lista_io); i++) {
    t_io_conectado *io = list_get(lista_io, i);
    if (io->socket == socket)
      return io;
  }
  return NULL;
}

// Pide los datos del segmento al Kernel Memory para enviarlos al IO de STDOUT
static char *obtener_datos_stdout(int pid, int segmento, int tamanio) {
  t_paquete *paq = crear_paquete(KS_STDOUT_LEER);
  agregar_a_paquete(paq, &pid, sizeof(int));
  agregar_a_paquete(paq, &segmento, sizeof(int));
  agregar_a_paquete(paq, &tamanio, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paq, socket_kernel_memory);
  eliminar_paquete(paq);

  int op = recibir_operacion(socket_kernel_memory);
  if (op == KM_RESPUESTA_DATOS) {
    t_paquete *resp = recibir_paquete(socket_kernel_memory);
    if (resp) {
      char *datos = buffer_read_string(resp->buffer);
      eliminar_paquete(resp);
      pthread_mutex_unlock(&mutex_socket_km);
      return datos;
    }
  } else {
    t_paquete *resp = recibir_paquete(socket_kernel_memory);
    if (resp)
      eliminar_paquete(resp);
  }
  pthread_mutex_unlock(&mutex_socket_km);
  return NULL;
}

static void enviar_io_solicitud(t_io_conectado *io, int pid, const char *tipo,
                                int param1, int param2, const char *datos) {
  t_paquete *io_paq = crear_paquete(KS_IO_SOLICITUD);
  agregar_a_paquete(io_paq, &pid, sizeof(int));
  agregar_string_a_paquete(io_paq, tipo);
  agregar_a_paquete(io_paq, &param1, sizeof(int));
  agregar_a_paquete(io_paq, &param2, sizeof(int));
  // Para STDOUT enviamos los datos reales
  agregar_string_a_paquete(io_paq, datos ? datos : "");
  enviar_paquete(io_paq, io->socket);
  eliminar_paquete(io_paq);
  io->ocupado = 1;
  io->pcb_atendiendo = NULL; // se buscara por PID
}

static void procesar_io_pendientes(t_io_conectado *io) {
  pthread_mutex_lock(&mutex_planificacion);
  t_pcb *next_pcb = NULL;
  for (int i = 0; i < list_size(cola_block); i++) {
    t_pcb *cand = list_get(cola_block, i);
    if (cand->recurso_bloqueo && strcmp(cand->recurso_bloqueo, io->tipo) == 0) {
      next_pcb = cand;
      break;
    }
  }

  if (!next_pcb) {
    for (int i = 0; i < list_size(cola_susp_block); i++) {
      t_pcb *cand = list_get(cola_susp_block, i);
      if (cand->recurso_bloqueo &&
          strcmp(cand->recurso_bloqueo, io->tipo) == 0) {
        next_pcb = cand;
        break;
      }
    }
  }
  pthread_mutex_unlock(&mutex_planificacion);

  if (next_pcb) {
    pthread_mutex_lock(&mutex_io);
    if (!io->ocupado) {
      char *datos_stdout = NULL;
      if (strcmp(io->tipo, "STDOUT") == 0) {
        pthread_mutex_unlock(&mutex_io);
        datos_stdout = obtener_datos_stdout(
            next_pcb->pid, next_pcb->segmento_io, next_pcb->tamanio_io);
        pthread_mutex_lock(&mutex_io);
      }

      int param1 = next_pcb->segmento_io;
      int param2 = next_pcb->tamanio_io;
      if (strcmp(io->tipo, "SLEEP") == 0) {
        param1 = next_pcb->tiempo_sleep;
        param2 = 0;
      }

      enviar_io_solicitud(io, next_pcb->pid, io->tipo, param1, param2,
                          datos_stdout);
      io->pcb_atendiendo = next_pcb;
      free(datos_stdout);
    }
    pthread_mutex_unlock(&mutex_io);
  }
}

static void *hilo_cpu_dispatch(void *arg) {
  t_cpu_conectado *cpu = (t_cpu_conectado *)arg;

  while (1) {
    int op = recibir_operacion(cpu->socket_dispatch);
    if (op < 0) {
      log_error(logger, "CPU desconectado (dispatch)");
      break;
    }

    if (op != CPU_DESALOJO)
      continue;

    t_paquete *paquete = recibir_paquete(cpu->socket_dispatch);
    if (!paquete)
      continue;

    t_pcb *pcb = pcb_deserializar(paquete->buffer);
    int motivo = 0;
    buffer_read(&motivo, paquete->buffer, sizeof(int));

    char *recurso = NULL;
    int sys_param1 = 0, sys_param2 = 0;
    char *sys_str_param = NULL;

    if (motivo == MOTIVO_IO) {
      recurso = buffer_read_string(paquete->buffer);
      buffer_read(&sys_param1, paquete->buffer, sizeof(int));
      buffer_read(&sys_param2, paquete->buffer, sizeof(int));
    } else if (motivo == MOTIVO_SYSCALL) {
      recurso = buffer_read_string(paquete->buffer);
      buffer_read(&sys_param1, paquete->buffer, sizeof(int));
      buffer_read(&sys_param2, paquete->buffer, sizeof(int));
      sys_str_param = buffer_read_string(paquete->buffer);
    }
    eliminar_paquete(paquete);

    pthread_mutex_lock(&mutex_planificacion);

    // Remover de cola_exec
    for (int i = 0; i < list_size(cola_exec); i++) {
      t_pcb *ep = list_get(cola_exec, i);
      if (ep->pid == pcb->pid) {
        list_remove(cola_exec, i);
        hay_proceso_ejecutando = 0;

        pcb->thread_quantum = ep->thread_quantum;
        pcb->thread_quantum_active = ep->thread_quantum_active;
        pcb->thread_suspension = ep->thread_suspension;
        pcb->thread_suspension_active = ep->thread_suspension_active;
        pcb->block_entry_time = ep->block_entry_time;
        pcb->exec_entry_time = ep->exec_entry_time;
        pcb->quantum_restante = ep->quantum_restante;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - pcb->exec_entry_time.tv_sec) * 1000 +
                       (now.tv_nsec - pcb->exec_entry_time.tv_nsec) / 1000000;
        pcb->quantum_restante -= elapsed;
        if (pcb->quantum_restante < 0)
          pcb->quantum_restante = 0;

        if (pcb->thread_quantum_active) {
          pthread_cancel(pcb->thread_quantum);
          pcb->thread_quantum_active = 0;
        }

        pcb_destruir(ep);
        break;
      }
    }
    pthread_mutex_unlock(&mutex_planificacion);
    pcb->estado = ESTADO_EXEC;

    if (motivo == MOTIVO_IO || motivo == MOTIVO_SYSCALL) {
      log_info(logger, "## (%d) - Solicito syscall: %s", pcb->pid,
               recurso ? recurso : "?");
    }

    int volver_a_cpu = 0; // flag para re-enviar directamente

    switch (motivo) {
    case MOTIVO_EXIT:
      mutex_liberar_todos_de_proceso(pcb->pid);
      planificador_pasar_a_exit(pcb, "EXIT");
      break;

    case MOTIVO_INTERRUPCION:
      pthread_mutex_lock(&mutex_compactacion);
      if (compactacion_en_curso) {
        cpus_pendientes_desalojo--;
        if (cpus_pendientes_desalojo == 0) {
          pthread_cond_signal(&cv_cpus_idle);
        }
        while (compactacion_en_curso) {
          pthread_cond_wait(&cv_compactacion_fin, &mutex_compactacion);
        }
        pthread_mutex_unlock(&mutex_compactacion);
        volver_a_cpu = 1;
      } else {
        pthread_mutex_unlock(&mutex_compactacion);
        planificador_pasar_a_ready(pcb);
      }
      break;

    case MOTIVO_ERROR:
      mutex_liberar_todos_de_proceso(pcb->pid);
      planificador_pasar_a_exit(pcb, "ERROR");
      break;

    case MOTIVO_SEG_FAULT:
      mutex_liberar_todos_de_proceso(pcb->pid);
      planificador_pasar_a_exit(pcb, "SEG_FAULT");
      break;

    case MOTIVO_IO: {
      // STDOUT/STDIN llegan como MOTIVO_IO. SLEEP COMO MOTIVO_SYSCALL
      log_info(logger, "## PID: %d - Inicio de IO: %s", pcb->pid,
               recurso ? recurso : "?");
      pcb->recurso_bloqueo = recurso ? strdup(recurso) : NULL;
      pcb->segmento_io = sys_param1;
      pcb->tamanio_io = sys_param2;
      planificador_pasar_a_block(pcb);

      pthread_mutex_lock(&mutex_io);
      t_io_conectado *io = buscar_io_por_tipo(recurso ? recurso : "");

      if (io) {
        char *datos_stdout = NULL;
        if (recurso && strcmp(recurso, "STDOUT") == 0) {
          pthread_mutex_unlock(&mutex_io);
          datos_stdout = obtener_datos_stdout(pcb->pid, sys_param1, sys_param2);
          pthread_mutex_lock(&mutex_io);
          log_info(logger, "## PID: %d - %s", pcb->pid,
                   datos_stdout ? datos_stdout : "(sin datos)");
        }
        enviar_io_solicitud(io, pcb->pid, recurso ? recurso : "", sys_param1,
                            sys_param2, datos_stdout);
        io->pcb_atendiendo = pcb;
        free(datos_stdout);
      } else {
        log_warning(logger, "No hay dispositivo IO '%s' disponible",
                    recurso ? recurso : "?");
      }
      pthread_mutex_unlock(&mutex_io);
      break;
    }

    case MOTIVO_SYSCALL: {

      if (recurso && strcmp(recurso, "MUTEX_CREATE") == 0) {
        if (sys_str_param)
          mutex_crear(sys_str_param);
        volver_a_cpu = 1;
      } else if (recurso && strcmp(recurso, "MUTEX_LOCK") == 0) {
        if (sys_str_param) {
          int r = mutex_lock(sys_str_param, pcb);
          if (r == 0)
            volver_a_cpu = 1;
          else {
            pcb->recurso_bloqueo = strdup(sys_str_param);
            planificador_pasar_a_block(pcb);
          }
        }
      } else if (recurso && strcmp(recurso, "MUTEX_UNLOCK") == 0) {
        if (sys_str_param) {
          t_pcb *desbloqueado = mutex_unlock(sys_str_param, pcb);
          if (desbloqueado) {
            int estaba_suspendido = 0;
            int removido = 0;
            for (int i = 0; i < list_size(cola_block); i++) {
              if (list_get(cola_block, i) == desbloqueado) {
                list_remove(cola_block, i);
                removido = 1;
                break;
              }
            }
            if (!removido) {
              for (int i = 0; i < list_size(cola_susp_block); i++) {
                if (list_get(cola_susp_block, i) == desbloqueado) {
                  list_remove(cola_susp_block, i);
                  estaba_suspendido = 1;
                  break;
                }
              }
            }
            if (estaba_suspendido) {
              planificador_pasar_a_susp_ready(desbloqueado);
            } else {
              planificador_pasar_a_ready(desbloqueado);
            }
          }
          volver_a_cpu = 1;
        }
      } else if (recurso && strcmp(recurso, "MEM_ALLOC") == 0) {
        int resultado = solicitar_mem_alloc(pcb->pid, sys_param1, sys_param2);
        if (resultado == -2) {
          solicitar_compactacion();
          resultado = solicitar_mem_alloc(pcb->pid, sys_param1, sys_param2);
        }
        if (resultado != 0) {
          mutex_liberar_todos_de_proceso(pcb->pid);
          planificador_pasar_a_exit(pcb, "OUT_OF_MEMORY");
        } else {
          volver_a_cpu = 1;
        }
      } else if (recurso && strcmp(recurso, "MEM_FREE") == 0) {
        solicitar_mem_free(pcb->pid, sys_param1);
        volver_a_cpu = 1;
      } else if (recurso && strcmp(recurso, "SLEEP") == 0) {
        pcb->tiempo_sleep = sys_param1;
        pcb->recurso_bloqueo = strdup("SLEEP");
        planificador_pasar_a_block(pcb);

        pthread_mutex_lock(&mutex_io);
        t_io_conectado *io_sleep = buscar_io_por_tipo("SLEEP");
        if (io_sleep) {
          enviar_io_solicitud(io_sleep, pcb->pid, "SLEEP", sys_param1, 0, NULL);
          io_sleep->pcb_atendiendo = pcb;
        }
        pthread_mutex_unlock(&mutex_io);
      } else if (recurso && strcmp(recurso, "INIT_PROC") == 0) {
        static int pid_counter = 1;
        t_pcb *nuevo = pcb_crear(pid_counter++, sys_param1, sys_str_param);
        planificador_agregar_nuevo(nuevo);
        volver_a_cpu = 1;
      } else {
        planificador_pasar_a_ready(pcb);
      }
      free(recurso);
      free(sys_str_param);
      break;
    }

    default:
      planificador_pasar_a_ready(pcb);
      break;
    }

    if (volver_a_cpu) {
      actualizar_tabla_segmentos(pcb);
      pthread_mutex_lock(&mutex_planificacion);

      list_add(cola_exec, pcb);
      hay_proceso_ejecutando = 1;
      cpu->ocupado = 1;
      cpu->pid_ejecutando = pcb->pid;

      clock_gettime(CLOCK_MONOTONIC, &pcb->exec_entry_time);
      planificador_iniciar_quantum(pcb);
      pthread_mutex_unlock(&mutex_planificacion);

      t_paquete *paq_dispatch = crear_paquete(KS_DISPATCH);
      pcb_serializar(pcb, paq_dispatch);
      enviar_paquete(paq_dispatch, cpu->socket_dispatch);
      eliminar_paquete(paq_dispatch);
    } else {
      cpu->ocupado = 0;
      sem_post(&sem_cpu_libre);
    }
  }
  return NULL;
}

static void *hilo_io_listener(void *arg) {
  int socket_io = *(int *)arg;
  free(arg);

  while (1) {
    int op = recibir_operacion(socket_io);
    if (op < 0) {
      pthread_mutex_lock(&mutex_io);
      t_io_conectado *io = buscar_io_por_socket(socket_io);
      if (io) {
        log_warning(logger, "IO '%s' desconectado", io->nombre);
        for (int i = 0; i < list_size(lista_io); i++) {
          if (list_get(lista_io, i) == io) {
            list_remove(lista_io, i);
            break;
          }
        }
        free(io->nombre);
        free(io->tipo);
        free(io);
      }
      pthread_mutex_unlock(&mutex_io);
      close(socket_io);
      return NULL;
    }

    if (op == IO_FINALIZADA) {
      t_paquete *paquete = recibir_paquete(socket_io);
      if (!paquete)
        continue;

      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      char *datos_stdin = buffer_read_string(paquete->buffer);
      eliminar_paquete(paquete);

      pthread_mutex_lock(&mutex_io);
      t_io_conectado *io = buscar_io_por_socket(socket_io);
      if (io) {
        io->ocupado = 0;
        io->pcb_atendiendo = NULL;
      }
      pthread_mutex_unlock(&mutex_io);

      pthread_mutex_lock(&mutex_planificacion);
      int encontrado = 0;
      for (int i = 0; i < list_size(cola_block); i++) {
        t_pcb *pcb = list_get(cola_block, i);
        if (pcb->pid == pid) {
          list_remove(cola_block, i);
          encontrado = 1;
          // Si fue STDIN, escribir datos en memoria
          if (pcb->recurso_bloqueo &&
              strcmp(pcb->recurso_bloqueo, "STDIN") == 0 && datos_stdin) {
            pthread_mutex_unlock(&mutex_planificacion);
            t_paquete *mem_paq = crear_paquete(KS_STDIN_ESCRIBIR);
            agregar_a_paquete(mem_paq, &pid, sizeof(int));
            agregar_a_paquete(mem_paq, &pcb->segmento_io, sizeof(int));
            agregar_a_paquete(mem_paq, &pcb->tamanio_io, sizeof(int));
            int len = pcb->tamanio_io;
            agregar_a_paquete(mem_paq, &len, sizeof(int));
            if (len > 0) {
              agregar_a_paquete(mem_paq, datos_stdin, len);
            }
            pthread_mutex_lock(&mutex_socket_km);
            enviar_paquete(mem_paq, socket_kernel_memory);
            eliminar_paquete(mem_paq);
            int op_r = recibir_operacion(socket_kernel_memory);
            t_paquete *r = recibir_paquete(socket_kernel_memory);
            pthread_mutex_unlock(&mutex_socket_km);
            (void)op_r;
            if (r)
              eliminar_paquete(r);
          } else {
            pthread_mutex_unlock(&mutex_planificacion);
          }
          log_info(logger, "## (%d) finalizo IO y pasa a READY", pid);
          planificador_pasar_a_ready(pcb);
          break;
        }
      }

      if (!encontrado) {
        for (int i = 0; i < list_size(cola_susp_block); i++) {
          t_pcb *pcb = list_get(cola_susp_block, i);
          if (pcb->pid == pid) {
            list_remove(cola_susp_block, i);
            encontrado = 1;
            // si fue STDIN y el proceso esta suspendido,
            // la direccion fisica original es invalida porque la memoria esta
            // en SWAP. Para solucionarlo de raiz habria que cambiar la logica
            // para que el CPU envie la dir_logica, y que el Kernel Memory la
            // traduzca al momento de escribir. Por ahora, guardaremos el STDIN
            // y lo enviaremos al reanudarse.
            if (pcb->recurso_bloqueo &&
                strcmp(pcb->recurso_bloqueo, "STDIN") == 0 && datos_stdin) {
              pcb->pending_stdin = malloc(pcb->tamanio_io);
              memcpy(pcb->pending_stdin, datos_stdin, pcb->tamanio_io);
            }
            log_info(logger, "## (%d) finalizo IO y pasa a SUSP. READY", pid);
            pthread_mutex_unlock(&mutex_planificacion);
            planificador_pasar_a_susp_ready(pcb);
            break;
          }
        }
      }

      if (!encontrado) {
        pthread_mutex_unlock(&mutex_planificacion);
      }
      free(datos_stdin);

      if (io) {
        procesar_io_pendientes(io);
      }
    }
  }
  return NULL;
}

static void manejar_bsod(void) {
  log_error(logger,
            "## BSOD: Blue Screen of Death - Corrupcion de memoria detectada");
  pthread_mutex_lock(&mutex_planificacion);
  // Enviar EXIT a CPU si hay proceso ejecutando
  if (!list_is_empty(cola_exec)) {
    t_pcb *ep = list_get(cola_exec, 0);
    pthread_mutex_unlock(&mutex_planificacion);
    enviar_interrupcion_cpu(ep->pid, MOTIVO_EXIT);
    usleep(300000);
    pthread_mutex_lock(&mutex_planificacion);
  }
  // Finalizar todos los procesos
  t_list *todas[] = {cola_exec,       cola_ready,      cola_block,
                     cola_susp_ready, cola_susp_block, cola_new};
  for (int q = 0; q < 6; q++) {
    while (!list_is_empty(todas[q])) {
      t_pcb *p = list_remove(todas[q], 0);
      planificador_pasar_a_exit(p, "BSOD");
    }
  }
  pthread_mutex_unlock(&mutex_planificacion);
  log_error(logger, "Sistema finalizado por BSOD");
  exit(EXIT_FAILURE);
}

// Hilo para escuchar notificaciones asincronas de KM
static void *hilo_km_notificaciones(void *arg) {
  int socket_notif = *(int *)arg;
  free(arg);

  while (1) {
    int op = recibir_operacion(socket_notif);
    if (op < 0) {
      log_error(logger, "Conexion de notificaciones con KM cerrada");
      break;
    }

    if (op == KM_BSOD) {
      log_error(logger,
                "## BSOD DETECTADO - Memoria corrupta (MS desconectado)");
      exit(EXIT_FAILURE);
    } else if (op == KM_NOTIFICAR_NUEVA_MEMORIA) {
      log_info(logger, "## Nueva memoria disponible detectada");
      // Intenta des-suspender procesos si hay lugar
      planificador_des_suspender_procesos();
    } else {
      t_paquete *paquete = recibir_paquete(socket_notif);
      if (paquete)
        eliminar_paquete(paquete);
    }
  }

  close(socket_notif);
  return NULL;
}

// Hilo que acepta conexiones
static void *hilo_servidor(void *arg) {
  (void)arg;
  while (1) {
    int cliente = esperar_cliente(socket_servidor, logger);
    if (cliente < 0)
      continue;

    int op = recibir_operacion(cliente);
    if (op < 0) {
      close(cliente);
      continue;
    }

    if (op == HANDSHAKE_CPU) {
      int cpu_id = -1;
      t_paquete *paq = recibir_paquete(cliente);
      if (paq) {
        if (paq->buffer->size >= sizeof(int)) {
          buffer_read(&cpu_id, paq->buffer, sizeof(int));
        }
        eliminar_paquete(paq);
      }
      enviar_operacion(cliente, HANDSHAKE_OK);
      pthread_mutex_lock(&mutex_cpus);

      // Buscar si hay alguna CPU sin socket_interrupt asignado aun
      t_cpu_conectado *cpu_pendiente = NULL;
      for (int i = 0; i < list_size(lista_cpus); i++) {
        t_cpu_conectado *c = list_get(lista_cpus, i);
        if (c->socket_interrupt < 0) {
          cpu_pendiente = c;
          break;
        }
      }

      if (cpu_pendiente) {
        // Segunda conexion de la misma CPU: es el socket de interrupt
        cpu_pendiente->socket_interrupt = cliente;
        int cpu_idx = 0;
        for (int i = 0; i < list_size(lista_cpus); i++) {
          if (list_get(lista_cpus, i) == cpu_pendiente) {
            cpu_idx = i;
            break;
          }
        }
        log_info(logger, "## CPU %d Conectada", cpu_id);
        log_info(logger,
                 "CPU conectada (interrupt socket=%d) - index interno %d",
                 cliente, cpu_idx);
        // Actualizar referencia global para compatibilidad
        socket_cpu_interrupt = cliente;
        pthread_mutex_unlock(&mutex_cpus);
      } else {
        // Primera conexion: dispatch socket de una nueva CPU
        t_cpu_conectado *nueva_cpu = malloc(sizeof(t_cpu_conectado));
        nueva_cpu->socket_dispatch = cliente;
        nueva_cpu->socket_interrupt = -1;
        nueva_cpu->ocupado = 0;
        nueva_cpu->pid_ejecutando = -1;
        list_add(lista_cpus, nueva_cpu);
        int cpu_idx = list_size(lista_cpus) - 1;
        log_info(logger, "## CPU %d Conectada", cpu_id);
        log_info(logger,
                 "CPU conectada (dispatch socket=%d) - index interno %d",
                 cliente, cpu_idx);
        // Actualizar referencia global para compatibilidad
        socket_cpu_dispatch = cliente;
        pthread_mutex_unlock(&mutex_cpus);
        pthread_t hilo;
        sem_post(&sem_cpu_libre);
        pthread_create(&hilo, NULL, hilo_cpu_dispatch, nueva_cpu);
        pthread_detach(hilo);
      }
    } else if (op == HANDSHAKE_IO) {
      enviar_operacion(cliente, HANDSHAKE_OK);
      int op2 = recibir_operacion(cliente);
      if (op2 == IO_IDENTIFICACION) {
        t_paquete *paq = recibir_paquete(cliente);
        if (paq) {
          char *nombre = buffer_read_string(paq->buffer);
          char *tipo = buffer_read_string(paq->buffer);
          t_io_conectado *io = malloc(sizeof(t_io_conectado));
          io->socket = cliente;
          io->nombre = nombre;
          io->tipo = tipo;
          io->ocupado = 0;
          io->pcb_atendiendo = NULL;
          pthread_mutex_lock(&mutex_io);
          list_add(lista_io, io);
          pthread_mutex_unlock(&mutex_io);
          log_info(logger,
                   "## Conectado a Kernel Scheduler: IO nombre=%s tipo=%s",
                   nombre, tipo);
          int *sp = malloc(sizeof(int));
          *sp = cliente;
          pthread_t hilo;
          pthread_create(&hilo, NULL, hilo_io_listener, sp);
          pthread_detach(hilo);
          eliminar_paquete(paq);
          procesar_io_pendientes(io);
        }
      }
    } else if (op == KM_BSOD) {
      // Kernel Memory detecto corrupcion
      t_paquete *p = recibir_paquete(cliente);
      if (p)
        eliminar_paquete(p);
      manejar_bsod();
    } else {
      log_warning(logger, "Handshake desconocido: %d", op);
      close(cliente);
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *config_path = "kernel_scheduler.config";
  const char *nombre_proceso_inicial = NULL;

  if (argc > 1)
    config_path = argv[1];
  if (argc > 2)
    nombre_proceso_inicial = argv[2];

  if (!nombre_proceso_inicial) {
    fprintf(stderr, "Uso: ./bin/kernel_scheduler [Config] [Proceso Inicial]\n");
    return EXIT_FAILURE;
  }

  t_config *config = config_create((char *)config_path);
  if (!config) {
    fprintf(stderr, "No se pudo abrir config\n");
    return EXIT_FAILURE;
  }

  const char *puerto = config_get_string_value(config, "PUERTO_ESCUCHA");
  const char *ip_km = config_get_string_value(config, "IP_KERNEL_MEMORY");
  const char *puerto_km =
      config_get_string_value(config, "PUERTO_KERNEL_MEMORY");
  const char *alg = config_get_string_value(config, "PLANIFICATION_ALGORITHM");
  const char *log_level = config_get_string_value(config, "LOG_LEVEL");

  quantum_config = config_get_int_value(config, "RR_QUANTUM");
  if (config_has_property(config, "GRADO_MULTIPROGRAMACION"))
    grado_multiprogramacion =
        config_get_int_value(config, "GRADO_MULTIPROGRAMACION");
  else
    grado_multiprogramacion = 100;
  suspension_timeout_ms = config_get_int_value(config, "SUSPENSION_TIMEOUT");

  if (alg && strcmp(alg, "RR") == 0)
    algoritmo = ALG_RR;
  else if (alg && strcmp(alg, "CMN") == 0) {
    algoritmo = ALG_CMN;
    // Parsear QUEUE_PREEMPTION=TRUE/FALSE
    desalojo_entre_colas = 0;
    if (config_has_property(config, "QUEUE_PREEMPTION")) {
      const char *qp = config_get_string_value(config, "QUEUE_PREEMPTION");
      desalojo_entre_colas = (strcmp(qp, "TRUE") == 0) ? 1 : 0;
    }
    // Parsear QUEUES_ALGORITHMS=[FIFO,RR,RR,FIFO]
    if (config_has_property(config, "QUEUES_ALGORITHMS")) {
      char **arr = config_get_array_value(config, "QUEUES_ALGORITHMS");
      cant_colas_multinivel = 0;
      for (char **p = arr; *p != NULL; p++)
        cant_colas_multinivel++;
      algoritmo_por_cola = malloc(sizeof(char *) * cant_colas_multinivel);
      for (int i = 0; i < cant_colas_multinivel; i++)
        algoritmo_por_cola[i] = strdup(arr[i]);
      // Liberar array de config
      for (int i = 0; arr[i] != NULL; i++)
        free(arr[i]);
      free(arr);
    } else {
      cant_colas_multinivel = 4;
      algoritmo_por_cola = malloc(sizeof(char *) * cant_colas_multinivel);
      for (int i = 0; i < cant_colas_multinivel; i++)
        algoritmo_por_cola[i] = strdup("FIFO");
    }
  } else {
    algoritmo = ALG_FIFO;
  }

  logger = log_create("kernel_scheduler.log", "KS", 1,
                      log_level_from_string((char *)log_level));
  if (!logger) {
    fprintf(stderr, "No se pudo crear el logger\n");
    return EXIT_FAILURE;
  }

  log_info(logger,
           "Kernel Scheduler iniciando - Algoritmo: %s | Quantum: %d | GM: %d",
           alg, quantum_config, grado_multiprogramacion);

  planificador_inicializar();
  sincronizacion_inicializar();
  lista_io = list_create();
  lista_cpus = list_create();

  socket_kernel_memory =
      crear_conexion((char *)ip_km, (char *)puerto_km, logger);
  if (socket_kernel_memory < 0) {
    log_error(logger, "No se pudo conectar a Kernel Memory");
    return EXIT_FAILURE;
  }
  enviar_operacion(socket_kernel_memory, HANDSHAKE_KS);
  if (recibir_operacion(socket_kernel_memory) != HANDSHAKE_OK) {
    log_error(logger, "Handshake con Kernel Memory fallido");
    return EXIT_FAILURE;
  }
  log_info(logger, "## Conectado a Kernel Memory");

  socket_servidor = iniciar_servidor((char *)puerto, logger);
  log_info(logger, "Servidor escuchando en puerto %s", puerto);

  int socket_km_notificaciones =
      crear_conexion((char *)ip_km, (char *)puerto_km, logger);
  if (socket_km_notificaciones >= 0) {
    enviar_operacion(socket_km_notificaciones, HANDSHAKE_KS_NOTIF);
    if (recibir_operacion(socket_km_notificaciones) == HANDSHAKE_OK) {
      pthread_t hilo_notif;
      int *sock_ptr = malloc(sizeof(int));
      *sock_ptr = socket_km_notificaciones;
      pthread_create(&hilo_notif, NULL, hilo_km_notificaciones, sock_ptr);
      pthread_detach(hilo_notif);
      log_info(logger, "Conexion de notificaciones con KM establecida");
    }
  }

  log_info(logger, "Creando proceso inicial PID 0: %s", nombre_proceso_inicial);
  t_pcb *pcb_inicial = pcb_crear(0, 0, nombre_proceso_inicial);
  planificador_agregar_nuevo(pcb_inicial);

  pthread_t hilo_largo, hilo_corto;
  pthread_create(&hilo_largo, NULL, hilo_planificador_largo_plazo, NULL);
  pthread_detach(hilo_largo);
  pthread_create(&hilo_corto, NULL, hilo_planificador_corto_plazo, NULL);
  pthread_detach(hilo_corto);

  hilo_servidor(NULL);

  close(socket_servidor);
  log_destroy(logger);
  config_destroy(config);
  return EXIT_SUCCESS;
}
