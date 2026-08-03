#include "contexto.h"
#include <commons/collections/list.h>
#include <stdlib.h>
#include <string.h>

extern t_log *logger_km;

static t_list *lista_contextos = NULL;

void contexto_inicializar(void) { lista_contextos = list_create(); }

static t_contexto_km *buscar_contexto(int pid) {
  for (int i = 0; i < list_size(lista_contextos); i++) {
    t_contexto_km *ctx = list_get(lista_contextos, i);
    if (ctx->pid == pid)
      return ctx;
  }
  return NULL;
}

void contexto_guardar(int pid, uint32_t pc, uint8_t ax, uint8_t bx, uint8_t cx,
                      uint8_t dx, uint32_t eax, uint32_t ebx, uint32_t ecx,
                      uint32_t edx, uint32_t si, uint32_t di) {
  t_contexto_km *ctx = buscar_contexto(pid);
  if (!ctx) {
    ctx = malloc(sizeof(t_contexto_km));
    ctx->pid = pid;
    list_add(lista_contextos, ctx);
  }
  ctx->pc = pc;
  ctx->ax = ax;
  ctx->bx = bx;
  ctx->cx = cx;
  ctx->dx = dx;
  ctx->eax = eax;
  ctx->ebx = ebx;
  ctx->ecx = ecx;
  ctx->edx = edx;
  ctx->si = si;
  ctx->di = di;

  log_info(logger_km, "## Contexto guardado: PID=%d PC=%u", pid, pc);
}

t_contexto_km *contexto_obtener(int pid) { return buscar_contexto(pid); }

void contexto_liberar(int pid) {
  for (int i = 0; i < list_size(lista_contextos); i++) {
    t_contexto_km *ctx = list_get(lista_contextos, i);
    if (ctx->pid == pid) {
      list_remove(lista_contextos, i);
      free(ctx);
      return;
    }
  }
}
