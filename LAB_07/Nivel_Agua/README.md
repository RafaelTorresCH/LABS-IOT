# Nivel_Agua

Este perfil queda dedicado al nodo de nivel de agua y bomba sobre `OpenThread + CoAP`.

## Rol

- Se une a la malla como `child`.
- No usa `Wi-Fi`, `MQTT` ni gateway local en este build.
- Expone `GET /nivel` para telemetria.
- Expone `GET /act/valve` y `PUT /act/valve` para consulta y actuacion.

## Telemetria publicada

La respuesta de `GET /nivel` sale en `CBOR` con este contrato:

- `level`: nivel interpretado en porcentaje
- `raw`: estado crudo del sensor
- `pump`: estado actual de la bomba
- `node`: identificador del nodo

## Actuacion

`PUT /act/valve` acepta `CBOR {"v": 0|1}` para apagar o encender la salida de control.

## Pines usados

- `LAB2_WATER_LEVEL_GPIO`: entrada del sensor de nivel
- `LAB2_PUMP_CONTROL_GPIO`: salida al rele o transistor de la bomba

Los valores viven en [main/sensor_gateway_config.h](/home/musicunauta24/esp/LABS-IOT/LAB_07/Nivel_Agua/main/sensor_gateway_config.h).

## Archivos principales

- [main/esp_ot_cli.c](/home/musicunauta24/esp/LABS-IOT/LAB_07/Nivel_Agua/main/esp_ot_cli.c): arranque OpenThread y union como child
- [main/coap_demo.c](/home/musicunauta24/esp/LABS-IOT/LAB_07/Nivel_Agua/main/coap_demo.c): recurso `GET /nivel`
- [main/valve_demo.c](/home/musicunauta24/esp/LABS-IOT/LAB_07/Nivel_Agua/main/valve_demo.c): recurso `PUT /act/valve`
- [main/water_level_pump.c](/home/musicunauta24/esp/LABS-IOT/LAB_07/Nivel_Agua/main/water_level_pump.c): logica local de lectura y fail-safe de la bomba

## Compilacion y flasheo

```bash
cd /home/musicunauta24/esp/LABS-IOT/LAB_07/Nivel_Agua
source /home/musicunauta24/.espressif/v6.0/esp-idf/export.sh
idf.py set-target esp32c6
idf.py -p /dev/ttyACM0 flash monitor
```

## Verificacion minima

1. Confirmar en el monitor que el nodo inicia OpenThread.
2. Confirmar que el rol termina en `child`.
3. Desde Fedora, consultar telemetria:

```bash
coap-client -m get coap://[<ipv6-del-nodo>]/nivel
```

4. Probar actuacion:

```bash
coap-client -m put -e a1617601 coap://[<ipv6-del-nodo>]/act/valve
coap-client -m put -e a1617600 coap://[<ipv6-del-nodo>]/act/valve
```
