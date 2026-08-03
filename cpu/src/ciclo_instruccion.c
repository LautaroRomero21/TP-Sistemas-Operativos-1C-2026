#include "ciclo_instruccion.h"
#include "../../utils/src/utils/conexiones.h"
#include "../../utils/src/utils/protocolos.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int socket_dispatch = -1;
int socket_interrupt = -1;
int socket_memoria = -1;
t_log *logger_cpu = NULL;

typedef struct {
  char ip[32];
  char puerto[16];
  int socket;
} t_ms_conexion;

static t_list *lista_conexiones_ms = NULL;
static pthread_mutex_t mutex_conexiones_ms = PTHREAD_MUTEX_INITIALIZER;

int obtener_socket_ms(const char *ip, const char *puerto) {
  if (!lista_conexiones_ms) {
    pthread_mutex_lock(&mutex_conexiones_ms);
    if (!lista_conexiones_ms) {
      lista_conexiones_ms = list_create();
    }
    pthread_mutex_unlock(&mutex_conexiones_ms);
  }

  pthread_mutex_lock(&mutex_conexiones_ms);
  for (int i = 0; i < list_size(lista_conexiones_ms); i++) {
    t_ms_conexion *con = list_get(lista_conexiones_ms, i);
    if (strcmp(con->ip, ip) == 0 && strcmp(con->puerto, puerto) == 0) {
      pthread_mutex_unlock(&mutex_conexiones_ms);
      return con->socket;
    }
  }

  // No encontrado, conectar
  log_info(logger_cpu, "Conectando a Memory Stick dinamico en %s:%s", ip,
           puerto);
  int nuevo_sock = crear_conexion((char *)ip, (char *)puerto, logger_cpu);
  if (nuevo_sock >= 0) {
    // Handshake
    t_paquete *p = crear_paquete(HANDSHAKE_CPU);
    // Necesitamos cpu_id, pero no lo tenemos global aqui.
    // El handshake CPU envia el cpu_id. Lo enviaremos como 0 y MS no le
    // importa.
    int cpu_id = 0;
    agregar_a_paquete(p, &cpu_id, sizeof(int));
    enviar_paquete(p, nuevo_sock);
    eliminar_paquete(p);

    if (recibir_operacion(nuevo_sock) == HANDSHAKE_OK) {
      t_ms_conexion *con = malloc(sizeof(t_ms_conexion));
      strncpy(con->ip, ip, sizeof(con->ip) - 1);
      con->ip[sizeof(con->ip) - 1] = '\0';
      strncpy(con->puerto, puerto, sizeof(con->puerto) - 1);
      con->puerto[sizeof(con->puerto) - 1] = '\0';
      con->socket = nuevo_sock;
      list_add(lista_conexiones_ms, con);
      pthread_mutex_unlock(&mutex_conexiones_ms);
      return nuevo_sock;
    }
    close(nuevo_sock);
  }
  pthread_mutex_unlock(&mutex_conexiones_ms);
  return -1;
}

// ===== FETCH =====
char *fetch_instruccion(int pid, int pc) {
  t_paquete *paquete = crear_paquete(CPU_GET_INSTRUCCION);
  agregar_a_paquete(paquete, &pid, sizeof(int));
  agregar_a_paquete(paquete, &pc, sizeof(int));
  enviar_paquete(paquete, socket_memoria);
  eliminar_paquete(paquete);

  int op = recibir_operacion(socket_memoria);
  if (op != KM_INSTRUCCION) {
    return NULL;
  }

  t_paquete *resp = recibir_paquete(socket_memoria);
  if (!resp)
    return NULL;
  char *instruccion = buffer_read_string(resp->buffer);
  eliminar_paquete(resp);
  return instruccion;
}

// ===== DECODE =====
void decode_instruccion(const char *instruccion, char *nombre, char *param1,
                        char *param2, char *param3) {
  nombre[0] = '\0';
  param1[0] = '\0';
  param2[0] = '\0';
  param3[0] = '\0';
  if (!instruccion)
    return;
  sscanf(instruccion, "%63s %127s %127s %127s", nombre, param1, param2, param3);
}

