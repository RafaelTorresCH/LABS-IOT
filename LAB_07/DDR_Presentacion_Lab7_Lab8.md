# Design & Decision Record (DDR)
**GreenField Technologies | IoT Systems Design**

**Team Members:**
1. María de los Ángeles Prieto Ortega
2. Mariana Zuluaga Yepes
3. Fabian de Jesus Perez Salazar
4. Rafael Ricardo Torres Choperena

**Título del trabajo:** Integración de Thread, OTBR, telemetría operacional y puente hacia dashboard con persistencia histórica

**Curso / Entrega:** Laboratorios 5, 7 y 8

---

# 1. System Overview

* **System Type:** [ ] Component | [ ] System | [x] Environment

## Description

Este documento presenta el estado de diseño, implementación y validación del entorno SoilSense construido sobre una red Thread con nodos ESP32-C6, un Border Router OpenThread sobre Fedora, y una capa de integración host-side para extraer telemetría desde la malla y entregarla a un dashboard externo con almacenamiento histórico en InfluxDB.

La solución separa claramente las responsabilidades por perfil:

- **Mini board ESP32-C6 como RCP:** ejecuta firmware OpenThread RCP y actúa solo como radio del OTBR.
- **Host Fedora con OTBR:** ejecuta `otbr-agent`, forma o adjunta la red Thread y expone el acceso IPv6 a la malla.
- **Nodo FTD de batería:** mide batería, publica estado del nodo y refuerza cobertura como dispositivo Thread de capacidad completa.
- **Nodo de sensores ambientales:** entrega temperatura, humedad del aire y humedad del suelo mediante CoAP.
- **Nodo de nivel de agua / bomba:** entrega nivel de agua y soporta control del actuador.
- **Puentes host-side:** consumen telemetría CoAP/CBOR desde Thread y la convierten a formatos consumibles por dashboard y almacenamiento histórico.

## Objetivo técnico

El objetivo de esta fase fue dejar lista la base operativa para:

- sacar la red Thread de su condición de isla mediante OTBR,
- observar el estado de la red y de los nodos,
- sacar la telemetría fuera de la malla Thread,
- integrar el sistema con un dashboard ya existente sin romper su implementación,
- dejar preparado el andamiaje para el proyecto de consolidación del Lab 8.

---

# 2. Lab Log & Stakeholder Summaries

## LAB 5 — Border Router e integración de redes

### To Daniela (Customer)

El aporte central de esta fase fue dejar de depender de una observación puramente local dentro de la malla Thread. Con el OTBR sobre Fedora, la red de sensores puede exponer sus nodos hacia la red de acceso y permitir consumo desde otros dispositivos sin cambiar los recursos CoAP de los nodos.

En términos prácticos, esto convierte la malla en una red integrable con otros servicios. El recurso deja de ser útil solo para quien está “dentro” del Thread y pasa a poder ser consultado desde fuera, que es la base necesaria para dashboard, histórico y operación remota.

### To Samuel (Architect)

Lab 5 obligó a fijar el punto de frontera arquitectónica: el OTBR no es un dashboard ni una base de datos, sino el gateway entre la proximity network Thread y la access network IP del host. Esa separación fue clave para que Labs 7 y 8 se pudieran construir sobre una base limpia.

La arquitectura quedó alineada con el patrón enterprise networking de ISO/IEC 30141: nodos Thread en proximidad, OTBR como gateway en el borde, servicios externos fuera de la malla y consumo por aplicaciones en otra capa.

## LAB 7 — Operación, telemetría y dashboard

### To Gustavo (Product)

Se implementó la arquitectura base para que la información ya no quede atrapada dentro de la malla Thread. La solución usa un puente en Fedora que consulta recursos CoAP en los nodos Thread, decodifica CBOR y transforma los datos a un formato compatible con el dashboard externo.

La decisión principal de producto fue conservar intacto el dashboard ya funcional en `sample_project-main/` y adaptar el borde del sistema hacia ese contrato, en lugar de reescribir el dashboard. Esto reduce riesgo de integración y permite reemplazar nodos o firmware sin cambiar la interfaz de visualización.

