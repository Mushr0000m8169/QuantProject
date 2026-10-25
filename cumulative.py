#!/usr/bin/env python3
"""
Plot cumulative (accumulating) strategy return for all stocks.

Supports both:
  1. Original backtester results  → column "equity"
  2. Options backtester results   → columns "stock_equity", "opt_equity", "combined_equity"

Usage:
    python plot_cumulative_return.py
    python plot_cumulative_return.py /path/to/testResult_options
"""

import os
import sys
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from pathlib import Path


DEFAULT_DIR = "/Users/markp/Desktop/QuantProject@3.12/testResult"
if not os.path.isdir(DEFAULT_DIR):
    DEFAULT_DIR = "/home/workdir/artifacts/testResult"


def detect_mode(directory: str) -> str:
    """Return 'options' if combined_equity exists, else 'stock'."""
    files = glob.glob(os.path.join(directory, "*_backtest.csv"))
    if not files:
        return "stock"
    try:
        cols = pd.read_csv(files[0], nrows=0).columns.tolist()
        if "combined_equity" in cols or "opt_equity" in cols:
            return "options"
    except Exception:
        pass
    return "stock"


def load_curves(directory: str, column: str) -> pd.DataFrame:
    """Load one equity column from every *_backtest.csv."""
    files = sorted(glob.glob(os.path.join(directory, "*_backtest.csv")))
    if not files:
        print(f"No *_backtest.csv found in {directory}")
        return None

    curves = {}
    for f in files:
        code = Path(f).stem.replace("_backtest", "")
        try:
            df = pd.read_csv(f, parse_dates=["time_key"])
            df = df.set_index("time_key").sort_index()
            if column in df.columns:
                curves[code] = df[column]
            elif column == "equity" and "strategy_ret" in df.columns:
                curves[code] = (1 + df["strategy_ret"].fillna(0)).cumprod()
        except Exception as e:
            print(f"  skip {code}: {e}")

    if not curves:
        return None
    out = pd.DataFrame(curves).sort_index()
    return out


def plot_single_mode(equity_df: pd.DataFrame, title: str, save_path: str, color_port="#27ae60"):
    """Original-style 2-panel cumulative plot for one equity series set."""
    port = equity_df.mean(axis=1)

    fig, axes = plt.subplots(2, 1, figsize=(15, 10), sharex=True,
                             gridspec_kw={"height_ratios": [3, 1.3]})

    ax = axes[0]
    for col in equity_df.columns:
        ax.plot(equity_df.index, equity_df[col], alpha=0.4, linewidth=0.85, label=col)
    ax.plot(port.index, port.values, color="black", linewidth=2.4,
            label="Equal-weight Portfolio", zorder=10)
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.9)
    ax.set_ylabel("Cumulative Equity (start = 1.0)")
    ax.set_title(title, fontsize=14, fontweight="bold")
    ax.legend(loc="upper left", fontsize=7, ncol=4, framealpha=0.9)
    ax.grid(True, alpha=0.3)

    ax = axes[1]
    ax.plot(port.index, port.values, color=color_port, linewidth=2.0, label="Portfolio")
    ax.fill_between(port.index, 1.0, port.values,
                     where=port.values >= 1.0, color=color_port, alpha=0.25)
    ax.fill_between(port.index, 1.0, port.values,
                     where=port.values < 1.0, color="#c0392b", alpha=0.25)
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.9)
    ax.set_ylabel("Portfolio Equity")
    ax.legend(loc="upper left")
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    ax.xaxis.set_major_locator(mdates.MonthLocator(interval=2))
    plt.xticks(rotation=35)
    plt.tight_layout()
    fig.savefig(save_path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Plot saved → {save_path}")
    return port


def plot_options_overview(stock_df, opt_df, comb_df, save_dir: str):
    """3-panel overview: stock / option / combined portfolio curves."""
    port_s = stock_df.mean(axis=1) if stock_df is not None else None
    port_o = opt_df.mean(axis=1) if opt_df is not None else None
    port_c = comb_df.mean(axis=1) if comb_df is not None else None

    fig, axes = plt.subplots(3, 1, figsize=(15, 12), sharex=True)

    # Stock
    ax = axes[0]
    if stock_df is not None:
        for col in stock_df.columns:
            ax.plot(stock_df.index, stock_df[col], alpha=0.3, linewidth=0.7)
        if port_s is not None:
            ax.plot(port_s.index, port_s.values, color="#2980b9", linewidth=2.2, label="Stock Portfolio")
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
    ax.set_ylabel("Stock Equity")
    ax.set_title("Options Backtest – Cumulative Returns", fontsize=14, fontweight="bold")
    ax.legend(loc="upper left")
    ax.grid(True, alpha=0.3)

    # Option
    ax = axes[1]
    if opt_df is not None:
        for col in opt_df.columns:
            ax.plot(opt_df.index, opt_df[col], alpha=0.3, linewidth=0.7)
        if port_o is not None:
            ax.plot(port_o.index, port_o.values, color="#e67e22", linewidth=2.2, label="Option (Call) Portfolio")
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
    ax.set_ylabel("Option Equity")
    ax.legend(loc="upper left")
    ax.grid(True, alpha=0.3)

    # Combined
    ax = axes[2]
    if port_s is not None:
        ax.plot(port_s.index, port_s.values, color="#2980b9", linewidth=1.5, linestyle="--", label="Stock only")
    if port_o is not None:
        ax.plot(port_o.index, port_o.values, color="#e67e22", linewidth=1.5, linestyle="--", label="Option only")
    if port_c is not None:
        ax.plot(port_c.index, port_c.values, color="#27ae60", linewidth=2.4, label="Combined Portfolio")
        ax.fill_between(port_c.index, 1.0, port_c.values,
                         where=port_c.values >= 1.0, color="#27ae60", alpha=0.2)
        ax.fill_between(port_c.index, 1.0, port_c.values,
                         where=port_c.values < 1.0, color="#c0392b", alpha=0.2)
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=0.8)
    ax.set_ylabel("Combined Equity")
    ax.legend(loc="upper left")
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    ax.xaxis.set_major_locator(mdates.MonthLocator(interval=2))
    plt.xticks(rotation=35)
    plt.tight_layout()

    out = os.path.join(save_dir, "cumulative_options_overview.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Overview plot saved → {out}")


