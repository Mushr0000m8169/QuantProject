# QuantProject — HK Stock Backtesting Framework

C++ portfolio backtester for Hong Kong equities, with Futu OpenAPI data ingestion and Python plotting.

**Current strategy (in `backTester.cpp`):** HK Momentum Trend V4-LS — long stock on EMA stack + ADX + Bollinger mid, optional call overlay, HSI regime filter for new entries only.

---

## Directory layout

```text
QuantProject/
├── monitorCodeList.csv      # universe (one code per line, e.g. HK.00700)
├── d_history/               # daily OHLCV CSVs  (HK00700.csv, HSI.csv, ...)
├── w_history/               # weekly OHLCV CSVs
├── testResult/              # backtest outputs (per-stock CSVs, portfolio equity, trades)
├── correlationResult/       # pair / correlation strategy outputs
│
├── backTester.cpp           # main portfolio engine (edit strategy here)
├── summarizeResults.cpp     # aggregate testResult/*_backtest.csv → summary CSV
├── sentimentMomentum.cpp    # alternate strategy binary
├── correlationTracker.cpp   # rolling correlation tracker
├── correlationBacktester.cpp
│
├── fetch_history.py         # pull daily + weekly bars from Futu OpenD
├── plotResults.py           # equity / signal plots from testResult
├── plotCorrelation.py
├── Makefile
└── README.md
```

---

## Requirements

| Component | Notes |
|-----------|--------|
| **C++17** | `g++` with `-std=c++17` |
| **Python 3** | `pandas`, `numpy`, `matplotlib` |
| **Futu OpenD** | for data updates only (`futu-api`) |

```bash
# Python deps
pip install pandas numpy matplotlib futu-api
```

---

## 1. Universe & data

### Stock list

`monitorCodeList.csv` — one Futu-style code per line:

```text
code
HK.00700
HK.09988
HK.00100
```

Typical screen (via Futu filter): large market cap, lot price band, recent amplitude.

### Download / refresh history

OpenD must be running (`127.0.0.1:11111`).

**First-time full download (~400 calendar days):**

```bash
python3 fetch_history.py \
  --list monitorCodeList.csv \
  --daily-dir d_history \
  --weekly-dir w_history \
  --full --days 400
```

**Daily append (recommended cron / morning job):**

```bash
python3 fetch_history.py \
  --list monitorCodeList.csv \
  --daily-dir d_history \
  --weekly-dir w_history \
  --days 60
```

**Single name:**

```bash
python3 fetch_history.py --codes HK.00700 --full --days 400
```

Files are saved as `HK00700.csv` (dots removed). Optional indicators (EMA5/13, KDJ, Bollinger 40,2) are written into the CSV.

**HSI benchmark:** place index daily bars at `d_history/HSI.csv` (or set `HSI_PATH` in `backTester.cpp`). Columns should include `time_key,open,high,low,close,volume`.

---

## 2. Build

```bash
make              # all C++ binaries
make backtester   # main engine only
```

Or manually:

```bash
g++ -O2 -std=c++17 -Wall backTester.cpp -o backtester
g++ -O2 -std=c++17 -Wall summarizeResults.cpp -o summarizeResults
```

---

## 3. Run backtest

```bash
./backtester
# or
make summarize    # runs backtester then summarizeResults
```

### Outputs (`testResult/`)

| File | Description |
|------|-------------|
| `PORTFOLIO_equity.csv` | Daily portfolio equity, cash, stock/call/put value, position counts |
| `trades.csv` | Every closed trade: code, type, dates, PnL%, MFE, MAE, exit reason |
| `metrics.txt` | CAGR, Sharpe, Sortino, max DD, win rate, profit factor, etc. |
| `summary_all_stocks.csv` | Per-code trade count / sum PnL (also written by `summarizeResults`) |
| `HK*****_backtest.csv` | Per-stock skeleton (compatible with older plot scripts) |

Terminal also prints top/bottom trades and exit-reason breakdown.

### Config knobs (`backTester.cpp`)

Near the top of the file:

