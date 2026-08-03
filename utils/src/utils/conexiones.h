#ifndef UTILS_CONEXIONES_H_
#define UTILS_CONEXIONES_H_

#include <arpa/inet.h>
#include <commons/config.h>
#include <commons/log.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief Inicia un servidor en el puerto indicado
 * @param puerto Puerto en el que escuchara el servidor
 * @param logger Logger para registrar eventos
 * @return File descriptor del socket servidor, o -1 en caso de error
 */
int iniciar_servidor(char *puerto, t_log *logger);

/**
 * @brief Espera y acepta una nueva conexion de un cliente
 * @param socket_servidor File descriptor del socket servidor
 * @param logger Logger para registrar eventos
 * @return File descriptor del socket cliente aceptado, o -1 en caso de error
 */
int esperar_cliente(int socket_servidor, t_log *logger);

/**
 * @brief Crea una conexion como cliente hacia un servidor
 * @param ip IP del servidor al que conectarse
 * @param puerto Puerto del servidor al que conectarse
 * @param logger Logger para registrar eventos
 * @return File descriptor del socket conectado, o -1 en caso de error
 */
int crear_conexion(char *ip, char *puerto, t_log *logger);

/**
 * @brief Libera la conexion cerrando el socket
 * @param socket_cliente File descriptor del socket a cerrar
 */
void liberar_conexion(int socket_cliente);

#endif
