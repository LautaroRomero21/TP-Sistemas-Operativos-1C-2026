#include "sincronizacion.h"
#include "planificador.h"
#include <stdlib.h>
#include <string.h>

static t_list *lista_mutexes = NULL;
pthread_mutex_t mutex_sincronizacion = PTHREAD_MUTEX_INITIALIZER;

void sincronizacion_inicializar(void) { lista_mutexes = list_create(); }

static t_mutex *buscar_mutex(const char *nombre) {
  for (int i = 0; i < list_size(lista_mutexes); i++) {
    t_mutex *m = list_get(lista_mutexes, i);
    if (strcmp(m->nombre, nombre) == 0)
      return m;
  }
  return NULL;
}

int mutex_crear(const char *nombre) {
  pthread_mutex_lock(&mutex_sincronizacion);
  if (buscar_mutex(nombre) != NULL) {
    log_warning(logger, "Mutex '%s' ya existe", nombre);
    pthread_mutex_unlock(&mutex_sincronizacion);
    return -1;
  }

  t_mutex *m = malloc(sizeof(t_mutex));
  m->nombre = strdup(nombre);
  m->bloqueado = 0;
  m->pid_owner = -1;
  m->prioridad_original = -1;
  m->cola_espera = list_create();
  list_add(lista_mutexes, m);

  log_info(logger, "Mutex '%s' creado", nombre);
  pthread_mutex_unlock(&mutex_sincronizacion);
  return 0;
}

int mutex_lock(const char *nombre, t_pcb *pcb) {
  pthread_mutex_lock(&mutex_sincronizacion);
  t_mutex *m = buscar_mutex(nombre);
  if (!m) {
    log_error(logger, "Mutex '%s' no existe (PID %d intento lock)", nombre,
              pcb->pid);
    pthread_mutex_unlock(&mutex_sincronizacion);
    return -2; // Error: no existe
  }

  if (!m->bloqueado) {
    // Mutex libre, tomarlo
    m->bloqueado = 1;
    m->pid_owner = pcb->pid;
    m->prioridad_original = pcb->prioridad;
    log_info(logger, "## (%d) Toma el Mutex %s", pcb->pid, nombre);
    pthread_mutex_unlock(&mutex_sincronizacion);
    return 0;
  }

  // Mutex ocupado, el proceso debe bloquearse
  list_add(m->cola_espera, pcb);
  log_info(logger, "PID %d: bloqueado esperando mutex '%s'", pcb->pid, nombre);

  // Herencia de prioridad real: si el proceso que espera tiene mayor prioridad
  // (numero menor), el owner HEREDA temporalmente esa prioridad
  if (pcb->prioridad < m->prioridad_original) {
    extern t_list *cola_ready, *cola_exec, *cola_block;
    extern t_list **colas_multinivel;
    extern int cant_colas_multinivel;
    extern pthread_mutex_t mutex_planificacion;

    pthread_mutex_lock(&mutex_planificacion);
    t_list *colas[] = {cola_exec, cola_ready, cola_block};
    int encontrado = 0;
    for (int c = 0; c < 3 && !encontrado; c++) {
      for (int i = 0; i < list_size(colas[c]); i++) {
        t_pcb *owner = list_get(colas[c], i);
        if (owner->pid == m->pid_owner) {
          log_info(logger, "## PID %d: Cambio de prioridad: %d - %d",
                   owner->pid, owner->prioridad, pcb->prioridad);
          owner->prioridad = pcb->prioridad;
          encontrado = 1;
          break;
        }
      }
    }
    if (!encontrado && colas_multinivel) {
      for (int p = 0; p < cant_colas_multinivel && !encontrado; p++) {
        for (int i = 0; i < list_size(colas_multinivel[p]); i++) {
          t_pcb *owner = list_get(colas_multinivel[p], i);
          if (owner->pid == m->pid_owner) {
            log_info(logger, "## PID %d: Cambio de prioridad: %d - %d",
                     owner->pid, owner->prioridad, pcb->prioridad);
            owner->prioridad = pcb->prioridad;
            encontrado = 1;
            break;
          }
        }
      }
    }
    pthread_mutex_unlock(&mutex_planificacion);
  }

  pthread_mutex_unlock(&mutex_sincronizacion);
  return -1; // Debe bloquearse
}

