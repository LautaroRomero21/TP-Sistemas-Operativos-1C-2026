#ifndef PCB_H_
#define PCB_H_

#include "../../utils/src/utils/protocolos.h"
#include <commons/collections/list.h>
#include <stdint.h>
#include <time.h>

// Estados del proceso
typedef enum {
  ESTADO_NEW,
  ESTADO_READY,
  ESTADO_EXEC,
  ESTADO_BLOCK,
  ESTADO_SUSP_READY,
  ESTADO_SUSP_BLOCK,
  ESTADO_EXIT
} t_estado_proceso;

// Registros del CPU
typedef struct {
  uint32_t pc;  // Program Counter (4 bytes)
  uint8_t ax;   // Registro AX (1 byte)
  uint8_t bx;   // Registro BX (1 byte)
  uint8_t cx;   // Registro CX (1 byte)
  uint8_t dx;   // Registro DX (1 byte)
  uint32_t eax; // Registro EAX (4 bytes)
  uint32_t ebx; // Registro EBX (4 bytes)
  uint32_t ecx; // Registro ECX (4 bytes)
  uint32_t edx; // Registro EDX (4 bytes)
  uint32_t si;  // Registro SI (4 bytes)
  uint32_t di;  // Registro DI (4 bytes)
} t_registros_cpu;

typedef struct {
  int id_segmento;
  uint32_t base;    // Direccion base en memoria fisica
  uint32_t tamanio; // Tamanio del segmento
} t_entrada_segmento;

// PCB - Bloque de control de proceso
typedef struct {
  int pid;
  int prioridad;
  t_estado_proceso estado;
  t_registros_cpu registros;
  t_list *tabla_segmentos;  // Lista de t_entrada_segmento*
  char *path_instrucciones; // Ruta del archivo de pseudocodigo
  int socket_consola;       // Para responder a la consola
  // Campos para planificacion
  int quantum_restante;
  int motivo_bloqueo;    // Motivo por el que se bloqueo
  char *recurso_bloqueo; // Nombre del recurso (mutex, IO, etc.)
  int segmento_io;       // Segmento para IO
  int tamanio_io;        // Tamanio para IO
  char *pending_stdin;
  int tiempo_sleep; // Tiempo de sleep en ms
  // Timestamp para suspension por timeout
  struct timespec block_entry_time; // Cuando entro a BLOCK
  struct timespec exec_entry_time;  // Cuando entro a EXEC
  pthread_t thread_quantum;
  int thread_quantum_active;
  pthread_t thread_suspension;
  int thread_suspension_active;
  int interrupcion_enviada;
} t_pcb;

// Crear un nuevo PCB
t_pcb *pcb_crear(int pid, int prioridad, const char *path);

// Liberar un PCB
void pcb_destruir(t_pcb *pcb);

// Inicializar registros en 0
void pcb_inicializar_registros(t_pcb *pcb);

// Serializar PCB para enviar por socket
void pcb_serializar(t_pcb *pcb, t_paquete *paquete);

// Deserializar PCB recibido por socket
t_pcb *pcb_deserializar(t_buffer *buffer);

// Serializar solo registros
void registros_serializar(t_registros_cpu *regs, t_paquete *paquete);

// Deserializar solo registros
void registros_deserializar(t_registros_cpu *regs, t_buffer *buffer);

// Copiar tabla de segmentos
void pcb_copiar_tabla_segmentos(t_pcb *dest, t_list *tabla_origen);

#endif
