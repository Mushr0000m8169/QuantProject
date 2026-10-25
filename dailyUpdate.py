#!/usr/bin/env python3
"""
Daily + Weekly K-line Updater using Futu OpenAPI

Append today's daily bar and update the current weekly bar into existing history folders.

Requirements:
  1. Futu OpenD running (default 127.0.0.1:11111)
  2. pip install futu-api pandas numpy

Typical usage (append today only):
    python daily_update.py \\
        --list monitorCodeList.csv \\
        --daily-dir d_history \\
        --weekly-dir w_history \\
        --append-today

Full refresh example:
    python3 dailyUpdate.py --daily-dir d_history --weekly-dir w_history --days 400 --full
"""

import os
import sys
import argparse
import time
from datetime import datetime, timedelta

import numpy as np
import pandas as pd

try:
    from futu import OpenQuoteContext, RET_OK, KLType, AuType, KL_FIELD
except ImportError:
    print("Please install futu-api:  pip install futu-api")
    sys.exit(1)


# ===================== defaults =====================
DEFAULT_LIST       = "monitorCodeList.csv"
DEFAULT_DAILY_DIR  = "d_history"
DEFAULT_WEEKLY_DIR = "w_history"
DEFAULT_DAYS       = 60          # enough for indicators when appending
DEFAULT_HOST       = "127.0.0.1"
DEFAULT_PORT       = 11111
# ====================================================


def calc_ema(series: pd.Series, span: int) -> pd.Series:
    return series.ewm(span=span, adjust=False).mean()


def calc_bollinger(close: pd.Series, window: int = 40, num_std: float = 2.0):
    mid = close.rolling(window).mean()
    std = close.rolling(window).std(ddof=0)
    upper = mid + num_std * std
    lower = mid - num_std * std
    return mid, upper, lower


def calc_kdj(high: pd.Series, low: pd.Series, close: pd.Series,
             n: int = 9, m1: int = 3, m2: int = 3):
    lowest = low.rolling(n).min()
    highest = high.rolling(n).max()
    rsv = (close - lowest) / (highest - lowest).replace(0, np.nan) * 100
    rsv = rsv.fillna(50)
    k = rsv.ewm(alpha=1.0 / m1, adjust=False).mean()
    d = k.ewm(alpha=1.0 / m2, adjust=False).mean()
    j = 3 * k - 2 * d
    return k, d, j


def fetch_kline(quote_ctx, code: str, start: str, end: str, ktype) -> pd.DataFrame:
    """Fetch all pages of K-line for one code."""
    all_rows = []
    page_req_key = None
    while True:
        ret, data, page_req_key = quote_ctx.request_history_kline(
            code=code,
            start=start,
            end=end,
            ktype=ktype,
            autype=AuType.QFQ,
            fields=[KL_FIELD.ALL],
            max_count=1000,
            page_req_key=page_req_key,
        )
        if ret != RET_OK:
            print(f"  [{code}] Futu error: {data}")
            return None
        if data is None or data.empty:
            break
        all_rows.append(data)
        if page_req_key is None:
            break
    if not all_rows:
        return None
    df = pd.concat(all_rows, ignore_index=True)
    df = df.drop_duplicates(subset=["time_key"]).sort_values("time_key").reset_index(drop=True)
    return df


