"""
Plot pairs / statistical-arbitrage backtest results.

Reads files produced by correlationBacktester:
  correlationResult/*_equity.csv
  correlationResult/pairs_summary.csv

Usage:
    python plotCorrelation.py
"""

import os
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from pathlib import Path

# ===================== CONFIG =====================
CORR_DIR = "/Users/markp/Desktop/QuantProject@3.12/correlationResult"

# Fallback for sandbox
if not os.path.isdir(CORR_DIR):
    CORR_DIR = "/home/workdir/artifacts/correlationResult"

PLOT_DIR = os.path.join(CORR_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)

SUMMARY_CSV = os.path.join(CORR_DIR, "pairs_summary.csv")
# ==================================================


def max_drawdown(equity: pd.Series) -> float:
    if equity.empty:
        return np.nan
    peak = equity.cummax()
    return ((equity - peak) / peak).min()


def load_summary() -> pd.DataFrame:
    if not os.path.exists(SUMMARY_CSV):
        print(f"Summary not found: {SUMMARY_CSV}")
        print("Run correlationBacktester first.")
        return pd.DataFrame()
    df = pd.read_csv(SUMMARY_CSV)
    if df.empty:
        return df
    df = df.sort_values("final_equity", ascending=False).reset_index(drop=True)
    print(f"Loaded summary: {len(df)} pairs")
    print(df[["pair", "n_trades", "win_rate", "final_equity", "max_drawdown"]].head(10).to_string())
    return df


def plot_overall_return():
    """Equal-weight average equity across all pairs → overall portfolio return."""
    files = sorted(glob.glob(os.path.join(CORR_DIR, "*_equity.csv")))
    if not files:
        print("No *_equity.csv found in", CORR_DIR)
        return

    frames = []
    for f in files:
        try:
            df = pd.read_csv(f, parse_dates=["time_key"])
            if "equity" not in df.columns or df.empty:
                continue
            name = Path(f).stem.replace("_equity", "")
            s = df.set_index("time_key")["equity"].rename(name)
            frames.append(s)
        except Exception as e:
            print(f"  skip {f}: {e}")

    if not frames:
        print("No valid equity series.")
        return

    all_eq = pd.concat(frames, axis=1).sort_index()
    overall = all_eq.mean(axis=1, skipna=True)

    # normalise to start at 1.0
    first = overall.first_valid_index()
    if first is not None:
        overall = overall / overall.loc[first]

    fig, ax = plt.subplots(figsize=(14, 7))
    ax.plot(overall.index, overall.values,
            color="#1f77b4", linewidth=2.0,
            label=f"Pairs Overall (final {overall.iloc[-1]:.3f}x)")
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
    ax.set_ylabel("Accumulated Return (start = 1.0)", fontsize=12)
    ax.set_title("Overall Pairs Portfolio Return (Equal-Weight)", fontsize=14, fontweight="bold")
    ax.legend(loc="upper left", fontsize=11)
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    ax.xaxis.set_major_locator(mdates.MonthLocator(interval=3))
    plt.xticks(rotation=45)
    plt.tight_layout()

    out = os.path.join(PLOT_DIR, "00_pairs_overall_return.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Saved: {out}")
    print(f"  Overall final equity: {overall.iloc[-1]:.4f}x")
    print(f"  Overall max drawdown: {max_drawdown(overall)*100:.2f}%")


def plot_summary_bars(summary: pd.DataFrame):
    if summary.empty:
        return

    # Top 20 by final equity
    top = summary.head(20)
    colors = ["#2ecc71" if x >= 1 else "#e74c3c" for x in top["final_equity"]]

    fig, ax = plt.subplots(figsize=(14, 8))
    ax.barh(top["pair"][::-1], top["final_equity"][::-1], color=colors[::-1])
    ax.axvline(1.0, color="black", linestyle="--", linewidth=1, label="Break-even")
    ax.set_xlabel("Final Equity")
    ax.set_title("Pairs Strategy – Top 20 by Final Equity")
    ax.legend()
    plt.tight_layout()
    out = os.path.join(PLOT_DIR, "01_pairs_final_equity_top20.png")
    fig.savefig(out, dpi=150)
    plt.close()
    print(f"Saved: {out}")


def plot_winrate_scatter(summary: pd.DataFrame):
    if summary.empty or "win_rate" not in summary.columns:
        return

    fig, ax = plt.subplots(figsize=(10, 7))
    sc = ax.scatter(summary["win_rate"] * 100, summary["final_equity"],
                    c=summary["n_trades"], cmap="viridis", s=60, alpha=0.8)
    ax.axhline(1.0, color="gray", linestyle="--")
    ax.axvline(50, color="gray", linestyle="--")
    ax.set_xlabel("Win Rate (%)")
    ax.set_ylabel("Final Equity")
    ax.set_title("Pairs: Win Rate vs Final Equity (color = # trades)")
    plt.colorbar(sc, label="# Trades")
    plt.tight_layout()
    out = os.path.join(PLOT_DIR, "02_pairs_winrate_vs_equity.png")
    fig.savefig(out, dpi=150)
    plt.close()
    print(f"Saved: {out}")


