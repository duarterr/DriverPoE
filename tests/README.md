# Automated dimming / power sweep

Drives a DriverPoE board through a list of dimming levels while a **Rigol
DP1308A** feeds its PoE bus, and records the mean power drawn on each
supply rail at every level — the same measurement as the reference table
(*Modo dim / Dimming % / Fonte 1 / Fonte 2 / Total*), automated and
repeatable across many boards.

## Wiring

The PoE bus is fed from the DP1308A's two 25 V rails **in series**:

```
CH2 (+)  ──►  bus +
CH3 (−)  ──►  bus −        bus voltage = CH2 + |CH3|
```

Set the bus voltage in the UI (e.g. `52`); the harness programs each rail
to half of it and turns both outputs on. The board only powers up while
the source is on. "Fonte 1" is CH2, "Fonte 2" is CH3; power per rail is
`mean(|V·I|)` over the samples, and *Total* is their sum.

## Run it

```
cd tests
pip install -r requirements.txt
python server.py            # → http://127.0.0.1:8001/
```

In the page:

1. **Bench connection** — PSU host/IP + port (Rigol raw-socket SCPI,
   default `5555`), *Test PSU* to confirm. Load your admin **keys file**
   (same format as `tools/webui/`, RAM only). *Scan network* to list boards.
2. **Boards** — tick one or more; they are swept one after another.
3. **Driver config & source** — pick a **preset** (`Hibrido 20%`,
   `Adim 60k`, `Pdim 2k, 2us`) or set mode/frequencies/crossover by hand,
   toggle **linearization**, set the **bus voltage** and per-rail current
   limit.
4. **Sweep** — dim levels, settle time, samples per point, sample
   interval.
5. **Play** — applies the driver config to each board, powers the bus,
   waits for PoE, sends `ON`, then for each level: `DIM` → wait *settle* →
   average *samples* readings → record the row. Results stream into the
   table and the log; **Download CSV** when done (also written to
   `tests/results/`).

## Headless

```
python harness.py --psu 192.168.1.50 --board 192.168.1.61 \
    --preset "Hibrido 20%" --lin --keys ../keys.txt
python harness.py --fake            # no hardware — simulated PSU + board loop
```

## SCPI dialect note

The DP1308A predates Rigol's unified DP800 command set. The exact command
strings it accepts (`:APPLy`, `:OUTPut CHx,ON`, `:MEASure:VOLTage? CHx`,
…) are collected as class constants at the top of
[`psu_rigol.py`](psu_rigol.py) — adjust them there if a firmware revision
on your unit differs. Every write is followed by a `:SYSTem:ERRor?` check
so a rejected command fails loudly.

## Files

| file | role |
|------|------|
| `psu_rigol.py` | DP1308A SCPI driver (raw socket) + `FakePSU` |
| `harness.py`   | sweep engine + `SweepRunner` + headless CLI |
| `server.py`    | FastAPI backend (scan, keys, run control, CSV) |
| `static/index.html` | single-page UI |
| `results/`     | CSV output (git-ignored) |