```cpp
const string LIST_PATH = "monitorCodeList.csv";
const string D_DIR     = "d_history";
const string OUT_DIR   = "testResult";
const string HSI_PATH  = "d_history/HSI.csv";

const double INIT_CAPITAL      = 1'000'000.0;
const double RISK_PER_TRADE    = 0.015;   // 1.5% risk per trade
const double MAX_PER_STOCK     = 0.12;
const double MAX_PORT_EXPOSURE = 0.90;
const int    MAX_LONGS         = 10;

const double ADX_ENTRY         = 15.0;
const double ATR_MULT          = 2.5;     // stop / trail distance
const double HARD_LOSS         = 0.08;    // hard -8% stop
const double BE_MFE            = 0.08;    // +8% → stop to entry
// ... locks, options toggles, etc.
```

Change strategy logic in the same file, recompile, re-run. Output format is kept stable so `plotResults.py` and `summarizeResults` keep working.

---

## 4. Plot

```bash
make plot
# or
python3 plotResults.py
```

Plots land under `testResult/` (and `testResult/plots/` if configured). Portfolio equity curve uses `PORTFOLIO_equity.csv`.

Correlation strategy:

```bash
make plot-corr
```

---

## 5. Alternate strategies

| Binary / source | Role |
|-----------------|------|
| `backTester.cpp` | **Active** portfolio engine (edit this for live experiments) |
| `sentimentMomentum.cpp` | Sentiment / momentum variant; same CSV schema |
| `correlationTracker.cpp` + `correlationBacktester.cpp` | Pair / mean-reversion correlation pipeline → `correlationResult/` |
| `aggressiveTrend.cpp` | Earlier aggressive trend draft |

```bash
make summarize-sentiment   # build + run sentimentMomentum + summarize
```

To try a new strategy: implement in a `.cpp` file, match the output columns of `*_backtest.csv` / `PORTFOLIO_equity.csv` / `trades.csv`, then either copy into `backTester.cpp` or add a Makefile target.

---

## 6. Strategy snapshot (V4-LS family)

**Long stock entry** (signal on close → fill next open):

- EMA5 > EMA13 > EMA34  
- ADX14 > entry threshold  
- Close > Bollinger middle (20, 2)  
- Optional: ATR% and HSI bull filter for *new* entries only  

**Exits:**

- Initial stop: entry − *k* × ATR  
- Hard loss floor (e.g. −8%)  
- Breakeven after MFE threshold  
- ATR trailing stop after open gains  
- Trend failure: EMA5 < EMA13 and close < EMA34  

**Options (optional):** ATM call overlay on strong breakouts; puts disabled in recent versions after backtests showed large option drag under the simplified intrinsic model.

**Portfolio rules:** risk-based sizing, max positions, max exposure, signal ranking when multiple names fire the same day.

Iterate by changing constants and exit rules; keep trade frequency high if the universe is already pre-filtered (liquid, amplitude screen).

---

## 7. Typical workflow

```bash
# Morning: refresh bars
python3 fetch_history.py --list monitorCodeList.csv --days 60

# Backtest
make summarize

# Charts
make plot

# Inspect
less testResult/metrics.txt
head testResult/trades.csv
```

---

## 8. Makefile targets

| Target | Action |
|--------|--------|
| `make` / `make all` | Build all C++ binaries |
| `make backtester` | Build main engine |
| `make summarize` | Run backtester + summarizeResults |
| `make summarize-sentiment` | Sentiment pipeline + summarize |
| `make plot` | `plotResults.py` |
| `make plot-corr` | Correlation plots |
| `make clean` | Remove build artifacts / generated PNGs (see Makefile) |

---

## 9. Notes & caveats

- **Lookahead:** entries are signalled on day *T* close and filled at *T+1* open.  
- **Option PnL** is a simplified intrinsic + fixed premium model — not a live options chain.  
- **HSI path** must exist if the strategy uses a market regime filter; otherwise longs/puts may always be allowed.  
- Rate-limit Futu requests (`--sleep`); OpenD must stay up during `fetch_history.py`.  
- Past versions (dual-regime, V5–V7 breakout locks, pure option legs) showed that **over-filtering exits** (aggressive time-stops) hurt more than slightly looser entries on this universe.

---

## License / disclaimer

Research and education only. Not investment advice. Past backtest results do not guarantee future performance.

