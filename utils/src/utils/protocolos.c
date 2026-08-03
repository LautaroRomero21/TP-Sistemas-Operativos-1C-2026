#include "protocolos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

ssize_t enviar_todo(int socket_destino, const void *buffer, size_t size) {
  size_t total = 0;
  const char *ptr = buffer;

  while (total < size) {
    ssize_t sent = send(socket_destino, ptr + total, size - total, 0);
    if (sent <= 0) {
      return sent;
    }
    total += (size_t)sent;
  }
  return (ssize_t)total;
}

ssize_t recibir_todo(int socket, void *buffer, size_t size) {
  size_t total = 0;
  char *ptr = buffer;

  while (total < size) {
    ssize_t recvd = recv(socket, ptr + total, size - total, 0);
    if (recvd <= 0) {
      return recvd;
    }
    total += (size_t)recvd;
  }
  return (ssize_t)total;
}

t_paquete *crear_paquete(op_code codigo_operacion) {
  t_paquete *paquete = malloc(sizeof(t_paquete));
  paquete->codigo_operacion = codigo_operacion;
  paquete->buffer = malloc(sizeof(t_buffer));
  paquete->buffer->size = 0;
  paquete->buffer->offset = 0;
  paquete->buffer->stream = NULL;
  return paquete;
}

void agregar_a_paquete(t_paquete *paquete, void *valor, int tamanio) {
  paquete->buffer->stream =
      realloc(paquete->buffer->stream, paquete->buffer->size + tamanio);
  memcpy((char *)paquete->buffer->stream + paquete->buffer->size, valor,
         tamanio);
  paquete->buffer->size += tamanio;
}

void agregar_string_a_paquete(t_paquete *paquete, const char *valor) {
  int len = 0;
  if (valor) {
    len = (int)strlen(valor) + 1;
  }
  agregar_a_paquete(paquete, &len, sizeof(int));
  if (len > 0) {
    agregar_a_paquete(paquete, (void *)valor, len);
  }
}

int enviar_paquete(t_paquete *paquete, int socket_destino) {
  int bytes = paquete->buffer->size + (int)sizeof(op_code) + (int)sizeof(int);
  void *a_enviar = malloc(bytes);
  int offset = 0;

  memcpy((char *)a_enviar + offset, &(paquete->codigo_operacion),
         sizeof(op_code));
  offset += sizeof(op_code);
  memcpy((char *)a_enviar + offset, &(paquete->buffer->size), sizeof(int));
  offset += sizeof(int);
  memcpy((char *)a_enviar + offset, paquete->buffer->stream,
         paquete->buffer->size);

  int result = (int)enviar_todo(socket_destino, a_enviar, bytes);
  if (result <= 0) {
    printf("Error al enviar el paquete\n");
  }
  free(a_enviar);
  return result;
}

void eliminar_paquete(t_paquete *paquete) {
  if (!paquete) {
    return;
  }
  free(paquete->buffer->stream);
  free(paquete->buffer);
  free(paquete);
}

void buffer_read(void *dest, t_buffer *buffer, int size) {
  memcpy(dest, (char *)buffer->stream + buffer->offset, size);
  buffer->offset += size;
}

char *buffer_read_string(t_buffer *buffer) {
  int len = 0;
  buffer_read(&len, buffer, sizeof(int));
  if (len <= 0) {
    return NULL;
  }
  char *out = malloc((size_t)len);
  buffer_read(out, buffer, len);
  return out;
}

t_paquete *recibir_paquete(int socket) {
  t_paquete *paquete = malloc(sizeof(t_paquete));
  paquete->buffer = malloc(sizeof(t_buffer));
  paquete->buffer->offset = 0;

  if (recibir_todo(socket, &paquete->buffer->size, sizeof(int)) <= 0) {
    free(paquete->buffer);
    free(paquete);
    return NULL;
  }

  if (paquete->buffer->size > 0) {
    paquete->buffer->stream = malloc((size_t)paquete->buffer->size);
    if (recibir_todo(socket, paquete->buffer->stream,
                     (size_t)paquete->buffer->size) <= 0) {
      free(paquete->buffer->stream);
      free(paquete->buffer);
      free(paquete);
      return NULL;
    }
  } else {
    paquete->buffer->stream = NULL;
  }

  return paquete;
}

int enviar_operacion(int socket_destino, op_code codigo_operacion) {
  int result =
      (int)enviar_todo(socket_destino, &codigo_operacion, sizeof(op_code));
  if (result <= 0) {
    printf("Error al enviar la operacion (solo opcode)\n");
  }
  return result;
}

int recibir_operacion(int socket) {
  op_code codigo_operacion;
  int bytes_recibidos =
      (int)recibir_todo(socket, &codigo_operacion, sizeof(op_code));
  if (bytes_recibidos <= 0) {
    return -1;
  }
  return (int)codigo_operacion;
}