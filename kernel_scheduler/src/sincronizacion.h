#ifndef SINCRONIZACION_H_
#define SINCRONIZACION_H_

#include "pcb.h"
#include <commons/collections/list.h>
#include <commons/log.h>
#include <pthread.h>

extern t_log *logger;
extern pthread_mutex_t mutex_sincronizacion;

typedef struct {
  char *nombre;
  int bloqueado;          // 1 si esta tomado
  int pid_owner;          // PID del proceso que lo tiene
  int prioridad_original; // Prioridad original del owner (para herencia)
  t_list *cola_espera;    // Lista de t_pcb* esperando
} t_mutex;

// Inicializar el sistema de mutexes
void sincronizacion_inicializar(void);

// Crear un nuevo mutex
int mutex_crear(const char *nombre);

// Intentar tomar un mutex. Retorna 0 si lo tomo, -1 si debe bloquearse
int mutex_lock(const char *nombre, t_pcb *pcb);

// Liberar un mutex. Retorna el PCB que estaba esperando (o NULL)
t_pcb *mutex_unlock(const char *nombre, t_pcb *pcb);

// Destruir un mutex
void mutex_destruir(const char *nombre);

// Obtener la lista de mutexes
t_list *obtener_lista_mutexes(void);

// Liberar todos los mutexes de un proceso (cuando termina)
void mutex_liberar_todos_de_proceso(int pid);

#endif