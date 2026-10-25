#!/usr/bin/env python3
"""
Fetch daily + weekly K-line history via Futu OpenAPI
and store into d_history / w_history.

Requirements:
  1. Futu OpenD running (default 127.0.0.1:11111)
  2. pip install futu-api pandas numpy

Usage examples:
  # Full history (first-time download, ~400 calendar days)
  python3 fetch_history.py --list monitorCodeList.csv \\
      --daily-dir d_history --weekly-dir w_history --full --days 400

  # Append / refresh recent bars only (daily job)
  python3 fetch_history.py --list monitorCodeList.csv \\
      --daily-dir d_history --weekly-dir w_history --days 60

  # Single stock test
  python3 fetch_history.py --codes HK.00700 --full --days 400
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from datetime import datetime, timedelta
from typing import List, Optional

import numpy as np
import pandas as pd

try:
    from futu import OpenQuoteContext, RET_OK, KLType, AuType, KL_FIELD
except ImportError:
    print("Please install futu-api:  pip install futu-api")
    sys.exit(1)


# ===================== defaults =====================
DEFAULT_LIST = "monitorCodeList1.csv"
DEFAULT_DAILY_DIR = "d_history1"
DEFAULT_WEEKLY_DIR = "w_history1"
DEFAULT_DAYS = 1000
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 11111
DEFAULT_SLEEP = 1.5  # seconds between stocks (avoid rate limit)
# ====================================================


# ---------- indicators (optional, matches your backtester inputs) ----------

def calc_ema(series: pd.Series, span: int) -> pd.Series:
    return series.ewm(span=span, adjust=False).mean()


def calc_bollinger(close: pd.Series, window: int = 40, num_std: float = 2.0):
    mid = close.rolling(window).mean()
    std = close.rolling(window).std(ddof=0)
    return mid, mid + num_std * std, mid - num_std * std


def calc_kdj(
    high: pd.Series,
    low: pd.Series,
    close: pd.Series,
    n: int = 9,
    m1: int = 3,
    m2: int = 3,
):
    lowest = low.rolling(n).min()
    highest = high.rolling(n).max()
    rsv = (close - lowest) / (highest - lowest).replace(0, np.nan) * 100
    rsv = rsv.fillna(50)
    k = rsv.ewm(alpha=1.0 / m1, adjust=False).mean()
    d = k.ewm(alpha=1.0 / m2, adjust=False).mean()
    j = 3 * k - 2 * d
    return k, d, j


def add_indicators(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()
    df["ema5"] = calc_ema(df["close"], 5)
    df["ema13"] = calc_ema(df["close"], 13)
    df["k"], df["d"], df["j"] = calc_kdj(df["high"], df["low"], df["close"])
    df["boll_mid"], df["boll_upper"], df["boll_lower"] = calc_bollinger(
        df["close"], 40, 2.0
    )
    return df


# ---------- helpers ----------

def code_to_filename(code: str) -> str:
    """HK.00100 → HK00100.csv"""
    return code.strip().upper().replace(".", "") + ".csv"


def load_codes(list_path: str, extra_codes: Optional[List[str]] = None) -> List[str]:
    codes: List[str] = []
    if extra_codes:
        codes.extend(extra_codes)
    if list_path and os.path.exists(list_path):
        with open(list_path, encoding="utf-8") as f:
            for ln in f:
                ln = ln.strip()
                if not ln:
                    continue
                if ln.lower() in ("code", "symbol", "stock_code"):
                    continue
                codes.append(ln)
    # unique, keep order
    seen = set()
    out = []
    for c in codes:
        c = c.strip()
        if c and c not in seen:
            seen.add(c)
            out.append(c)
    return out


def normalize_columns(df: pd.DataFrame, code: str) -> pd.DataFrame:
    needed = [
        "code",
        "name",
        "time_key",
        "open",
        "close",
        "high",
        "low",
        "pe_ratio",
        "turnover_rate",
        "volume",
        "turnover",
        "change_rate",
        "last_close",
        "ema5",
        "ema13",
        "k",
        "d",
        "j",
        "boll_mid",
        "boll_upper",
        "boll_lower",
    ]
    for col in needed:
        if col not in df.columns:
            df[col] = np.nan
    df["code"] = code
    # keep time_key as string YYYY-MM-DD...
    if "time_key" in df.columns:
        df["time_key"] = df["time_key"].astype(str)
    return df[needed]


def fetch_kline(
    quote_ctx: OpenQuoteContext,
    code: str,
    start: str,
    end: str,
    ktype,
) -> Optional[pd.DataFrame]:
    """Fetch all pages of history K-line."""
    all_rows = []
    page_req_key = None
    while True:
        ret, data, page_req_key = quote_ctx.request_history_kline(
            code=code,
            start=start,
            end=end,
            ktype=ktype,
            autype=AuType.QFQ,  # forward-adjusted
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
        time.sleep(0.3)  # small pause between pages
    if not all_rows:
        return None
    df = pd.concat(all_rows, ignore_index=True)
    df = (
        df.drop_duplicates(subset=["time_key"])
        .sort_values("time_key")
        .reset_index(drop=True)
    )
    return df


def merge_append(
    new_df: pd.DataFrame, path: str, replace_last: bool = False
) -> pd.DataFrame:
    """
    Append bars after last date in existing file.
    If replace_last=True (weekly), replace the last bar when dates match
    (current incomplete week).
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
        old = old[old["_dt"] < last_old]
        combined = pd.concat(
            [old, new_df[new_df["_dt"] == last_new]], ignore_index=True
        )
        print(f"    updated last bar ({last_old.date()})")
    else:
        fresh = new_df[new_df["_dt"] > last_old]
        if fresh.empty:
            print(f"    already up-to-date (last={last_old.date()})")
            return old.drop(columns=["_dt"])
        print(f"    appending {len(fresh)} bar(s) after {last_old.date()}")
        combined = pd.concat(
            [old.drop(columns=["_dt"]), fresh.drop(columns=["_dt"])],
            ignore_index=True,
        )

    if "_dt" in combined.columns:
        combined = combined.drop(columns=["_dt"])
    combined = (
        combined.drop_duplicates(subset=["time_key"], keep="last")
        .sort_values("time_key")
        .reset_index(drop=True)
    )
    return combined