### To Edwin (Ops)

La operación del sistema queda centrada en el host Fedora: allí viven el OTBR, el bridge de telemetría y las pruebas rápidas de validación. Esto facilita diagnosticar si una falla proviene del radio Thread, del join de los nodos, del recurso CoAP o del camino hacia el dashboard.

La telemetría operacional priorizada en esta fase fue batería, uptime, RSSI, rol Thread y direccionamiento IPv6. Estos son los datos mínimos para detectar si un nodo cayó, se reinició, perdió cobertura o está vivo pero degradado.

---

## LAB 8 — Golden Master, endurecimiento y cierre arquitectónico

### To Samuel (Architect)

La implementación se estructuró en perfiles separados para evitar mezclar responsabilidades incompatibles en una misma imagen:

- `main/` para el RCP del OTBR,
- `ftd_battery/` para el nodo FTD de batería,
- `Sensores/` para el nodo de variables ambientales,
- `Nivel_Agua/` para nivel de agua y actuador.

Esta separación permite construir, flashear, probar y evolucionar cada rol de forma independiente. Arquitectónicamente, esto fue importante porque el RCP no debe comportarse como nodo de aplicación, y el nodo FTD de batería no debe cargar lógica de gateway ni de dashboard.

### To Daniela (Customer)

Aunque el usuario final no interactúa con Thread ni con OTBR directamente, esta fase asegura que el sistema pueda crecer hacia una solución de monitoreo agrícola usable: los datos salen de la red de sensores, pueden verse en otra máquina y pueden guardarse históricamente para análisis posterior.

La estrategia elegida favorece continuidad tecnológica y menor dependencia: la red local sigue usando protocolos abiertos como Thread y CoAP, mientras que el dashboard y la persistencia histórica quedan desacoplados a través de un bridge que traduce formatos.

### To Business Manager

Desde la perspectiva de negocio, SoilSense aporta valor porque reduce supervisión manual, concentra la información en una sola interfaz y habilita reacción remota ante eventos relevantes. En lugar de que el operador tenga que revisar nodo por nodo dentro de la malla, el sistema construye una salida consolidada hacia un dashboard y una base histórica.

Esto tiene implicaciones económicas importantes aunque en esta fase no se hayan cerrado todavía todas las cifras finales: menor tiempo de inspección, menor costo de operación manual, mejor trazabilidad para decisiones y una arquitectura modular que protege la inversión porque permite cambiar nodos, ampliar sensores o sustituir el dashboard sin rehacer toda la solución.

---

# 3. Architecture Decision Records (ADRs)

## ADR-005: Usar un OTBR host-side como frontera entre la malla Thread y los consumidores externos

### Context

Los nodos de SoilSense exponen recursos CoAP útiles dentro de Thread, pero el resto del sistema no vive dentro de la red 802.15.4. Sin un Border Router, la malla queda aislada del dashboard, de los servicios de persistencia y de cualquier consumidor fuera del mesh.

### Decision

Se decidió usar un OTBR ejecutándose en Fedora con una mini board ESP32-C6 como RCP dedicado.

### Rationale

- Hace visible la red Thread desde la red del host.
- Conserva la lógica de aplicación en los nodos y deja el borde como función de gateway.
- Prepara el camino para telemetría, dashboard e históricos sin reescribir los recursos CoAP.
- Permite que Lab 7 y Lab 8 se construyan sobre una infraestructura de borde explícita.

### Status

* **Status:** [ ] Proposed | [x] Accepted | [ ] Deprecated

---

## ADR-007: Separar la telemetría operacional del dashboard mediante un bridge host-side

### Context

El dashboard objetivo ya existe y funciona en `sample_project-main/`, incluyendo lógica de visualización y escritura a InfluxDB. Modificarlo profundamente en esta fase habría incrementado el riesgo y el tiempo de integración.

