import os
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from pathlib import Path

# ===================== CONFIG =====================
# On your machine this should be:
RESULT_DIR = "/Users/markp/Desktop/QuantProject@3.12/testResult"

# Fallback for sandbox / local testing
if not os.path.isdir(RESULT_DIR):
    RESULT_DIR = "/home/workdir/artifacts/testResult"

SUMMARY_CSV = os.path.join(RESULT_DIR, "summary_all_stocks.csv")
PLOT_DIR = os.path.join(RESULT_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)
# ==================================================


def max_drawdown(equity_series: pd.Series) -> float:
    """Compute maximum drawdown from an equity curve (1.0 start)."""
    if equity_series.empty:
        return np.nan
    peak = equity_series.cummax()
    dd = (equity_series - peak) / peak
    return dd.min()  # negative number


def analyze_one(file_path: str) -> dict:
    """Extract key performance metrics from one backtest result CSV."""
    code = Path(file_path).stem.replace("_backtest", "")
    try:
        df = pd.read_csv(file_path)
    except Exception as e:
        print(f"  [SKIP] {code}: {e}")
        return None

    if df.empty or "equity" not in df.columns:
        return None

    trades = df[df["pnl"].notna() & (df["pnl"] != 0)].copy()
    n_trades = len(trades)
    n_stop = (trades["exit_reason"] == "stop_loss").sum() if n_trades else 0
    n_win = (trades["pnl"] > 0).sum() if n_trades else 0
    win_rate = n_win / n_trades if n_trades else 0.0

    if n_trades:
        total_ret_trades = (1 + trades["pnl"]).prod() - 1
    else:
        total_ret_trades = 0.0

    final_equity = df["equity"].iloc[-1]
    final_bh = df["bh_equity"].iloc[-1]
    mdd = max_drawdown(df["equity"])
    mdd_bh = max_drawdown(df["bh_equity"])
    avg_pnl = trades["pnl"].mean() if n_trades else 0.0
    days_in_pos = (df["position"] == 1).sum()

    return {
        "code": code,
        "n_trades": n_trades,
        "n_stop_loss": int(n_stop),
        "n_wins": int(n_win),
        "win_rate": round(win_rate, 4),
        "avg_trade_pnl": round(avg_pnl, 6),
        "total_ret_from_trades": round(total_ret_trades, 6),
        "final_equity": round(final_equity, 6),
        "final_bh_equity": round(final_bh, 6),
        "excess_return": round(final_equity - final_bh, 6),
        "max_drawdown": round(mdd, 6),
        "max_drawdown_bh": round(mdd_bh, 6),
        "days_in_position": int(days_in_pos),
        "start_date": df["time_key"].iloc[0],
        "end_date": df["time_key"].iloc[-1],
    }


def build_summary():
    pattern = os.path.join(RESULT_DIR, "*_backtest.csv")
    files = sorted(glob.glob(pattern))
    if not files:
        print(f"No *_backtest.csv found in {RESULT_DIR}")
        return None

    print(f"Found {len(files)} backtest files in {RESULT_DIR}")
    records = []
    for f in files:
        rec = analyze_one(f)
        if rec:
            records.append(rec)
            print(f"  {rec['code']}: equity={rec['final_equity']:.3f}x  "
                  f"trades={rec['n_trades']}  win%={rec['win_rate']*100:.1f}")

    if not records:
        print("No valid results.")
        return None

    summary = pd.DataFrame(records)
    summary = summary.sort_values("final_equity", ascending=False).reset_index(drop=True)
    summary.to_csv(SUMMARY_CSV, index=False)
    print(f"\nSummary written to:\n  {SUMMARY_CSV}")
    print(f"Top 5 by final equity:\n{summary[['code','final_equity','win_rate','n_trades']].head()}\n")
    return summary


# ============================================================
# OVERALL / PORTFOLIO RETURN
# ============================================================
def build_overall_equity():
    """
    Build an equal-weight portfolio equity curve by averaging
    the daily equity of every stock that has a backtest file.
    Also builds the corresponding average Buy-&-Hold curve.
    """
    pattern = os.path.join(RESULT_DIR, "*_backtest.csv")
    files = sorted(glob.glob(pattern))
    if not files:
        return None, None

    equity_frames = []
    bh_frames = []

    for f in files:
        try:
            df = pd.read_csv(f, parse_dates=["time_key"])
            if "equity" not in df.columns or df.empty:
                continue
            code = Path(f).stem.replace("_backtest", "")
            eq = df[["time_key", "equity"]].set_index("time_key").rename(columns={"equity": code})
            bh = df[["time_key", "bh_equity"]].set_index("time_key").rename(columns={"bh_equity": code})
            equity_frames.append(eq)
            bh_frames.append(bh)
        except Exception as e:
            print(f"  [SKIP overall] {f}: {e}")

    if not equity_frames:
        return None, None

    # Align all stocks on the union of dates, forward-fill missing (stock not yet listed / already finished)
    all_eq = pd.concat(equity_frames, axis=1).sort_index()
    all_bh = pd.concat(bh_frames, axis=1).sort_index()

    # Equal-weight: mean across available stocks each day
    overall_eq = all_eq.mean(axis=1, skipna=True)
    overall_bh = all_bh.mean(axis=1, skipna=True)

    # Normalise so both start at 1.0 on the first common valid day
    first_valid = overall_eq.first_valid_index()
    if first_valid is not None:
        overall_eq = overall_eq / overall_eq.loc[first_valid]
        overall_bh = overall_bh / overall_bh.loc[first_valid]

    return overall_eq, overall_bh


