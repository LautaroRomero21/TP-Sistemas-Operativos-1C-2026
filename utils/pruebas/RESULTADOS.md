# Resultados

## Pruebas Finales

Remitirse al contenido del [Documento de Pruebas Finales](https://faq.utnso.com.ar/plug-n-pray-pruebas)

## Pruebas Preliminares

Dentro de este archivo se encontrara una breve descripcion de los resultados esperados en las pruebas.

### Planificacion Preliminar
Esta prueba busca validar que pueden planificar procesos en corto plazo, sin incluir nada relacionado a la memoria.
Debe iniciar todo el TP y conectar 1 sola CPU.
Si el parametro de `SUSPENSION_TIMEOUT` es suficientemente alto no deberian encontrar dicha casuistica. En caso de que quieran validar dicha chasuistica, pueden simplemente poner un valor menor a 20000 y al menos los procesos creados con el script `PLANI_PRE_1.prc` deberian intentar suspenderse.

### Memoria Preliminar
Teniendo conectado al menos 1 memory stick de 256 bytes alcanza para hacer la prueba, el valor del `SEGMENT_MAX_SIZE` debe ser de 128
La idea de esta prueba es crear segmentos, escribir en ellos, leer su contenido y eliminar algunos.
Adicionalmente pueden probar con varios memory stick de menor tamaño para validar los segmentos que se encuentren en varios Memory Stick.
El proceso `MEMORIA_PRE_3.prc` finaliza rapidamente por segmentation fault.