Por otro lado, los nodos Thread publican datos en CoAP/CBOR dentro de una red IPv6 restringida, mientras que el dashboard consume entradas estructuradas desde fuera de la malla.

### Decision

Se decidió implementar un bridge host-side en Fedora que:

- consulte recursos CoAP de los nodos Thread,
- decodifique CBOR,
- normalice la telemetría,
- y entregue un JSON compatible con el dashboard existente.

### Rationale

- Preserva el dashboard existente.
- Reduce el acoplamiento entre firmware embebido y frontend.
- Permite cambiar el destino final sin tocar los nodos.
- Convierte al bridge, y no al dashboard, en el punto de integración arquitectónico.

### Status

* **Status:** [ ] Proposed | [x] Accepted | [ ] Deprecated

---

## ADR-008: Mantener perfiles de firmware separados por rol

### Context

Durante la integración aparecieron necesidades claramente distintas:

- el mini board debe ejecutar OT-RCP y servir exclusivamente al OTBR;
- el nodo de batería debe ser FTD y exponer recursos de salud;
- los nodos de sensores deben enfocarse en observación y control, no en routing del host.

### Decision

Se mantuvieron perfiles separados de firmware y estructura de proyecto por rol.

### Rationale

- Evita confusión de puertos, binarios y responsabilidades.
- Reduce errores de despliegue.
- Facilita pruebas unitarias y pruebas E2E por módulo.
- Refleja mejor la arquitectura real del sistema.

### Status

* **Status:** [ ] Proposed | [x] Accepted | [ ] Deprecated

---

## ADR-009: Conservar CoAP/CBOR dentro de Thread y traducir a JSON fuera de la malla

### Context

Dentro de Thread interesa minimizar overhead de radio y mantener contratos compactos. Fuera de la malla interesa interoperabilidad con herramientas web y persistencia histórica.

### Decision

Usar:

- **CoAP + CBOR** dentro de la malla Thread,
- **JSON** fuera de la malla para integración con dashboard e Influx-side adapters.

### Rationale

- CoAP/CBOR es eficiente para nodos restringidos.
- JSON simplifica integración con frontend y middlewares existentes.
- La traducción en el bridge evita imponer costos de texto a los nodos embebidos.

### Status

* **Status:** [ ] Proposed | [x] Accepted | [ ] Deprecated

---

## ADR-010: Priorizar bajo costo de integración y protección de inversión existente

### Context

El proyecto ya contaba con una carpeta `sample_project-main/` funcional para dashboard y persistencia histórica. Reemplazar o rehacer ese componente habría incrementado tiempo, costo de integración y riesgo técnico.

### Decision

Se decidió integrar SoilSense con el dashboard existente mediante adaptación en el borde, manteniendo intacta la carpeta de aplicación principal siempre que fuera posible.

### Rationale

- Disminuye costo de desarrollo incremental.
- Reduce riesgo de regresión sobre un sistema ya funcional.
- Protege la inversión en frontend, almacenamiento e interfaz de usuario.
- Permite que el valor nuevo provenga de la red Thread y del bridge, no de rehacer capas que ya cumplen su objetivo.

### Status

* **Status:** [ ] Proposed | [x] Accepted | [ ] Deprecated

---

# 4. ISO/IEC 30141 Mapping

## Domain Mapping

| Component | ISO Domain | Justification | Estado |
|-----------|------------|---------------|--------|
| ESP32-C6 mini con OT-RCP | SCD | Subsistema de comunicación del borde Thread | Implementado |
| `otbr-agent` en Fedora | SCD-hosted gateway | Puente entre la red Thread y la red del host | Implementado |
| Nodo FTD de batería | SCD | Nodo de observación y routing interno de la malla | Implementado |
| Nodo de sensores ambientales | SCD + ASD endpoint | Dispositivo sensor con recursos CoAP de aplicación | Implementado |
| Nodo de nivel de agua / bomba | SCD + ASD endpoint | Sensor/actuador con recurso de lectura y control | Implementado |
| Recurso `/sys/health` | OMD / ASD | Expone salud operativa del nodo | Implementado |
| Recurso `/env/temp` | ASD | Expone lectura ambiental | Implementado |
| Recurso `/nivel` | ASD | Expone nivel de agua | Implementado |
| Bridge CoAP→JSON | RAID interchange subsystem | Fusiona y traduce información para consumidores externos | Implementado |
| Dashboard externo | OMD / Usage viewpoint | Interfaz para operación y visualización del sistema | Integrado por contrato |
| InfluxDB | Data storage support | Persistencia histórica de telemetría | Integrado por contrato |

