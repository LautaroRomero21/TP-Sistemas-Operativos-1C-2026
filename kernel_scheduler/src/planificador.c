#include "planificador.h"
#include "../../utils/src/utils/protocolos.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Variables globales
t_log *logger = NULL;
t_algoritmo_planificacion algoritmo = ALG_FIFO;
int quantum_config = 5;
int grado_multiprogramacion = 4;
int suspension_timeout_ms = 10000;
int cant_colas_multinivel = 0;
char **algoritmo_por_cola = NULL;
int desalojo_entre_colas = 0;

t_list *cola_new = NULL;
t_list *cola_ready = NULL;
t_list **colas_multinivel = NULL;
t_list *cola_exec = NULL;
t_list *cola_block = NULL;
t_list *cola_susp_ready = NULL;
t_list *cola_susp_block = NULL;
t_list *cola_exit = NULL;

pthread_mutex_t mutex_planificacion = PTHREAD_MUTEX_INITIALIZER;
sem_t sem_multiprogramacion;
sem_t sem_proceso_ready;
sem_t sem_cpu_libre;

int socket_cpu_dispatch = -1;
int socket_cpu_interrupt = -1;
int socket_kernel_memory = -1;
pthread_mutex_t mutex_socket_km = PTHREAD_MUTEX_INITIALIZER;
int hay_proceso_ejecutando = 0;

pthread_mutex_t mutex_compactacion = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv_compactacion_fin = PTHREAD_COND_INITIALIZER;
pthread_cond_t cv_cpus_idle = PTHREAD_COND_INITIALIZER;
bool compactacion_en_curso = false;
int cpus_pendientes_desalojo = 0;

// ─────────────────────────────────────────────────
// Helpers internos
// ─────────────────────────────────────────────────

static void *hilo_suspension_proceso(void *arg);
static void *hilo_quantum_proceso(void *arg);

static const char *nombre_estado(t_estado_proceso estado) {
  switch (estado) {
  case ESTADO_NEW:
    return "NEW";
  case ESTADO_READY:
    return "READY";
  case ESTADO_EXEC:
    return "EXEC";
  case ESTADO_BLOCK:
    return "BLOCK";
  case ESTADO_SUSP_READY:
    return "SUSP. READY";
  case ESTADO_SUSP_BLOCK:
    return "SUSP. BLOCK";
  case ESTADO_EXIT:
    return "EXIT";
  default:
    return "DESCONOCIDO";
  }
}

static void log_cambio_estado(t_pcb *pcb, t_estado_proceso anterior,
                              t_estado_proceso nuevo) {
  log_info(logger, "## (%d) Pasa del estado %s al estado %s", pcb->pid,
           nombre_estado(anterior), nombre_estado(nuevo));
}

// ─────────────────────────────────────────────────
// Inicializacion
// ─────────────────────────────────────────────────

void planificador_inicializar(void) {
  cola_new = list_create();
  cola_ready = list_create();
  cola_exec = list_create();
  cola_block = list_create();
  cola_susp_ready = list_create();
  cola_susp_block = list_create();
  cola_exit = list_create();

  if (algoritmo == ALG_CMN && cant_colas_multinivel > 0) {
    colas_multinivel = malloc(sizeof(t_list *) * cant_colas_multinivel);
    for (int i = 0; i < cant_colas_multinivel; i++)
      colas_multinivel[i] = list_create();
  }

  sem_init(&sem_multiprogramacion, 0, grado_multiprogramacion);
  sem_init(&sem_proceso_ready, 0, 0);
  sem_init(&sem_cpu_libre, 0, 0);
}

// ─────────────────────────────────────────────────
// Transiciones de estado
// ─────────────────────────────────────────────────

void planificador_agregar_nuevo(t_pcb *pcb) {
  pthread_mutex_lock(&mutex_planificacion);
  pcb->estado = ESTADO_NEW;
  list_add(cola_new, pcb);
  log_info(logger, "## (%d) Se crea el proceso - Estado: NEW", pcb->pid);
  pthread_mutex_unlock(&mutex_planificacion);
}