// ===== Helpers de registros =====
static uint32_t leer_valor_registro(t_registros *regs, const char *nombre) {
  int tipo = 0;
  void *ptr = registro_obtener_por_nombre(regs, nombre, &tipo);
  if (!ptr)
    return 0;
  if (tipo == 0)
    return (uint32_t)(*(uint8_t *)ptr);
  return *(uint32_t *)ptr;
}

static void escribir_valor_registro(t_registros *regs, const char *nombre,
                                    uint32_t valor) {
  int tipo = 0;
  void *ptr = registro_obtener_por_nombre(regs, nombre, &tipo);
  if (!ptr)
    return;
  if (tipo == 0)
    *(uint8_t *)ptr = (uint8_t)(valor & 0xFF);
  else
    *(uint32_t *)ptr = valor;
}

typedef struct {
  uint32_t base_addr;
  uint32_t tam_memoria;
  char ip[32];
  char puerto[16];
} t_ms_info;

static t_list *lista_ms_local = NULL;

void actualizar_lista_ms(void) {
  if (!lista_ms_local)
    lista_ms_local = list_create();

  // Limpiar lista anterior
  for (int i = 0; i < list_size(lista_ms_local); i++) {
    free(list_get(lista_ms_local, i));
  }
  list_clean(lista_ms_local);

  t_paquete *p_req = crear_paquete(CPU_GET_MS_LIST);
  enviar_paquete(p_req, socket_memoria);
  eliminar_paquete(p_req);
  int op = recibir_operacion(socket_memoria);
  if (op == KM_MS_LIST) {
    t_paquete *paq = recibir_paquete(socket_memoria);
    if (paq) {
      int count = 0;
      buffer_read(&count, paq->buffer, sizeof(int));
      for (int i = 0; i < count; i++) {
        t_ms_info *ms = malloc(sizeof(t_ms_info));
        buffer_read(&ms->base_addr, paq->buffer, sizeof(uint32_t));
        buffer_read(&ms->tam_memoria, paq->buffer, sizeof(uint32_t));
        char *ip = buffer_read_string(paq->buffer);
        char *puerto = buffer_read_string(paq->buffer);
        if (ip) {
          strncpy(ms->ip, ip, sizeof(ms->ip) - 1);
          ms->ip[sizeof(ms->ip) - 1] = '\0';
          free(ip);
        }
        if (puerto) {
          strncpy(ms->puerto, puerto, sizeof(ms->puerto) - 1);
          ms->puerto[sizeof(ms->puerto) - 1] = '\0';
          free(puerto);
        }
        list_add(lista_ms_local, ms);
      }
      eliminar_paquete(paq);
    }
  }
}

// ===== Acceso a memoria fisica =====
int leer_memoria_fisica(int pid, uint32_t dir_fisica, void *destino,
                        int tamanio) {
  int offset = 0;
  uint32_t current_dir = dir_fisica;
  int bytes_left = tamanio;

  while (bytes_left > 0) {
    t_ms_info *target_ms = NULL;
    if (lista_ms_local) {
      for (int i = 0; i < list_size(lista_ms_local); i++) {
        t_ms_info *ms = list_get(lista_ms_local, i);
        if (current_dir >= ms->base_addr &&
            current_dir < ms->base_addr + ms->tam_memoria) {
          target_ms = ms;
          break;
        }
      }
    }

    if (!target_ms) {
      actualizar_lista_ms();
      for (int i = 0; i < list_size(lista_ms_local); i++) {
        t_ms_info *ms = list_get(lista_ms_local, i);
        if (current_dir >= ms->base_addr &&
            current_dir < ms->base_addr + ms->tam_memoria) {
          target_ms = ms;
          break;
        }
      }
    }

    if (!target_ms) {
      log_error(logger_cpu, "No se encontro Memory Stick para la dir fisica %u",
                current_dir);
      return -1;
    }

    int socket_ms = obtener_socket_ms(target_ms->ip, target_ms->puerto);
    if (socket_ms < 0)
      return -1;

    uint32_t base_relativa = current_dir - target_ms->base_addr;
    int chunk = bytes_left;
    if (base_relativa + chunk > target_ms->tam_memoria) {
      chunk = target_ms->tam_memoria - base_relativa;
    }

    t_paquete *paquete = crear_paquete(MS_LEER);
    agregar_a_paquete(paquete, &base_relativa, sizeof(uint32_t));
    agregar_a_paquete(paquete, &chunk, sizeof(int));
    enviar_paquete(paquete, socket_ms);
    eliminar_paquete(paquete);

    int op = recibir_operacion(socket_ms);
    if (op != MS_RESPUESTA_LEER) {
      t_paquete *resp = recibir_paquete(socket_ms);
      if (resp)
        eliminar_paquete(resp);
      return -1;
    }
    t_paquete *resp = recibir_paquete(socket_ms);
    if (!resp)
      return -1;
    buffer_read(destino + offset, resp->buffer, chunk);
    eliminar_paquete(resp);

    current_dir += chunk;
    offset += chunk;
    bytes_left -= chunk;
  }

  // Log obligatorio
  uint32_t val = 0;
  memcpy(&val, destino, tamanio <= 4 ? tamanio : 4);
  log_info(logger_cpu,
           "## PID: %d - Accion: LEER - Direccion Fisica: %u - Valor: %u", pid,
           dir_fisica, val);
  return 0;
}

