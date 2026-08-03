#ifndef INSTRUCCIONES_H_
#define INSTRUCCIONES_H_

#include <commons/collections/list.h>
#include <commons/log.h>

// Estructura que almacena las instrucciones de un proceso
typedef struct {
  int pid;
  t_list *lineas; // Lista de char* (cada linea es una instruccion)
} t_programa;

// Inicializar el almacenamiento de instrucciones
void instrucciones_inicializar(void);

// Cargar instrucciones de un archivo para un PID
int instrucciones_cargar(int pid, const char *path_base,
                         const char *nombre_archivo);

// Obtener una instruccion por PID y numero de linea (PC)
char *instrucciones_obtener(int pid, int pc);

// Obtener cantidad de instrucciones de un proceso
int instrucciones_cantidad(int pid);

// Liberar las instrucciones de un proceso
void instrucciones_liberar(int pid);

#endif
