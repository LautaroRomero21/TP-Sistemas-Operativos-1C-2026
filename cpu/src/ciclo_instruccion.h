#ifndef CICLO_INSTRUCCION_H_
#define CICLO_INSTRUCCION_H_

#include "mmu.h"
#include "registros.h"
#include <commons/collections/list.h>
#include <commons/log.h>

// Resultado de la ejecucion del ciclo
typedef enum {
  CICLO_CONTINUAR,        // Seguir ejecutando
  CICLO_DESALOJO_EXIT,    // Proceso termino (EXIT)
  CICLO_DESALOJO_IO,      // Proceso solicita IO
  CICLO_DESALOJO_SYSCALL, // Proceso solicita syscall
  CICLO_DESALOJO_INT,     // Desalojo por interrupcion
  CICLO_SEG_FAULT,        // Segmentation Fault
  CICLO_ERROR             // Error generico
} t_resultado_ciclo;

// Contexto de ejecucion del CPU
typedef struct {
  int pid;
  int prioridad;
  t_registros registros;
  t_list *tabla_segmentos; // Lista de t_segmento_mmu*
  char *path_instrucciones;
  int interrumpido; // Flag de interrupcion pendiente
  int pc_modificado; // Flag si el PC fue modificado manualmente (por JNZ, SET PC)
  int motivo_interrupcion;

  // Datos de desalojo
  t_resultado_ciclo resultado;
  char *syscall_nombre;    // Nombre de la syscall
  int syscall_param1;      // Parametro 1
  int syscall_param2;      // Parametro 2
  char *syscall_str_param; // Parametro string
} t_contexto_cpu;

// Sockets del CPU
extern int socket_dispatch;  // Conexion con KS para dispatch
extern int socket_interrupt; // Conexion con KS para interrupciones
extern int socket_memoria;   // Conexion con Kernel Memory
extern int socket_mem_stick; // Conexion con Memory Stick
extern t_log *logger_cpu;

// Ejecutar ciclo de instruccion completo para un proceso
t_resultado_ciclo ejecutar_ciclo_instruccion(t_contexto_cpu *contexto);

// Fetch: obtener instruccion de Kernel Memory
char *fetch_instruccion(int pid, int pc);

// Decode: parsear la instruccion
// Retorna el nombre de la instruccion y sus parametros
void decode_instruccion(const char *instruccion, char *nombre, char *param1,
                        char *param2, char *param3);

// Execute: ejecutar la instruccion
t_resultado_ciclo execute_instruccion(t_contexto_cpu *contexto,
                                      const char *nombre, const char *param1,
                                      const char *param2, const char *param3);

// Check Interrupt: verificar si hay interrupciones pendientes
int check_interrupt(t_contexto_cpu *contexto);

// Leer de Memory Stick (acceso fisico directo)
int leer_memoria_fisica(int pid, uint32_t dir_fisica, void *destino, int tamanio);

int escribir_memoria_fisica(int pid, uint32_t dir_fisica, void *origen, int tamanio);

int obtener_socket_ms(const char *ip, const char *puerto);

#endif