int escribir_memoria_fisica(int pid, uint32_t dir_fisica, void *origen,
                            int tamanio) {
  int offset = 0;
  uint32_t current_dir = dir_fisica;
  int bytes_left = tamanio;

  while (bytes_left > 0) {
    t_ms_info *target_ms = NULL;
    if (lista_ms_local) {
      for (int i = 0; i < list_size(lista_ms_local); i++) {
        t_ms_info *ms = list_get(lista_ms_local, i);
        if (current_dir >= ms->base_addr &&
            current_dir < ms->base_addr + ms->tam_memoria) {
          target_ms = ms;
          break;
        }
      }
    }

    if (!target_ms) {
      actualizar_lista_ms();
      for (int i = 0; i < list_size(lista_ms_local); i++) {
        t_ms_info *ms = list_get(lista_ms_local, i);
        if (current_dir >= ms->base_addr &&
            current_dir < ms->base_addr + ms->tam_memoria) {
          target_ms = ms;
          break;
        }
      }
    }

    if (!target_ms) {
      log_error(logger_cpu, "No se encontro Memory Stick para la dir fisica %u",
                current_dir);
      return -1;
    }

    int socket_ms = obtener_socket_ms(target_ms->ip, target_ms->puerto);
    if (socket_ms < 0)
      return -1;

    uint32_t base_relativa = current_dir - target_ms->base_addr;
    int chunk = bytes_left;
    if (base_relativa + chunk > target_ms->tam_memoria) {
      chunk = target_ms->tam_memoria - base_relativa;
    }

    t_paquete *paquete = crear_paquete(MS_ESCRIBIR);
    agregar_a_paquete(paquete, &base_relativa, sizeof(uint32_t));
    agregar_a_paquete(paquete, &chunk, sizeof(int));
    agregar_a_paquete(paquete, origen + offset, chunk);
    enviar_paquete(paquete, socket_ms);
    eliminar_paquete(paquete);

    int op = recibir_operacion(socket_ms);
    if (op != MS_RESPUESTA_OK) {
      t_paquete *resp = recibir_paquete(socket_ms);
      if (resp)
        eliminar_paquete(resp);
      return -1;
    }

    current_dir += chunk;
    offset += chunk;
    bytes_left -= chunk;
  }

  uint32_t val = 0;
  memcpy(&val, origen, tamanio <= 4 ? tamanio : 4);
  log_info(logger_cpu,
           "## PID: %d - Accion: ESCRIBIR - Direccion Fisica: %u - Valor: %u",
           pid, dir_fisica, val);
  return 0;
}

