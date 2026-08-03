#ifndef PLANIFICADOR_H_
#define PLANIFICADOR_H_

#include "pcb.h"
#include <commons/collections/list.h>
#include <commons/log.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

typedef enum {
  ALG_FIFO,
  ALG_RR,
  ALG_CMN // Colas Multinivel
} t_algoritmo_planificacion;

// Variables globales del planificador
extern t_log *logger;
extern t_algoritmo_planificacion algoritmo;
extern int quantum_config;
extern int grado_multiprogramacion;
extern int suspension_timeout_ms;
extern int cant_colas_multinivel;
extern char **algoritmo_por_cola; // array de strings "FIFO" o "RR" por cola
extern int desalojo_entre_colas;  // 1 si hay preemption entre colas

extern t_list *cola_new;
extern t_list *cola_ready;        // Para FIFO y RR
extern t_list **colas_multinivel; // Array de colas para CMN
extern t_list *cola_exec;
extern t_list *cola_block;
extern t_list *cola_susp_ready;
extern t_list *cola_susp_block;
extern t_list *cola_exit;

extern pthread_mutex_t mutex_planificacion;
extern sem_t sem_multiprogramacion;
extern sem_t sem_proceso_ready;
extern sem_t sem_cpu_libre;

extern int socket_cpu_dispatch;
extern int socket_cpu_interrupt;
extern int socket_kernel_memory;
extern pthread_mutex_t mutex_socket_km;
extern int hay_proceso_ejecutando;

// Lista de CPUs conectadas (multi-CPU support)
extern t_list *lista_cpus;
extern pthread_mutex_t mutex_cpus;

// Variables de sincronizacion para compactacion
extern bool compactacion_en_curso;
extern pthread_mutex_t mutex_compactacion;
extern pthread_cond_t cv_compactacion_fin;
extern pthread_cond_t cv_cpus_idle;
extern int cpus_pendientes_desalojo;

// Funciones del planificador
void planificador_inicializar(void);
void planificador_agregar_nuevo(t_pcb *pcb);
void planificador_pasar_a_ready(t_pcb *pcb);
void planificador_pasar_a_exec(t_pcb *pcb);
void planificador_pasar_a_block(t_pcb *pcb);
void planificador_pasar_a_exit(t_pcb *pcb, const char *motivo);
void planificador_pasar_a_susp_block(t_pcb *pcb);
void planificador_pasar_a_susp_ready(t_pcb *pcb);
void planificador_iniciar_quantum(t_pcb *pcb);

t_pcb *planificador_obtener_siguiente_ready(void);

// Hilos del planificador
void *hilo_planificador_largo_plazo(void *arg);
void *hilo_planificador_corto_plazo(void *arg);

// Enviar PCB al CPU
void enviar_pcb_a_cpu(t_pcb *pcb);

// Solicitar interrupcion al CPU
void enviar_interrupcion_cpu(int pid, int motivo);

// Inicializar proceso en Kernel Memory
int solicitar_init_proceso(t_pcb *pcb);

// Finalizar proceso en Kernel Memory
void solicitar_fin_proceso(int pid);

// Suspender proceso en Kernel Memory (mover a SWAP)
int solicitar_suspender_proceso(int pid);

// Reanudar proceso en Kernel Memory (traer de SWAP)
int solicitar_reanudar_proceso(int pid);

// Solicitar asignacion de segmento
int solicitar_mem_alloc(int pid, int id_segmento, int tamanio);

// Solicitar liberacion de segmento
void solicitar_mem_free(int pid, int id_segmento);

// Solicitar compactacion (desaloja CPUs primero)
void solicitar_compactacion(void);

// Actualizar tabla de segmentos
void actualizar_tabla_segmentos(t_pcb *pcb);

// Evaluar si hay un proceso de mayor prioridad en READY que el actual en EXEC
void evaluar_desalojo_por_prioridad(void);

// Intentar des-suspender procesos de SUSP_READY a READY si hay espacio en
// memoria
void planificador_des_suspender_procesos(void);

#endif