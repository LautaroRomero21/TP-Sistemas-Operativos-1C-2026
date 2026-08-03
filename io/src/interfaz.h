#ifndef INTERFAZ_H_
#define INTERFAZ_H_

#include <commons/log.h>

typedef enum {
  INTERFAZ_STDIN,
  INTERFAZ_STDOUT,
  INTERFAZ_SLEEP
} t_tipo_interfaz;

// Parsear tipo de interfaz desde string
t_tipo_interfaz interfaz_parsear_tipo(const char *tipo);

// Ejecutar operacion STDIN: leer del teclado
// Retorna los datos leidos (caller debe liberar)
char *interfaz_stdin(int tamanio, t_log *logger);

// Ejecutar operacion STDOUT: imprimir por pantalla
void interfaz_stdout(const char *datos, int tamanio, t_log *logger);

// Ejecutar operacion SLEEP: dormir por X milisegundos
void interfaz_sleep(int tiempo_ms, t_log *logger);

#endif