## Viewpoint emphasis by lab

| Lab | Viewpoint / Lens | Evidence in this project |
|-----|------------------|--------------------------|
| Lab 5 | Enterprise system pattern + enterprise networking pattern | OTBR, frontera Thread↔host, validación de dataset/prefix/topología |
| Lab 7 | Usage + OMD + RAID Interchange | Bridge de telemetría, dashboard target, chequeos de operación |
| Lab 8 | Business + Construction + Trustworthiness | Separación de perfiles, pruebas automáticas, preparación para endurecimiento |

---

# 5. First Principles Reflections

## Lab 5

- **El Border Router no cambia el recurso, cambia el alcance.** `/env/temp` y `/nivel` siguen siendo recursos CoAP; lo nuevo es que dejan de quedar encerrados en la malla.
- **La red no se vuelve accesible por “magia”.** El valor del OTBR está en exponer prefijos, rutas y conectividad entre redes distintas.
- **La frontera importa arquitectónicamente.** El OTBR pertenece al borde de comunicación, no a la lógica del dashboard ni del nodo sensor.

## Lab 7

- **La telemetría no es el dato de negocio.** Temperatura, humedad o nivel sirven al cultivo; batería, RSSI y uptime sirven a operación y mantenimiento.
- **El bridge es parte de la arquitectura, no un parche.** El sistema necesita una frontera explícita entre red restringida y consumo externo.
- **El dashboard no debe imponer su formato a la malla.** La malla optimiza energía y radio; la interfaz optimiza interoperabilidad.

## Lab 8

- **Separar roles reduce complejidad accidental.** Un RCP no debe cargar lógica de nodo de aplicación.
- **Disponibilidad es una propiedad de negocio.** Si OTBR cae o un nodo no logra adjuntarse, el agricultor no recibe servicio.
- **No agregar features innecesarias fue una decisión de ingeniería.** Primero se estabiliza el camino Thread → OTBR → bridge → dashboard.

---

# 6. Performance Baselines

## Baselines definidos para la entrega

| Métrica | Objetivo | Estado actual |
|---------|----------|---------------|
| Formación de red OTBR | Border Router operativo y accesible por `ot-ctl` | Parcialmente validado |
| Dataset / prefix OTBR | Dataset activo y prefijos visibles en `netdata` | Implementado, validación final pendiente |
| Join de nodo FTD | Nodo visible en topología Thread | Validado en sesiones previas |
| Resolución CoAP local | `GET /sys/health`, `/env/temp`, `/nivel` desde Fedora | Implementado, validación final pendiente |
| Traducción a JSON | Datos listos para dashboard externo | Implementado |
| Persistencia histórica | Compatible con flujo hacia InfluxDB | Implementado por contrato |
| Pruebas repetibles | Suite de checks Lab 7 / Lab 8 | Implementado |

## Estado de medición

En esta iteración se dejó automatizada la comprobación funcional, pero no todas las métricas finales de estrés y recuperación quedaron cerradas con evidencia definitiva, principalmente por inestabilidad intermitente del OTBR durante pruebas de hardware. Por ello, este DDR distingue explícitamente entre:

- **implementado,**
- **validado localmente,**
- **y pendiente de validación final en hardware.**

---

# 7. Ethics & Sustainability Checklist

## Data minimization

Se priorizó exponer únicamente los datos necesarios para la operación:

