#include <commons/collections/list.h>
#include <commons/config.h>
#include <commons/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../utils/src/utils/conexiones.h"
#include "../../utils/src/utils/protocolos.h"
#include "swap_archivo.h"

t_log *logger_swap = NULL;

static int socket_km = -1;
static pthread_mutex_t mutex_swap = PTHREAD_MUTEX_INITIALIZER;

static void responder_ok(int socket) {
  t_paquete *resp = crear_paquete(SWAP_RESPUESTA_OK);
  enviar_paquete(resp, socket);
  eliminar_paquete(resp);
}

static void responder_error(int socket) {
  t_paquete *resp = crear_paquete(SWAP_RESPUESTA_ERROR);
  enviar_paquete(resp, socket);
  eliminar_paquete(resp);
}

/*
 * Protocolo rediseñado (KM gestiona el espacio, SWAP solo lee/escribe bloques):
 *
 *  SWAP_ESCRIBIR_BLOQUE: recibe { numero_bloque(int), tamanio(int),
 * datos(bytes) } SWAP_LEER_BLOQUE:     recibe { numero_bloque(int) } ->
 * responde { datos(tam_bloque bytes) } SWAP_LIBERAR:         recibe {
 * numero_bloque(int) } (KM indica que bloque liberar)
 *
 * El KM es el responsable de administrar el mapa de bloques libres.
 */
static void atender_km(void) {
  while (1) {
    int op = recibir_operacion(socket_km);
    if (op < 0) {
      log_error(logger_swap, "Kernel Memory desconectado");
      break;
    }

    t_paquete *paquete = recibir_paquete(socket_km);
    if (!paquete)
      continue;

    pthread_mutex_lock(&mutex_swap);

    switch (op) {
    case SWAP_ESCRIBIR_BLOQUE: {
      int numero_bloque = 0, tamanio = 0;
      buffer_read(&numero_bloque, paquete->buffer, sizeof(int));
      buffer_read(&tamanio, paquete->buffer, sizeof(int));

      void *datos = malloc((size_t)tamanio);
      buffer_read(datos, paquete->buffer, tamanio);

      log_info(logger_swap, "## Escritura del bloque: %d", numero_bloque);

      int resultado = swap_escribir_bloque(numero_bloque, datos, tamanio);
      free(datos);

      if (resultado == 0)
        responder_ok(socket_km);
      else
        responder_error(socket_km);
      break;
    }

    case SWAP_LEER_BLOQUE: {
      int numero_bloque = 0;
      buffer_read(&numero_bloque, paquete->buffer, sizeof(int));

      log_info(logger_swap, "## Lectura del bloque: %d", numero_bloque);

      int tam_out = 0;
      void *datos = swap_leer_bloque(numero_bloque, &tam_out);

      t_paquete *resp = crear_paquete(SWAP_RESPUESTA_DATOS);
      agregar_a_paquete(resp, &tam_out, sizeof(int));
      if (datos && tam_out > 0)
        agregar_a_paquete(resp, datos, tam_out);
      enviar_paquete(resp, socket_km);
      eliminar_paquete(resp);

      free(datos);
      break;
    }

    case SWAP_LIBERAR: {
      int numero_bloque = 0;
      buffer_read(&numero_bloque, paquete->buffer, sizeof(int));
      swap_liberar_bloque(numero_bloque);
      log_info(logger_swap, "Bloque %d liberado", numero_bloque);
      responder_ok(socket_km);
      break;
    }

    default:
      log_warning(logger_swap, "Operacion desconocida: %d", op);
      responder_error(socket_km);
      break;
    }

    pthread_mutex_unlock(&mutex_swap);
    eliminar_paquete(paquete);
  }
}

int main(int argc, char **argv) {
  const char *config_path = "SWAP.config";
  if (argc > 1)
    config_path = argv[1];

  t_config *config = config_create((char *)config_path);
  if (!config) {
    fprintf(stderr, "No se pudo abrir config\n");
    return EXIT_FAILURE;
  }

  const char *ip_km = config_get_string_value(config, "IP_KERNEL_MEMORY");
  const char *puerto_km =
      config_get_string_value(config, "PUERTO_KERNEL_MEMORY");
  const char *path_swap = config_get_string_value(config, "SWAP_FILE_PATH");
  const char *log_level = config_get_string_value(config, "LOG_LEVEL");
  int tam_archivo = config_get_int_value(config, "SWAP_FILE_SIZE");
  int tam_bloque = config_get_int_value(config, "BLOCK_SIZE");

  logger_swap = log_create("SWAP.log", "SWAP", 1,
                           log_level_from_string((char *)log_level));
  if (!logger_swap) {
    fprintf(stderr, "No se pudo crear logger\n");
    return EXIT_FAILURE;
  }

  log_info(logger_swap, "SWAP iniciando...");
  log_info(logger_swap, "Path: %s | Tam: %d | Bloque: %d", path_swap,
           tam_archivo, tam_bloque);

  if (swap_inicializar(path_swap, tam_archivo, tam_bloque) < 0) {
    log_error(logger_swap, "Error al inicializar SWAP");
    return EXIT_FAILURE;
  }

  socket_km = crear_conexion((char *)ip_km, (char *)puerto_km, logger_swap);
  if (socket_km < 0) {
    log_error(logger_swap, "No se pudo conectar a Kernel Memory");
    return EXIT_FAILURE;
  }
  enviar_operacion(socket_km, HANDSHAKE_SWAP);
  int op = recibir_operacion(socket_km);
  if (op != HANDSHAKE_OK) {
    log_error(logger_swap, "Handshake fallido con Kernel Memory");
    return EXIT_FAILURE;
  }

  // Informar block_size y swap_size al Kernel Memory
  t_paquete *paq_info = crear_paquete(SWAP_INFO);
  agregar_a_paquete(paq_info, &tam_bloque, sizeof(int));
  agregar_a_paquete(paq_info, &tam_archivo, sizeof(int));
  enviar_paquete(paq_info, socket_km);
  eliminar_paquete(paq_info);

  log_info(logger_swap, "## Conectado a Kernel Memory");

  atender_km();

  swap_destruir();
  close(socket_km);
  log_destroy(logger_swap);
  config_destroy(config);
  return EXIT_SUCCESS;
}