def add_indicators(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()
    df["ema5"]  = calc_ema(df["close"], 5)
    df["ema13"] = calc_ema(df["close"], 13)
    df["k"], df["d"], df["j"] = calc_kdj(df["high"], df["low"], df["close"])
    df["boll_mid"], df["boll_upper"], df["boll_lower"] = calc_bollinger(df["close"], 40, 2.0)
    return df


def normalize_columns(df: pd.DataFrame, code: str) -> pd.DataFrame:
    needed = [
        "code", "name", "time_key",
        "open", "close", "high", "low",
        "pe_ratio", "turnover_rate", "volume", "turnover",
        "change_rate", "last_close",
        "ema5", "ema13", "k", "d", "j",
        "boll_mid", "boll_upper", "boll_lower",
    ]
    for col in needed:
        if col not in df.columns:
            df[col] = np.nan
    df["code"] = code
    return df[needed]


def merge_append(new_df: pd.DataFrame, path: str, replace_last: bool = False) -> pd.DataFrame:
    """
    Append new bars after the last date in existing file.
    If replace_last=True (useful for weekly), also replace the last bar
    when its date is the same as the newest fetched bar (current incomplete week).
    """
    if not os.path.exists(path):
        return new_df

    old = pd.read_csv(path)
    if old.empty:
        return new_df

    old["_dt"] = pd.to_datetime(old["time_key"]).dt.normalize()
    new_df = new_df.copy()
    new_df["_dt"] = pd.to_datetime(new_df["time_key"]).dt.normalize()

    last_old = old["_dt"].max()
    last_new = new_df["_dt"].max()

    if replace_last and last_new == last_old:
        # update the last (current) bar
        old = old[old["_dt"] < last_old]
        combined = pd.concat([old, new_df[new_df["_dt"] == last_new]], ignore_index=True)
        print(f"    updated last bar ({last_old.date()})")
    else:
        fresh = new_df[new_df["_dt"] > last_old]
        if fresh.empty:
            print(f"    already up-to-date (last={last_old.date()})")
            old = old.drop(columns=["_dt"])
            return old
        print(f"    appending {len(fresh)} bar(s) after {last_old.date()}")
        combined = pd.concat([old.drop(columns=["_dt"]), fresh.drop(columns=["_dt"])], ignore_index=True)

    if "_dt" in combined.columns:
        combined = combined.drop(columns=["_dt"])
    combined = combined.drop_duplicates(subset=["time_key"], keep="last")
    combined = combined.sort_values("time_key").reset_index(drop=True)
    return combined


def code_to_filename(code: str) -> str:
    """
    Convert Futu-style code to your file name style.
      HK.00100  →  HK00100.csv
      HK.09988  →  HK09988.csv
    """
    name = code.strip().upper().replace(".", "")
    return f"{name}.csv"


def process_one(quote_ctx, code: str, start_str: str, end_str: str,
                daily_dir: str, weekly_dir: str, full: bool):
    """Fetch daily + weekly, compute indicators, merge into existing files only."""
    fname = code_to_filename(code)          # e.g. HK00100.csv
    daily_path  = os.path.join(daily_dir,  fname)
    weekly_path = os.path.join(weekly_dir, fname)

    # ----- Daily -----
    if not full and not os.path.exists(daily_path):
        print(f"  [{code}] daily file not found ({fname}), skip (won't create new)")
        return False

    daily_raw = fetch_kline(quote_ctx, code, start_str, end_str, KLType.K_DAY)
    if daily_raw is None or daily_raw.empty:
        print(f"  [{code}] no daily data")
        return False

    daily_df = normalize_columns(add_indicators(daily_raw), code)

    if full or not os.path.exists(daily_path):
        daily_df.to_csv(daily_path, index=False, float_format="%.6f")
        print(f"    daily  : wrote {len(daily_df)} rows → {fname}")
    else:
        merged = merge_append(daily_df, daily_path, replace_last=False)
        merged.to_csv(daily_path, index=False, float_format="%.6f")
        print(f"    daily  : total {len(merged)} rows → {fname}")

    # ----- Weekly -----
    if not full and not os.path.exists(weekly_path):
        print(f"    weekly : file not found ({fname}), skip")
        return True

    weekly_raw = fetch_kline(quote_ctx, code, start_str, end_str, KLType.K_WEEK)
    if weekly_raw is None or weekly_raw.empty:
        print(f"    weekly : no data")
        return True

    weekly_df = normalize_columns(add_indicators(weekly_raw), code)

    if full or not os.path.exists(weekly_path):
        weekly_df.to_csv(weekly_path, index=False, float_format="%.6f")
        print(f"    weekly : wrote {len(weekly_df)} rows → {fname}")
    else:
        # replace_last=True → update the current (incomplete) week bar
        merged_w = merge_append(weekly_df, weekly_path, replace_last=True)
        merged_w.to_csv(weekly_path, index=False, float_format="%.6f")
        print(f"    weekly : total {len(merged_w)} rows → {fname}")

    return True


def main():
    parser = argparse.ArgumentParser(description="Append today daily + weekly K-line via Futu")
    parser.add_argument("--list", default=DEFAULT_LIST, help="monitorCodeList.csv")
    parser.add_argument("--daily-dir",  default=DEFAULT_DAILY_DIR,  help="daily history folder")
    parser.add_argument("--weekly-dir", default=DEFAULT_WEEKLY_DIR, help="weekly history folder")
    parser.add_argument("--days", type=int, default=DEFAULT_DAYS,
                        help="how many calendar days to pull (for indicator calc)")
    parser.add_argument("--full", action="store_true", help="overwrite all history")
    parser.add_argument("--append-today", action="store_true",
                        help="shortcut: only pull recent days and append (default behaviour when not --full)")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--sleep", type=float, default=0.35)
    args = parser.parse_args()

    # locate list
    list_path = args.list
    if not os.path.exists(list_path):
        for c in [list_path, "monitorCodeList.csv",
                  "/Users/markp/Desktop/QuantProject@3.12/monitorCodeList.csv"]:
            if os.path.exists(c):
                list_path = c
                break
        else:
            print(f"Cannot find {args.list}")
            sys.exit(1)

    codes = [ln.strip() for ln in open(list_path) if ln.strip() and ln.strip().lower() != "code"]
    print(f"Codes      : {len(codes)}")
    print(f"Daily dir  : {args.daily_dir}")
    print(f"Weekly dir : {args.weekly_dir}")
    print(f"Mode       : {'FULL overwrite' if args.full else 'APPEND (today / recent)'}")
    print(f"OpenD      : {args.host}:{args.port}")
    print("-" * 55)

    os.makedirs(args.daily_dir, exist_ok=True)
    os.makedirs(args.weekly_dir, exist_ok=True)

    end_dt = datetime.now()
    # when appending we still need enough history for EMA/BB/KDJ
    days = args.days if not args.full else max(args.days, 400)
    start_str = (end_dt - timedelta(days=days)).strftime("%Y-%m-%d")
    end_str   = end_dt.strftime("%Y-%m-%d")

    quote_ctx = OpenQuoteContext(host=args.host, port=args.port)
    print("Connected to Futu OpenD.\n")

    ok = fail = 0
    try:
        for i, code in enumerate(codes, 1):
            print(f"[{i}/{len(codes)}] {code}")
            success = process_one(
                quote_ctx, code, start_str, end_str,
                args.daily_dir, args.weekly_dir, args.full
            )
            if success:
                ok += 1
            else:
                fail += 1
            time.sleep(1.5)
    finally:
        quote_ctx.close()
        print("\nOpenD connection closed.")

    print("-" * 55)
    print(f"Done. Success: {ok}  Failed: {fail}")


if __name__ == "__main__":
    main()