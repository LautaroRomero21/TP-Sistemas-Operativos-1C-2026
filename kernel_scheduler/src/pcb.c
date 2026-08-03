#include "pcb.h"
#include "../../utils/src/utils/protocolos.h"
#include <stdlib.h>
#include <string.h>

t_pcb *pcb_crear(int pid, int prioridad, const char *path) {
  t_pcb *pcb = calloc(1, sizeof(t_pcb));
  pcb->pid = pid;
  pcb->prioridad = prioridad;
  pcb->estado = ESTADO_NEW;
  pcb->tabla_segmentos = list_create();
  pcb->path_instrucciones = path ? strdup(path) : NULL;
  pcb->socket_consola = -1;
  pcb->quantum_restante = 0;
  pcb->motivo_bloqueo = 0;
  pcb->recurso_bloqueo = NULL;
  pcb->segmento_io = -1;
  pcb->tamanio_io = 0;
  pcb->tiempo_sleep = 0;
  pcb->thread_quantum_active = 0;
  pcb->thread_suspension_active = 0;
  pcb->interrupcion_enviada = 0;
  pcb->pending_stdin = NULL;
  pcb_inicializar_registros(pcb);
  return pcb;
}

void pcb_destruir(t_pcb *pcb) {
  if (!pcb)
    return;
  free(pcb->path_instrucciones);
  free(pcb->recurso_bloqueo);
  if (pcb->tabla_segmentos) {
    for (int i = 0; i < list_size(pcb->tabla_segmentos); i++) {
      free(list_get(pcb->tabla_segmentos, i));
    }
    list_destroy(pcb->tabla_segmentos);
  }
  free(pcb->pending_stdin);
  free(pcb);
}

void pcb_inicializar_registros(t_pcb *pcb) {
  memset(&pcb->registros, 0, sizeof(t_registros_cpu));
}

void registros_serializar(t_registros_cpu *regs, t_paquete *paquete) {
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

void registros_deserializar(t_registros_cpu *regs, t_buffer *buffer) {
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

void pcb_serializar(t_pcb *pcb, t_paquete *paquete) {
  agregar_a_paquete(paquete, &pcb->pid, sizeof(int));
  agregar_a_paquete(paquete, &pcb->prioridad, sizeof(int));
  int estado = (int)pcb->estado;
  agregar_a_paquete(paquete, &estado, sizeof(int));
  registros_serializar(&pcb->registros, paquete);
  agregar_string_a_paquete(paquete, pcb->path_instrucciones);

  // Serializar tabla de segmentos
  int cant_segmentos =
      pcb->tabla_segmentos ? list_size(pcb->tabla_segmentos) : 0;
  agregar_a_paquete(paquete, &cant_segmentos, sizeof(int));
  for (int i = 0; i < cant_segmentos; i++) {
    t_entrada_segmento *seg = list_get(pcb->tabla_segmentos, i);
    agregar_a_paquete(paquete, &seg->id_segmento, sizeof(int));
    agregar_a_paquete(paquete, &seg->base, sizeof(uint32_t));
    agregar_a_paquete(paquete, &seg->tamanio, sizeof(uint32_t));
  }
}

t_pcb *pcb_deserializar(t_buffer *buffer) {
  t_pcb *pcb = calloc(1, sizeof(t_pcb));
  buffer_read(&pcb->pid, buffer, sizeof(int));
  buffer_read(&pcb->prioridad, buffer, sizeof(int));
  int estado = 0;
  buffer_read(&estado, buffer, sizeof(int));
  pcb->estado = (t_estado_proceso)estado;
  registros_deserializar(&pcb->registros, buffer);
  pcb->path_instrucciones = buffer_read_string(buffer);

  // Deserializar tabla de segmentos
  int cant_segmentos = 0;
  buffer_read(&cant_segmentos, buffer, sizeof(int));
  pcb->tabla_segmentos = list_create();
  for (int i = 0; i < cant_segmentos; i++) {
    t_entrada_segmento *seg = malloc(sizeof(t_entrada_segmento));
    buffer_read(&seg->id_segmento, buffer, sizeof(int));
    buffer_read(&seg->base, buffer, sizeof(uint32_t));
    buffer_read(&seg->tamanio, buffer, sizeof(uint32_t));
    list_add(pcb->tabla_segmentos, seg);
  }

  pcb->recurso_bloqueo = NULL;
  pcb->socket_consola = -1;
  return pcb;
}

void pcb_copiar_tabla_segmentos(t_pcb *dest, t_list *tabla_origen) {
  if (dest->tabla_segmentos) {
    for (int i = 0; i < list_size(dest->tabla_segmentos); i++) {
      free(list_get(dest->tabla_segmentos, i));
    }
    list_destroy(dest->tabla_segmentos);
  }
  dest->tabla_segmentos = list_create();
  if (!tabla_origen)
    return;

  for (int i = 0; i < list_size(tabla_origen); i++) {
    t_entrada_segmento *orig = list_get(tabla_origen, i);
    t_entrada_segmento *copia = malloc(sizeof(t_entrada_segmento));
    memcpy(copia, orig, sizeof(t_entrada_segmento));
    list_add(dest->tabla_segmentos, copia);
  }
}