def process_one(
    quote_ctx: OpenQuoteContext,
    code: str,
    start_str: str,
    end_str: str,
    daily_dir: str,
    weekly_dir: str,
    full: bool,
    with_indicators: bool,
) -> bool:
    fname = code_to_filename(code)
    daily_path = os.path.join(daily_dir, fname)
    weekly_path = os.path.join(weekly_dir, fname)

    # ----- Daily -----
    if not full and not os.path.exists(daily_path):
        print(f"  [{code}] daily file missing ({fname}); use --full to create")
        return False

    daily_raw = fetch_kline(quote_ctx, code, start_str, end_str, KLType.K_DAY)
    if daily_raw is None or daily_raw.empty:
        print(f"  [{code}] no daily data")
        return False

    daily_df = daily_raw
    if with_indicators:
        daily_df = add_indicators(daily_df)
    daily_df = normalize_columns(daily_df, code)

    if full or not os.path.exists(daily_path):
        daily_df.to_csv(daily_path, index=False, float_format="%.6f")
        print(f"    daily  : wrote {len(daily_df)} rows → {fname}")
    else:
        merged = merge_append(daily_df, daily_path, replace_last=False)
        # recompute indicators on full merged history so EMA/BB stay consistent
        if with_indicators:
            merged = normalize_columns(add_indicators(merged), code)
        merged.to_csv(daily_path, index=False, float_format="%.6f")
        print(f"    daily  : total {len(merged)} rows → {fname}")

    # ----- Weekly -----
    if not full and not os.path.exists(weekly_path):
        print(f"    weekly : file missing ({fname}); skip")
        return True

    weekly_raw = fetch_kline(quote_ctx, code, start_str, end_str, KLType.K_WEEK)
    if weekly_raw is None or weekly_raw.empty:
        print(f"    weekly : no data")
        return True

    weekly_df = weekly_raw
    if with_indicators:
        weekly_df = add_indicators(weekly_df)
    weekly_df = normalize_columns(weekly_df, code)

    if full or not os.path.exists(weekly_path):
        weekly_df.to_csv(weekly_path, index=False, float_format="%.6f")
        print(f"    weekly : wrote {len(weekly_df)} rows → {fname}")
    else:
        merged_w = merge_append(weekly_df, weekly_path, replace_last=True)
        if with_indicators:
            merged_w = normalize_columns(add_indicators(merged_w), code)
        merged_w.to_csv(weekly_path, index=False, float_format="%.6f")
        print(f"    weekly : total {len(merged_w)} rows → {fname}")

    return True


