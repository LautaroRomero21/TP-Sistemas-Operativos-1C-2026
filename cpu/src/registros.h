#ifndef REGISTROS_H_
#define REGISTROS_H_

#include "../../utils/src/utils/protocolos.h"
#include <stdint.h>

// Registros del CPU
typedef struct {
  uint32_t pc;
  uint8_t ax;
  uint8_t bx;
  uint8_t cx;
  uint8_t dx;
  uint32_t eax;
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
  uint32_t si;
  uint32_t di;
} t_registros;

// Inicializar registros en 0
void registros_inicializar(t_registros *regs);

// Serializar registros en un paquete
void registros_empaquetar(t_registros *regs, t_paquete *paquete);

// Deserializar registros desde un buffer
void registros_desempaquetar(t_registros *regs, t_buffer *buffer);

// Obtener puntero al registro por nombre (retorna NULL si no existe)
// tipo: 0 = uint8_t (1 byte), 1 = uint32_t (4 bytes)
void *registro_obtener_por_nombre(t_registros *regs, const char *nombre,
                                  int *tipo);

// Obtener el tamanio del registro en bytes por nombre
int registro_tamanio_por_nombre(const char *nombre);

#endif