// ===== EXECUTE =====
t_resultado_ciclo execute_instruccion(t_contexto_cpu *contexto,
                                      const char *nombre, const char *param1,
                                      const char *param2, const char *param3) {
  t_registros *regs = &contexto->registros;

  // NOOP
  if (strcasecmp(nombre, "NOOP") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: NOOP", contexto->pid);
    return CICLO_CONTINUAR;
  }

  // SET registro valor
  if (strcasecmp(nombre, "SET") == 0) {
    uint32_t valor = (uint32_t)atoi(param2);
    escribir_valor_registro(regs, param1, valor);
    log_info(logger_cpu, "## PID: %d - Ejecutando: SET - %s %s", contexto->pid,
             param1, param2);
    return CICLO_CONTINUAR;
  }

  // MOV_IN registro_destino (leer memoria[SI] -> registro)
  if (strcasecmp(nombre, "MOV_IN") == 0) {
    // param1 es nombre de registro de destino, SI contiene la dir logica
    uint32_t dir_logica = leer_valor_registro(regs, "SI");
    int tipo = 0;
    if (!registro_obtener_por_nombre(regs, param1, &tipo))
      return CICLO_ERROR;
    int tamanio = (tipo == 0) ? 1 : 4;
    uint32_t dir_fisica = 0;
    if (mmu_traducir(dir_logica, tamanio, contexto->tabla_segmentos,
                     &dir_fisica) < 0) {
      log_error(logger_cpu,
                "## PID: %d - Segmentation Fault en MOV_IN (dir=%u)",
                contexto->pid, dir_logica);
      return CICLO_SEG_FAULT;
    }
    uint32_t valor = 0;
    if (leer_memoria_fisica(contexto->pid, dir_fisica, &valor, tamanio) < 0)
      return CICLO_ERROR;
    escribir_valor_registro(regs, param1, valor);
    log_info(logger_cpu, "## PID: %d - Ejecutando: MOV_IN - %s %s",
             contexto->pid, param1, param2);
    return CICLO_CONTINUAR;
  }

  // MOV_OUT registro_origen (escribir registro -> memoria[DI])
  if (strcasecmp(nombre, "MOV_OUT") == 0) {
    uint32_t dir_logica = leer_valor_registro(regs, "DI");
    int tipo = 0;
    if (!registro_obtener_por_nombre(regs, param1, &tipo))
      return CICLO_ERROR;
    int tamanio = (tipo == 0) ? 1 : 4;
    uint32_t dir_fisica = 0;
    if (mmu_traducir(dir_logica, tamanio, contexto->tabla_segmentos,
                     &dir_fisica) < 0) {
      log_error(logger_cpu,
                "## PID: %d - Segmentation Fault en MOV_OUT (dir=%u)",
                contexto->pid, dir_logica);
      return CICLO_SEG_FAULT;
    }
    uint32_t valor = leer_valor_registro(regs, param1);
    if (escribir_memoria_fisica(contexto->pid, dir_fisica, &valor, tamanio) < 0)
      return CICLO_ERROR;
    log_info(logger_cpu, "## PID: %d - Ejecutando: MOV_OUT - %s %s",
             contexto->pid, param1, param2);
    return CICLO_CONTINUAR;
  }

  // SUM destino origen
  if (strcasecmp(nombre, "SUM") == 0) {
    uint32_t v1 = leer_valor_registro(regs, param1);
    uint32_t v2 = leer_valor_registro(regs, param2);
    escribir_valor_registro(regs, param1, v1 + v2);
    log_info(logger_cpu, "## PID: %d - Ejecutando: SUM - %s %s", contexto->pid,
             param1, param2);
    return CICLO_CONTINUAR;
  }

  // SUB destino origen
  if (strcasecmp(nombre, "SUB") == 0) {
    uint32_t v1 = leer_valor_registro(regs, param1);
    uint32_t v2 = leer_valor_registro(regs, param2);
    escribir_valor_registro(regs, param1, v1 - v2);
    log_info(logger_cpu, "## PID: %d - Ejecutando: SUB - %s %s", contexto->pid,
             param1, param2);
    return CICLO_CONTINUAR;
  }

  // JNZ registro dir_instruccion
  if (strcasecmp(nombre, "JNZ") == 0) {
    uint32_t valor = leer_valor_registro(regs, param1);
    if (valor != 0) {
      uint32_t nueva_pc = (uint32_t)atoi(param2);
      regs->pc = nueva_pc;
      contexto->pc_modificado = 1;
      log_info(logger_cpu, "## PID: %d - Ejecutando: JNZ - %s %s",
               contexto->pid, param1, param2);
    } else {
      log_info(logger_cpu, "## PID: %d - Ejecutando: JNZ - %s %s",
               contexto->pid, param1, param2);
    }
    return CICLO_CONTINUAR;
  }

  if (strcasecmp(nombre, "COPY_MEM") == 0) {
    uint32_t dir_orig = leer_valor_registro(regs, "SI");
    uint32_t dir_dest = leer_valor_registro(regs, "DI");
    int tam = (int)leer_valor_registro(regs, param1);

    if (tam <= 0)
      return CICLO_CONTINUAR;

    uint32_t fis_dest = 0, fis_orig = 0;
    if (mmu_traducir(dir_dest, tam, contexto->tabla_segmentos, &fis_dest) < 0 ||
        mmu_traducir(dir_orig, tam, contexto->tabla_segmentos, &fis_orig) < 0) {
      log_error(logger_cpu, "## PID: %d - Segmentation Fault en COPY_MEM",
                contexto->pid);
      return CICLO_SEG_FAULT;
    }

    void *buffer = malloc((size_t)tam);
    if (leer_memoria_fisica(contexto->pid, fis_orig, buffer, tam) < 0 ||
        escribir_memoria_fisica(contexto->pid, fis_dest, buffer, tam) < 0) {
      free(buffer);
      return CICLO_ERROR;
    }
    free(buffer);
    log_info(logger_cpu, "## PID: %d - Ejecutando: COPY_MEM - %s",
             contexto->pid, param1);
    return CICLO_CONTINUAR;
  }

  // LOG registro  (loguea valor del registro)
  if (strcasecmp(nombre, "LOG") == 0) {
    uint32_t valor = leer_valor_registro(regs, param1);
    log_info(logger_cpu, "## PID: %d - Ejecutando: LOG - %s - Valor: %u",
             contexto->pid, param1, valor);
    return CICLO_CONTINUAR;
  }

  // DUMP_MEMORY  (solicita volcado de memoria al Kernel Memory)
  if (strcasecmp(nombre, "DUMP_MEMORY") == 0) {
    log_info(logger_cpu, "## PID: %d - DUMP_MEMORY", contexto->pid);
    // Notificar al KS via SYSCALL para que KM registre el estado
    contexto->syscall_nombre = strdup("DUMP_MEMORY");
    contexto->syscall_str_param = strdup("");
    return CICLO_DESALOJO_SYSCALL;
  }

  // ===== SYSCALLS =====

  // EXIT
  if (strcasecmp(nombre, "EXIT") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: EXIT", contexto->pid);
    return CICLO_DESALOJO_EXIT;
  }

  // MUTEX_CREATE nombre
  if (strcasecmp(nombre, "MUTEX_CREATE") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: MUTEX_CREATE - %s",
             contexto->pid, param1);
    contexto->syscall_nombre = strdup("MUTEX_CREATE");
    contexto->syscall_str_param = strdup(param1);
    return CICLO_DESALOJO_SYSCALL;
  }

  // MUTEX_LOCK nombre
  if (strcasecmp(nombre, "MUTEX_LOCK") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: MUTEX_LOCK - %s",
             contexto->pid, param1);
    contexto->syscall_nombre = strdup("MUTEX_LOCK");
    contexto->syscall_str_param = strdup(param1);
    return CICLO_DESALOJO_SYSCALL;
  }

  // MUTEX_UNLOCK nombre
  if (strcasecmp(nombre, "MUTEX_UNLOCK") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: MUTEX_UNLOCK - %s",
             contexto->pid, param1);
    contexto->syscall_nombre = strdup("MUTEX_UNLOCK");
    contexto->syscall_str_param = strdup(param1);
    return CICLO_DESALOJO_SYSCALL;
  }

  // MEM_ALLOC id_segmento tamanio
  if (strcasecmp(nombre, "MEM_ALLOC") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: MEM_ALLOC - %s %s",
             contexto->pid, param1, param2);
    contexto->syscall_nombre = strdup("MEM_ALLOC");
    contexto->syscall_param1 = atoi(param1);
    contexto->syscall_param2 = atoi(param2);
    return CICLO_DESALOJO_SYSCALL;
  }

  // MEM_FREE id_segmento
  if (strcasecmp(nombre, "MEM_FREE") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: MEM_FREE - %s",
             contexto->pid, param1);
    contexto->syscall_nombre = strdup("MEM_FREE");
    contexto->syscall_param1 = atoi(param1);
    return CICLO_DESALOJO_SYSCALL;
  }

  // SLEEP tiempo_ms — es una syscall que delega al IO de tipo SLEEP
  if (strcasecmp(nombre, "SLEEP") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: SLEEP - %s ms",
             contexto->pid, param1);
    contexto->syscall_nombre = strdup("SLEEP");
    contexto->syscall_param1 = atoi(param1);
    return CICLO_DESALOJO_SYSCALL; // el KS deriva al IO de SLEEP
  }

  // STDIN reg_dir_logica reg_tamanio
  if (strcasecmp(nombre, "STDIN") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: STDIN - %s %s",
             contexto->pid, param1, param2);
    contexto->syscall_nombre = strdup("STDIN");
    uint32_t dir_logica = leer_valor_registro(regs, param1);
    uint32_t tamanio = leer_valor_registro(regs, param2);
    contexto->syscall_param1 = (int)dir_logica;
    contexto->syscall_param2 = (int)tamanio;
    return CICLO_DESALOJO_IO;
  }

  // STDOUT reg_dir_logica reg_tamanio
  if (strcasecmp(nombre, "STDOUT") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: STDOUT - %s %s",
             contexto->pid, param1, param2);
    contexto->syscall_nombre = strdup("STDOUT");
    uint32_t dir_logica = leer_valor_registro(regs, param1);
    uint32_t tamanio = leer_valor_registro(regs, param2);
    contexto->syscall_param1 = (int)dir_logica;
    contexto->syscall_param2 = (int)tamanio;
    return CICLO_DESALOJO_IO;
  }

  // INIT_PROC path prioridad
  if (strcasecmp(nombre, "INIT_PROC") == 0) {
    log_info(logger_cpu, "## PID: %d - Ejecutando: INIT_PROC - %s %s",
             contexto->pid, param1, param2);
    contexto->syscall_nombre = strdup("INIT_PROC");
    contexto->syscall_str_param = strdup(param1);
    contexto->syscall_param1 = atoi(param2);
    return CICLO_DESALOJO_SYSCALL;
  }

  log_warning(logger_cpu, "PID %d: instruccion desconocida '%s'", contexto->pid,
              nombre);
  return CICLO_ERROR;
}