def plot_overall_return(overall_eq: pd.Series, overall_bh: pd.Series):
    """Plot the overall (equal-weight portfolio) accumulated return."""
    if overall_eq is None or overall_eq.empty:
        print("No overall equity data to plot.")
        return

    fig, ax = plt.subplots(figsize=(14, 7))

    ax.plot(overall_eq.index, overall_eq.values,
            label=f"Strategy Overall (final {overall_eq.iloc[-1]:.3f}x)",
            color="#1f77b4", linewidth=2.0)
    ax.plot(overall_bh.index, overall_bh.values,
            label=f"Buy & Hold Overall (final {overall_bh.iloc[-1]:.3f}x)",
            color="#ff7f0e", linewidth=1.5, linestyle="--", alpha=0.85)

    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
    ax.set_ylabel("Accumulated Return (Equity, start = 1.0)", fontsize=12)
    ax.set_title("Overall Portfolio Return – Equal-Weight across All Stocks",
                 fontsize=14, fontweight="bold")
    ax.legend(loc="upper left", fontsize=11)
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    ax.xaxis.set_major_locator(mdates.MonthLocator(interval=3))
    plt.xticks(rotation=45)
    plt.tight_layout()

    out = os.path.join(PLOT_DIR, "00_overall_return.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Overall return plot saved: {out}")

    # Also print a short summary
    print(f"  Overall Strategy final equity : {overall_eq.iloc[-1]:.4f}x")
    print(f"  Overall B&H final equity      : {overall_bh.iloc[-1]:.4f}x")
    print(f"  Excess                        : {overall_eq.iloc[-1] - overall_bh.iloc[-1]:+.4f}x")


def plot_summary(summary: pd.DataFrame):
    """Generate several overview plots."""
    if summary is None or summary.empty:
        return

    # ----- 1. Final equity bar chart (top 20) -----
    fig, ax = plt.subplots(figsize=(14, 7))
    top = summary.head(20)
    colors = ["#2ecc71" if x >= 1 else "#e74c3c" for x in top["final_equity"]]
    ax.barh(top["code"][::-1], top["final_equity"][::-1], color=colors[::-1])
    ax.axvline(1.0, color="black", linestyle="--", linewidth=1, label="Break-even")
    ax.set_xlabel("Final Equity (multiple of starting capital)")
    ax.set_title("Strategy Final Equity – Top 20 Stocks")
    ax.legend()
    plt.tight_layout()
    p1 = os.path.join(PLOT_DIR, "01_final_equity_top20.png")
    fig.savefig(p1, dpi=150)
    plt.close()
    print(f"Plot saved: {p1}")

    # ----- 2. Win-rate vs Final Equity scatter -----
    fig, ax = plt.subplots(figsize=(10, 7))
    sc = ax.scatter(summary["win_rate"] * 100, summary["final_equity"],
                    c=summary["n_trades"], cmap="viridis", s=60, alpha=0.8)
    ax.axhline(1.0, color="gray", linestyle="--")
    ax.axvline(50, color="gray", linestyle="--")
    ax.set_xlabel("Win Rate (%)")
    ax.set_ylabel("Final Equity")
    ax.set_title("Win Rate vs Final Equity (color = number of trades)")
    plt.colorbar(sc, label="# Trades")
    plt.tight_layout()
    p2 = os.path.join(PLOT_DIR, "02_winrate_vs_equity.png")
    fig.savefig(p2, dpi=150)
    plt.close()
    print(f"Plot saved: {p2}")

    # ----- 3. Excess return distribution -----
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.hist(summary["excess_return"], bins=30, color="#3498db", edgecolor="white")
    ax.axvline(0, color="red", linestyle="--")
    ax.set_xlabel("Excess Return (Strategy Equity - B&H Equity)")
    ax.set_ylabel("Count")
    ax.set_title("Distribution of Excess Return vs Buy-&-Hold")
    plt.tight_layout()
    p3 = os.path.join(PLOT_DIR, "03_excess_return_hist.png")
    fig.savefig(p3, dpi=150)
    plt.close()
    print(f"Plot saved: {p3}")

    # ----- 4. Max Drawdown comparison -----
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.hist(summary["max_drawdown"] * 100, bins=25, color="#e67e22",
            edgecolor="white", alpha=0.8, label="Strategy")
    ax.hist(summary["max_drawdown_bh"] * 100, bins=25, color="#9b59b6",
            edgecolor="white", alpha=0.5, label="Buy & Hold")
    ax.set_xlabel("Max Drawdown (%)")
    ax.set_ylabel("Count")
    ax.set_title("Maximum Drawdown Distribution")
    ax.legend()
    plt.tight_layout()
    p4 = os.path.join(PLOT_DIR, "04_max_drawdown_hist.png")
    fig.savefig(p4, dpi=150)
    plt.close()
    print(f"Plot saved: {p4}")


