#ifndef CONTEXTO_H_
#define CONTEXTO_H_

#include <commons/log.h>
#include <stdint.h>

// Registros del CPU almacenados en Kernel Memory
typedef struct {
  int pid;
  uint32_t pc;
  uint8_t ax, bx, cx, dx;
  uint32_t eax, ebx, ecx, edx;
  uint32_t si, di;
} t_contexto_km;

// Inicializar almacenamiento de contextos
void contexto_inicializar(void);

// Guardar contexto de un proceso (CPU_SET_CONTEXTO)
void contexto_guardar(int pid, uint32_t pc, uint8_t ax, uint8_t bx, uint8_t cx,
                      uint8_t dx, uint32_t eax, uint32_t ebx, uint32_t ecx,
                      uint32_t edx, uint32_t si, uint32_t di);

// Obtener contexto de un proceso (CPU_GET_CONTEXTO)
t_contexto_km *contexto_obtener(int pid);

// Liberar contexto de un proceso (al finalizar)
void contexto_liberar(int pid);

#endif