void planificador_pasar_a_ready(t_pcb *pcb) {
  pthread_mutex_lock(&mutex_planificacion);
  if (pcb->thread_suspension_active) {
    pthread_cancel(pcb->thread_suspension);
    pcb->thread_suspension_active = 0;
  }
  t_estado_proceso anterior = pcb->estado;
  pcb->estado = ESTADO_READY;
  pcb->quantum_restante = quantum_config;

  if (algoritmo == ALG_CMN && cant_colas_multinivel > 0) {
    int prio = pcb->prioridad;
    if (prio < 0)
      prio = 0;
    if (prio >= cant_colas_multinivel)
      prio = cant_colas_multinivel - 1;
    list_add(colas_multinivel[prio], pcb);
    log_info(logger, "## PID: %d - Ingresado a cola READY de prioridad %d",
             pcb->pid, prio);
  } else {
    list_add(cola_ready, pcb);
  }

  log_cambio_estado(pcb, anterior, ESTADO_READY);
  sem_post(&sem_proceso_ready);

  if (desalojo_entre_colas && algoritmo == ALG_CMN) {
    evaluar_desalojo_por_prioridad();
  }
  pthread_mutex_unlock(&mutex_planificacion);
}

void planificador_pasar_a_exec(t_pcb *pcb) {
  pthread_mutex_lock(&mutex_planificacion);
  t_estado_proceso anterior = pcb->estado;
  pcb->estado = ESTADO_EXEC;
  list_add(cola_exec, pcb);
  hay_proceso_ejecutando = 1;
  log_cambio_estado(pcb, anterior, ESTADO_EXEC);
  pthread_mutex_unlock(&mutex_planificacion);
}

void planificador_pasar_a_block(t_pcb *pcb) {
  pthread_mutex_lock(&mutex_planificacion);
  t_estado_proceso anterior = pcb->estado;
  pcb->estado = ESTADO_BLOCK;
  // Guardar timestamp de entrada a BLOCK para suspension por timeout
  clock_gettime(CLOCK_MONOTONIC, &pcb->block_entry_time);
  list_add(cola_block, pcb);
  hay_proceso_ejecutando = 0;

  pcb->thread_suspension_active = 1;
  pthread_create(&pcb->thread_suspension, NULL, hilo_suspension_proceso,
                 (void *)(intptr_t)pcb->pid);
  pthread_detach(pcb->thread_suspension);

  log_cambio_estado(pcb, anterior, ESTADO_BLOCK);
  pthread_mutex_unlock(&mutex_planificacion);
}

void planificador_pasar_a_exit(t_pcb *pcb, const char *motivo) {
  pthread_mutex_lock(&mutex_planificacion);
  if (pcb->thread_suspension_active) {
    pthread_cancel(pcb->thread_suspension);
    pcb->thread_suspension_active = 0;
  }
  if (pcb->thread_quantum_active) {
    pthread_cancel(pcb->thread_quantum);
    pcb->thread_quantum_active = 0;
  }
  t_estado_proceso anterior = pcb->estado;
  pcb->estado = ESTADO_EXIT;
  list_add(cola_exit, pcb);
  hay_proceso_ejecutando = 0;
  log_cambio_estado(pcb, anterior, ESTADO_EXIT);
  log_info(logger, "## (%d) finalizo su ejecucion con motivo de %s", pcb->pid,
           motivo);
  pthread_mutex_unlock(&mutex_planificacion);
  sem_post(&sem_multiprogramacion);
  solicitar_fin_proceso(pcb->pid);
}

