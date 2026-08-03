#include "../../utils/src/utils/conexiones.h"
#include "../../utils/src/utils/protocolos.h"
#include <commons/config.h>
#include <commons/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memoria_fisica.h"

t_log *logger_ms = NULL;
int retardo_memoria = 0;
static int cpu_counter = 0;

void *atender_cliente(void *arg) {
  int socket_cliente = *(int *)arg;
  free(arg);

  while (1) {
    int op = recibir_operacion(socket_cliente);
    if (op < 0) {
      log_warning(logger_ms, "Cliente desconectado del Memory Stick");
      break;
    }

    if (op == MS_LEER) {
      t_paquete *paquete = recibir_paquete(socket_cliente);
      if (!paquete)
        continue;

      uint32_t direccion;
      int tamanio_leer;
      buffer_read(&direccion, paquete->buffer, sizeof(uint32_t));
      buffer_read(&tamanio_leer, paquete->buffer, sizeof(int));
      eliminar_paquete(paquete);

      void *datos_leidos = malloc((size_t)tamanio_leer);
      int res = memoria_fisica_leer(direccion, datos_leidos, tamanio_leer);

      if (res == 0) {
        t_paquete *resp = crear_paquete(MS_RESPUESTA_LEER);
        agregar_a_paquete(resp, datos_leidos, tamanio_leer);
        enviar_paquete(resp, socket_cliente);
        eliminar_paquete(resp);
      } else {
        // En caso de error, podriamos mandar un paquete de error o algo.
        // Asumimos comportamiento ideal o terminamos.
        enviar_operacion(socket_cliente, KM_RESPUESTA_ERROR);
      }
      free(datos_leidos);
    } else if (op == MS_ESCRIBIR) {
      t_paquete *paquete = recibir_paquete(socket_cliente);
      if (!paquete)
        continue;

      uint32_t direccion;
      int tamanio_escribir;
      buffer_read(&direccion, paquete->buffer, sizeof(uint32_t));
      buffer_read(&tamanio_escribir, paquete->buffer, sizeof(int));

      void *datos_escribir = malloc((size_t)tamanio_escribir);
      buffer_read(datos_escribir, paquete->buffer, tamanio_escribir);
      eliminar_paquete(paquete);

      int res =
          memoria_fisica_escribir(direccion, datos_escribir, tamanio_escribir);

      if (res == 0) {
        enviar_operacion(socket_cliente, MS_RESPUESTA_OK);
      } else {
        enviar_operacion(socket_cliente, KM_RESPUESTA_ERROR);
      }
      free(datos_escribir);
    } else if (op == HANDSHAKE_CPU) {
      int cpu_id = -1;
      t_paquete *paq = recibir_paquete(socket_cliente);
      if (paq) {
        if (paq->buffer->size >= sizeof(int)) {
            buffer_read(&cpu_id, paq->buffer, sizeof(int));
        }
        eliminar_paquete(paq);
      }
      log_info(logger_ms, "## CPU %d Conectada", cpu_id);
      enviar_operacion(socket_cliente, HANDSHAKE_OK);
    } else {
      log_warning(logger_ms, "Operacion desconocida: %d", op);
    }
  }

  close(socket_cliente);
  return NULL;
}

int main(int argc, char *argv[]) {
  // Configuracion por defecto
  char *config_path = "memory_stick.config";
  int tamanio = 256;

  if (argc >= 2) {
    config_path = argv[1];
  }
  if (argc >= 3) {
    tamanio = atoi(argv[2]);
  }

  // Leer archivo de configuracion primero (para obtener LOG_LEVEL)
  t_config *config = config_create(config_path);
  if (config == NULL) {
    fprintf(stderr, "Error al leer el archivo de configuracion %s\n",
            config_path);
    return EXIT_FAILURE;
  }

  // Obtener parametros
  char *ip_kernel_memory = config_get_string_value(config, "IP_KERNEL_MEMORY");
  char *puerto_kernel_memory =
      config_get_string_value(config, "PUERTO_KERNEL_MEMORY");
  char *puerto_escucha = config_get_string_value(config, "PUERTO_ESCUCHA");
  const char *log_level_str = config_get_string_value(config, "LOG_LEVEL");
  if (config_has_property(config, "MEMORY_DELAY")) {
    retardo_memoria = config_get_int_value(config, "MEMORY_DELAY");
  }

  // Crear logger con LOG_LEVEL del config
  logger_ms = log_create("memory_stick.log", "MEMORY_STICK", true,
                         log_level_from_string((char *)log_level_str));
  if (logger_ms == NULL) {
    fprintf(stderr, "Error al crear el logger\n");
    config_destroy(config);
    return EXIT_FAILURE;
  }

  // Inicializar memoria fisica
  memoria_fisica_inicializar(tamanio, retardo_memoria);

  // Conectar al Kernel Memory
  int conexion_kernel_memory =
      crear_conexion(ip_kernel_memory, puerto_kernel_memory, logger_ms);
  if (conexion_kernel_memory == -1) {
    log_error(logger_ms, "No se pudo conectar al Kernel Memory");
    memoria_fisica_destruir();
    config_destroy(config);
    log_destroy(logger_ms);
    return EXIT_FAILURE;
  }
  log_info(logger_ms, "## Conectado a Kernel Memory");

  // Handshake y enviar info de tamaño a KM
  enviar_operacion(conexion_kernel_memory, HANDSHAKE_MS);
  int resp_hs = recibir_operacion(conexion_kernel_memory);
  if (resp_hs == HANDSHAKE_OK) {
    t_paquete *paq_tam = crear_paquete(MS_INFO_CONFIG);
    agregar_a_paquete(paq_tam, &tamanio, sizeof(int));
    agregar_string_a_paquete(paq_tam, puerto_escucha);
    enviar_paquete(paq_tam, conexion_kernel_memory);
    eliminar_paquete(paq_tam);
  }

  // Iniciar hilo para atender peticiones del Kernel Memory
  int *sock_km = malloc(sizeof(int));
  *sock_km = conexion_kernel_memory;
  pthread_t hilo_km;
  pthread_create(&hilo_km, NULL, atender_cliente, sock_km);
  pthread_detach(hilo_km);

  // Iniciar servidor para CPUs
  int socket_servidor = iniciar_servidor(puerto_escucha, logger_ms);
  if (socket_servidor < 0) {
    log_error(logger_ms, "Error al iniciar servidor en puerto %s",
              puerto_escucha);
    memoria_fisica_destruir();
    config_destroy(config);
    log_destroy(logger_ms);
    return EXIT_FAILURE;
  }
  log_info(logger_ms, "Memory Stick servidor escuchando en puerto %s",
           puerto_escucha);

  // Bucle infinito esperando conexiones
  while (1) {
    int *socket_cliente = malloc(sizeof(int));
    *socket_cliente = esperar_cliente(socket_servidor, logger_ms);
    if (*socket_cliente < 0) {
      free(socket_cliente);
      continue;
    }

    pthread_t hilo_cliente;
    pthread_create(&hilo_cliente, NULL, atender_cliente, socket_cliente);
    pthread_detach(hilo_cliente);
  }

  // Limpieza (inaccesible pero buena practica)
  liberar_conexion(conexion_kernel_memory);
  memoria_fisica_destruir();
  config_destroy(config);
  log_destroy(logger_ms);

  return EXIT_SUCCESS;
}
