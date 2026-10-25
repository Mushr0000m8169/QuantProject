import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

# =========================
# Load data
# =========================
df = pd.read_csv("testResult/PORTFOLIO_equity.csv")

# Change these if your column names differ
DATE_COL = "time_key"
EQUITY_COL = "equity"

df[DATE_COL] = pd.to_datetime(df[DATE_COL])
df = df.sort_values(DATE_COL)

equity = df[EQUITY_COL]

# =========================
# Performance Metrics
# =========================
returns = equity.pct_change().dropna()

total_return = (equity.iloc[-1] / equity.iloc[0] - 1) * 100

days = (df[DATE_COL].iloc[-1] - df[DATE_COL].iloc[0]).days
years = max(days / 365.25, 1e-9)

cagr = ((equity.iloc[-1] / equity.iloc[0]) ** (1 / years) - 1) * 100

running_max = equity.cummax()
drawdown = (equity / running_max - 1)

max_dd = drawdown.min() * 100

volatility = returns.std() * np.sqrt(252) * 100

sharpe = (
    returns.mean() / returns.std() * np.sqrt(252)
    if returns.std() > 0 else 0
)

wins = returns[returns > 0]
losses = returns[returns < 0]

win_rate = len(wins) / len(returns) * 100

gross_profit = wins.sum()
gross_loss = abs(losses.sum())

profit_factor = (
    gross_profit / gross_loss
    if gross_loss > 0 else np.inf
)

avg_win = wins.mean() * 100 if len(wins) else 0
avg_loss = losses.mean() * 100 if len(losses) else 0

best_day = returns.max() * 100
worst_day = returns.min() * 100

# =========================
# Plot
# =========================
fig, (ax1, ax2) = plt.subplots(
    2, 1,
    figsize=(15, 9),
    sharex=True,
    gridspec_kw={"height_ratios": [3, 1]}
)

# Equity Curve
ax1.plot(
    df[DATE_COL],
    equity,
    color="navy",
    linewidth=2
)

ax1.set_title("Portfolio Equity Curve")
ax1.grid(True, alpha=0.3)

metrics_text = f"""
Total Return : {total_return:.2f}%
CAGR         : {cagr:.2f}%
Sharpe       : {sharpe:.2f}
Volatility   : {volatility:.2f}%

Max DD       : {max_dd:.2f}%

Win Rate     : {win_rate:.2f}%
ProfitFactor : {profit_factor:.2f}

Avg Win      : {avg_win:.2f}%
Avg Loss     : {avg_loss:.2f}%

Best Day     : {best_day:.2f}%
Worst Day    : {worst_day:.2f}%
"""

ax1.text(
    0.02,
    0.98,
    metrics_text,
    transform=ax1.transAxes,
    fontsize=10,
    family="monospace",
    verticalalignment="top",
    bbox=dict(facecolor="white", alpha=0.9)
)

# Drawdown
ax2.fill_between(
    df[DATE_COL],
    drawdown * 100,
    0,
    color="red",
    alpha=0.35
)

ax2.plot(
    df[DATE_COL],
    drawdown * 100,
    color="red",
    linewidth=1
)

ax2.set_ylabel("Drawdown %")
ax2.grid(True, alpha=0.3)

plt.tight_layout()
plt.show()

# =========================
# Console Summary
# =========================
print("=" * 50)
print("PERFORMANCE SUMMARY")
print("=" * 50)
print(f"Total Return : {total_return:.2f}%")
print(f"CAGR         : {cagr:.2f}%")
print(f"Sharpe       : {sharpe:.2f}")
print(f"Volatility   : {volatility:.2f}%")
print(f"Max DD       : {max_dd:.2f}%")
print(f"Win Rate     : {win_rate:.2f}%")
print(f"ProfitFactor : {profit_factor:.2f}")
print(f"Avg Win      : {avg_win:.2f}%")
print(f"Avg Loss     : {avg_loss:.2f}%")
print(f"Best Day     : {best_day:.2f}%")
print(f"Worst Day    : {worst_day:.2f}%")
