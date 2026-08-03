#include <utils/conexiones.h>

int iniciar_servidor(char *puerto, t_log *logger) {
  int socket_servidor;
  struct sockaddr_in direccion_servidor;

  socket_servidor = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_servidor == -1) {
    log_error(logger, "Error al crear el socket servidor");
    return -1;
  }

  // Permitir reutilizar el puerto inmediatamente
  int opt = 1;
  setsockopt(socket_servidor, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  direccion_servidor.sin_family = AF_INET;
  direccion_servidor.sin_addr.s_addr = INADDR_ANY;
  direccion_servidor.sin_port = htons(atoi(puerto));
  memset(&(direccion_servidor.sin_zero), 0, 8);

  if (bind(socket_servidor, (struct sockaddr *)&direccion_servidor,
           sizeof(direccion_servidor)) == -1) {
    log_error(logger, "Error en bind del servidor en puerto %s", puerto);
    close(socket_servidor);
    return -1;
  }

  if (listen(socket_servidor, SOMAXCONN) == -1) {
    log_error(logger, "Error en listen del servidor en puerto %s", puerto);
    close(socket_servidor);
    return -1;
  }

  log_info(logger, "Servidor escuchando en puerto %s", puerto);
  return socket_servidor;
}

int esperar_cliente(int socket_servidor, t_log *logger) {
  struct sockaddr_in direccion_cliente;
  socklen_t tam_direccion = sizeof(direccion_cliente);

  int socket_cliente = accept(
      socket_servidor, (struct sockaddr *)&direccion_cliente, &tam_direccion);
  if (socket_cliente == -1) {
    log_error(logger, "Error al aceptar cliente");
    return -1;
  }

  log_info(logger, "Cliente conectado (socket %d)", socket_cliente);
  return socket_cliente;
}

int crear_conexion(char *ip, char *puerto, t_log *logger) {
  int socket_cliente;
  struct sockaddr_in direccion_servidor;

  socket_cliente = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_cliente == -1) {
    log_error(logger, "Error al crear socket cliente");
    return -1;
  }

  direccion_servidor.sin_family = AF_INET;
  direccion_servidor.sin_port = htons(atoi(puerto));
  direccion_servidor.sin_addr.s_addr = inet_addr(ip);
  memset(&(direccion_servidor.sin_zero), 0, 8);

  if (connect(socket_cliente, (struct sockaddr *)&direccion_servidor,
              sizeof(direccion_servidor)) == -1) {
    log_error(logger, "Error al conectar con %s:%s", ip, puerto);
    close(socket_cliente);
    return -1;
  }

  log_info(logger, "Conectado a %s:%s (socket %d)", ip, puerto, socket_cliente);
  return socket_cliente;
}

void liberar_conexion(int socket_cliente) { close(socket_cliente); }