- batería,
- RSSI,
- uptime,
- rol del nodo,
- direccionamiento o identificadores operativos,
- variables de sensores necesarias para el dashboard.

No se introdujeron datos personales ni de geolocalización avanzada en esta fase.

## Sustainability

- El uso de CoAP/CBOR dentro de Thread reduce consumo de red comparado con formatos más pesados.
- Mantener el dashboard sin cambios evita retrabajo y desperdicio de desarrollo.
- La separación por perfiles facilita mantenimiento y extensión futura sin reescribir toda la solución.

## Transparency

El flujo de datos queda claramente documentado:

1. los nodos producen datos dentro de la malla,
2. Fedora los consume vía CoAP,
3. el bridge los transforma,
4. el dashboard los visualiza,
5. InfluxDB conserva el histórico.

Esto es importante para que el usuario sepa en qué punto los datos abandonan la red local Thread.

---

# 8. Viewpoint Analysis

| Viewpoint | Labs Addressed | Key Concerns | Respuesta en SoilSense |
|-----------|----------------|--------------|------------------------|
| Functional | Labs 1-4, 7-8 | sensado, control, comunicación | recursos CoAP por nodo y perfiles separados |
| System | Labs 5-8 | integración entre redes y servicios | OTBR + host Fedora + bridge + dashboard |
| Usage | Lab 7 | operación, monitoreo, salud de flota | dashboard y telemetría operacional |
| Trustworthiness | Labs 6-8 | seguridad, resiliencia, fallas | CoAP/Thread seguros, endurecimiento progresivo |
| Business | Lab 8 | valor del sistema, mantenibilidad, costo operativo | reutilización del dashboard y desacoplamiento mediante bridge |
| Construction | Lab 8 | qué se construyó realmente | perfiles separados, contratos CoAP, tools de integración y pruebas |

---

# 9. Business Viewpoint

## 9.1 Stakeholders de negocio

| Stakeholder | Interés principal en este proyecto |
|-------------|------------------------------------|
| Business Manager | generación de valor, reducción de costos operativos, escalabilidad del servicio |
| System Owner | disponibilidad, mantenimiento, sostenibilidad y protección de la inversión |
| Architect | modularidad, interoperabilidad, extensibilidad y coherencia técnica |
| Usuario final | monitoreo simple, alertas, históricos y control remoto |

## 9.2 Business concerns

Este trabajo responde a tres preocupaciones centrales del Business Viewpoint:

1. cómo las capacidades del sistema generan valor real para la operación,
2. cómo la arquitectura habilita nuevos servicios y crecimiento futuro,
3. cómo las decisiones técnicas impactan costo, riesgo y sostenibilidad del sistema.

## 9.3 Objetivos de negocio

- Reducir la necesidad de supervisión manual continua.
- Centralizar la información del sistema en una única interfaz.
- Permitir control remoto de actuadores cuando sea necesario.
- Mantener históricos para análisis y trazabilidad.
- Aumentar la disponibilidad percibida del servicio mediante observabilidad y red mallada.
- Proteger la inversión usando protocolos abiertos y una arquitectura modular.
- Habilitar crecimiento futuro con nuevos sensores, nodos o dashboards sin rediseño completo.

## 9.4 Variables económicas y de costo relevantes

En este caso, las variables económicas más pertinentes no son solo el costo del hardware, sino el costo total de operación e integración:

| Variable económica | Relevancia en SoilSense | Impacto esperado |
|--------------------|-------------------------|------------------|
| Costo de hardware por nodo | uso de ESP32-C6 y componentes de laboratorio | contenido y replicable |
| Costo de integración | reutilización del dashboard existente | menor retrabajo y menor riesgo |
| Costo de supervisión manual | inspecciones físicas y revisión nodo a nodo | disminuye con dashboard centralizado |
| Costo por falla operativa | pérdida de visibilidad, decisiones tardías, posibles eventos no detectados | disminuye con telemetría operacional |
| Costo de escalamiento | agregar nuevos nodos o servicios | menor por modularidad y protocolos abiertos |
| Costo de mantenimiento futuro | actualizaciones, cambios de firmware, reemplazo de nodos | menor al separar perfiles y contratos |
| Costo de dependencia tecnológica | quedar atado a una plataforma propietaria | menor por usar Thread, CoAP, MQTT e InfluxDB |

