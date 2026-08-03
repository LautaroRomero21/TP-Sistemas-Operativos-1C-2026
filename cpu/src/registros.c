#include "registros.h"
#include <string.h>
#include <strings.h>

void registros_inicializar(t_registros *regs) {
  memset(regs, 0, sizeof(t_registros));
}

void registros_empaquetar(t_registros *regs, t_paquete *paquete) {
  agregar_a_paquete(paquete, &regs->pc, sizeof(uint32_t));
  agregar_a_paquete(paquete, &regs->ax, sizeof(uint8_t));
  agregar_a_paquete(paquete, &regs->bx, sizeof(uint8_t));
  agregar_a_paquete(paquete, &regs->cx, sizeof(uint8_t));
  agregar_a_paquete(paquete, &regs->dx, sizeof(uint8_t));
  agregar_a_paquete(paquete, &regs->eax, sizeof(uint32_t));
  agregar_a_paquete(paquete, &regs->ebx, sizeof(uint32_t));
  agregar_a_paquete(paquete, &regs->ecx, sizeof(uint32_t));
  agregar_a_paquete(paquete, &regs->edx, sizeof(uint32_t));
  agregar_a_paquete(paquete, &regs->si, sizeof(uint32_t));
  agregar_a_paquete(paquete, &regs->di, sizeof(uint32_t));
}

void registros_desempaquetar(t_registros *regs, t_buffer *buffer) {
  buffer_read(&regs->pc, buffer, sizeof(uint32_t));
  buffer_read(&regs->ax, buffer, sizeof(uint8_t));
  buffer_read(&regs->bx, buffer, sizeof(uint8_t));
  buffer_read(&regs->cx, buffer, sizeof(uint8_t));
  buffer_read(&regs->dx, buffer, sizeof(uint8_t));
  buffer_read(&regs->eax, buffer, sizeof(uint32_t));
  buffer_read(&regs->ebx, buffer, sizeof(uint32_t));
  buffer_read(&regs->ecx, buffer, sizeof(uint32_t));
  buffer_read(&regs->edx, buffer, sizeof(uint32_t));
  buffer_read(&regs->si, buffer, sizeof(uint32_t));
  buffer_read(&regs->di, buffer, sizeof(uint32_t));
}

void *registro_obtener_por_nombre(t_registros *regs, const char *nombre,
                                  int *tipo) {
  if (strcasecmp(nombre, "PC") == 0) {
    *tipo = 1;
    return &regs->pc;
  }
  if (strcasecmp(nombre, "AX") == 0) {
    *tipo = 0;
    return &regs->ax;
  }
  if (strcasecmp(nombre, "BX") == 0) {
    *tipo = 0;
    return &regs->bx;
  }
  if (strcasecmp(nombre, "CX") == 0) {
    *tipo = 0;
    return &regs->cx;
  }
  if (strcasecmp(nombre, "DX") == 0) {
    *tipo = 0;
    return &regs->dx;
  }
  if (strcasecmp(nombre, "EAX") == 0) {
    *tipo = 1;
    return &regs->eax;
  }
  if (strcasecmp(nombre, "EBX") == 0) {
    *tipo = 1;
    return &regs->ebx;
  }
  if (strcasecmp(nombre, "ECX") == 0) {
    *tipo = 1;
    return &regs->ecx;
  }
  if (strcasecmp(nombre, "EDX") == 0) {
    *tipo = 1;
    return &regs->edx;
  }
  if (strcasecmp(nombre, "SI") == 0) {
    *tipo = 1;
    return &regs->si;
  }
  if (strcasecmp(nombre, "DI") == 0) {
    *tipo = 1;
    return &regs->di;
  }
  *tipo = -1;
  return NULL;
}

int registro_tamanio_por_nombre(const char *nombre) {
  if (strcasecmp(nombre, "AX") == 0 || strcasecmp(nombre, "BX") == 0 ||
      strcasecmp(nombre, "CX") == 0 || strcasecmp(nombre, "DX") == 0)
    return 1;
  return 4; // PC, EAX, EBX, ECX, EDX, SI, DI
}
