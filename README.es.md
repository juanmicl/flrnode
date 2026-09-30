# FLRNode

**El enlace de radio cifrado para makers: 12 $ por nodo, hasta 1 Mbps y 2 km, sin licencia.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)](LICENSE)
[![CI](https://img.shields.io/github/actions/workflow/status/juanmicl/flrnode/ci.yml?style=for-the-badge&label=CI)](https://github.com/juanmicl/flrnode/actions/workflows/ci.yml)

[English README](README.md)

## Por qué

Las opciones off-grid de hoy te obligan a elegir:

| | Alcance | Ancho de banda |
| --- | --- | --- |
| LoRa 868/915 | varios km 🟢 | 0,3-20 kbps 🔴 |
| WiFi | 30-100 m 🔴 | 100+ Mbps 🟢 |

**FLRNode llena el hueco del medio: de cientos de metros a 2 km, hasta
1 Mbps, cifrado, legal en la UE (y en cualquier sitio con banda ISM de
2,4 GHz).**

Lo consigue con dos transportes en un solo firmware:

- **Nivel ESP-NOW (0 $ extra)**: el ESP32 (o incluso un ESP8266 de 5 $)
  que ya tienes en el cajón. ~100-300 m, latencia de milisegundos.
- **Nivel FLRC (un módulo de 5 $)**: el modo *Fast Long Range
  Communication* del Semtech SX1280, casi olvidado: FSK con FEC turbo
  pensado para carreras de drones. 260/520/1040 kbps, 0,5-2 km. Casi
  nadie lo usa. Nosotros sí.

Sobre ese enlace corre [Reticulum](https://github.com/markqvist/Reticulum),
una pila de red mesh criptográfica: mensajería cifrada extremo a
extremo, transferencia de archivos y más, sin configuración.

## Inicio rápido

Dos WEMOS D1 mini, dos hosts (o un host + una Raspberry Pi):

```sh
git clone https://github.com/juanmicl/flrnode && cd flrnode
sh scripts/install.sh            # instala flrn (CLI host) + rns, y lanza flrn setup
```

Flashea ambos puentes — CADA uno con la MAC del OTRO como par (la
línea de arranque de cada uno imprime la MAC que necesitas; los
comandos exactos por placa están en `TEST-CHECKLIST.md`):

```sh
pio run -e d1_mini_bridge -t upload
```

Y después:

```sh
flrn scan                                   # busca puentes: fw, modo, canal, MAC
flrn stats /dev/ttyUSB0                     # contadores ARQ en vivo (--watch)
flrn config set channel 6 --all-bridges     # aplica y persiste en ambos puentes
flrn test /dev/ttyUSB0 /dev/ttyUSB1         # test de enlace por radio + veredicto
flrn doctor                                 # chequeos de salud con soluciones
```

Requiere: un puente FLRNode por USB (CH340), Python 3.10+ y `rnsd`
corriendo en cada host (`docs/host-setup.md`).
*(M5: el instalador de una línea `curl -sSL flrnode.sh | sh` — hoy,
ejecuta el script desde un clon.)*

## Estado

M1 (transporte ESP-NOW) y M2 (puente Reticulum) están cerrados con
resultados verificados en banco. M3 (CLI `flrn`, este inicio rápido)
está completo en código con suite de integración headless; M4 trae el
nivel FLRC y benchmarks publicados.

Limitación conocida hoy: cada puente se empareja con un solo par —
los enlaces son punto a punto hasta que llegue el soporte multi-par
en M4/M5.

## Hardware soportado

| Placa | Coste | Transportes | Notas |
| --- | --- | --- | --- |
| WEMOS D1 mini (ESP8266) | ~5 $ | ESP-NOW | el enlace cifrado más barato que puedes construir |
| Cualquier ESP32 DevKit | ~7 $ | ESP-NOW (+modo LR) | mejor alcance sin hardware extra |
| LilyGO T3S3 (SX1280) | ~25 $ | ESP-NOW + FLRC | todo en uno, OLED, batería |
| ESP32 + módulo SX1280 suelto | ~12 $ | ESP-NOW + FLRC | cableado DIY: SPI + BUSY + DIO1 |

## Qué construyes con ello

- 🔐 Comms cifradas off-grid: [Sideband](https://github.com/markqvist/Sideband) / [MeshChat](https://github.com/liamcottle/reticulum-meshchat) / [NomadNet](https://github.com/markqvist/NomadNet) por radio
- 🔌 Tubería serie transparente: sustituye un cable entre una finca, un local o un edificio
- 📡 Backbone mesh de alta velocidad: une nodos Reticulum demasiado cerca para la paciencia de LoRa y demasiado lejos para las piernas del WiFi
- 🤖 Telemetría de robots/drones: FLRC se diseñó para esto. Latencia de ms, FEC, 1 Mbps *(roadmap)*

## Cómo funciona

```
[Host A]                                [Host B]
  rnsd ── SerialInterface (HDLC)          rnsd
    │ USB serie @ 921600                   ▲
    ▼                                       │
┌─────────────────────────────┐   ┌────────┴──────────┐
│ Firmware FLRNode            │   │ Firmware FLRNode  │
│  ┌─────────┐  ┌──────────┐  │   │                   │
│  │ HDLC    │◄─┤ Transport│◄─┼───┼─► radio ──────────┤
│  │ framer  │  │ ESP-NOW  │  │ 2,4 GHz ISM          │
│  │ + frag. │  │ o FLRC   │  │   └───────────────────┘
│  └─────────┘  └──────────┘  │
│  LBT CSMA · stats · config  │
└─────────────────────────────┘
```

El núcleo es agnóstico al transporte: una capa de framing, dos radios,
más por venir.

## Benchmarks: medidos, no marketing

Publicaremos números reales de hardware real, para cada velocidad y
distancia, en `docs/benchmarks.md` a medida que los hitos de hardware
completen. Números simulados, nunca.

## Radio y cumplimiento legal

- Banda ISM de 2,4 GHz (2400-2483,5 MHz), **≤ 20 dBm EIRP incluyendo
  antena** según ETSI EN 300 328
- CSMA listen-before-talk siempre activo (equipo adaptativo)
- El firmware rechaza configuraciones que excedan los límites EIRP
- Canal por defecto 2440 MHz (limpio de los bordes WiFi ocupados)
- Las notas completas de cumplimiento viven en `docs/compliance.md`

**FLRNode es legal de operar sin licencia en toda la UE y en cualquier
región con banda ISM de 2,4 GHz.** Consulta siempre tus normas locales.

## Roadmap

- [x] M1: transporte ESP-NOW, enlace de dos nodos
- [x] M2: integración Reticulum (funciona con Sideband/MeshChat)
- [ ] M3: CLI `flrn` + instalador de host de una línea
- [ ] M4: transporte FLRC (SX1280) + benchmarks publicados
- [ ] M5: web flasher + lanzamiento v1.0
- [ ] Puente de interop Meshtastic 2,4 GHz
- [ ] Preset mini-nodo ESP8266 (5 $ extremo a extremo)

## Comparación

| | FLRNode | Meshtastic | RNode |
| --- | --- | --- | --- |
| Ancho de banda | **260k-1 Mbps** | 0,3-20 kbps | 0,3-50 kbps |
| Alcance (típico) | 0,1-2 km | 1-10 km | 1-10 km |
| Cifrado E2E | ✅ (Reticulum) | a nivel de app | ✅ |
| Mesh | ✅ multi-salto | ✅ | ✅ |
| Nodo más barato | **5 $** | 20 $ | 30 $ |
| Funciona con | apps Reticulum | apps Meshtastic | apps Reticulum |

*Somos amigos de todos. Meshtastic y RNode son tu elección para máximo
alcance; FLRNode es tu elección cuando necesitas megabits.*

## Contribuir

Issues, PRs, definiciones de placas, benchmarks y traducciones son
bienvenidos.

## Créditos

- [Mark Qvist](https://github.com/markqvist): Reticulum, LXMF, RNode
  (la razón por la que esto funciona)
- [jgromes](https://github.com/jgromes/RadioLib): RadioLib
- La comunidad Meshtastic, por demostrar que el mesh de 2,4 GHz se
  quiere

## Licencia

MIT. Ver [LICENSE](LICENSE).