def print_ranking(df: pd.DataFrame, label: str):
    if df is None or df.empty:
        return
    final = df.iloc[-1].sort_values(ascending=False)
    print(f"\n{'='*50}")
    print(f"  {label} – Final Ranking")
    print(f"{'='*50}")
    for i, (code, eq) in enumerate(final.items(), 1):
        print(f"  {i:2d}. {code:<12s}  {eq:.4f}x   ({(eq-1)*100:+.2f}%)")
    port = df.mean(axis=1).iloc[-1]
    print(f"{'-'*50}")
    print(f"  Portfolio (equal-weight) : {port:.4f}x   ({(port-1)*100:+.2f}%)")
    print(f"{'='*50}")


def save_ranking_csv(df: pd.DataFrame, path: str):
    if df is None or df.empty:
        return
    final = df.iloc[-1].sort_values(ascending=False)
    rank = pd.DataFrame({
        "code": final.index,
        "final_equity": final.values,
        "total_return_pct": (final.values - 1) * 100
    })
    rank.to_csv(path, index=False)
    print(f"Ranking CSV → {path}")


def main():
    if len(sys.argv) > 1:
        directory = sys.argv[1]
    else:
        print(f"Default directory: {DEFAULT_DIR}")
        user = input("Enter directory (press Enter for default): ").strip()
        directory = user if user else DEFAULT_DIR

    if not os.path.isdir(directory):
        print(f"Directory not found: {directory}")
        sys.exit(1)

    mode = detect_mode(directory)
    print(f"Detected mode: {mode.upper()}")
    print(f"Loading from: {directory}")

    if mode == "options":
        stock_df = load_curves(directory, "stock_equity")
        opt_df   = load_curves(directory, "opt_equity")
        comb_df  = load_curves(directory, "combined_equity")

        if stock_df is not None:
            print(f"Stock curves : {stock_df.shape[1]} stocks, "
                  f"{stock_df.index.min().date()} → {stock_df.index.max().date()}")
        if opt_df is not None:
            print(f"Option curves: {opt_df.shape[1]} stocks")
        if comb_df is not None:
            print(f"Combined     : {comb_df.shape[1]} stocks")

        # individual plots
        if stock_df is not None:
            plot_single_mode(stock_df,
                             "Stock Leg – Cumulative Return",
                             os.path.join(directory, "cumulative_stock.png"),
                             color_port="#2980b9")
            print_ranking(stock_df, "STOCK")
            save_ranking_csv(stock_df, os.path.join(directory, "ranking_stock.csv"))

        if opt_df is not None:
            plot_single_mode(opt_df,
                             "Option (Call) Leg – Cumulative Return",
                             os.path.join(directory, "cumulative_option.png"),
                             color_port="#e67e22")
            print_ranking(opt_df, "OPTION (CALL)")
            save_ranking_csv(opt_df, os.path.join(directory, "ranking_option.csv"))

        if comb_df is not None:
            plot_single_mode(comb_df,
                             "Combined (Stock + Option) – Cumulative Return",
                             os.path.join(directory, "cumulative_combined.png"),
                             color_port="#27ae60")
            print_ranking(comb_df, "COMBINED")
            save_ranking_csv(comb_df, os.path.join(directory, "ranking_combined.csv"))

        # overview with all three portfolios
        plot_options_overview(stock_df, opt_df, comb_df, directory)

        # show
        plt.show()

    else:
        # original single-equity mode
        equity_df = load_curves(directory, "equity")
        if equity_df is None:
            # try combined_equity as fallback name
            equity_df = load_curves(directory, "combined_equity")
        if equity_df is None:
            print("No usable equity columns found.")
            sys.exit(1)

        print(f"Date range : {equity_df.index.min().date()} → {equity_df.index.max().date()}")
        print(f"Stocks     : {len(equity_df.columns)}")

        plot_single_mode(equity_df,
                         "Accumulating Strategy Return – All Stocks",
                         os.path.join(directory, "cumulative_return_all.png"))
        print_ranking(equity_df, "STRATEGY")
        save_ranking_csv(equity_df, os.path.join(directory, "cumulative_return_ranking.csv"))
        plt.show()


if __name__ == "__main__":
    main()