def main():
    parser = argparse.ArgumentParser(
        description="Download / append HK daily+weekly K-line via Futu OpenD"
    )
    parser.add_argument("--list", default=DEFAULT_LIST, help="CSV with stock codes")
    parser.add_argument(
        "--codes",
        nargs="+",
        default=None,
        help="Optional explicit codes, e.g. HK.00700 HK.09988",
    )
    parser.add_argument("--daily-dir", default=DEFAULT_DAILY_DIR)
    parser.add_argument("--weekly-dir", default=DEFAULT_WEEKLY_DIR)
    parser.add_argument(
        "--days",
        type=int,
        default=DEFAULT_DAYS,
        help="Calendar days of history to request",
    )
    parser.add_argument(
        "--full",
        action="store_true",
        help="Overwrite / create files (first-time bulk download)",
    )
    parser.add_argument("--no-indicators", action="store_true", help="Skip EMA/KDJ/BB")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument(
        "--sleep",
        type=float,
        default=DEFAULT_SLEEP,
        help="Seconds between stocks (rate limit)",
    )
    args = parser.parse_args()

    list_path = args.list
    if not args.codes and not os.path.exists(list_path):
        for cand in [
            list_path,
            "monitorCodeList.csv",
            os.path.expanduser(
                "~/Desktop/QuantProject@3.12/monitorCodeList.csv"
            ),
        ]:
            if os.path.exists(cand):
                list_path = cand
                break
        else:
            print(f"Cannot find code list: {args.list}")
            print("Pass --list path or --codes HK.00700 ...")
            sys.exit(1)

    codes = load_codes(list_path if os.path.exists(list_path) else "", args.codes)
    if not codes:
        print("No stock codes found.")
        sys.exit(1)

    os.makedirs(args.daily_dir, exist_ok=True)
    os.makedirs(args.weekly_dir, exist_ok=True)

    end_dt = datetime.now()
    days = args.days if not args.full else max(args.days, 400)
    start_str = (end_dt - timedelta(days=days)).strftime("%Y-%m-%d")
    end_str = end_dt.strftime("%Y-%m-%d")
    with_ind = not args.no_indicators

    print(f"Codes      : {len(codes)}")
    print(f"Daily dir  : {args.daily_dir}")
    print(f"Weekly dir : {args.weekly_dir}")
    print(f"Range      : {start_str} → {end_str}")
    print(f"Mode       : {'FULL overwrite' if args.full else 'APPEND / update'}")
    print(f"Indicators : {'ON' if with_ind else 'OFF'}")
    print(f"OpenD      : {args.host}:{args.port}")
    print("-" * 55)

    quote_ctx = OpenQuoteContext(host=args.host, port=args.port)
    print("Connected to Futu OpenD.\n")

    ok = fail = 0
    try:
        for i, code in enumerate(codes, 1):
            print(f"[{i}/{len(codes)}] {code}")
            try:
                success = process_one(
                    quote_ctx,
                    code,
                    start_str,
                    end_str,
                    args.daily_dir,
                    args.weekly_dir,
                    args.full,
                    with_ind,
                )
            except Exception as e:
                print(f"  [{code}] exception: {e}")
                success = False
            if success:
                ok += 1
            else:
                fail += 1
            time.sleep(args.sleep)
    finally:
        quote_ctx.close()
        print("\nOpenD connection closed.")

    print("-" * 55)
    print(f"Done. Success: {ok}  Failed: {fail}")
    print(f"Daily CSVs  → {os.path.abspath(args.daily_dir)}")
    print(f"Weekly CSVs → {os.path.abspath(args.weekly_dir)}")


if __name__ == "__main__":
    main()