t_pcb *mutex_unlock(const char *nombre, t_pcb *pcb) {
  pthread_mutex_lock(&mutex_sincronizacion);
  t_mutex *m = buscar_mutex(nombre);
  if (!m) {
    log_error(logger, "Mutex '%s' no existe (PID %d intento unlock)", nombre,
              pcb->pid);
    pthread_mutex_unlock(&mutex_sincronizacion);
    return NULL;
  }

  if (m->pid_owner != pcb->pid) {
    log_warning(
        logger,
        "PID %d intento unlock de mutex '%s' pero no es el owner (owner=%d)",
        pcb->pid, nombre, m->pid_owner);
    pthread_mutex_unlock(&mutex_sincronizacion);
    return NULL;
  }

  // Restaurar prioridad original del owner
  if (pcb->prioridad != m->prioridad_original) {
    log_info(logger, "## PID %d: Cambio de prioridad: %d - %d", pcb->pid,
             pcb->prioridad, m->prioridad_original);
    pcb->prioridad = m->prioridad_original;
  }

  // Si hay alguien esperando, darle el mutex
  if (!list_is_empty(m->cola_espera)) {
    t_pcb *siguiente = list_remove(m->cola_espera, 0);
    m->pid_owner = siguiente->pid;
    m->prioridad_original = siguiente->prioridad;
    log_info(logger, "Mutex '%s' transferido de PID %d a PID %d", nombre,
             pcb->pid, siguiente->pid);
    pthread_mutex_unlock(&mutex_sincronizacion);
    return siguiente;
  }

  // Nadie esperando, liberar el mutex
  m->bloqueado = 0;
  m->pid_owner = -1;
  m->prioridad_original = -1;
  log_info(logger, "## (%d) Libera el Mutex %s", pcb->pid, nombre);
  pthread_mutex_unlock(&mutex_sincronizacion);
  return NULL;
}

void mutex_destruir(const char *nombre) {
  pthread_mutex_lock(&mutex_sincronizacion);
  for (int i = 0; i < list_size(lista_mutexes); i++) {
    t_mutex *m = list_get(lista_mutexes, i);
    if (strcmp(m->nombre, nombre) == 0) {
      list_destroy(m->cola_espera);
      free(m->nombre);
      free(m);
      list_remove(lista_mutexes, i);
      pthread_mutex_unlock(&mutex_sincronizacion);
      return;
    }
  }
  pthread_mutex_unlock(&mutex_sincronizacion);
}

void mutex_liberar_todos_de_proceso(int pid) {
  pthread_mutex_lock(&mutex_sincronizacion);
  for (int i = 0; i < list_size(lista_mutexes); i++) {
    t_mutex *m = list_get(lista_mutexes, i);
    if (m->pid_owner == pid) {
      m->bloqueado = 0;
      m->pid_owner = -1;
      m->prioridad_original = -1;
      // Desbloquear al siguiente en la cola si hay
      if (!list_is_empty(m->cola_espera)) {
        t_pcb *siguiente = list_remove(m->cola_espera, 0);
        m->bloqueado = 1;
        m->pid_owner = siguiente->pid;
        m->prioridad_original = siguiente->prioridad;

        extern t_list *cola_block;
        extern t_list *cola_susp_block;
        int estaba_suspendido = 0;
        int removido = 0;
        extern pthread_mutex_t mutex_planificacion;
        pthread_mutex_lock(&mutex_planificacion);
        for (int j = 0; j < list_size(cola_block); j++) {
          if (list_get(cola_block, j) == siguiente) {
            list_remove(cola_block, j);
            removido = 1;
            break;
          }
        }
        if (!removido) {
          for (int j = 0; j < list_size(cola_susp_block); j++) {
            if (list_get(cola_susp_block, j) == siguiente) {
              list_remove(cola_susp_block, j);
              estaba_suspendido = 1;
              break;
            }
          }
        }
        pthread_mutex_unlock(&mutex_planificacion);

        if (estaba_suspendido) {
          planificador_pasar_a_susp_ready(siguiente);
        } else {
          planificador_pasar_a_ready(siguiente);
        }
      }
    }
  }
  pthread_mutex_unlock(&mutex_sincronizacion);
}

t_list *obtener_lista_mutexes(void) { return lista_mutexes; }