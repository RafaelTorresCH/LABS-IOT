# Sensores

Este perfil queda dedicado al nodo de sensores sobre `OpenThread + CoAP`.

## Rol

- Se une a la malla como `child`.
- No usa `Wi-Fi`, `MQTT` ni gateway local en este build.
- Expone `GET /env/temp` por `CoAP` sobre `UDP/IPv6`.

## Telemetria publicada

La respuesta de `GET /env/temp` sale en `CBOR` con este contrato:

- `t_x10`: temperatura del aire en decimas de grado C
- `h_x10`: humedad del aire en decimas de porcentaje
- `soil_x10`: humedad de suelo en decimas de porcentaje
- `node`: identificador del nodo

## Pines usados

- `LAB2_DHT_DATA_GPIO`: DHT11/DHT22
- `LAB2_SOIL_MOISTURE_ADC_GPIO`: salida analogica del sensor de suelo

## Notas de hardware

- El `DHT11` debe llevar `VCC`, `GND` y `DATA` al GPIO configurado.
- Para el `DHT11` es recomendable un pull-up externo de `4.7k` a `10k` entre `DATA` y `3.3V`, aunque el firmware tambien habilita el pull-up interno.
- El sensor de humedad de suelo debe entrar por un GPIO valido de `ADC1` en ESP32-C6 (`GPIO0` a `GPIO6`).
- El firmware actual usa por defecto `GPIO18` para `DHT11` y `GPIO0` para humedad de suelo.

Los valores viven en [main/sensor_gateway_config.h](/home/musicunauta24/esp/LABS-IOT/LAB_07/Sensores/main/sensor_gateway_config.h).

## Archivos principales

- [main/esp_ot_cli.c](/home/musicunauta24/esp/LABS-IOT/LAB_07/Sensores/main/esp_ot_cli.c): arranque OpenThread y union como child
- [main/coap_demo.c](/home/musicunauta24/esp/LABS-IOT/LAB_07/Sensores/main/coap_demo.c): recurso `GET /env/temp`
- [main/sensor_node.c](/home/musicunauta24/esp/LABS-IOT/LAB_07/Sensores/main/sensor_node.c): lectura local de DHT y humedad de suelo

## Compilacion y flasheo

```bash
cd /home/musicunauta24/esp/LABS-IOT/LAB_07/Sensores
source /home/musicunauta24/.espressif/v6.0/esp-idf/export.sh
idf.py set-target esp32c6
idf.py -p /dev/ttyACM0 flash monitor
```

## Verificacion minima

1. Confirmar en el monitor que el nodo inicia OpenThread.
2. Confirmar que el rol termina en `child`.
3. Desde Fedora, consultar el recurso:

```bash
coap-client -m get coap://[<ipv6-del-nodo>]/env/temp
```
