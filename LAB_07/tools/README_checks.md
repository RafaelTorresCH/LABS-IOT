# Fast checks

Use the helper suite to shorten the manual verification loop.

## Lab 5

Checks:

- OTBR role from `ot-ctl`
- active dataset presence
- OTBR IPv6 addressing
- advertised prefix visibility in `netdata`
- topology table readability
- optional `GET /env/temp` if `SOILSENSE_SENSOR_ADDR` is set

Run:

```bash
python3 tools/test_lab7_lab8.py lab5
```

## Lab 7

Checks:

- OTBR Thread role
- remote router discovery
- `GET /sys/health` live read
- bridge payload formatting

Run:

```bash
python3 tools/test_lab7_lab8.py lab7
```

## Lab 8

Checks:

- release version markers
- `secrets.h` hygiene
- bridge defaults

Run:

```bash
python3 tools/test_lab7_lab8.py lab8
```

## Full sweep

```bash
python3 tools/test_lab7_lab8.py all
```