## 9.5 Propuesta de valor

La propuesta de valor de SoilSense es convertir una red de nodos IoT restringidos en un servicio observable y operable desde fuera de la malla, sin perder eficiencia en el borde.

El sistema aporta valor porque:

- transforma lecturas distribuidas en información centralizada,
- permite detectar estado del sistema y no solo del cultivo,
- habilita control remoto,
- conserva históricos para análisis,
- y prepara una base real para operación escalable.

## 9.6 Implicaciones de negocio

### Eficiencia operativa

La arquitectura reduce la necesidad de conectarse manualmente a cada nodo para inspeccionar estado o lecturas. Esto ahorra tiempo de operación y reduce fricción de soporte.

### Visibilidad del sistema

El dashboard y la telemetría operacional permiten saber no solo qué mide el sistema, sino si el sistema mismo está sano. Eso mejora la capacidad de respuesta y reduce incertidumbre operativa.

### Toma de decisiones basada en datos

La salida hacia InfluxDB aporta trazabilidad e históricos, lo que permite analizar comportamiento y justificar decisiones con datos acumulados.

### Escalabilidad

El uso de Thread, CoAP, MQTT y una capa de bridge desacoplada facilita incorporar nuevos nodos o nuevos consumidores externos sin reconstruir la arquitectura.

### Interoperabilidad

La elección de tecnologías abiertas protege la inversión y evita dependencia innecesaria de soluciones cerradas.

### Integración de redes

La presencia del OTBR evita que el sistema IoT quede encapsulado en una red local sin salida útil. Desde negocio, esto es importante porque un sistema que no puede integrarse con otros servicios tiene menor valor operativo y menor posibilidad de evolución.

## 9.7 Riesgos de negocio

| Riesgo | Impacto posible | Estado |
|--------|-----------------|--------|
| Inestabilidad del OTBR | interrupción del acceso entre malla y servicios externos | identificado |
| Falla de sensores o actuadores | decisiones erróneas o pérdida de control | identificado |
| Falla del dashboard o base histórica | pérdida de visualización o trazabilidad temporal | identificado |
| Saturación o mala integración | degradación del servicio y retrasos | mitigado parcialmente con separación por capas |
| Dependencia de un único host Fedora | punto fuerte de operación, pero también punto crítico | identificado |

## 9.8 Estrategias de mitigación

- Separar firmwares por rol para reducir errores de despliegue.
- Mantener el dashboard estable y adaptar el borde mediante un bridge.
- Usar pruebas automáticas para repetir validaciones.
- Documentar contratos y puertos de despliegue.
- Migrar a identificadores seriales persistentes `by-id` para reducir errores operativos.
- Completar validación final de estabilidad OTBR antes del cierre definitivo.

---

# 10. Trustworthiness Audit

## Closed or improved gaps

- Se formalizó un canal de telemetría operacional para detectar estado del nodo.
- Se preparó una ruta de integración externa sin exponer directamente la malla al dashboard.
- Se añadieron pruebas automatizadas para revalidar funcionalidad base.

## Remaining gaps

- La estabilidad del OTBR todavía requiere validación final sostenida en hardware real.
- La evidencia final de recuperación automática y pruebas de caos de Lab 8 aún debe completarse con todas las placas disponibles.
- El endurecimiento total del camino de actuación debe cerrarse junto con la validación E2E completa.

## Honest assessment

El sistema está bien encaminado a nivel de arquitectura y software de integración, pero no debe presentarse como “cerrado al 100%” hasta completar la validación final del OTBR y de la red Thread con todos los nodos activos simultáneamente.

---

# 11. Construction Viewpoint - IoT System Pattern