void planificador_pasar_a_susp_block(t_pcb *pcb) {
  // Asegurar orden de los mensajes de red: pedir lock de KM primero.
  pthread_mutex_lock(&mutex_socket_km);

  pthread_mutex_lock(&mutex_planificacion);

  int idx = -1;
  for (int i = 0; i < list_size(cola_block); i++) {
    t_pcb *p = list_get(cola_block, i);
    if (p->pid == pcb->pid) {
      idx = i;
      break;
    }
  }

  // Si ya no esta en block, significa que el IO termino justo a tiempo.
  if (idx == -1) {
    pthread_mutex_unlock(&mutex_planificacion);
    pthread_mutex_unlock(&mutex_socket_km);
    return;
  }

  t_estado_proceso anterior = pcb->estado;
  pcb->estado = ESTADO_SUSP_BLOCK;
  list_remove(cola_block, idx);
  list_add(cola_susp_block, pcb);
  log_cambio_estado(pcb, anterior, ESTADO_SUSP_BLOCK);
  pthread_mutex_unlock(&mutex_planificacion);

  t_paquete *paquete = crear_paquete(KS_SUSPENDER_PROCESO);
  agregar_a_paquete(paquete, &pcb->pid, sizeof(int));
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  (void)op;
  if (resp)
    eliminar_paquete(resp);

  sem_post(&sem_multiprogramacion);
}

void planificador_pasar_a_susp_ready(t_pcb *pcb) {
  pthread_mutex_lock(&mutex_planificacion);
  t_estado_proceso anterior = pcb->estado;
  pcb->estado = ESTADO_SUSP_READY;

  for (int i = 0; i < list_size(cola_susp_block); i++) {
    if (list_get(cola_susp_block, i) == pcb) {
      list_remove(cola_susp_block, i);
      break;
    }
  }

  list_add(cola_susp_ready, pcb);
  log_cambio_estado(pcb, anterior, ESTADO_SUSP_READY);
  log_info(logger, "## Des-suspension de Proceso: PID %d", pcb->pid);
  pthread_mutex_unlock(&mutex_planificacion);
}

// ─────────────────────────────────────────────────
// Seleccion de siguiente proceso READY
// ─────────────────────────────────────────────────

// Para CMN: devuelve cual es la cola de mayor prioridad no vacia (numero menor
// = mayor prioridad)
static int cmn_cola_con_proceso(void) {
  for (int i = 0; i < cant_colas_multinivel; i++) {
    if (!list_is_empty(colas_multinivel[i]))
      return i;
  }
  return -1;
}

t_pcb *planificador_obtener_siguiente_ready(void) {
  if (algoritmo == ALG_CMN && cant_colas_multinivel > 0) {
    int prio = cmn_cola_con_proceso();
    if (prio < 0)
      return NULL;
    return list_remove(colas_multinivel[prio], 0);
  }
  // FIFO y RR: lista unica
  if (list_is_empty(cola_ready))
    return NULL;
  return list_remove(cola_ready, 0);
}

// ─────────────────────────────────────────────────
// Desalojo por prioridad (CMN)
// ─────────────────────────────────────────────────

void evaluar_desalojo_por_prioridad(void) {
  if (algoritmo != ALG_CMN || !desalojo_entre_colas)
    return;
  if (list_is_empty(cola_exec))
    return;

  // 1. Si hay CPUs libres, el proceso de alta prioridad entrara a ejecutar
  // directamente
  int cpus_libres = 0;
  sem_getvalue(&sem_cpu_libre, &cpus_libres);
  if (cpus_libres > 0)
    return;

  // 2. Buscar el proceso con PEOR prioridad en EXEC (numero mas alto)
  t_pcb *peor_exec = list_get(cola_exec, 0);
  for (int i = 1; i < list_size(cola_exec); i++) {
    t_pcb *p = list_get(cola_exec, i);
    if (p->prioridad > peor_exec->prioridad) {
      peor_exec = p;
    }
  }

  int prio_peor = peor_exec->prioridad;

  // 3. Ver si hay un proceso listo con MEJOR prioridad (numero menor al peor)
  for (int i = 0; i < prio_peor; i++) {
    if (!list_is_empty(colas_multinivel[i])) {
      t_pcb *nuevo = list_get(colas_multinivel[i], 0);
      if (!peor_exec->interrupcion_enviada) {
        log_info(logger,
                 "## (%d) Prioridad: %d - Desalojado por cola mas prioritaria "
                 "por el proceso %d con prioridad %d",
                 peor_exec->pid, prio_peor, nuevo->pid, nuevo->prioridad);
        peor_exec->interrupcion_enviada = 1;
        enviar_interrupcion_cpu(peor_exec->pid, MOTIVO_INTERRUPCION);
      }
      return;
    }
  }
}

