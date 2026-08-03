#ifndef PROTOCOLOS_H_
#define PROTOCOLOS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
  // ============== Handshakes ==============
  HANDSHAKE_CPU = 100,
  HANDSHAKE_IO = 101,
  HANDSHAKE_KS = 102, // Kernel Scheduler
  HANDSHAKE_KM = 103, // Kernel Memory
  HANDSHAKE_MS = 104, // Memory Stick
  HANDSHAKE_SWAP = 105,
  HANDSHAKE_KS_NOTIF = 106, // Notificaciones de KM a KS
  HANDSHAKE_OK = 199,

  // ============== Kernel Scheduler <-> CPU ==============
  KS_DISPATCH = 200,  // KS envia PCB al CPU para ejecutar
  KS_INTERRUPT = 201, // KS envia interrupcion al CPU
  CPU_DESALOJO = 210, // CPU devuelve PCB con motivo de desalojo

  // ============== Kernel Scheduler <-> IO ==============
  IO_IDENTIFICACION = 300, // IO se identifica (nombre + tipo)
  KS_IO_SOLICITUD = 301,   // KS envia solicitud IO al dispositivo
  IO_FINALIZADA = 302,     // IO avisa que termino

  // ============== Kernel Scheduler <-> Kernel Memory ==============
  KS_INIT_PROCESO = 400,      // Crear estructuras para proceso
  KS_FIN_PROCESO = 401,       // Liberar estructuras del proceso
  KS_SUSPENDER_PROCESO = 402, // Suspender proceso (mover a SWAP)
  KS_REANUDAR_PROCESO = 403,  // Reanudar proceso (traer de SWAP)
  KS_MEM_ALLOC = 404,         // Asignar segmento
  KS_MEM_FREE = 405,          // Liberar segmento
  KS_STDIN_ESCRIBIR = 406,    // Escribir datos de STDIN en memoria
  KS_STDOUT_LEER = 407,       // Leer datos de memoria para STDOUT
  KS_GET_TABLA = 408,         // Solicita tabla de segmentos al Kernel Memory
  KS_COMPACTAR = 409,         // Solicita compactacion de memoria
  KS_VERIFICAR_ESPACIO = 410, // Verifica si hay espacio suficiente
  KM_RESPUESTA_OK = 450,
  KM_RESPUESTA_ERROR = 451,
  KM_RESPUESTA_DATOS = 452,
  KM_RESPUESTA_COMPACTAR = 453, // Necesita compactacion
  KM_RESPUESTA_TABLA = 454,     // Devuelve tabla de segmentos
  KM_BSOD = 455,                // Kernel Memory detecto corrupcion fatal
  KM_NOTIFICAR_NUEVA_MEMORIA =
      456, // KM notifica que hay mas memoria disponible

  // ============== CPU <-> Kernel Memory ==============
  CPU_GET_INSTRUCCION = 500, // CPU pide instruccion (PID, PC)
  CPU_GET_CONTEXTO = 501,    // CPU pide contexto de ejecucion
  CPU_SET_CONTEXTO = 502,    // CPU guarda contexto
  CPU_GET_MS_LIST = 503,     // CPU pide lista de Memory Sticks
  KM_INSTRUCCION = 550,      // KM devuelve instruccion
  KM_CONTEXTO = 551,         // KM devuelve contexto
  KM_MS_LIST = 552,          // KM devuelve lista de Memory Sticks

  // ============== CPU / Kernel Memory <-> Memory Stick ==============
  MS_LEER = 600,           // Leer bytes de memoria fisica
  MS_ESCRIBIR = 601,       // Escribir bytes en memoria fisica
  MS_RESPUESTA_LEER = 650, // Respuesta con datos leidos
  MS_RESPUESTA_OK = 651,
  MS_INFO_CONFIG = 660, // MS informa tamanio y puerto de escucha

  // ============== Kernel Memory <-> SWAP ==============
  SWAP_ESCRIBIR_BLOQUE = 700, // Escribir bloque en SWAP
  SWAP_LEER_BLOQUE = 701,     // Leer bloque de SWAP
  SWAP_LIBERAR = 702,         // Liberar bloques de un PID
  SWAP_RESERVAR = 703,        // Reservar espacio para un PID
  SWAP_RESPUESTA_OK = 750,
  SWAP_RESPUESTA_DATOS = 751,
  SWAP_RESPUESTA_ERROR = 752,
  SWAP_INFO = 760 // SWAP informa block_size y swap_size al KM
} op_code;

typedef enum { RESPUESTA_OK, RESPUESTA_ERROR } t_respuesta;

typedef enum {
  MOTIVO_EXIT = 0,
  MOTIVO_IO = 1,
  MOTIVO_SYSCALL = 2,
  MOTIVO_INTERRUPCION = 3,
  MOTIVO_ERROR = 4,
  MOTIVO_SEG_FAULT = 5
} t_cpu_motivo;

typedef struct {
  int size;
  int offset;
  void *stream;
} t_buffer;

typedef struct {
  op_code codigo_operacion;
  t_buffer *buffer;
} t_paquete;

ssize_t enviar_todo(int socket_destino, const void *buffer, size_t size);
ssize_t recibir_todo(int socket, void *buffer, size_t size);

int enviar_operacion(int socket_destino, op_code codigo_operacion);
int recibir_operacion(int socket);

t_paquete *crear_paquete(op_code codigo_operacion);
void agregar_a_paquete(t_paquete *paquete, void *valor, int tamanio);
void agregar_string_a_paquete(t_paquete *paquete, const char *valor);
int enviar_paquete(t_paquete *paquete, int socket_destino);
void eliminar_paquete(t_paquete *paquete);

t_paquete *recibir_paquete(int socket);
void buffer_read(void *dest, t_buffer *buffer, int size);
char *buffer_read_string(t_buffer *buffer);

#endif