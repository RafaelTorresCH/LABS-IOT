# Guide Completo: Conectar ACM1 como Child al OTBR en ACM0

## Arquitectura
- **ACM0** (ESP32-C6 Mini): OpenThread RCP (Radio Co-Processor)
- **ACM1** (ESP32-C6): FTD Battery Node (Sensores)
- **Fedora**: OTBR host (Border Router + CLI)

## Requisitos Previos
```bash
# En Fedora, verificar que tienes instalado:
which otbr-agent    # Border Router Agent
which ot-ctl        # OpenThread CLI
```

---

## PASO 1: Build y Flash del RCP (ACM0)

### 1.1 Build del RCP
```bash
cd /home/musicunauta24/esp/LABS-IOT/LAB_07
idf.py -B build/main set-target esp32c6
idf.py -B build/main build
```

### 1.2 Flash del RCP
```bash
idf.py -p /dev/ttyACM0 -B build/main flash monitor
```

**Esperado:**
```
I (XXX) ot_esp_rcp: SoilSense Golden Master v...
```

Presiona `Ctrl+]` para salir del monitor.

---

## PASO 2: Build y Flash del FTD Battery (ACM1)

### 2.1 Build del FTD Battery
```bash
cd /home/musicunauta24/esp/LABS-IOT/LAB_07
idf.py -B build/ftd_battery set-target esp32c6
idf.py -B build/ftd_battery -C ftd_battery build
```

### 2.2 Flash del FTD Battery
```bash
idf.py -p /dev/ttyACM1 -B build/ftd_battery -C ftd_battery flash monitor
```

**Esperado:**
```
I (XXX) soilsense_ftd: SoilSense Battery Node v...
I (XXX) soilsense_ftd: Thread role=detached rloc16=0x0000
```

Presiona `Ctrl+]` para salir del monitor.

---

## PASO 3: Iniciar el OTBR en Fedora

### 3.1 Verificar dispositivos conectados
```bash
ls -l /dev/ttyACM*
# Debe mostrar:
# /dev/ttyACM0  - RCP
# /dev/ttyACM1  - FTD Battery
```

### 3.2 Iniciar otbr-agent
```bash
sudo systemctl stop otbr-agent  # Si está corriendo
sudo otbr-agent -I wpan0 "spinel+hdlc+uart:///dev/ttyACM0"
```

**Esperado:**
```
[NCP] = OpenThread NCP v... (RCP from ACM0)
```

### 3.3 En otra terminal, verificar el estado
```bash
ot-ctl status
# Esperado: "leader" o "router"
```

---

## PASO 4: Crear el Dataset de Thread

### 4.1 Si es la primera vez, crear dataset
```bash
ot-ctl dataset init new
ot-ctl dataset commit active
ot-ctl ifconfig up
ot-ctl thread start
```

### 4.2 Esperar 5-10 segundos y verificar estado
```bash
ot-ctl state
# Esperado: "leader"
```

### 4.3 Obtener el dataset activo (IMPORTANTE)
```bash
ot-ctl dataset active -x
# Copiar el valor hexadecimal (ej: 0e080000000000010000000300001835...)
```

---

## PASO 5: Sincronizar Dataset en ACM1

### 5.1 Monitorear ACM1 mientras se conecta
```bash
idf.py -p /dev/ttyACM1 -B build/ftd_battery -C ftd_battery monitor
```

### 5.2 En otra terminal, enviar dataset al FTD
```bash
# Reemplazar con el valor obtenido en 4.3
ot-ctl dataset set active 0e080000000000010000000300001835...
```

**En el monitor ACM1, esperado:**
```
I (XXX) soilsense_ftd: Thread role=child rloc16=0x1001
```

---

## PASO 6: Verificar Conectividad

### 6.1 Verificar topología en OTBR
```bash
ot-ctl child list
# Esperado: 1 child con rloc16=0x1001
```

### 6.2 Obtener direcciones IPv6 del FTD (desde OTBR)
```bash
ot-ctl child <rloc16>
# O lista completa de children
```

### 6.3 Verificar CoAP desde ACM1
En el monitor ACM1:
```
> ot coap resource
 sys/health
```

---

## PASO 7: Validar Reconexión Automática

### 7.1 Desconectar ACM1 del USB
```bash
# El FTD perderá alimentación USB
```

### 7.2 Reconectar ACM1 a otra fuente de alimentación
```bash
# O simplemente reconectar el USB
# El FTD debe rebootear y reconectarse como child automáticamente
```

### 7.3 Verificar reconexión
```bash
idf.py -p /dev/ttyACM1 -B build/ftd_battery -C ftd_battery monitor

# Esperado en 5-10 segundos:
# I (XXX) soilsense_ftd: Thread role=child rloc16=0x1001
```

---

## PASO 8: Test Automatizado (Opcional)

```bash
cd /home/musicunauta24/esp/LABS-IOT/LAB_07
python3 tools/test_lab7_lab8.py lab7
# Verifica topología, CoAP, y bridge
```

---

## Troubleshooting

### ACM1 se queda en "detached"
- Verificar que el dataset está sincronizado: `ot-ctl dataset active -x`
- Verificar que ACM0 (RCP) está respondiendo
- Verificar que OTBR está corriendo: `ps aux | grep otbr-agent`

### ACM1 no aparece como child en `ot-ctl child list`
- Esperar 10-30 segundos (el attachment toma tiempo)
- Verificar RSSI: `ot-ctl child <rloc16>` debe mostrar RSSI negativo

### No hay respuesta CoAP
- Verificar que FTD esté en rol "child": `Thread role=child`
- Verificar conectividad IPv6: `ot-ctl ipaddr` en OTBR debe listar la dirección del FTD

### Reconexión automática no funciona
- Verificar NVS en ACM1: `idf.py -p /dev/ttyACM1 -C ftd_battery menuconfig`
  - `Partition Table` → debe estar habilitada
- Verificar que el código hace `start_thread_from_active_dataset()` antes de `thread start`

---

## Notas Importantes

1. **Dataset compartido**: Ambas placas deben usar el MISMO dataset
2. **Persistencia NVS**: El FTD guarda el dataset en NVS (no necesita reprogramación)
3. **Roles automáticos**: 
   - ACM0 (RCP) = no tiene rol (solo es radio)
   - OTBR = Leader
   - ACM1 (FTD) = Child (forzado en código)

4. **Reconexión automática**:
   - El FTD carga el dataset desde NVS al boot
   - Se reintentan el attachment hasta 3 veces
   - Si falla, espera 15 minutos antes de reintentar
