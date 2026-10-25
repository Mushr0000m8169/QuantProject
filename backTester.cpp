/*
 * backTester.cpp
 * -------------------------------------------------------------
 * Dual-regime mean-reversion / trend strategy
 *
 * Regime BULL  (close > midBB_daily AND close > midBB_weekly):
 *   Buy  : KDJ golden cross  (K crosses above D)  → enter next open
 *   Sell : close < EMA5  OR  KDJ death cross
 *
 * Regime BEAR  (close < midBB_daily AND close < midBB_weekly):
 *   Buy  : EMA5 crosses above EMA13
 *   Sell : (close < EMA5 AND close < midBB_daily)  OR  KDJ death cross
 *
 * Bollinger Bands (daily + weekly): n=40, sd=2
 * Hard stop-loss: 3%
 *
 * Data:
 *   monitorCodeList.csv
 *   d_history/HK00100.csv   (or HK.00100.csv)
 *   w_history/HK00100.csv
 *
 * Output → testResult/<code>_backtest.csv
 *
 * Compile:
 *   g++ -O2 -std=c++17 backTester.cpp -o backtester
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <filesystem>
#include <limits>

using namespace std;
namespace fs = std::filesystem;

#ifndef NAN
#define NAN (numeric_limits<double>::quiet_NaN())
#endif

// ===================== CONFIG =====================
const string DAILY_DIR   = "d_history";
const string WEEKLY_DIR  = "w_history";
const string CODE_LIST   = "monitorCodeList.csv";
const string OUT_DIR     = "testResult";

const int    BB_N        = 40;     // Bollinger period
const double BB_SD       = 2.0;    // Bollinger std multiplier
const double STOP_LOSS   = 0.03;   // 3% hard stop
// ==================================================

struct Bar {
    string date;
    double open = NAN, high = NAN, low = NAN, close = NAN;
    // indicators (filled later)
    double ema5 = NAN, ema13 = NAN;
    double k = NAN, d = NAN, j = NAN;
    double mid_bb_d = NAN, upper_bb_d = NAN, lower_bb_d = NAN;
    double mid_bb_w = NAN;
};

struct ResultRow {
    string date;
    double open, high, low, close;
    double mid_bb_d, mid_bb_w, ema5, ema13, k, d;
    int signal = 0;       // 1 buy, -1 sell
    int position = 0;
    double entry_price = NAN;
    double exit_price  = NAN;
    double pnl = 0.0;
    string exit_reason;
    double strategy_ret = 0.0;
    double equity = 1.0;
    double bh_equity = 1.0;
};

// ---------- helpers ----------
vector<string> split(const string& s, char delim = ',') {
    vector<string> t;
    string item;
    istringstream ss(s);
    while (getline(ss, item, delim)) t.push_back(item);
    return t;
}

double to_d(const string& s) {
    if (s.empty() || s == "nan" || s == "NaN") return NAN;
    try { return stod(s); } catch (...) { return NAN; }
}

bool isnan_d(double x) { return x != x; }

string code_to_stem(const string& code) {
    string s;
    for (char c : code) if (c != '.') s.push_back(c);
    return s;
}

vector<string> load_codes() {
    vector<string> codes;
    ifstream fin(CODE_LIST);
    if (!fin) {
        cerr << "Cannot open " << CODE_LIST << "\n";
        return codes;
    }
    string line;
    bool first = true;
    while (getline(fin, line)) {
        if (line.empty()) continue;
        auto t = split(line);
        string c = t[0];
        if (first) {
            first = false;
            string lower = c;
            for (char& ch : lower) ch = (char)tolower(ch);
            if (lower.find("code") != string::npos || lower == "symbol") continue;
        }
        c.erase(remove_if(c.begin(), c.end(), ::isspace), c.end());
        if (!c.empty()) codes.push_back(c);
    }
    return codes;
}

// Load OHLC (+ optional precomputed KDJ) from d_history or w_history
vector<Bar> load_bars(const string& dir, const string& code) {
    string stem = code_to_stem(code);
    vector<string> candidates = {
        dir + "/" + code + ".csv",
        dir + "/" + stem + ".csv",
        dir + "/" + code + "_daily.csv",
        dir + "/" + stem + "_daily.csv",
        dir + "/" + code + "_weekly.csv",
        dir + "/" + stem + "_weekly.csv",
    };
    string path;
    for (auto& p : candidates)
        if (fs::exists(p)) { path = p; break; }

    if (path.empty() && fs::exists(dir)) {
        for (auto& entry : fs::directory_iterator(dir)) {
            string name = entry.path().filename().string();
            if (name.find(stem) != string::npos &&
                name.size() >= 4 && name.substr(name.size()-4) == ".csv") {
                path = entry.path().string();
                break;
            }
        }
    }
    if (path.empty()) return {};

    ifstream fin(path);
    if (!fin) return {};

    string line;
    getline(fin, line);
    auto header = split(line);

    auto find_col = [&](initializer_list<const char*> names) -> int {
        for (size_t i = 0; i < header.size(); ++i) {
            string h = header[i];
            for (char& c : h) c = (char)tolower(c);
            for (auto* n : names)
                if (h == n) return (int)i;
        }
        return -1;
    };

    int date_col  = find_col({"time_key", "date", "time"});
    int open_col  = find_col({"open"});
    int high_col  = find_col({"high"});
    int low_col   = find_col({"low"});
    int close_col = find_col({"close", "adj_close", "adjclose"});
    int k_col     = find_col({"k"});
    int d_col     = find_col({"d"});
    int j_col     = find_col({"j"});

    // fallback matching your layout: code,name,time_key,open,close,high,low
    if (date_col  < 0) date_col  = 2;
    if (open_col  < 0) open_col  = 3;
    if (close_col < 0) close_col = 4;
    if (high_col  < 0) high_col  = 5;
    if (low_col   < 0) low_col   = 6;

    vector<Bar> bars;
    while (getline(fin, line)) {
        if (line.empty()) continue;
        auto t = split(line);
        int need = max({date_col, open_col, high_col, low_col, close_col});
        if ((int)t.size() <= need) continue;
        Bar b;
        b.date  = t[date_col].substr(0, 10);
        b.open  = to_d(t[open_col]);
        b.high  = to_d(t[high_col]);
        b.low   = to_d(t[low_col]);
        b.close = to_d(t[close_col]);
        if (k_col >= 0 && k_col < (int)t.size()) b.k = to_d(t[k_col]);
        if (d_col >= 0 && d_col < (int)t.size()) b.d = to_d(t[d_col]);
        if (j_col >= 0 && j_col < (int)t.size()) b.j = to_d(t[j_col]);
        if (!isnan_d(b.close) && b.close > 0) bars.push_back(b);
    }
    return bars;
}

// ---------- indicators ----------
void compute_ema(vector<Bar>& bars, int span, vector<double>& out) {
    out.assign(bars.size(), NAN);
    if (bars.empty()) return;
    double alpha = 2.0 / (span + 1.0);
    out[0] = bars[0].close;
    for (size_t i = 1; i < bars.size(); ++i)
        out[i] = alpha * bars[i].close + (1.0 - alpha) * out[i - 1];
}

void compute_bollinger(vector<Bar>& bars, int n, double sd) {
    int N = (int)bars.size();
    for (int i = 0; i < N; ++i) {
        if (i + 1 < n) continue;
        double sum = 0, sum2 = 0;
        for (int j = i - n + 1; j <= i; ++j) {
            sum  += bars[j].close;
            sum2 += bars[j].close * bars[j].close;
        }
        double mean = sum / n;
        double var  = sum2 / n - mean * mean;
        double std  = sqrt(max(0.0, var));
        bars[i].mid_bb_d   = mean;
        bars[i].upper_bb_d = mean + sd * std;
        bars[i].lower_bb_d = mean - sd * std;
    }
}

// Simple KDJ if not present in file (RSV 9, K/D smooth 3)
void compute_kdj_if_needed(vector<Bar>& bars, int rsv_n = 9) {
    bool need = false;
    for (auto& b : bars)
        if (isnan_d(b.k) || isnan_d(b.d)) { need = true; break; }
    if (!need) return;

    int N = (int)bars.size();
    double k = 50, d = 50;
    for (int i = 0; i < N; ++i) {
        if (i + 1 < rsv_n) {
            bars[i].k = bars[i].d = bars[i].j = 50;
            continue;
        }
        double hh = bars[i].high, ll = bars[i].low;
        for (int j = i - rsv_n + 1; j <= i; ++j) {
            hh = max(hh, bars[j].high);
            ll = min(ll, bars[j].low);
        }
        double rsv = (hh - ll < 1e-12) ? 50.0
                    : (bars[i].close - ll) / (hh - ll) * 100.0;
        k = (2.0 / 3.0) * k + (1.0 / 3.0) * rsv;
        d = (2.0 / 3.0) * d + (1.0 / 3.0) * k;
        bars[i].k = k;
        bars[i].d = d;
        bars[i].j = 3 * k - 2 * d;
    }
}

// Weekly mid BB (n=40, sd=2) → forward-fill onto daily
void attach_weekly_bb(vector<Bar>& daily, const vector<Bar>& weekly) {
    // compute weekly mid BB into a temp map
    vector<Bar> w = weekly;
    int Nw = (int)w.size();
    for (int i = 0; i < Nw; ++i) {
        if (i + 1 < BB_N) continue;
        double sum = 0, sum2 = 0;
        for (int j = i - BB_N + 1; j <= i; ++j) {
            sum  += w[j].close;
            sum2 += w[j].close * w[j].close;
        }
        double mean = sum / BB_N;
        w[i].mid_bb_d = mean;   // reuse field as weekly mid
    }

    size_t wi = 0;
    for (size_t di = 0; di < daily.size(); ++di) {
        while (wi + 1 < w.size() && w[wi + 1].date <= daily[di].date)
            ++wi;
        if (wi < w.size() && !isnan_d(w[wi].mid_bb_d))
            daily[di].mid_bb_w = w[wi].mid_bb_d;
    }
}

// ---------- backtest one stock ----------
void backtest_one(const string& code) {
    auto daily  = load_bars(DAILY_DIR,  code);
    auto weekly = load_bars(WEEKLY_DIR, code);

    if (daily.size() < (size_t)BB_N + 10) {
        cerr << "  Skip " << code << " – not enough daily bars (" << daily.size() << ")\n";
        return;
    }

    // indicators
    compute_bollinger(daily, BB_N, BB_SD);

    vector<double> ema5, ema13;
    compute_ema(daily, 5,  ema5);
    compute_ema(daily, 13, ema13);
    for (size_t i = 0; i < daily.size(); ++i) {
        daily[i].ema5  = ema5[i];
        daily[i].ema13 = ema13[i];
    }

    compute_kdj_if_needed(daily);

    if (!weekly.empty())
        attach_weekly_bb(daily, weekly);
    else
        cerr << "  Warn " << code << " – no weekly data, weekly BB filter disabled\n";

    int N = (int)daily.size();
    vector<ResultRow> rows(N);

    // ----- signal generation (decision at close t → trade open t+1) -----
    for (int i = 1; i < N; ++i) {
        rows[i].date     = daily[i].date;
        rows[i].open     = daily[i].open;
        rows[i].high     = daily[i].high;
        rows[i].low      = daily[i].low;
        rows[i].close    = daily[i].close;
        rows[i].mid_bb_d = daily[i].mid_bb_d;
        rows[i].mid_bb_w = daily[i].mid_bb_w;
        rows[i].ema5     = daily[i].ema5;
        rows[i].ema13    = daily[i].ema13;
        rows[i].k        = daily[i].k;
        rows[i].d        = daily[i].d;

        bool has_bb = !isnan_d(daily[i].mid_bb_d);
        bool has_wb = !isnan_d(daily[i].mid_bb_w);

        bool bull = has_bb && (!has_wb || daily[i].close > daily[i].mid_bb_w)
                            && daily[i].close > daily[i].mid_bb_d;
        bool bear = has_bb && (!has_wb || daily[i].close < daily[i].mid_bb_w)
                            && daily[i].close < daily[i].mid_bb_d;

        bool kdj_golden = !isnan_d(daily[i].k) && !isnan_d(daily[i].d)
                       && !isnan_d(daily[i-1].k) && !isnan_d(daily[i-1].d)
                       && (daily[i].k > daily[i].d) && (daily[i-1].k <= daily[i-1].d);
        bool kdj_death  = !isnan_d(daily[i].k) && !isnan_d(daily[i].d)
                       && !isnan_d(daily[i-1].k) && !isnan_d(daily[i-1].d)
                       && (daily[i].k < daily[i].d) && (daily[i-1].k >= daily[i-1].d);
        bool ema_golden = !isnan_d(daily[i].ema5) && !isnan_d(daily[i].ema13)
                       && !isnan_d(daily[i-1].ema5) && !isnan_d(daily[i-1].ema13)
                       && (daily[i].ema5 > daily[i].ema13)
                       && (daily[i-1].ema5 <= daily[i-1].ema13);

        int sig = 0;
        if (bull) {
            if (kdj_golden) sig = 1;
            if (daily[i].close < daily[i].ema5 || kdj_death) sig = -1;
        }
        if (bear) {
            if (ema_golden) sig = 1;
            if ((daily[i].close < daily[i].ema5 && daily[i].close < daily[i].mid_bb_d) || kdj_death)
                sig = -1;
        }
        rows[i].signal = sig;
    }
    // row 0
    rows[0].date = daily[0].date;
    rows[0].open = daily[0].open;
    rows[0].high = daily[0].high;
    rows[0].low  = daily[0].low;
    rows[0].close = daily[0].close;

    // ----- execute with 3% stop-loss -----
    int position = 0;
    double entry_price = 0.0;
    double equity = 1.0, peak = 1.0;

    for (int i = 1; i < N; ++i) {
        // stop-loss check first (using today's bar while already in position)
        if (position == 1) {
            double stop_px = entry_price * (1.0 - STOP_LOSS);
            if (daily[i].low <= stop_px) {
                double exit_px = (daily[i].open < stop_px) ? daily[i].open : stop_px;
                rows[i].exit_price  = exit_px;
                rows[i].pnl         = exit_px / entry_price - 1.0;
                rows[i].exit_reason = "stop_loss";
                rows[i].position    = 0;
                position = 0;
                // equity update for this bar
                rows[i].strategy_ret = rows[i].pnl; // approximate for stop day
                equity *= (1.0 + rows[i].strategy_ret);
                rows[i].equity = equity;
                peak = max(peak, equity);
                continue;
            }
            if (daily[i].close <= stop_px) {
                rows[i].exit_price  = daily[i].close;
                rows[i].pnl         = daily[i].close / entry_price - 1.0;
                rows[i].exit_reason = "stop_loss";
                rows[i].position    = 0;
                position = 0;
                rows[i].strategy_ret = rows[i].pnl;
                equity *= (1.0 + rows[i].strategy_ret);
                rows[i].equity = equity;
                peak = max(peak, equity);
                continue;
            }
        }

        rows[i].position = position;

        // execute previous day's signal at today's open
        int prev_sig = rows[i - 1].signal;
        if (prev_sig == 1 && position == 0) {
            position = 1;
            entry_price = daily[i].open;
            rows[i].entry_price = entry_price;
            rows[i].position    = 1;
        } else if (prev_sig == -1 && position == 1) {
            double exit_px = daily[i].open;
            rows[i].exit_price  = exit_px;
            rows[i].pnl         = exit_px / entry_price - 1.0;
            rows[i].exit_reason = "signal";
            position = 0;
            rows[i].position = 0;
        }

        // mark-to-market return when in position from previous bar
        if (rows[i - 1].position == 1) {
            rows[i].strategy_ret = daily[i].close / daily[i - 1].close - 1.0;
        }
        equity *= (1.0 + rows[i].strategy_ret);
        rows[i].equity = equity;
        peak = max(peak, equity);
    }

    // close open position at end
    if (position == 1) {
        int last = N - 1;
        rows[last].exit_price  = daily[last].close;
        rows[last].pnl         = daily[last].close / entry_price - 1.0;
        rows[last].exit_reason = "end_of_data";
        rows[last].position    = 0;
    }

    // buy & hold equity
    rows[0].bh_equity = 1.0;
    rows[0].equity = 1.0;
    for (int i = 1; i < N; ++i) {
        double bh_ret = daily[i].close / daily[i - 1].close - 1.0;
        rows[i].bh_equity = rows[i - 1].bh_equity * (1.0 + bh_ret);
    }

    // write result
    string safe = code_to_stem(code);
    string out_path = OUT_DIR + "/" + safe + "_backtest.csv";
    ofstream fout(out_path);
    fout << fixed << setprecision(6);
    fout << "time_key,open,high,low,close,"
         << "mid_bb_d,mid_bb_w,ema5,ema13,K,D,"
         << "signal,position,entry_price,exit_price,pnl,exit_reason,"
         << "strategy_ret,equity,bh_equity\n";

    for (auto& r : rows) {
        fout << r.date << ","
             << r.open << "," << r.high << "," << r.low << "," << r.close << ","
             << r.mid_bb_d << "," << r.mid_bb_w << ","
             << r.ema5 << "," << r.ema13 << ","
             << r.k << "," << r.d << ","
             << r.signal << "," << r.position << ","
             << r.entry_price << "," << r.exit_price << ","
             << r.pnl << "," << r.exit_reason << ","
             << r.strategy_ret << "," << r.equity << "," << r.bh_equity << "\n";
    }
    fout.close();

    // quick stats
    int n_trades = 0, n_wins = 0, n_stop = 0;
    for (auto& r : rows) {
        if (r.pnl != 0.0) {
            ++n_trades;
            if (r.pnl > 0) ++n_wins;
            if (r.exit_reason == "stop_loss") ++n_stop;
        }
    }
    cout << "  " << code
         << "  equity=" << rows.back().equity
         << "  trades=" << n_trades
         << "  win%=" << (n_trades ? 100.0 * n_wins / n_trades : 0)
         << "  stops=" << n_stop
         << "  → " << out_path << "\n";
}

int main() {
    cout << "=== Dual-Regime Backtester (BB n=40 sd=2, 3% stop) ===\n";
    fs::create_directories(OUT_DIR);

    auto codes = load_codes();
    cout << "Codes: " << codes.size() << "\n";

    for (auto& c : codes) {
        cout << "Backtesting " << c << " ...\n";
        backtest_one(c);
    }
    cout << "Done. Results in " << OUT_DIR << "/\n";
    return 0;
}





