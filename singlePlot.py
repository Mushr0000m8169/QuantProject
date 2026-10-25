#!/usr/bin/env python3
"""
Interactive plotter for backtest result CSVs.

Usage:
    python plot_specific.py

You will be prompted for:
  1. The directory that contains the CSV files
  2. Which CSV (or code) you want to plot
"""

import os
import sys
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from pathlib import Path


def list_csvs(directory: str):
    """Return sorted list of relevant CSV files in the directory."""
    patterns = [
        os.path.join(directory, "*_backtest.csv"),
        os.path.join(directory, "*.csv"),
    ]
    files = []
    for p in patterns:
        files.extend(glob.glob(p))
    # unique + sort
    files = sorted(set(files))
    return files


def choose_file(directory: str) -> str:
    files = list_csvs(directory)
    if not files:
        print(f"No CSV files found in: {directory}")
        return None

    print(f"\nFound {len(files)} CSV file(s):\n")
    for i, f in enumerate(files, 1):
        name = os.path.basename(f)
        size = os.path.getsize(f) / 1024
        print(f"  [{i:3d}]  {name:<40s}  ({size:.1f} KB)")

    print()
    while True:
        choice = input("Enter number, filename, or stock code (e.g. HK00135)  [q=quit]: ").strip()
        if choice.lower() in ("q", "quit", "exit"):
            return None

        # by number
        if choice.isdigit():
            idx = int(choice) - 1
            if 0 <= idx < len(files):
                return files[idx]
            print("  Invalid number, try again.")
            continue

        # by exact / partial filename or code
        matches = [f for f in files if choice.lower() in os.path.basename(f).lower()]
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            print(f"  Multiple matches ({len(matches)}). Please be more specific:")
            for m in matches:
                print(f"    - {os.path.basename(m)}")
            continue

        print("  No match found. Try again.")


def max_drawdown(equity: pd.Series) -> float:
    peak = equity.cummax()
    dd = (equity - peak) / peak
    return float(dd.min())


def print_stats(df: pd.DataFrame, code: str):
    trades = df[df["pnl"].notna() & (df["pnl"] != 0)]
    n_trades = len(trades)
    n_stop = int((trades["exit_reason"] == "stop_loss").sum()) if n_trades else 0
    n_win = int((trades["pnl"] > 0).sum()) if n_trades else 0
    win_rate = n_win / n_trades if n_trades else 0.0
    total_ret = (1 + trades["pnl"]).prod() - 1 if n_trades else 0.0
    final_eq = df["equity"].iloc[-1]
    final_bh = df["bh_equity"].iloc[-1]
    mdd = max_drawdown(df["equity"])
    mdd_bh = max_drawdown(df["bh_equity"])

    print("\n" + "=" * 55)
    print(f"  {code}  Performance Summary")
    print("=" * 55)
    print(f"  Period            : {df['time_key'].iloc[0]}  →  {df['time_key'].iloc[-1]}")
    print(f"  Bars              : {len(df)}")
    print(f"  Trades            : {n_trades}  (stop-loss: {n_stop})")
    print(f"  Win rate          : {win_rate*100:.1f}%")
    print(f"  Avg trade PnL     : {trades['pnl'].mean()*100:.2f}%" if n_trades else "  Avg trade PnL     : n/a")
    print(f"  Total return (trades): {total_ret*100:.2f}%")
    print(f"  Final equity      : {final_eq:.4f}x")
    print(f"  Buy & Hold equity : {final_bh:.4f}x")
    print(f"  Excess return     : {(final_eq - final_bh)*100:.2f}%")
    print(f"  Max Drawdown      : {mdd*100:.2f}%  (B&H: {mdd_bh*100:.2f}%)")
    print("=" * 55)