// ─────────────────────────────────────────────────
// Comunicacion con CPU
// ─────────────────────────────────────────────────

void enviar_pcb_a_cpu(t_pcb *pcb) {
  int despacho_socket = -1;

  // Recorrer lista de CPUs conectadas buscando una libre con ambos sockets
  typedef struct {
    int sd;
    int si;
    int ocu;
    int pid;
  } _cpu_t;
  pthread_mutex_lock(&mutex_cpus);
  for (int i = 0; i < list_size(lista_cpus); i++) {
    _cpu_t *c = list_get(lista_cpus, i);
    if (!c->ocu && c->sd >= 0 && c->si >= 0) {
      c->ocu = 1;
      c->pid = pcb->pid;
      despacho_socket = c->sd;
      socket_cpu_dispatch = c->sd;
      socket_cpu_interrupt = c->si;
      break;
    }
  }
  pthread_mutex_unlock(&mutex_cpus);

  if (despacho_socket < 0)
    despacho_socket = socket_cpu_dispatch; // fallback

  if (despacho_socket < 0) {
    log_error(logger, "No hay CPU disponible para despachar PID %d", pcb->pid);
    return;
  }

  t_paquete *paquete = crear_paquete(KS_DISPATCH);
  pcb_serializar(pcb, paquete);
  enviar_paquete(paquete, despacho_socket);
  eliminar_paquete(paquete);
  log_info(logger, "PID %d: enviado al CPU (socket=%d) para ejecucion",
           pcb->pid, despacho_socket);
}

void enviar_interrupcion_cpu(int pid, int motivo) {
  int interrupt_socket = -1;

  typedef struct {
    int sd;
    int si;
    int ocu;
    int pid;
  } _cpu_t;

  pthread_mutex_lock(&mutex_cpus);
  for (int i = 0; i < list_size(lista_cpus); i++) {
    _cpu_t *c = list_get(lista_cpus, i);
    if (c->ocu && c->pid == pid) {
      interrupt_socket = c->si;
      break;
    }
  }
  pthread_mutex_unlock(&mutex_cpus);

  if (interrupt_socket < 0)
    interrupt_socket = socket_cpu_interrupt; // fallback

  if (interrupt_socket < 0)
    return;

  t_paquete *paquete = crear_paquete(KS_INTERRUPT);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  agregar_a_paquete(paquete, &motivo, sizeof(int));
  enviar_paquete(paquete, interrupt_socket);
  eliminar_paquete(paquete);
  log_info(logger, "## Interrupcion enviada al CPU para PID %d (motivo=%d)",
           pid, motivo);
}

// ─────────────────────────────────────────────────
// Comunicacion con Kernel Memory
// ─────────────────────────────────────────────────

int solicitar_init_proceso(t_pcb *pcb) {
  t_paquete *paquete = crear_paquete(KS_INIT_PROCESO);
  agregar_a_paquete(paquete, &pcb->pid, sizeof(int));
  agregar_string_a_paquete(paquete, pcb->path_instrucciones);
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  if (resp)
    eliminar_paquete(resp);
  return (op == KM_RESPUESTA_OK) ? 0 : -1;
}

void solicitar_fin_proceso(int pid) {
  t_paquete *paquete = crear_paquete(KS_FIN_PROCESO);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  (void)op;
  if (resp)
    eliminar_paquete(resp);
}

