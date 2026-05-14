# Referencia Rapida Del Proyecto

## Resumen

Proyecto ESP-IDF minimo para un nodo `esp32_esclavo` que usa `ESP-NOW` en modo `STA`.

## Archivo Principal

- `main/main.c`

## Configuracion Actual

- MAC del peer principal: `DC:B4:D9:17:91:04`
- Canal `ESP-NOW`: `11`
- Tag de logs: `espnow_peer`

## Rele En GPIO

- GPIO asignado al rele: `GPIO_NUM_4`
- Estado inicial al arrancar: `apagado`
- Tipo de salida actual: activa en `HIGH`
- Variable de estado usada en firmware: `relay_state`

## Mapa De Pines

| Funcion | GPIO | Estado | Notas |
| --- | --- | --- | --- |
| Rele principal | `GPIO_NUM_4` | Definido | Salida digital, arranca apagado, activo en `HIGH` |
| Wi-Fi / ESP-NOW | N/A | Definido | Usa radio en modo `STA`, canal `11` |
| Otros GPIO | Pendiente | Sin definir | Completar cuando se agreguen sensores, botones o actuadores |

## Comandos ESP-NOW Soportados

- `hola`
  Responde con un `ack`.
- `rele on`
  Enciende el rele.
- `rele off`
  Apaga el rele.
- `rele toggle`
  Alterna el estado del rele.
- `relay on`
- `relay off`
- `relay toggle`

## Notas De Implementacion

- La inicializacion del rele ocurre antes de `wifi_init()`.
- El rele se controla con `gpio_config()` y `gpio_set_level()`.
- Si el modulo real es activo en `LOW`, invertir la logica en `relay_apply()`.
- Si el cableado usa otro pin, cambiar la constante `RELAY_GPIO`.

## Pendientes Posibles

- Confirmar si `GPIO_NUM_4` es el pin fisico correcto.
- Confirmar si el rele es activo en `HIGH` o en `LOW`.
- Completar el mapa de pines a medida que se agreguen perifericos.
- Validar compilacion cuando `idf.py` este disponible en el entorno.
