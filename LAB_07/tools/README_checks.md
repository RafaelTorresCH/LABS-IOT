# Fast checks

Use the helper suite to shorten the manual verification loop.

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