int solicitar_suspender_proceso(int pid) {
  t_paquete *paquete = crear_paquete(KS_SUSPENDER_PROCESO);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  if (resp)
    eliminar_paquete(resp);
  return (op == KM_RESPUESTA_OK) ? 0 : -1;
}

int solicitar_verificar_espacio(int pid) {
  t_paquete *paquete = crear_paquete(KS_VERIFICAR_ESPACIO);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  if (resp)
    eliminar_paquete(resp);
  return (op == KM_RESPUESTA_OK) ? 1 : 0;
}

int solicitar_reanudar_proceso(int pid) {
  t_paquete *paquete = crear_paquete(KS_REANUDAR_PROCESO);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  if (resp)
    eliminar_paquete(resp);
  return (op == KM_RESPUESTA_OK) ? 0 : -1;
}

int solicitar_mem_alloc(int pid, int id_segmento, int tamanio) {
  t_paquete *paquete = crear_paquete(KS_MEM_ALLOC);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  agregar_a_paquete(paquete, &id_segmento, sizeof(int));
  agregar_a_paquete(paquete, &tamanio, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  if (op == KM_RESPUESTA_COMPACTAR) {
    if (resp)
      eliminar_paquete(resp);
    return -2;
  }
  if (resp)
    eliminar_paquete(resp);
  return (op == KM_RESPUESTA_OK) ? 0 : -1;
}

void solicitar_mem_free(int pid, int id_segmento) {
  t_paquete *paquete = crear_paquete(KS_MEM_FREE);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  agregar_a_paquete(paquete, &id_segmento, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  (void)op;
  if (resp)
    eliminar_paquete(resp);

  // Si liberamos memoria, revisamos si podemos traer a algun suspendido
  planificador_des_suspender_procesos();
}

void solicitar_compactacion(void) {
  log_info(logger, "## Inicio de compactacion");

  pthread_mutex_lock(&mutex_compactacion);
  compactacion_en_curso = true;
  cpus_pendientes_desalojo = 0;

  pthread_mutex_lock(&mutex_planificacion);
  int cpus_ejecutando = list_size(cola_exec);
  if (cpus_ejecutando > 0) {
    cpus_pendientes_desalojo = cpus_ejecutando;
    for (int i = 0; i < cpus_ejecutando; i++) {
      t_pcb *en_exec = list_get(cola_exec, i);
      log_info(logger, "Desalojando PID %d antes de compactar", en_exec->pid);
      enviar_interrupcion_cpu(en_exec->pid, MOTIVO_INTERRUPCION);
    }
  }
  pthread_mutex_unlock(&mutex_planificacion);

  // Esperar a que todas las CPUs informen desalojo (en hilo_cpu_dispatch)
  while (cpus_pendientes_desalojo > 0) {
    pthread_cond_wait(&cv_cpus_idle, &mutex_compactacion);
  }

  // Ahora si, pedir compactacion al KM
  t_paquete *paquete = crear_paquete(KS_COMPACTAR);
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  t_paquete *resp = recibir_paquete(socket_kernel_memory);
  pthread_mutex_unlock(&mutex_socket_km);

  (void)op;
  if (resp)
    eliminar_paquete(resp);

  log_info(logger, "## Fin de compactacion");

  compactacion_en_curso = false;
  pthread_cond_broadcast(&cv_compactacion_fin);
  pthread_mutex_unlock(&mutex_compactacion);

  pthread_mutex_lock(&mutex_planificacion);
  for (int i = 0; i < list_size(cola_susp_block); i++) {

    (void)i;
  }
  sem_post(&sem_multiprogramacion);
  pthread_mutex_unlock(&mutex_planificacion);
}

void actualizar_tabla_segmentos(t_pcb *pcb) {
  t_paquete *paquete = crear_paquete(KS_GET_TABLA);
  agregar_a_paquete(paquete, &pcb->pid, sizeof(int));
  pthread_mutex_lock(&mutex_socket_km);
  enviar_paquete(paquete, socket_kernel_memory);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_kernel_memory);
  if (op == KM_RESPUESTA_TABLA) {
    t_paquete *resp = recibir_paquete(socket_kernel_memory);
    pthread_mutex_unlock(&mutex_socket_km);
    if (!resp)
      return;
    int cant = 0;
    buffer_read(&cant, resp->buffer, sizeof(int));

    if (pcb->tabla_segmentos) {
      for (int i = 0; i < list_size(pcb->tabla_segmentos); i++)
        free(list_get(pcb->tabla_segmentos, i));
      list_clean(pcb->tabla_segmentos);
    } else {
      pcb->tabla_segmentos = list_create();
    }

    for (int i = 0; i < cant; i++) {
      t_entrada_segmento *seg = malloc(sizeof(t_entrada_segmento));
      buffer_read(&seg->id_segmento, resp->buffer, sizeof(int));
      buffer_read(&seg->base, resp->buffer, sizeof(uint32_t));
      buffer_read(&seg->tamanio, resp->buffer, sizeof(uint32_t));

      list_add(pcb->tabla_segmentos, seg);
    }
    eliminar_paquete(resp);
  } else {
    pthread_mutex_unlock(&mutex_socket_km);
  }
}

// ─────────────────────────────────────────────────
// Hilo: Quantum timer para RR
// ─────────────────────────────────────────────────

static void *hilo_quantum_proceso(void *arg) {
  int pid = (int)(intptr_t)arg;

  pthread_mutex_lock(&mutex_planificacion);
  int sleep_time = quantum_config;
  t_pcb *pcb_actual = NULL;
  for (int i = 0; i < list_size(cola_exec); i++) {
    t_pcb *p = list_get(cola_exec, i);
    if (p->pid == pid) {
      pcb_actual = p;
      sleep_time = p->quantum_restante;
      break;
    }
  }
  pthread_mutex_unlock(&mutex_planificacion);

  if (!pcb_actual)
    return NULL;

  usleep(sleep_time > 0 ? (useconds_t)sleep_time * 1000 : 0);

  int oldstate;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

  pthread_mutex_lock(&mutex_planificacion);
  pcb_actual = NULL;
  for (int i = 0; i < list_size(cola_exec); i++) {
    t_pcb *p = list_get(cola_exec, i);
    if (p->pid == pid) {
      pcb_actual = p;
      break;
    }
  }

  if (pcb_actual && pcb_actual->thread_quantum_active) {
    pcb_actual->thread_quantum_active = 0;
    log_info(logger, "## (%d) - Desalojado por fin de quantum",
             pcb_actual->pid);
    enviar_interrupcion_cpu(pcb_actual->pid, MOTIVO_INTERRUPCION);
  }
  pthread_mutex_unlock(&mutex_planificacion);

  pthread_setcancelstate(oldstate, NULL);
  return NULL;
}

void planificador_iniciar_quantum(t_pcb *pcb) {
  int alg_proceso = algoritmo;
  if (algoritmo == ALG_CMN && algoritmo_por_cola != NULL) {
    int p = pcb->prioridad;
    if (p < 0)
      p = 0;
    if (p >= cant_colas_multinivel)
      p = cant_colas_multinivel - 1;
    if (strcmp(algoritmo_por_cola[p], "RR") == 0)
      alg_proceso = ALG_RR;
    else
      alg_proceso = ALG_FIFO;
  }
  if (alg_proceso == ALG_RR) {
    pcb->thread_quantum_active = 1;
    pthread_create(&pcb->thread_quantum, NULL, hilo_quantum_proceso,
                   (void *)(intptr_t)pcb->pid);
    pthread_detach(pcb->thread_quantum);
  }
}

// ─────────────────────────────────────────────────
// Helper: Buscar mejor suspendido (prioridad + FIFO)
// ─────────────────────────────────────────────────

static t_pcb *planificador_buscar_mejor_suspendido(int *out_idx) {
  if (list_is_empty(cola_susp_ready))
    return NULL;

  int cant_susp = list_size(cola_susp_ready);
  t_pcb **candidatos = malloc(cant_susp * sizeof(t_pcb *));
  for (int i = 0; i < cant_susp; i++) {
    candidatos[i] = list_get(cola_susp_ready, i);
  }

  // ordenamos por prioridad (para mantener orden FIFO)
  for (int i = 0; i < cant_susp - 1; i++) {
    for (int j = 0; j < cant_susp - i - 1; j++) {
      if (candidatos[j]->prioridad > candidatos[j + 1]->prioridad) {
        t_pcb *temp = candidatos[j];
        candidatos[j] = candidatos[j + 1];
        candidatos[j + 1] = temp;
      }
    }
  }

  t_pcb *elegido = NULL;
  for (int i = 0; i < cant_susp; i++) {
    t_pcb *cand = candidatos[i];

    pthread_mutex_unlock(&mutex_planificacion);
    int cabe = solicitar_verificar_espacio(cand->pid);
    pthread_mutex_lock(&mutex_planificacion);

    if (cabe) {
      // Verificar que siga en la lista (por si fue modificado concurrentemente)
      int sigue = 0;
      for (int k = 0; k < list_size(cola_susp_ready); k++) {
        if (list_get(cola_susp_ready, k) == cand) {
          sigue = 1;
          if (out_idx)
            *out_idx = k;
          break;
        }
      }
      if (sigue) {
        elegido = cand;
        break; // Al estar ordenados, este es el de mejor prioridad que cabe
      }
    }
  }

  free(candidatos);
  return elegido;
}

// ─────────────────────────────────────────────────
// Hilo: Planificador de Largo Plazo (NEW -> READY)
// ─────────────────────────────────────────────────

void *hilo_planificador_largo_plazo(void *arg) {
  (void)arg;
  while (1) {
    sem_wait(&sem_multiprogramacion);

    pthread_mutex_lock(&mutex_planificacion);

    // 1. Primero intentar des-suspender procesos en SUSP_READY por prioridad
    if (!list_is_empty(cola_susp_ready)) {
      int idx_elegido = -1;
      t_pcb *elegido = planificador_buscar_mejor_suspendido(&idx_elegido);

      if (elegido && idx_elegido >= 0) {
        list_remove(cola_susp_ready, idx_elegido);
        pthread_mutex_unlock(&mutex_planificacion);
        solicitar_reanudar_proceso(elegido->pid);
        planificador_pasar_a_ready(elegido);
        continue;
      }
    }

    // 2. Tomar de NEW
    if (!list_is_empty(cola_new)) {
      t_pcb *pcb = list_remove(cola_new, 0);
      pthread_mutex_unlock(&mutex_planificacion);
      if (solicitar_init_proceso(pcb) == 0) {
        planificador_pasar_a_ready(pcb);
      } else {
        log_error(logger, "PID %d: error al inicializar en memoria", pcb->pid);
        planificador_pasar_a_exit(pcb, "ERROR");
      }
      continue;
    }

    // No habia nada, devolver el semaforo
    sem_post(&sem_multiprogramacion);
    pthread_mutex_unlock(&mutex_planificacion);
    usleep(5000);
  }
  return NULL;
}

// ─────────────────────────────────────────────────
// Hilo: Planificador de Corto Plazo (READY -> EXEC)
// ─────────────────────────────────────────────────

void *hilo_planificador_corto_plazo(void *arg) {
  (void)arg;

  while (1) {
    sem_wait(&sem_proceso_ready);
    sem_wait(&sem_cpu_libre);

    pthread_mutex_lock(&mutex_planificacion);

    if (socket_cpu_dispatch < 0) {
      sem_post(&sem_cpu_libre);
      sem_post(&sem_proceso_ready);
      pthread_mutex_unlock(&mutex_planificacion);
      usleep(200000);
      continue;
    }

    t_pcb *pcb = planificador_obtener_siguiente_ready();
    if (pcb) {
      pthread_mutex_unlock(&mutex_planificacion);
      actualizar_tabla_segmentos(pcb);

      if (pcb->pending_stdin) {
        t_paquete *mem_paq = crear_paquete(KS_STDIN_ESCRIBIR);
        agregar_a_paquete(mem_paq, &pcb->pid, sizeof(int));
        agregar_a_paquete(mem_paq, &pcb->segmento_io, sizeof(int));
        agregar_a_paquete(mem_paq, &pcb->tamanio_io, sizeof(int));
        int len = pcb->tamanio_io;
        agregar_a_paquete(mem_paq, &len, sizeof(int));
        if (len > 0) {
          agregar_a_paquete(mem_paq, pcb->pending_stdin, len);
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
        free(pcb->pending_stdin);
        pcb->pending_stdin = NULL;
      }

      pthread_mutex_lock(&mutex_planificacion);
      pcb->interrupcion_enviada = 0;
      clock_gettime(CLOCK_MONOTONIC, &pcb->exec_entry_time);
      planificador_iniciar_quantum(pcb);
      pthread_mutex_unlock(&mutex_planificacion);

      planificador_pasar_a_exec(pcb);
      enviar_pcb_a_cpu(pcb);
    } else {
      sem_post(&sem_cpu_libre);
      pthread_mutex_unlock(&mutex_planificacion);
    }
  }
  return NULL;
}

// ─────────────────────────────────────────────────
// Hilo: Suspension por timeout de BLOCK
// ─────────────────────────────────────────────────

static void *hilo_suspension_proceso(void *arg) {
  int pid = (int)(intptr_t)arg;

  usleep((useconds_t)suspension_timeout_ms * 1000);

  int oldstate;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

  pthread_mutex_lock(&mutex_planificacion);
  t_pcb *pcb_actual = NULL;
  for (int i = 0; i < list_size(cola_block); i++) {
    t_pcb *p = list_get(cola_block, i);
    if (p->pid == pid) {
      pcb_actual = p;
      break;
    }
  }

  int activate = 0;
  if (pcb_actual && pcb_actual->thread_suspension_active) {
    pcb_actual->thread_suspension_active = 0;
    activate = 1;
    log_info(logger, "PID %d: timeout de suspension alcanzado (%d ms >= %d ms)",
             pcb_actual->pid, suspension_timeout_ms, suspension_timeout_ms);
  }
  pthread_mutex_unlock(&mutex_planificacion);

  if (activate && pcb_actual) {
    planificador_pasar_a_susp_block(pcb_actual);
  }

  pthread_setcancelstate(oldstate, NULL);
  return NULL;
}

void planificador_des_suspender_procesos(void) {
  pthread_mutex_lock(&mutex_planificacion);

  if (list_is_empty(cola_susp_ready)) {
    pthread_mutex_unlock(&mutex_planificacion);
    return;
  }

  // Repetir mientras haya procesos que podamos des-suspender
  while (1) {
    // 1. Verificamos si podemos incrementar el nivel de multiprogramacion
    if (sem_trywait(&sem_multiprogramacion) != 0) {
      // Alcanzamos el limite de multiprogramacion, no podemos traer mas a RAM
      break;
    }

    int idx_elegido = -1;
    t_pcb *elegido = planificador_buscar_mejor_suspendido(&idx_elegido);

    if (elegido && idx_elegido >= 0) {
      list_remove(cola_susp_ready, idx_elegido);
      pthread_mutex_unlock(&mutex_planificacion);

      solicitar_reanudar_proceso(elegido->pid);
      planificador_pasar_a_ready(elegido);

      pthread_mutex_lock(&mutex_planificacion);
    } else {
      // Tomamos un token pero no pudimos des-suspender a nadie (nadie cabe).
      // Devolvemos el token.
      sem_post(&sem_multiprogramacion);
      break; // Salimos del while
    }
  }

  pthread_mutex_unlock(&mutex_planificacion);
}