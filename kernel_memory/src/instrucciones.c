#include "instrucciones.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern t_log *logger_km;

static t_list *lista_programas = NULL;

void instrucciones_inicializar(void) { lista_programas = list_create(); }

static t_programa *buscar_programa(int pid) {
  for (int i = 0; i < list_size(lista_programas); i++) {
    t_programa *prog = list_get(lista_programas, i);
    if (prog->pid == pid)
      return prog;
  }
  return NULL;
}

int instrucciones_cargar(int pid, const char *path_base,
                         const char *nombre_archivo) {
  // Construir path completo
  char path_completo[512];
  if (path_base && strlen(path_base) > 0)
    snprintf(path_completo, sizeof(path_completo), "%s/%s", path_base,
             nombre_archivo);
  else
    snprintf(path_completo, sizeof(path_completo), "%s", nombre_archivo);

  FILE *archivo = fopen(path_completo, "r");
  if (!archivo) {
    // Intentar solo con el nombre del archivo
    archivo = fopen(nombre_archivo, "r");
    if (!archivo) {
      log_error(logger_km, "No se pudo abrir archivo de instrucciones: %s",
                path_completo);
      return -1;
    }
  }

  t_programa *prog = malloc(sizeof(t_programa));
  prog->pid = pid;
  prog->lineas = list_create();

  char linea[256];
  while (fgets(linea, sizeof(linea), archivo)) {
    // Remover newline
    size_t len = strlen(linea);
    while (len > 0 && (linea[len - 1] == '\n' || linea[len - 1] == '\r')) {
      linea[len - 1] = '\0';
      len--;
    }
    if (len > 0) {
      list_add(prog->lineas, strdup(linea));
    }
  }

  fclose(archivo);
  list_add(lista_programas, prog);

  log_info(logger_km, "PID %d: %d instrucciones cargadas desde '%s'", pid,
           list_size(prog->lineas), path_completo);

  return 0;
}

char *instrucciones_obtener(int pid, int pc) {
  t_programa *prog = buscar_programa(pid);
  if (!prog)
    return NULL;

  if (pc < 0 || pc >= list_size(prog->lineas))
    return NULL;

  return strdup(list_get(prog->lineas, pc));
}

int instrucciones_cantidad(int pid) {
  t_programa *prog = buscar_programa(pid);
  if (!prog)
    return 0;
  return list_size(prog->lineas);
}

void instrucciones_liberar(int pid) {
  for (int i = 0; i < list_size(lista_programas); i++) {
    t_programa *prog = list_get(lista_programas, i);
    if (prog->pid == pid) {
      for (int j = 0; j < list_size(prog->lineas); j++) {
        free(list_get(prog->lineas, j));
      }
      list_destroy(prog->lineas);
      free(prog);
      list_remove(lista_programas, i);
      return;
    }
  }
}
