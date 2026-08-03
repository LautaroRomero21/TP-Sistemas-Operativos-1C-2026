#include <commons/config.h>
#include <commons/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../utils/src/utils/conexiones.h"
#include "../../utils/src/utils/protocolos.h"
#include "interfaz.h"

static t_log *logger_io = NULL;
static int socket_ks = -1;
static char *nombre_interfaz = NULL;
static char *tipo_interfaz_str = NULL;
static t_tipo_interfaz tipo_interfaz;

int main(int argc, char **argv) {
  // Uso: ./bin/io [Archivo Config] [Tipo]
  const char *config_path = "IO.config";
  if (argc > 1)
    config_path = argv[1];

  // Tipo de interfaz puede venir por CLI (argv[2]) o por config
  const char *tipo_cli = (argc > 2) ? argv[2] : NULL;

  t_config *config = config_create((char *)config_path);
  if (!config) {
    fprintf(stderr, "No se pudo abrir el config: %s\n", config_path);
    return EXIT_FAILURE;
  }

  const char *ip_ks = config_get_string_value(config, "IP_KERNEL_SCHEDULER");
  const char *puerto_ks =
      config_get_string_value(config, "PUERTO_KERNEL_SCHEDULER");
  const char *log_level = config_get_string_value(config, "LOG_LEVEL");
  nombre_interfaz = strdup(config_get_string_value(config, "NOMBRE_INTERFAZ"));

  // Tipo: primero CLI, dsps config
  if (tipo_cli)
    tipo_interfaz_str = strdup(tipo_cli);
  else
    tipo_interfaz_str =
        strdup(config_get_string_value(config, "TIPO_INTERFAZ"));

  tipo_interfaz = interfaz_parsear_tipo(tipo_interfaz_str);

  logger_io =
      log_create("IO.log", "IO", 1, log_level_from_string((char *)log_level));
  if (!logger_io) {
    fprintf(stderr, "No se pudo crear el logger\n");
    config_destroy(config);
    return EXIT_FAILURE;
  }

  log_info(logger_io, "IO iniciando: nombre=%s tipo=%s", nombre_interfaz,
           tipo_interfaz_str);

  // Conectar al Kernel Scheduler
  socket_ks = crear_conexion((char *)ip_ks, (char *)puerto_ks, logger_io);
  if (socket_ks < 0) {
    log_error(logger_io, "No se pudo conectar al Kernel Scheduler (%s:%s)",
              ip_ks, puerto_ks);
    return EXIT_FAILURE;
  }

  // Handshake
  enviar_operacion(socket_ks, HANDSHAKE_IO);
  int op = recibir_operacion(socket_ks);
  if (op != HANDSHAKE_OK) {
    log_error(logger_io, "Handshake fallido con Kernel Scheduler");
    return EXIT_FAILURE;
  }
  log_info(logger_io, "## Conectado a Kernel Scheduler");

  // Identificacion: enviar nombre y tipo
  t_paquete *paq_id = crear_paquete(IO_IDENTIFICACION);
  agregar_string_a_paquete(paq_id, nombre_interfaz);
  agregar_string_a_paquete(paq_id, tipo_interfaz_str);
  enviar_paquete(paq_id, socket_ks);
  eliminar_paquete(paq_id);

  log_info(logger_io, "Esperando solicitudes de IO...");

  // Loop principal
  while (1) {
    op = recibir_operacion(socket_ks);
    if (op < 0) {
      log_error(logger_io, "Kernel Scheduler desconectado");
      break;
    }

    if (op == KS_IO_SOLICITUD) {
      t_paquete *paquete = recibir_paquete(socket_ks);
      if (!paquete)
        continue;

      int pid = 0;
      buffer_read(&pid, paquete->buffer, sizeof(int));
      char *tipo_solicitud = buffer_read_string(paquete->buffer);
      int param1 = 0, param2 = 0;
      buffer_read(&param1, paquete->buffer, sizeof(int));
      buffer_read(&param2, paquete->buffer, sizeof(int));
      char *datos_extra =
          buffer_read_string(paquete->buffer); // datos de STDOUT
      eliminar_paquete(paquete);

      log_info(logger_io, "## PID: %d - Inicio de IO", pid);

      char *datos_resultado = NULL;

      switch (tipo_interfaz) {
      case INTERFAZ_STDIN:
        log_info(logger_io, "## PID: %d - Ingrese %d caracteres:", pid, param2);
        datos_resultado = interfaz_stdin(param2, logger_io);
        break;

      case INTERFAZ_STDOUT:
        // datos_extra contiene el string a imprimir (enviado por el KS desde
        // KM)
        log_info(logger_io, "## PID: %d - %s", pid,
                 datos_extra ? datos_extra : "(sin datos)");
        interfaz_stdout(datos_extra, param2, logger_io);
        break;

      case INTERFAZ_SLEEP:
        log_info(logger_io, "## PID: %d - Haciendo sleep por %d milisegundos.",
                 pid, param1);
        interfaz_sleep(param1, logger_io);
        break;
      }

      // Notificar finalizacion al Kernel Scheduler
      t_paquete *resp = crear_paquete(IO_FINALIZADA);
      agregar_a_paquete(resp, &pid, sizeof(int));
      if (tipo_interfaz == INTERFAZ_STDIN) {
        int len = param2;
        agregar_a_paquete(resp, &len, sizeof(int));
        if (len > 0) {
          agregar_a_paquete(resp, datos_resultado, len);
        }
      } else {
        agregar_string_a_paquete(resp, datos_resultado ? datos_resultado : "");
      }
      enviar_paquete(resp, socket_ks);
      eliminar_paquete(resp);

      log_info(logger_io, "## PID: %d - Fin de IO", pid);

      free(datos_resultado);
      free(tipo_solicitud);
      free(datos_extra);
    }
  }

  close(socket_ks);
  free(nombre_interfaz);
  free(tipo_interfaz_str);
  log_destroy(logger_io);
  config_destroy(config);

  return EXIT_SUCCESS;
}