def plot_backtest(file_path: str, save_dir: str = None):
    """Create a detailed 3-panel chart for one backtest CSV."""
    code = Path(file_path).stem.replace("_backtest", "")
    df = pd.read_csv(file_path, parse_dates=["time_key"])

    required = {"time_key", "close", "equity", "bh_equity", "position"}
    missing = required - set(df.columns)
    if missing:
        print(f"Missing columns: {missing}")
        return

    print_stats(df, code)

    fig, axes = plt.subplots(3, 1, figsize=(15, 11), sharex=True,
                             gridspec_kw={"height_ratios": [3.2, 1.0, 1.6]})

    # ---------- Panel 1: Price + indicators + entries/exits ----------
    ax = axes[0]
    ax.plot(df["time_key"], df["close"], color="black", linewidth=1.2, label="Close")

    if "mid_bb_d" in df.columns and df["mid_bb_d"].notna().any():
        ax.plot(df["time_key"], df["mid_bb_d"], color="#3498db", alpha=0.7, linewidth=1, label="Daily BB mid")
    if "upper_bb_d" in df.columns and df["upper_bb_d"].notna().any():
        ax.plot(df["time_key"], df["upper_bb_d"], color="#3498db", alpha=0.35, linewidth=0.8, linestyle="--")
    if "lower_bb_d" in df.columns and df["lower_bb_d"].notna().any():
        ax.plot(df["time_key"], df["lower_bb_d"], color="#3498db", alpha=0.35, linewidth=0.8, linestyle="--")
    if "mid_bb_w" in df.columns and df["mid_bb_w"].notna().any():
        ax.plot(df["time_key"], df["mid_bb_w"], color="#9b59b6", alpha=0.7, linewidth=1.1, label="Weekly BB mid")
    if "ema5" in df.columns and df["ema5"].notna().any():
        ax.plot(df["time_key"], df["ema5"], color="#e67e22", alpha=0.6, linewidth=0.9, label="EMA5")
    if "ema13" in df.columns and df["ema13"].notna().any():
        ax.plot(df["time_key"], df["ema13"], color="#e74c3c", alpha=0.6, linewidth=0.9, label="EMA13")

    # entries / exits
    if "entry_price" in df.columns:
        buys = df[df["entry_price"].notna()]
        ax.scatter(buys["time_key"], buys["entry_price"],
                   marker="^", color="#27ae60", s=90, zorder=5, label="Entry")
    if "exit_price" in df.columns:
        sells = df[df["exit_price"].notna()]
        # different marker color for stop-loss vs signal
        stop = sells[sells.get("exit_reason", "") == "stop_loss"] if "exit_reason" in sells.columns else pd.DataFrame()
        normal = sells[sells.get("exit_reason", "") != "stop_loss"] if "exit_reason" in sells.columns else sells
        if len(normal):
            ax.scatter(normal["time_key"], normal["exit_price"],
                       marker="v", color="#c0392b", s=90, zorder=5, label="Exit (signal)")
        if len(stop):
            ax.scatter(stop["time_key"], stop["exit_price"],
                       marker="x", color="#8e44ad", s=90, zorder=5, label="Exit (stop-loss)")

    ax.set_ylabel("Price")
    ax.set_title(f"{code}  –  Price / Indicators / Trades", fontsize=13, fontweight="bold")
    ax.legend(loc="upper left", fontsize=8, ncol=3)
    ax.grid(True, alpha=0.3)

    # ---------- Panel 2: Position ----------
    ax = axes[1]
    ax.fill_between(df["time_key"], 0, df["position"],
                     step="mid", color="#27ae60", alpha=0.45)
    ax.set_ylabel("Position")
    ax.set_ylim(-0.15, 1.15)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["Flat", "Long"])
    ax.grid(True, alpha=0.3)

    # ---------- Panel 3: Equity curves ----------
    ax = axes[2]
    ax.plot(df["time_key"], df["equity"], color="#27ae60", linewidth=1.6, label="Strategy")
    ax.plot(df["time_key"], df["bh_equity"], color="#2980b9", linewidth=1.3,
            linestyle="--", label="Buy & Hold")
    ax.axhline(1.0, color="gray", linestyle=":", linewidth=0.8)
    ax.set_ylabel("Equity (start = 1.0)")
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(True, alpha=0.3)

    ax.xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    ax.xaxis.set_major_locator(mdates.MonthLocator(interval=2))
    plt.xticks(rotation=35)
    plt.tight_layout()

    # save
    if save_dir is None:
        save_dir = os.path.dirname(file_path) or "."
    os.makedirs(save_dir, exist_ok=True)
    out_path = os.path.join(save_dir, f"plot_{code}.png")
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"\nChart saved → {out_path}")

    # also show interactively
    plt.show()
    plt.close()


def main():
    print("=" * 60)
    print("  Interactive Backtest CSV Plotter")
    print("=" * 60)

    # 1. Directory
    default_dir = "/Users/markp/Desktop/QuantProject@3.12/testResult"
    if not os.path.isdir(default_dir):
        default_dir = "/home/workdir/artifacts/testResult"

    print(f"\nDefault directory: {default_dir}")
    user_dir = input("Enter directory containing CSV files (press Enter for default): ").strip()
    if not user_dir:
        user_dir = default_dir

    if not os.path.isdir(user_dir):
        print(f"Directory does not exist: {user_dir}")
        sys.exit(1)

    # 2. Choose file (loop so you can plot multiple)
    while True:
        file_path = choose_file(user_dir)
        if file_path is None:
            print("Bye.")
            break

        print(f"\nSelected: {file_path}")
        try:
            plot_backtest(file_path, save_dir=user_dir)
        except Exception as e:
            print(f"Error plotting: {e}")
            import traceback
            traceback.print_exc()

        again = input("\nPlot another file from the same directory? [y/N]: ").strip().lower()
        if again not in ("y", "yes"):
            print("Done.")
            break


if __name__ == "__main__":
    main()