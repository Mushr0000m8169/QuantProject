QuantProject – HK Stock Strategy Backtester

A quantitative trading toolkit for Hong Kong equities.
It covers data update → signal generation → backtesting (stock + options) → performance analysis & plotting.



Project Structure

QuantProject/
├── monitorCodeList.csv          # List of HK stock codes to track (e.g. HK.00135)
├── d_history/                   # Daily K-line + indicators (HK00135.csv style)
├── w_history/                   # Weekly K-line + indicators
├── testResult/                  # Stock-only backtest outputs
├── AggressiceFolder/            # Posterior-probability backtest outputs
│
├── dailyUpdate.py               # Fetch / append daily & weekly data via Futu OpenAPI
├── backtester.cpp               # Original multi-stock backtester (C++)
├── AggressiveTester.cpp         # Applied some probability that I've learnt
│
├── cumulative.py                # Cumulative equity plots (stock / option / combined)
└── singlePlot.py                # Interactive single-stock detailed chart

File naming convention
History and result files use the no-dot form: HK00135.csv, HK00135_backtest.csv.



**File naming convention**  
History and result files use the no-dot form: `HK00135.csv`, `HK00135_backtest.csv`.

---

## 1. Data Update (`dailyUpdate.py`)

Pulls daily & weekly K-lines from **Futu OpenAPI** and maintains indicator columns.

**Requirements**
- Futu OpenD running (`127.0.0.1:11111`)
- `pip install futu-api pandas numpy`

**Indicators computed**
| Indicator | Parameters |
|-----------|------------|
| EMA       | 5, 13 |
| KDJ       | 9, 3, 3 |
| Bollinger | 40-period, 2σ |

**Daily append (recommended)**
```bash
python dailyUpdate.py \
  --list monitorCodeList.csv \
  --daily-dir  d_history \
  --weekly-dir w_history \
  --days 60
```

- Only updates **existing** `HKxxxxx.csv` files (does not create new ones)
- Appends new daily bars
- Updates the current (possibly incomplete) weekly bar

**Full refresh**
```bash
python daily_update.py --daily-dir d_history --weekly-dir w_history --days 400 --full
```

---

## 2. Original Strategy Backtester (`backtester.cpp`)

Long-only strategy using Daily Bollinger regime + KDJ + EMA.

### Signal Rules

| Regime | Condition | Entry | Exit |
|--------|-----------|-------|------|
| **Bull** | Close > Daily BB mid | Strict KDJ golden cross (K & J both cross above D) | KDJ death cross |
| **Bear** | Close < Daily BB mid | EMA5 crosses above EMA13 | KDJ death cross |

- Hard **stop-loss: 3%**
- No immediate exit on “price below EMA5”
- Weekly Bollinger is **ignored** (daily only)

### Strict KDJ Cross

```
Golden:  prev D ≥ K and D ≥ J   →   curr K > D and J > D
Death :  prev K ≥ D and J ≥ D   →   curr K < D and J < D
```

### Compile & Run
```bash
g++ -std=c++17 -O2 -o backtester backtester.cpp
./backtester
```

Paths are configured near the top of `main()` (list / d_history / w_history / output folder).

---

## 3. Aggressive Posterior Backtester

Estimates **P(next bar up | current indicator state)** from recent history and trades on that probability.

**State** is a discrete bucket of:
- KDJ regime (oversold / overbought / K>D / K<D)
- J extreme
- Price vs Bollinger mid
- EMA5 vs EMA13

| Action | Rule |
|--------|------|
| Buy  | Posterior ≥ entry threshold (default 0.55) |
| Sell | Posterior ≤ exit threshold (default 0.45) **or** stop-loss **or** time-stop |

```bash
g++ -std=c++17 -O2 -o AggressiveTester AggressiveTester.cpp
./AggressiveTester --daily-dir d_history --list monitorCodeList.csv \
  --out testResult_aggressive --entry 0.55 --exit 0.45 --stop 0.03 --time-stop 8
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--stock-stop` | 0.03 | Stock hard stop |
| `--opt-premium` | 0.05 | Call premium as % of spot |
| `--opt-stop` | 0.50 | Exit after losing 50% of premium |
| `--opt-alloc` | 0.30 | Capital weight on options |
| `--opt-leverage` | 1.0 | Extra multiplier on option P&L |

---

## 5. Plotting & Analysis

### Cumulative returns (`cumulative.py`)
Auto-detects result type:

- **Stock-only** results → single equity curves + portfolio
- **Options** results → stock / option / combined curves + overview

```bash
python cumulative.py
```

### Interactive single-stock chart (`singlePlot.py`)
```bash
python singlePlot.py
# Enter directory → choose file by number / code → detailed 3-panel chart
```
---

## Typical Workflow

```bash
# 0. (optional) run filter.py to get the filtered stock and will write to monitorCodeList.csv
python filter.py

# 1. After market close – append today’s data
python dailyUpdate.py

# 2. Run backtests
./backtester

# 3. Analyse
python cumulative.py testResult
python singlePlot.py
```

---

## Notes

- History files must already exist under `d_history/` / `w_history/` with names like `HK00135.csv`. The updater will not create missing files unless `--full` is used.
- Futu OpenD must be running before `dailyUpdate.py`.
- All C++ tools require C++17.
- The python version is 3.12
- Python scripts need: `pandas`, `numpy`, `matplotlib` (and `futu-api` for data update).

'''bash
pip install pandas
pip install numpy
pip install matplotlib
pip install futu-api
'''

---

## Disclaimer

This project is for research and educational purposes only.  
Past backtest results do not guarantee future performance. Trade at your own risk.
This is a personal trading strategy practice
