#include <commons/collections/list.h>
#include <commons/config.h>
#include <commons/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../utils/src/utils/conexiones.h"
#include "../../utils/src/utils/protocolos.h"
#include "ciclo_instruccion.h"
#include "mmu.h"
#include "registros.h"

static pthread_mutex_t mutex_interrupcion = PTHREAD_MUTEX_INITIALIZER;
static int interrupcion_pendiente = 0;
static int interrupcion_pid = -1;

// Hilo que escucha interrupciones del Kernel Scheduler
static void *hilo_interrupciones(void *arg) {
  (void)arg;

  while (1) {
    int op = recibir_operacion(socket_interrupt);
    if (op < 0) {
      log_error(logger_cpu, "Kernel Scheduler desconectado (interrupt)");
      break;
    }

    if (op == KS_INTERRUPT) {
      t_paquete *paquete = recibir_paquete(socket_interrupt);
      if (!paquete)
        continue;

      int pid = 0, motivo = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      buffer_read(&motivo, paquete->buffer, sizeof(int));
      eliminar_paquete(paquete);

      pthread_mutex_lock(&mutex_interrupcion);
      interrupcion_pendiente = 1;
      interrupcion_pid = pid;
      pthread_mutex_unlock(&mutex_interrupcion);

      log_info(logger_cpu, "## Interrupcion recibida");
    }
  }

  return NULL;
}

