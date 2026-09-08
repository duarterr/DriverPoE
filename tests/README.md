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

1. **Bench connection** — PSU host/IP + port (the DP1308A Web UI, HTTP,
   default `80`), *Test PSU* to confirm — it echoes a live per-rail
   reading. Load your admin **keys file** (same format as `tools/webui/`,
   RAM only). *Scan network* to list boards.
2. **Boards** — tick one or more; they are swept one after another.
3. **Driver config & source** — pick a **preset** (`Hibrido 20%`,
   `Adim 60k`, `Pdim 2k, 2us`) or set mode/frequencies/crossover by hand,
   toggle **linearization**, set the **bus voltage** and per-rail current
   limit.
4. **Sweep** — dim levels, settle time, samples per point, sample
   interval, **command retries**, and **Run full matrix** (with/without
   linearization × hybrid/analog/PWM — 6 sweeps back to back, one workbook).
5. **Play** — reads the PSU state (only reprograms a rail that is
   off-target), makes sure the outputs are ON (the board only answers on
   the network when powered), then per combo / per board applies the
   driver config, waits for PoE, sends `ON`, and for each level: `DIM` →
   wait *settle* → average *samples* readings → record the row. Any failed
   PSU/board command is retried. Results stream into the table and the
   log; **Download Excel** when done (also written to `tests/results/`).

## Result workbook

`results/<stamp>_<label>.xlsx`, in the layout of the reference
`results/Aquisições_Dimming.xlsx`:

- **Dados** — the full flat table, one row per measured point (every
  column: serials, fw, per-rail V/A/W, samples, timestamp…);
- **NãoLinearizado** / **Linearizado** (only the ones with data) — the
  `Modo dim | Dimming (%) | Fonte 1 (W) | Fonte 2 (W) | Total` table with
  `Total` as a live `=C+D` formula, and a smooth-line scatter chart of
  Total vs Dimming %, one series per driver mode.

## Headless

```
python harness.py --psu 192.168.1.50 --board 192.168.1.61 \
    --preset "Hibrido 20%" --lin --keys ../keys.txt
python harness.py --psu 192.168.1.50 --board 192.168.1.61 \
    --matrix --keys ../keys.txt     # 6 sweeps: lin off/on x hybrid/analog/pwm
python harness.py --fake --matrix   # no hardware — simulated PSU + board loop
python -m psu_rigol 192.168.1.50    # PSU connectivity probe (dumps raw XML)
```

## PSU protocol note

The DP1308A has **no raw-socket SCPI**. This unit is driven through its
legacy **Web Control** protocol: HTTP `POST` of numeric front-panel key
codes to `http://<host>/lxi/infomation.xml`, with an XML status document
in reply. All the key codes are class constants at the top of
[`psu_rigol.py`](psu_rigol.py). Notes:

- the reply is declared `encoding="GB2312"` — decoded manually, the
  `<?xml?>` prolog stripped, before parsing;
- `POST` vs `GET` transport is auto-probed on `connect()`;
- the tiny HTTP server drops connections under load — requests are paced
  and idempotent ones (status reads) retried;
- `ALL ON` / `ALL OFF` raise an "are you sure?" prompt on the panel that
  the driver acknowledges automatically.

## Files

| file | role |
|------|------|
| `psu_rigol.py` | DP1308A legacy Web UI driver + `FakePSU` |
| `harness.py`   | sweep engine + `SweepRunner` + headless CLI |
| `report.py`    | writes the `.xlsx` result workbook (tables + charts) |
| `server.py`    | FastAPI backend (scan, keys, run control, report) |
| `static/index.html` | single-page UI |
| `results/`     | `.xlsx` output (generated ones git-ignored) |