def plot_equity_curves(top_n: int = 5):
    """Overlay equity curves of the best-performing stocks."""
    pattern = os.path.join(RESULT_DIR, "*_backtest.csv")
    files = sorted(glob.glob(pattern))
    if not files:
        return

    if not os.path.exists(SUMMARY_CSV):
        return
    summary = pd.read_csv(SUMMARY_CSV)
    top_codes = summary.head(top_n)["code"].tolist()

    fig, ax = plt.subplots(figsize=(14, 7))
    for code in top_codes:
        f = os.path.join(RESULT_DIR, f"{code}_backtest.csv")
        if not os.path.exists(f):
            continue
        df = pd.read_csv(f, parse_dates=["time_key"])
        ax.plot(df["time_key"], df["equity"],
                label=f"{code} (final {df['equity'].iloc[-1]:.2f}x)", linewidth=1.5)
        ax.plot(df["time_key"], df["bh_equity"], linestyle="--", alpha=0.4, linewidth=1)

    ax.set_ylabel("Equity (starting = 1.0)")
    ax.set_title(f"Equity Curves – Top {top_n} Stocks (solid = strategy, dashed = buy & hold)")
    ax.legend(loc="upper left")
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    ax.xaxis.set_major_locator(mdates.MonthLocator(interval=3))
    plt.xticks(rotation=45)
    plt.tight_layout()
    p5 = os.path.join(PLOT_DIR, "05_top_equity_curves.png")
    fig.savefig(p5, dpi=150)
    plt.close()
    print(f"Plot saved: {p5}")


def plot_single_stock(code: str):
    """Detailed plot for one stock: price + signals + equity."""
    f = os.path.join(RESULT_DIR, f"{code}_backtest.csv")
    if not os.path.exists(f):
        print(f"File not found: {f}")
        return
    df = pd.read_csv(f, parse_dates=["time_key"])

    fig, axes = plt.subplots(3, 1, figsize=(14, 10), sharex=True,
                             gridspec_kw={"height_ratios": [3, 1.2, 1.5]})

    ax = axes[0]
    ax.plot(df["time_key"], df["close"], label="Close", color="black", linewidth=1)
    if "mid_bb_d" in df.columns:
        ax.plot(df["time_key"], df["mid_bb_d"], label="Daily BB mid", color="blue", alpha=0.6)
    if "mid_bb_w" in df.columns:
        ax.plot(df["time_key"], df["mid_bb_w"], label="Weekly BB mid", color="purple", alpha=0.6)

    buys = df[df["entry_price"].notna()]
    sells = df[df["exit_price"].notna()]
    ax.scatter(buys["time_key"], buys["entry_price"], marker="^", color="green", s=80, zorder=5, label="Entry")
    ax.scatter(sells["time_key"], sells["exit_price"], marker="v", color="red", s=80, zorder=5, label="Exit")
    ax.set_ylabel("Price")
    ax.set_title(f"{code} – Price, Signals & Equity")
    ax.legend(loc="upper left", fontsize=8)
    ax.grid(True, alpha=0.3)

    ax = axes[1]
    ax.fill_between(df["time_key"], 0, df["position"], step="mid", color="green", alpha=0.4)
    ax.set_ylabel("Position")
    ax.set_ylim(-0.1, 1.1)
    ax.grid(True, alpha=0.3)

    ax = axes[2]
    ax.plot(df["time_key"], df["equity"], label="Strategy", color="#2ecc71", linewidth=1.5)
    ax.plot(df["time_key"], df["bh_equity"], label="Buy & Hold", color="#3498db",
            linewidth=1.2, linestyle="--")
    ax.set_ylabel("Equity")
    ax.legend(loc="upper left")
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))

    plt.tight_layout()
    p = os.path.join(PLOT_DIR, f"detail_{code}.png")
    fig.savefig(p, dpi=150)
    plt.close()
    print(f"Detail plot saved: {p}")


if __name__ == "__main__":
    print("=" * 60)
    print("Backtest Summary & Plotting")
    print("=" * 60)

    # 1) Per-stock summary
    summary = build_summary()

    # 2) Overall (equal-weight portfolio) return – the key new plot
    print("\nBuilding overall portfolio equity curve ...")
    overall_eq, overall_bh = build_overall_equity()
    plot_overall_return(overall_eq, overall_bh)

    # 3) Other overview plots
    if summary is not None:
        plot_summary(summary)
        plot_equity_curves(top_n=min(5, len(summary)))

        best = summary.iloc[0]["code"]
        print(f"\nGenerating detailed chart for best stock: {best}")
        plot_single_stock(best)

    print("\nDone.")
    print(f"Summary CSV : {SUMMARY_CSV}")
    print(f"Plots folder: {PLOT_DIR}")
    print("Overall return plot: 00_overall_return.png")