// Enviar PCB de vuelta al Kernel Scheduler con motivo de desalojo
static void devolver_pcb(t_contexto_cpu *contexto, int motivo) {
  // 1. Guardar contexto de ejecucion en Kernel Memory (CPU_SET_CONTEXTO)
  {
    t_paquete *ctx_paq = crear_paquete(CPU_SET_CONTEXTO);
    agregar_a_paquete(ctx_paq, &contexto->pid, sizeof(int));
    agregar_a_paquete(ctx_paq, &contexto->registros.pc, sizeof(uint32_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.ax, sizeof(uint8_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.bx, sizeof(uint8_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.cx, sizeof(uint8_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.dx, sizeof(uint8_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.eax, sizeof(uint32_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.ebx, sizeof(uint32_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.ecx, sizeof(uint32_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.edx, sizeof(uint32_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.si, sizeof(uint32_t));
    agregar_a_paquete(ctx_paq, &contexto->registros.di, sizeof(uint32_t));
    enviar_paquete(ctx_paq, socket_memoria);
    eliminar_paquete(ctx_paq);
    // Esperar ACK del KM
    int op_ack = recibir_operacion(socket_memoria);
    t_paquete *ack = recibir_paquete(socket_memoria);
    (void)op_ack;
    if (ack)
      eliminar_paquete(ack);
  }

  // 2. Devolver PCB al Kernel Scheduler (CPU_DESALOJO)
  t_paquete *paquete = crear_paquete(CPU_DESALOJO);

  agregar_a_paquete(paquete, &contexto->pid, sizeof(int));
  agregar_a_paquete(paquete, &contexto->prioridad, sizeof(int));
  int estado = 0;
  agregar_a_paquete(paquete, &estado, sizeof(int));
  registros_empaquetar(&contexto->registros, paquete);
  agregar_string_a_paquete(paquete, contexto->path_instrucciones);

  int cant_seg =
      contexto->tabla_segmentos ? list_size(contexto->tabla_segmentos) : 0;
  agregar_a_paquete(paquete, &cant_seg, sizeof(int));
  for (int i = 0; i < cant_seg; i++) {
    t_segmento_mmu *seg = list_get(contexto->tabla_segmentos, i);
    agregar_a_paquete(paquete, &seg->id_segmento, sizeof(int));
    agregar_a_paquete(paquete, &seg->base, sizeof(uint32_t));
    agregar_a_paquete(paquete, &seg->tamanio, sizeof(uint32_t));
  }

  agregar_a_paquete(paquete, &motivo, sizeof(int));

  if (motivo == MOTIVO_IO) {
    agregar_string_a_paquete(paquete, contexto->syscall_nombre);
    agregar_a_paquete(paquete, &contexto->syscall_param1, sizeof(int));
    agregar_a_paquete(paquete, &contexto->syscall_param2, sizeof(int));
  } else if (motivo == MOTIVO_SYSCALL) {
    agregar_string_a_paquete(paquete, contexto->syscall_nombre);
    agregar_a_paquete(paquete, &contexto->syscall_param1, sizeof(int));
    agregar_a_paquete(paquete, &contexto->syscall_param2, sizeof(int));
    agregar_string_a_paquete(paquete, contexto->syscall_str_param);
  }

  enviar_paquete(paquete, socket_dispatch);
  eliminar_paquete(paquete);

  log_info(logger_cpu, "PID %d: devuelto al KS (motivo=%d)", contexto->pid,
           motivo);
}

// Liberar contexto
static void liberar_contexto(t_contexto_cpu *contexto) {
  if (!contexto)
    return;
  free(contexto->path_instrucciones);
  free(contexto->syscall_nombre);
  free(contexto->syscall_str_param);
  if (contexto->tabla_segmentos) {
    for (int i = 0; i < list_size(contexto->tabla_segmentos); i++) {
      free(list_get(contexto->tabla_segmentos, i));
    }
    list_destroy(contexto->tabla_segmentos);
  }
  free(contexto);
}

// Hilo principal de ejecucion: espera dispatch del KS
static void *hilo_ejecucion(void *arg) {
  (void)arg;

  while (1) {
    // Esperar que el KS envie un PCB para ejecutar
    int op = recibir_operacion(socket_dispatch);
    if (op < 0) {
      log_error(logger_cpu, "Kernel Scheduler desconectado (dispatch)");
      break;
    }

    if (op != KS_DISPATCH) {
      t_paquete *resp = recibir_paquete(socket_dispatch);
      if (resp)
        eliminar_paquete(resp);
      continue;
    }

    t_paquete *paquete = recibir_paquete(socket_dispatch);
    if (!paquete)
      continue;

    // Deserializar el contexto del proceso
    t_contexto_cpu *contexto = calloc(1, sizeof(t_contexto_cpu));
    buffer_read(&contexto->pid, paquete->buffer, sizeof(int));
    buffer_read(&contexto->prioridad, paquete->buffer, sizeof(int));
    int estado_dummy = 0;
    buffer_read(&estado_dummy, paquete->buffer, sizeof(int));
    registros_desempaquetar(&contexto->registros, paquete->buffer);
    contexto->path_instrucciones = buffer_read_string(paquete->buffer);

    // Tabla de segmentos
    int cant_seg = 0;
    buffer_read(&cant_seg, paquete->buffer, sizeof(int));
    contexto->tabla_segmentos = list_create();
    for (int i = 0; i < cant_seg; i++) {
      t_segmento_mmu *seg = calloc(1, sizeof(t_segmento_mmu));
      buffer_read(&seg->id_segmento, paquete->buffer, sizeof(int));
      buffer_read(&seg->base, paquete->buffer, sizeof(uint32_t));
      buffer_read(&seg->tamanio, paquete->buffer, sizeof(uint32_t));
      list_add(contexto->tabla_segmentos, seg);
    }

    eliminar_paquete(paquete);

    contexto->interrumpido = 0;
    contexto->pc_modificado = 0;
    contexto->syscall_nombre = NULL;
    contexto->syscall_str_param = NULL;

    // Resetear interrupcion
    pthread_mutex_lock(&mutex_interrupcion);
    interrupcion_pendiente = 0;
    interrupcion_pid = -1;
    pthread_mutex_unlock(&mutex_interrupcion);

    log_info(logger_cpu, "PID %d: recibido para ejecucion (PC=%u)",
             contexto->pid, contexto->registros.pc);

    // Ejecutar ciclo de instrucciones
    t_resultado_ciclo resultado = CICLO_CONTINUAR;
    while (resultado == CICLO_CONTINUAR) {
      // Verificar interrupciones antes de cada ciclo
      pthread_mutex_lock(&mutex_interrupcion);
      if (interrupcion_pendiente && interrupcion_pid == contexto->pid) {
        contexto->interrumpido = 1;
        interrupcion_pendiente = 0;
      }
      pthread_mutex_unlock(&mutex_interrupcion);

      resultado = ejecutar_ciclo_instruccion(contexto);
    }

    // Devolver PCB al KS segun el resultado
    int motivo = MOTIVO_ERROR;
    switch (resultado) {
    case CICLO_DESALOJO_EXIT:
      motivo = MOTIVO_EXIT;
      break;
    case CICLO_DESALOJO_IO:
      motivo = MOTIVO_IO;
      break;
    case CICLO_DESALOJO_SYSCALL:
      motivo = MOTIVO_SYSCALL;
      break;
    case CICLO_DESALOJO_INT:
      motivo = MOTIVO_INTERRUPCION;
      break;
    case CICLO_SEG_FAULT:
      motivo = MOTIVO_SEG_FAULT;
      break;
    case CICLO_ERROR:
      motivo = MOTIVO_ERROR;
      break;
    default:
      motivo = MOTIVO_ERROR;
      break;
    }

    devolver_pcb(contexto, motivo);
    liberar_contexto(contexto);
  }

  return NULL;
}

int main(int argc, char **argv) {
  const char *config_path = "CPU.config";
  const char *cpu_id_str = "1"; // Identificador por defecto

  if (argc > 1)
    config_path = argv[1];
  if (argc > 2)
    cpu_id_str = argv[2];

  t_config *config = config_create((char *)config_path);
  if (!config) {
    fprintf(stderr, "No se pudo abrir el config: %s\n", config_path);
    return EXIT_FAILURE;
  }

  const char *ip_ks = config_get_string_value(config, "IP_KERNEL_SCHEDULER");
  const char *puerto_ks =
      config_get_string_value(config, "PUERTO_KERNEL_SCHEDULER");
  const char *ip_km = config_get_string_value(config, "IP_KERNEL_MEMORY");
  const char *puerto_km =
      config_get_string_value(config, "PUERTO_KERNEL_MEMORY");
  const char *log_level = config_get_string_value(config, "LOG_LEVEL");

  char log_name[64];
  snprintf(log_name, sizeof(log_name), "CPU_%s.log", cpu_id_str);

  logger_cpu =
      log_create(log_name, "CPU", 1, log_level_from_string((char *)log_level));
  if (logger_cpu == NULL) {
    fprintf(stderr, "No se pudo crear el logger\n");
    config_destroy(config);
    return EXIT_FAILURE;
  }

  int tam_max_seg = 64; // default
  if (config_has_property(config, "TAM_MAX_SEGMENTO")) {
    tam_max_seg = config_get_int_value(config, "TAM_MAX_SEGMENTO");
  } else if (config_has_property(config, "SEGMENT_MAX_SIZE")) {
    tam_max_seg = config_get_int_value(config, "SEGMENT_MAX_SIZE");
  } else {
    tam_max_seg = 128; // default to match tests
  }
  mmu_configurar(tam_max_seg);
  log_info(logger_cpu, "MMU configurada con SEGMENT_MAX_SIZE = %d",
           tam_max_seg);

  int cpu_id = atoi(cpu_id_str);

  // Conectar al Kernel Scheduler
  socket_dispatch =
      crear_conexion((char *)ip_ks, (char *)puerto_ks, logger_cpu);
  if (socket_dispatch == -1) {
    log_error(logger_cpu, "No se pudo conectar al Kernel Scheduler");
    liberar_conexion(socket_memoria);
    config_destroy(config);
    log_destroy(logger_cpu);
    return EXIT_FAILURE;
  }
  {
    t_paquete *p = crear_paquete(HANDSHAKE_CPU);
    agregar_a_paquete(p, &cpu_id, sizeof(int));
    enviar_paquete(p, socket_dispatch);
    eliminar_paquete(p);
  }
  if (recibir_operacion(socket_dispatch) != HANDSHAKE_OK) {
    log_error(logger_cpu, "Handshake dispatch fallido con Kernel Scheduler");
    return EXIT_FAILURE;
  }
  log_info(logger_cpu, "## Conectado a Kernel Scheduler");

  // Conectar a Kernel Scheduler (interrupt)
  socket_interrupt =
      crear_conexion((char *)ip_ks, (char *)puerto_ks, logger_cpu);
  if (socket_interrupt < 0) {
    log_error(logger_cpu, "No se pudo conectar a KS (interrupt)");
    liberar_conexion(socket_memoria);
    liberar_conexion(socket_dispatch);
    config_destroy(config);
    log_destroy(logger_cpu);
    return EXIT_FAILURE;
  }
  {
    t_paquete *p = crear_paquete(HANDSHAKE_CPU);
    agregar_a_paquete(p, &cpu_id, sizeof(int));
    enviar_paquete(p, socket_interrupt);
    eliminar_paquete(p);
  }
  if (recibir_operacion(socket_interrupt) != HANDSHAKE_OK) {
    log_error(logger_cpu, "Handshake fallido con KS (interrupt)");
    return EXIT_FAILURE;
  }
  log_info(logger_cpu, "Conectado a KS (interrupt)");

  // Conectar a Kernel Memory
  socket_memoria = crear_conexion((char *)ip_km, (char *)puerto_km, logger_cpu);
  if (socket_memoria < 0) {
    log_error(logger_cpu, "No se pudo conectar a Kernel Memory");
    return EXIT_FAILURE;
  }
  {
    t_paquete *p = crear_paquete(HANDSHAKE_CPU);
    agregar_a_paquete(p, &cpu_id, sizeof(int));
    enviar_paquete(p, socket_memoria);
    eliminar_paquete(p);
  }
  if (recibir_operacion(socket_memoria) != HANDSHAKE_OK) {
    log_error(logger_cpu, "Handshake fallido con Kernel Memory");
    return EXIT_FAILURE;
  }
  log_info(logger_cpu, "Conectado a Kernel Memory");

  // Conexiones dinamicas a Memory Stick se manejan en obtener_socket_ms()

  // Lanzar hilo de interrupciones
  pthread_t hilo_int;
  pthread_create(&hilo_int, NULL, hilo_interrupciones, NULL);
  pthread_detach(hilo_int);

  // Ejecutar hilo principal (dispatch)
  hilo_ejecucion(NULL);

  // Limpieza
  close(socket_dispatch);
  close(socket_interrupt);
  close(socket_memoria);
  log_destroy(logger_cpu);
  config_destroy(config);

  return EXIT_SUCCESS;
}