| Pattern Element | Category | SoilSense Implementation |
|---|---|---|
| IoT System | — | SoilSense Lab 7 / Lab 8 integration scaffold |
| IoT Components | Physical entities | 1 mini RCP, 1 host Fedora, 1 FTD battery node, 1 sensor node, 1 water-level / pump node |
| Digital Network | Connectivity | Thread over 802.15.4 + host Wi-Fi/Ethernet |
| IoT Devices | Hardware | ESP32-C6 family boards |
| Primary Capability | Physical observation | temperatura, humedad ambiente, humedad de suelo, nivel de agua, batería |
| Primary Capability | Control of entities | control de bomba / válvula |
| Secondary Capability | Data processing | decodificación CBOR, normalización a JSON, health reporting |
| Secondary Capability | Data transferring | CoAP dentro de Thread, bridge host-side hacia dashboard |
| Secondary Capability | Data storage | InfluxDB en el sistema de dashboard |
| Interface | Network | IEEE 802.15.4, Thread IPv6, Wi-Fi del host |
| Interface | Human UI | dashboard web externo |
| Interface | Application | `/sys/health`, `/env/temp`, `/nivel`, contrato JSON para dashboard |
| Supplemental | Security | configuración Thread, separación de perfiles, base para endurecimiento |
| Supplemental | Orchestration | OTBR en Fedora, `otbr-agent`, serial RCP, topología Thread |
| Supplemental | Management | scripts de prueba, documentación, bridge de telemetría |

---

# 12. Estado actual de la implementación

## Implementado en el repositorio

- Firmware RCP para la placa mini en `main/`.
- Firmware FTD de batería en `ftd_battery/`.
- Nodo de sensores en `Sensores/`.
- Nodo de nivel de agua y bomba en `Nivel_Agua/`.
- Puente `tools/coap_to_influx.py`.
- Puente `tools/thread_to_scada_bridge.py`.
- Suite de validación `tools/test_lab7_lab8.py`.
- Pruebas del bridge `tools/test_scada_bridge.py`.

## Validado

- Compilación separada por perfiles.
- Flujo de pruebas estáticas y unitarias del bridge.
- Contrato de integración con el dashboard existente.
- Sesiones previas de join Thread con nodos visibles desde OTBR.

## Pendiente de validación final

- Estabilidad sostenida de `otbr-agent`.
- Validación de los tres nodos de aplicación simultáneamente como children/router según perfil.
- Medición final de latencias y drills de Lab 8 con evidencia cerrada.

---

# 13. Conclusiones para la presentación

1. El trabajo no se limitó a “conectar placas”; se diseñó una arquitectura IoT por capas con separación clara entre red restringida, borde y consumo externo.
2. Lab 5 quedó incorporado como base estructural: el OTBR define la frontera entre la malla y el resto del sistema.
3. La decisión más importante fue no romper el dashboard existente, sino integrar la malla Thread hacia él mediante un bridge explícito y mantenible.
4. El sistema ya tiene base real para observabilidad, persistencia histórica e integración futura con el proyecto final.
5. Desde el punto de vista económico, el valor principal está en reducir supervisión manual, reutilizar infraestructura existente y proteger la inversión con una arquitectura modular y abierta.
6. El principal punto abierto no es de diseño sino de estabilización final del OTBR y validación hardware completa.
7. Desde el punto de vista docente, este trabajo sí responde a los ejes de Lab 5, 7 y 8: enterprise networking pattern, OMD, Usage, RAID interchange, Business viewpoint y Construction viewpoint.

---

# 14. Recomendaciones de cierre

- Completar una corrida final de validación OTBR con puertos seriales fijados por `by-id`.
- Registrar evidencia de `ot-ctl state`, `child table`, `router table` y lecturas CoAP de cada nodo.
- Tomar capturas del flujo hacia el dashboard y del almacenamiento histórico en InfluxDB.
- Añadir al cierre de la presentación una diapositiva breve de “riesgos abiertos y siguiente paso”.