def plot_drawdown_hist(summary: pd.DataFrame):
    if summary.empty or "max_drawdown" not in summary.columns:
        return

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.hist(summary["max_drawdown"] * 100, bins=25, color="#e67e22",
            edgecolor="white", alpha=0.85)
    ax.set_xlabel("Max Drawdown (%)")
    ax.set_ylabel("Count")
    ax.set_title("Pairs Strategy – Max Drawdown Distribution")
    plt.tight_layout()
    out = os.path.join(PLOT_DIR, "03_pairs_max_drawdown_hist.png")
    fig.savefig(out, dpi=150)
    plt.close()
    print(f"Saved: {out}")


def plot_top_pair_curves(summary: pd.DataFrame, top_n: int = 5):
    if summary.empty:
        return

    top_pairs = summary.head(top_n)["pair"].tolist()
    fig, ax = plt.subplots(figsize=(14, 7))

    for pair in top_pairs:
        f = os.path.join(CORR_DIR, f"{pair}_equity.csv")
        if not os.path.exists(f):
            continue
        df = pd.read_csv(f, parse_dates=["time_key"])
        ax.plot(df["time_key"], df["equity"],
                label=f"{pair} ({df['equity'].iloc[-1]:.2f}x)", linewidth=1.5)

    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
    ax.set_ylabel("Equity (start = 1.0)")
    ax.set_title(f"Equity Curves – Top {top_n} Pairs")
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    ax.xaxis.set_major_locator(mdates.MonthLocator(interval=3))
    plt.xticks(rotation=45)
    plt.tight_layout()
    out = os.path.join(PLOT_DIR, "04_pairs_top_equity_curves.png")
    fig.savefig(out, dpi=150)
    plt.close()
    print(f"Saved: {out}")


def plot_single_pair(pair: str):
    """Detail chart for one pair: prices, z-score, position, equity."""
    f = os.path.join(CORR_DIR, f"{pair}_equity.csv")
    if not os.path.exists(f):
        # try the signal file if equity not present
        f2 = os.path.join(CORR_DIR, f"{pair}_pair.csv")
        if not os.path.exists(f2):
            print(f"File not found for pair: {pair}")
            return
        f = f2

    df = pd.read_csv(f, parse_dates=["time_key"])

    has_equity = "equity" in df.columns
    n_panels = 4 if has_equity else 3

    fig, axes = plt.subplots(n_panels, 1, figsize=(14, 3 * n_panels), sharex=True,
                             gridspec_kw={"height_ratios": [2.5, 1.5, 1] + ([1.5] if has_equity else [])})

    # 1) two price series (normalised to 1 at start for visual comparison)
    ax = axes[0]
    pa = df["close_a"] / df["close_a"].iloc[0]
    pb = df["close_b"] / df["close_b"].iloc[0]
    ax.plot(df["time_key"], pa, label="Stock A (norm)", color="#1f77b4", linewidth=1.2)
    ax.plot(df["time_key"], pb, label="Stock B (norm)", color="#ff7f0e", linewidth=1.2)
    ax.set_ylabel("Norm. Price")
    ax.set_title(f"{pair} – Prices / Z-Score / Position / Equity")
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(True, alpha=0.3)

    # 2) z-score
    ax = axes[1]
    if "zscore" in df.columns:
        ax.plot(df["time_key"], df["zscore"], color="#9b59b6", linewidth=1.2, label="Z-score")
        ax.axhline(2.0, color="red", linestyle="--", alpha=0.7)
        ax.axhline(-2.0, color="red", linestyle="--", alpha=0.7)
        ax.axhline(0.5, color="gray", linestyle=":", alpha=0.6)
        ax.axhline(-0.5, color="gray", linestyle=":", alpha=0.6)
        ax.axhline(0, color="black", linewidth=0.6)
    ax.set_ylabel("Z-score")
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(True, alpha=0.3)

    # 3) position
    ax = axes[2]
    ax.fill_between(df["time_key"], 0, df["position"], step="mid",
                    color="green", alpha=0.4, label="Position (+1 long ratio / -1 short)")
    ax.set_ylabel("Position")
    ax.set_ylim(-1.3, 1.3)
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(True, alpha=0.3)

    # 4) equity
    if has_equity:
        ax = axes[3]
        ax.plot(df["time_key"], df["equity"], color="#2ecc71", linewidth=1.5, label="Pair Equity")
        ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
        ax.set_ylabel("Equity")
        ax.legend(loc="upper left", fontsize=9)
        ax.grid(True, alpha=0.3)

    axes[-1].xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    plt.tight_layout()
    out = os.path.join(PLOT_DIR, f"detail_{pair}.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Saved: {out}")


if __name__ == "__main__":
    print("=" * 60)
    print("Pairs / Correlation Arbitrage – Plotting")
    print("=" * 60)

    summary = load_summary()

    print("\n[1] Overall portfolio return ...")
    plot_overall_return()

    if not summary.empty:
        print("\n[2] Summary charts ...")
        plot_summary_bars(summary)
        plot_winrate_scatter(summary)
        plot_drawdown_hist(summary)

        print("\n[3] Top pair equity curves ...")
        plot_top_pair_curves(summary, top_n=min(5, len(summary)))

        best = summary.iloc[0]["pair"]
        print(f"\n[4] Detail chart for best pair: {best}")
        plot_single_pair(best)

    print("\nDone.")
    print(f"Plots saved to: {PLOT_DIR}")