// ===== CHECK INTERRUPT =====
int check_interrupt(t_contexto_cpu *contexto) { return contexto->interrumpido; }

// ===== CICLO COMPLETO =====
t_resultado_ciclo ejecutar_ciclo_instruccion(t_contexto_cpu *contexto) {
  // 1. FETCH
  char *instruccion =
      fetch_instruccion(contexto->pid, (int)contexto->registros.pc);
  if (!instruccion) {
    log_error(logger_cpu, "PID %d: no se pudo obtener instruccion PC=%u",
              contexto->pid, contexto->registros.pc);
    return CICLO_ERROR;
  }
  log_info(logger_cpu, "## PID: %d - FETCH - Program Counter: %u",
           contexto->pid, contexto->registros.pc);

  // 2. DECODE
  char nombre[64], param1[128], param2[128], param3[128];
  decode_instruccion(instruccion, nombre, param1, param2, param3);
  free(instruccion);

  // 3. EXECUTE
  t_resultado_ciclo resultado =
      execute_instruccion(contexto, nombre, param1, param2, param3);

  // 4. Actualizar PC
  if (resultado != CICLO_ERROR && resultado != CICLO_SEG_FAULT) {
    if (contexto->pc_modificado == 0) {
      contexto->registros.pc++;
    } else {
      contexto->pc_modificado = 0;
    }
  }

  // 5. CHECK INTERRUPT (solo si seguimos ejecutando el mismo proceso)
  if (resultado == CICLO_CONTINUAR && check_interrupt(contexto)) {
    log_info(logger_cpu, "## Interrupcion recibida - PID: %d", contexto->pid);
    return CICLO_DESALOJO_INT;
  }

  return resultado;
}
