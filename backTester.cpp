/*
 * backTester.cpp
 * ============================================================
 * STRATEGY: HK Momentum Trend V4-LS-v2
 * (Long Stock + Selective Call; Puts DISABLED)
 * ============================================================
 *
 * Aggressive trend participation both sides.
 * No profit locks — 3×ATR trail lets winners run.
 *
 * LONG STOCK entry (T close → T+1 open):
 *   EMA5 > EMA13 > EMA34, ADX > 15, Close > BB mid
 * LONG CALL (optional overlay):
 *   Full EMA stack, ADX > 25, Close > 60d high, Vol > 1.5× MA
 *
 * LONG PUT entry (no short stock):
 *   EMA5 < EMA13 < EMA34, ADX > 15, Close < BB mid
 *   Overlay put only if ADX > 25, Close < 60d low, Vol > 1.5× MA
 *
 * HSI bias (entries only):
 *   HSI > EMA200 → allow longs/calls
 *   HSI < EMA200 → allow puts
 *
 * Stops: 3×ATR init + 3×ATR trail (favor direction only)
 *
 * g++ -O2 -std=c++17 backTester.cpp -o backtester && ./backtester
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <map>
#include <set>
#include <numeric>

using namespace std;
namespace fs = std::filesystem;

#ifndef NAN
#define NAN (numeric_limits<double>::quiet_NaN())
#endif

// ===================== CONFIG =====================
const string LIST_PATH = "monitorCodeList.csv";
const string D_DIR     = "d_history";
const string OUT_DIR   = "testResult";
const string HSI_PATH  = "d_history/HSI.csv";

const double INIT_CAPITAL      = 1'000'000.0;
const double RISK_PER_TRADE    = 0.03;  // was 1.5% (was 2%)
const double MAX_PER_STOCK     = 0.12;   // 12%
const double MAX_PORT_EXPOSURE = 0.85;   // 85%
const int    MAX_LONGS         = 8;         // default =8
const int    MAX_PUTS          = 0;      // DISABLED — puts wiped equity in V4-LS default = 0
const int    MAX_CALLS         = 3;      // fewer, higher quality only default =3 

const double ADX_ENTRY         = 20.0;   // stricter than 15
const double ADX_OPT           = 28.0;   // calls only on strong trends
const double VOL_MULT_OPT      = 1.8;
const double ATR_MULT          = 2.5;    // slightly tighter than 3
const double VOL_MULT_STOCK    = 1.2;    // stock needs mild volume

const bool   ENABLE_PUTS       = false;  // set true only after stock PF > 1.2
const bool   ENABLE_CALLS      = true;

const double OPTION_PREMIUM    = 0.04;   // 4% of spot
const double OPTION_STOP       = 0.60;   // give calls more room
const double OPTION_TP         = 2.00;   // +200% target
const double OPT_RISK_FRAC     = 0.003;  // 0.3% portfolio per call
const int    CALL_MIN_HOLD     = 3;      // don't evaluate opt_stop until 3 days held

const int BB_N = 20, ATR_N = 20, ADX_N = 14, VOL_MA_N = 20;
const int HH60 = 60, LL60 = 60;
// ==================================================

struct Bar {
    string date;
    double open = NAN, high = NAN, low = NAN, close = NAN, volume = NAN;
    double ema5 = NAN, ema13 = NAN, ema34 = NAN;
    double mid_bb = NAN, atr = NAN, adx = NAN, vol_ma = NAN;
    double hh60 = NAN, ll60 = NAN;
};

enum class Side { LONG_STOCK, LONG_CALL, LONG_PUT };

struct Position {
    string stem;
    Side side = Side::LONG_STOCK;
    double shares = 0;           // stock shares OR notionals for option
    double entry = 0;
    string entry_date;
    double init_stop = 0;
    double trail_stop = NAN;
    double peak = 0;             // peak close (long) or trough close (put)
    double peak_high = 0;
    double trough_low = 1e99;
    double atr_at_entry = 0;
    int    days_held = 0;
    // option fields
    bool is_option = false;
    double opt_premium = 0;      // $ paid
    double opt_mtm = 0;
};

struct TradeRec {
    string stem;
    string type;  // LONG_STOCK / LONG_CALL / LONG_PUT
    string entry_date, exit_date;
    int holding_days = 0;
    double entry_px = 0, exit_px = 0;
    double pnl = 0, mfe = 0, mae = 0;
    string exit_reason;
};

struct DayLog {
    string date;
    double equity = 0, cash = 0;
    double stock_val = 0, call_val = 0, put_val = 0;
    int n_long = 0, n_call = 0, n_put = 0;
};

vector<string> split(const string& s, char d = ',') {
    vector<string> t; string x; istringstream ss(s);
    while (getline(ss, x, d)) t.push_back(x);
    return t;
}
double to_d(const string& s) {
    if (s.empty() || s == "nan" || s == "NaN" || s == "null") return NAN;
    try { return stod(s); } catch (...) { return NAN; }
}
bool isnan_d(double x) { return x != x; }
string code_to_stem(const string& c) {
    string s; for (char ch : c) if (ch != '.') s.push_back(ch); return s;
}

string find_csv(const string& dir, const string& code) {
    string stem = code_to_stem(code);
    for (auto& p : {dir + "/" + code + ".csv", dir + "/" + stem + ".csv"})
        if (fs::exists(p)) return p;
    if (fs::exists(dir))
        for (auto& e : fs::directory_iterator(dir)) {
            string n = e.path().filename().string();
            if (n.find(stem) != string::npos && n.size() >= 4 &&
                n.substr(n.size()-4) == ".csv") return e.path().string();
        }
    return "";
}

vector<string> load_codes() {
    vector<string> codes;
    ifstream fin(LIST_PATH);
    if (!fin) { cerr << "Cannot open " << LIST_PATH << "\n"; return codes; }
    string line; bool first = true;
    while (getline(fin, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (!line.empty()) line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty()) continue;
        if (first) {
            first = false;
            string lo = line; for (char& c : lo) c = (char)tolower(c);
            if (lo.find("code") != string::npos || lo == "symbol") continue;
        }
        codes.push_back(line);
    }
    return codes;
}

vector<Bar> load_bars(const string& path) {
    vector<Bar> bars;
    ifstream fin(path);
    if (!fin) return bars;
    string line; getline(fin, line);
    auto header = split(line);
    map<string,int> col;
    for (size_t i = 0; i < header.size(); ++i) {
        string h = header[i];
        for (char& c : h) c = (char)tolower(c);
        col[h] = (int)i;
    }
    auto get = [&](const vector<string>& t, const string& n) -> double {
        auto it = col.find(n);
        if (it == col.end() || it->second >= (int)t.size()) return NAN;
        return to_d(t[it->second]);
    };
    auto gets = [&](const vector<string>& t, const string& n) -> string {
        auto it = col.find(n);
        if (it == col.end() || it->second >= (int)t.size()) return "";
        return t[it->second];
    };
    while (getline(fin, line)) {
        if (line.empty()) continue;
        auto t = split(line);
        Bar b;
        string tk = gets(t, "time_key");
        if (tk.empty()) tk = gets(t, "date");
        if (tk.empty() && t.size() > 1) tk = t[1];
        b.date = tk.substr(0, min<size_t>(10, tk.size()));
        b.open = get(t,"open"); b.high = get(t,"high");
        b.low = get(t,"low"); b.close = get(t,"close");
        b.volume = get(t,"volume");
        if (isnan_d(b.volume)) b.volume = get(t,"vol");
        if (!isnan_d(b.close) && b.close > 0) bars.push_back(b);
    }
    return bars;
}

void ema_series(const vector<Bar>& bars, int span, vector<double>& out) {
    int N = (int)bars.size();
    out.assign(N, NAN);
    if (!N) return;
    double a = 2.0 / (span + 1.0);
    out[0] = bars[0].close;
    for (int i = 1; i < N; ++i)
        out[i] = a * bars[i].close + (1 - a) * out[i-1];
}

void compute_indicators(vector<Bar>& bars) {
    int N = (int)bars.size();
    if (N < 60) return;
    vector<double> e5, e13, e34;
    ema_series(bars, 5, e5);
    ema_series(bars, 13, e13);
    ema_series(bars, 34, e34);
    for (int i = 0; i < N; ++i) {
        bars[i].ema5 = e5[i]; bars[i].ema13 = e13[i]; bars[i].ema34 = e34[i];
    }
    for (int i = BB_N - 1; i < N; ++i) {
        double s = 0;
        for (int j = i - BB_N + 1; j <= i; ++j) s += bars[j].close;
        bars[i].mid_bb = s / BB_N;
    }
    vector<double> tr(N, 0);
    for (int i = 1; i < N; ++i) {
        double h = bars[i].high, l = bars[i].low, pc = bars[i-1].close;
        tr[i] = max({h-l, fabs(h-pc), fabs(l-pc)});
    }
    for (int i = ATR_N; i < N; ++i) {
        double s = 0;
        for (int j = i - ATR_N + 1; j <= i; ++j) s += tr[j];
        bars[i].atr = s / ATR_N;
    }
    for (int i = VOL_MA_N - 1; i < N; ++i) {
        double s = 0; int c = 0;
        for (int j = i - VOL_MA_N + 1; j <= i; ++j)
            if (!isnan_d(bars[j].volume)) { s += bars[j].volume; ++c; }
        bars[i].vol_ma = c ? s / c : NAN;
    }
    // ADX
    vector<double> pdm(N,0), mdm(N,0), tr2(N,0);
    for (int i = 1; i < N; ++i) {
        double up = bars[i].high - bars[i-1].high;
        double dn = bars[i-1].low - bars[i].low;
        pdm[i] = (up > dn && up > 0) ? up : 0;
        mdm[i] = (dn > up && dn > 0) ? dn : 0;
        double h = bars[i].high, l = bars[i].low, pc = bars[i-1].close;
        tr2[i] = max({h-l, fabs(h-pc), fabs(l-pc)});
    }
    if (N > 2 * ADX_N) {
        double atr_s=0,p_s=0,m_s=0;
        for (int i = 1; i <= ADX_N; ++i) {
            atr_s += tr2[i]; p_s += pdm[i]; m_s += mdm[i];
        }
        vector<double> dx(N,0);
        for (int i = ADX_N; i < N; ++i) {
            if (i > ADX_N) {
                atr_s = atr_s - atr_s/ADX_N + tr2[i];
                p_s = p_s - p_s/ADX_N + pdm[i];
                m_s = m_s - m_s/ADX_N + mdm[i];
            }
            double pdi = atr_s>0 ? 100*p_s/atr_s : 0;
            double mdi = atr_s>0 ? 100*m_s/atr_s : 0;
            double den = pdi+mdi;
            dx[i] = den>0 ? 100*fabs(pdi-mdi)/den : 0;
        }
        double adx = 0;
        for (int i = ADX_N; i < 2*ADX_N; ++i) adx += dx[i];
        adx /= ADX_N;
        for (int i = 2*ADX_N; i < N; ++i) {
            adx = (adx*(ADX_N-1) + dx[i]) / ADX_N;
            bars[i].adx = adx;
        }
    }
    for (int i = 0; i < N; ++i) {
        if (i >= HH60) {
            double hh = bars[i-HH60].high;
            for (int j = i-HH60+1; j < i; ++j) hh = max(hh, bars[j].high);
            bars[i].hh60 = hh;
        }
        if (i >= LL60) {
            double ll = bars[i-LL60].low;
            for (int j = i-LL60+1; j < i; ++j) ll = min(ll, bars[j].low);
            bars[i].ll60 = ll;
        }
    }
}

struct StockData {
    string code, stem;
    vector<Bar> bars;
    map<string,int> date_idx;
};

bool long_entry(const Bar& b) {
    if (isnan_d(b.ema5)||isnan_d(b.ema13)||isnan_d(b.ema34)||
        isnan_d(b.adx)||isnan_d(b.mid_bb)||isnan_d(b.atr)) return false;
    if (!(b.ema5 > b.ema13 && b.ema13 > b.ema34)) return false;
    if (!(b.adx > ADX_ENTRY)) return false;
    if (!(b.close > b.mid_bb)) return false;
    // mild volume confirmation (skip if no volume data)
    if (!isnan_d(b.volume) && !isnan_d(b.vol_ma) && b.vol_ma > 0) {
        if (!(b.volume > VOL_MULT_STOCK * b.vol_ma)) return false;
    }
    return true;
}

bool call_ok(const Bar& b) {
    if (!ENABLE_CALLS) return false;
    if (!long_entry(b)) return false;
    if (b.adx <= ADX_OPT) return false;
    // confirmed breakout, not marginal
    if (isnan_d(b.hh60) || !(b.close > b.hh60 * 1.02)) return false;
    if (!isnan_d(b.volume) && !isnan_d(b.vol_ma) && b.vol_ma > 0)
        if (!(b.volume > VOL_MULT_OPT * b.vol_ma)) return false;
    return true;
}

bool put_entry(const Bar& b) {
    if (!ENABLE_PUTS) return false;
    if (isnan_d(b.ema5)||isnan_d(b.ema13)||isnan_d(b.ema34)||
        isnan_d(b.adx)||isnan_d(b.mid_bb)||isnan_d(b.atr)) return false;
    return b.ema5 < b.ema13 && b.ema13 < b.ema34 &&
           b.adx > ADX_ENTRY && b.close < b.mid_bb;
}

bool put_opt_ok(const Bar& b) {
    if (!ENABLE_PUTS) return false;
    if (!put_entry(b)) return false;
    if (b.adx <= ADX_OPT) return false;
    if (isnan_d(b.ll60) || !(b.close < b.ll60 * 0.98)) return false; // 2% below low
    if (!isnan_d(b.volume) && !isnan_d(b.vol_ma) && b.vol_ma > 0)
        if (!(b.volume > VOL_MULT_OPT * b.vol_ma)) return false;
    return true;
}

double call_ps(double spot, double entry) { return max(0.0, spot - entry); }
double put_ps(double spot, double entry)  { return max(0.0, entry - spot); }

int days_between(const string& a, const string& b) {
    auto ymd = [](const string& s) {
        if (s.size() < 10) return 0;
        try {
            return stoi(s.substr(0,4))*372 + stoi(s.substr(5,2))*31 + stoi(s.substr(8,2));
        } catch (...) { return 0; }
    };
    return max(0, ymd(b) - ymd(a));
}

string side_str(Side s) {
    if (s == Side::LONG_STOCK) return "LONG_STOCK";
    if (s == Side::LONG_CALL)  return "LONG_CALL";
    return "LONG_PUT";
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    cout << "=== HK Momentum Trend V4-LS-v2 (Stock-first) ===\n";
    cout << "Puts OFF | Selective calls | ADX>20 | trail after +5% | BE at +8%\n";
    fs::create_directories(OUT_DIR);

    auto codes = load_codes();
    cout << "Codes: " << codes.size() << "\n";

    vector<StockData> universe;
    set<string> all_dates;
    for (auto& code : codes) {
        string path = find_csv(D_DIR, code);
        if (path.empty()) continue;
        StockData sd;
        sd.code = code; sd.stem = code_to_stem(code);
        sd.bars = load_bars(path);
        if ((int)sd.bars.size() < 80) continue;
        compute_indicators(sd.bars);
        for (size_t i = 0; i < sd.bars.size(); ++i) {
            sd.date_idx[sd.bars[i].date] = (int)i;
            all_dates.insert(sd.bars[i].date);
        }
        universe.push_back(std::move(sd));
    }
    cout << "Loaded " << universe.size() << " stocks\n";

    // HSI EMA200
    vector<Bar> hsi;
    map<string,int> hsi_idx;
    bool have_hsi = false;
    {
        string p = HSI_PATH;
        if (!fs::exists(p)) {
            vector<string> cands = {
                D_DIR+"/HSI.csv", D_DIR+"/HSImain.csv",
                D_DIR+"/HK.HSImain.csv", string("HSI.csv"), string("HSImain.csv")
            };
            for (auto& c : cands) if (fs::exists(c)) { p = c; break; }
        }
        if (fs::exists(p)) {
            hsi = load_bars(p);
            if ((int)hsi.size() >= 220) {
                vector<double> e200;
                ema_series(hsi, 200, e200);
                for (size_t i = 0; i < hsi.size(); ++i) {
                    hsi[i].ema34 = e200[i]; // store EMA200
                    hsi_idx[hsi[i].date] = (int)i;
                }
                have_hsi = true;
                cout << "HSI ON path=" << p << " bars=" << hsi.size() << "\n";
            }
        }
    }
    if (!have_hsi) cout << "HSI not found — both long and put allowed always\n";

    vector<string> timeline(all_dates.begin(), all_dates.end());
    sort(timeline.begin(), timeline.end());

    map<string, StockData*> by_stem;
    for (auto& sd : universe) by_stem[sd.stem] = &sd;

    double cash = INIT_CAPITAL;
    // key = stem + "_" + type
    map<string, Position> positions;
    vector<DayLog> logs;
    vector<TradeRec> trades;

    auto pos_key = [](const string& stem, Side s) {
        return stem + "_" + side_str(s);
    };

    struct PendingExit { string key; string reason; };
    struct PendingEntry {
        string stem; Side side; int sig_idx; double score;
    };
    vector<PendingExit> pend_exits;
    vector<PendingEntry> pend_entries;

    auto count_side = [&](Side s) {
        int n = 0;
        for (auto& kv : positions) if (kv.second.side == s) ++n;
        return n;
    };

    for (size_t ti = 0; ti < timeline.size(); ++ti) {
        const string& day = timeline[ti];

        // ===== fills: exits =====
        for (auto& pe : pend_exits) {
            auto pit = positions.find(pe.key);
            if (pit == positions.end()) continue;
            Position& pos = pit->second;
            auto* sd = by_stem[pos.stem];
            if (!sd) continue;
            auto dit = sd->date_idx.find(day);
            double fill = pos.entry;
            if (dit != sd->date_idx.end())
                fill = sd->bars[dit->second].open;

            double pnl = 0, settle = 0;
            if (!pos.is_option) {
                settle = pos.shares * fill;
                cash += settle;
                pnl = fill / pos.entry - 1.0;
            } else {
                double pu = OPTION_PREMIUM * pos.entry;
                double n = pu > 1e-12 ? pos.opt_premium / pu : 0;
                double val = (pos.side == Side::LONG_CALL)
                           ? call_ps(fill, pos.entry) * n
                           : put_ps(fill, pos.entry) * n;
                cash += max(0.0, val);
                pnl = pos.opt_premium > 0 ? val / pos.opt_premium - 1.0 : -1;
            }

            TradeRec tr;
            tr.stem = pos.stem;
            tr.type = side_str(pos.side);
            tr.entry_date = pos.entry_date;
            tr.exit_date = day;
            tr.holding_days = days_between(tr.entry_date, tr.exit_date);
            tr.entry_px = pos.entry;
            tr.exit_px = fill;
            tr.pnl = pnl;
            if (!pos.is_option) {
                tr.mfe = pos.peak_high / pos.entry - 1.0;
                tr.mae = pos.trough_low / pos.entry - 1.0;
                if (pos.side == Side::LONG_PUT) {
                    // put on stock leg shouldn't happen; stock is long only
                }
            } else {
                // option MFE approx from peak/trough intrinsic
                double pu = OPTION_PREMIUM * pos.entry;
                double n = pu > 1e-12 ? pos.opt_premium / pu : 0;
                if (pos.side == Side::LONG_CALL) {
                    double best = call_ps(pos.peak_high, pos.entry) * n;
                    double worst = 0;
                    tr.mfe = pos.opt_premium > 0 ? best / pos.opt_premium - 1 : 0;
                    tr.mae = -1.0; // max loss premium
                } else {
                    double best = put_ps(pos.trough_low, pos.entry) * n;
                    tr.mfe = pos.opt_premium > 0 ? best / pos.opt_premium - 1 : 0;
                    tr.mae = -1.0;
                }
            }
            tr.exit_reason = pe.reason;
            trades.push_back(tr);
            positions.erase(pit);
        }
        pend_exits.clear();

        // ===== equity for sizing =====
        double stock_val = 0, call_val = 0, put_val = 0;
        for (auto& kv : positions) {
            auto* sd = by_stem[kv.second.stem];
            if (!sd) continue;
            auto dit = sd->date_idx.find(day);
            if (dit == sd->date_idx.end()) continue;
            double px = sd->bars[dit->second].open;
            if (!kv.second.is_option) {
                stock_val += kv.second.shares * px;
            } else {
                double pu = OPTION_PREMIUM * kv.second.entry;
                double n = pu > 1e-12 ? kv.second.opt_premium / pu : 0;
                double v = (kv.second.side == Side::LONG_CALL)
                         ? call_ps(px, kv.second.entry) * n
                         : put_ps(px, kv.second.entry) * n;
                if (kv.second.side == Side::LONG_CALL) call_val += v;
                else put_val += v;
            }
        }
        double equity = cash + stock_val + call_val + put_val;

        // ===== fills: entries =====
        sort(pend_entries.begin(), pend_entries.end(),
             [](const PendingEntry& a, const PendingEntry& b) {
                 return a.score > b.score;
             });

        for (auto& pe : pend_entries) {
            string key = pos_key(pe.stem, pe.side);
            if (positions.count(key)) continue;
            auto* sd = by_stem[pe.stem];
            if (!sd) continue;
            auto dit = sd->date_idx.find(day);
            if (dit == sd->date_idx.end()) continue;
            double fill = sd->bars[dit->second].open;
            double atr = sd->bars[pe.sig_idx].atr;
            if (isnan_d(atr) || atr <= 0 || fill <= 0) continue;

            if (pe.side == Side::LONG_STOCK) {
                if (count_side(Side::LONG_STOCK) >= MAX_LONGS) continue;
                double stop_dist = ATR_MULT * atr;
                double shares = (equity * RISK_PER_TRADE) / stop_dist;
                double notional = shares * fill;
                double max_n = equity * MAX_PER_STOCK;
                if (notional > max_n) { shares = max_n / fill; notional = max_n; }
                double room = equity * MAX_PORT_EXPOSURE - stock_val;
                if (room <= 0) continue;
                if (notional > room) { shares = room / fill; notional = room; }
                if (notional > cash * 0.99) {
                    shares = cash * 0.99 / fill;
                    notional = shares * fill;
                }
                if (shares < 1 || notional < 1000) continue;

                cash -= notional;
                stock_val += notional;

                Position pos;
                pos.stem = pe.stem;
                pos.side = Side::LONG_STOCK;
                pos.shares = shares;
                pos.entry = fill;
                pos.entry_date = day;
                pos.init_stop = fill - stop_dist;
                pos.trail_stop = NAN;
                pos.peak = fill;
                pos.peak_high = sd->bars[dit->second].high;
                pos.trough_low = sd->bars[dit->second].low;
                pos.atr_at_entry = atr;
                pos.is_option = false;
                positions[key] = pos;
            }
            else if (pe.side == Side::LONG_CALL) {
                if (count_side(Side::LONG_CALL) >= MAX_CALLS) continue;
                double budget = equity * OPT_RISK_FRAC;
                if (budget <= 0 || budget > cash) continue;
                cash -= budget;

                Position pos;
                pos.stem = pe.stem;
                pos.side = Side::LONG_CALL;
                pos.entry = fill;
                pos.entry_date = day;
                pos.is_option = true;
                pos.opt_premium = budget;
                pos.opt_mtm = budget;
                pos.peak = fill;
                pos.peak_high = fill;
                pos.trough_low = fill;
                positions[key] = pos;
            }
            else if (pe.side == Side::LONG_PUT) {
                if (count_side(Side::LONG_PUT) >= MAX_PUTS) continue;
                // put as option-only (no short stock)
                double budget = equity * OPT_RISK_FRAC;
                // if put_opt_ok was required at signal, still size as option
                // Spec: LONG PUT OVERLAY when conditions met — we treat put as option position
                if (budget <= 0 || budget > cash) continue;
                cash -= budget;

                Position pos;
                pos.stem = pe.stem;
                pos.side = Side::LONG_PUT;
                pos.entry = fill;
                pos.entry_date = day;
                pos.is_option = true;
                pos.opt_premium = budget;
                pos.opt_mtm = budget;
                pos.peak = fill;
                pos.peak_high = fill;
                pos.trough_low = fill;
                pos.init_stop = fill + ATR_MULT * atr; // for reference
                pos.atr_at_entry = atr;
                positions[key] = pos;
            }
        }
        pend_entries.clear();

        // ===== HSI bias =====
        bool allow_long = true, allow_put = true;
        if (have_hsi) {
            auto it = hsi_idx.find(day);
            if (it != hsi_idx.end()) {
                int hi = it->second;
                bool bull = !isnan_d(hsi[hi].ema34) && hsi[hi].close > hsi[hi].ema34;
                allow_long = bull;
                allow_put = !bull;
            }
        }

        // ===== manage positions =====
        for (auto& kv : positions) {
            Position& pos = kv.second;
            auto* sd = by_stem[pos.stem];
            if (!sd) continue;
            auto dit = sd->date_idx.find(day);
            if (dit == sd->date_idx.end()) continue;
            Bar& b = sd->bars[dit->second];

            if (!pos.is_option) {
                // LONG STOCK
                pos.days_held++;
                pos.peak = max(pos.peak, b.close);
                pos.peak_high = max(pos.peak_high, b.high);
                pos.trough_low = min(pos.trough_low, b.low);

                double gain = pos.entry > 0 ? b.close / pos.entry - 1.0 : 0;
                // soft BE: after +8% MFE, floor stop at entry (never full loser)
                double mfe_close = pos.entry > 0 ? pos.peak / pos.entry - 1.0 : 0;
                if (mfe_close >= 0.08 && pos.init_stop < pos.entry)
                    pos.init_stop = pos.entry;

                double atr_now = (!isnan_d(b.atr) && b.atr > 0) ? b.atr : pos.atr_at_entry;
                // activate trail only after +5% to avoid early noise
                if (mfe_close >= 0.05) {
                    double new_trail = pos.peak - ATR_MULT * atr_now;
                    if (isnan_d(pos.trail_stop) || new_trail > pos.trail_stop)
                        pos.trail_stop = new_trail;
                }

                bool exit_sig = false;
                string reason;
                if (b.close < pos.init_stop) {
                    exit_sig = true; reason = "init_stop";
                }
                if (!isnan_d(pos.trail_stop) && b.close < pos.trail_stop) {
                    exit_sig = true; reason = "trail_stop";
                }
                if (!isnan_d(b.ema5) && !isnan_d(b.ema13) && !isnan_d(b.ema34) &&
                    b.ema5 < b.ema13 && b.close < b.ema34) {
                    exit_sig = true; reason = "trend_fail";
                }
                if (exit_sig)
                    pend_exits.push_back({kv.first, reason});
            }
            else if (pos.side == Side::LONG_CALL) {
                pos.days_held++;
                pos.peak_high = max(pos.peak_high, b.high);
                pos.trough_low = min(pos.trough_low, b.low);
                double pu = OPTION_PREMIUM * pos.entry;
                double n = pu > 1e-12 ? pos.opt_premium / pu : 0;
                pos.opt_mtm = call_ps(b.close, pos.entry) * n;
                double ret = pos.opt_premium > 0 ? pos.opt_mtm / pos.opt_premium - 1 : -1;
                double low_v = call_ps(b.low, pos.entry) * n;
                double low_ret = pos.opt_premium > 0 ? low_v / pos.opt_premium - 1 : -1;

                bool exit_sig = false;
                string reason;
                if (ret >= OPTION_TP) { exit_sig = true; reason = "opt_tp"; }
                // do not stop out options in first CALL_MIN_HOLD days (avoid 1-day -100%)
                if (pos.days_held >= CALL_MIN_HOLD &&
                    (ret <= -OPTION_STOP || low_ret <= -OPTION_STOP)) {
                    exit_sig = true; reason = "opt_stop";
                }
                // exit if no matching long stock (stock exited)
                string sk = pos_key(pos.stem, Side::LONG_STOCK);
                if (!positions.count(sk)) {
                    // only force if stock was never taken same day — actually
                    // exit call when stock not held
                    // Check if stock is pending exit or already gone
                    bool stock_open = positions.count(sk);
                    // stock may exit same cycle; defer to next day via pending
                }
                // Call exits with stock: if stock has pending exit, call follows at fill
                for (auto& px : pend_exits) {
                    if (px.key == sk) {
                        exit_sig = true; reason = "stock_exit";
                        break;
                    }
                }
                if (exit_sig)
                    pend_exits.push_back({kv.first, reason});
            }
            else if (pos.side == Side::LONG_PUT) {
                pos.days_held++;
                pos.peak_high = max(pos.peak_high, b.high);
                pos.trough_low = min(pos.trough_low, b.low);
                pos.peak = min(pos.peak, b.close); // trough for put

                double atr_now = (!isnan_d(b.atr) && b.atr > 0) ? b.atr : pos.atr_at_entry;
                // trail for put underlying: lowest close + 3*ATR (stop when price rises)
                double new_trail = pos.peak + ATR_MULT * atr_now; // pos.peak is min close
                if (isnan_d(pos.trail_stop) || new_trail < pos.trail_stop)
                    pos.trail_stop = new_trail;

                double pu = OPTION_PREMIUM * pos.entry;
                double n = pu > 1e-12 ? pos.opt_premium / pu : 0;
                pos.opt_mtm = put_ps(b.close, pos.entry) * n;
                double ret = pos.opt_premium > 0 ? pos.opt_mtm / pos.opt_premium - 1 : -1;
                double high_v = put_ps(b.high, pos.entry) * n; // worst for put when price up
                double high_ret = pos.opt_premium > 0 ? high_v / pos.opt_premium - 1 : -1;

                bool exit_sig = false;
                string reason;
                if (ret >= OPTION_TP) { exit_sig = true; reason = "opt_tp"; }
                if (pos.days_held >= CALL_MIN_HOLD &&
                    (ret <= -OPTION_STOP || high_ret <= -OPTION_STOP)) {
                    exit_sig = true; reason = "opt_stop";
                }
                // trend failure for put: EMA5 > EMA13 AND close > EMA34
                if (!isnan_d(b.ema5) && !isnan_d(b.ema13) && !isnan_d(b.ema34) &&
                    b.ema5 > b.ema13 && b.close > b.ema34) {
                    exit_sig = true; reason = "trend_fail";
                }
                if (!isnan_d(pos.trail_stop) && b.close > pos.trail_stop) {
                    exit_sig = true; reason = "trail_stop";
                }
                if (exit_sig)
                    pend_exits.push_back({kv.first, reason});
            }
        }

        // ===== new signals =====
        for (auto& sd : universe) {
            auto dit = sd.date_idx.find(day);
            if (dit == sd.date_idx.end()) continue;
            int i = dit->second;
            Bar& b = sd.bars[i];

            // LONG STOCK
            if (allow_long && long_entry(b) &&
                !positions.count(pos_key(sd.stem, Side::LONG_STOCK)) &&
                count_side(Side::LONG_STOCK) < MAX_LONGS) {
                double sc = (!isnan_d(b.adx) ? b.adx : 0) +
                            (!isnan_d(b.ema13) && b.ema13 > 0 ?
                             (b.ema5 - b.ema13) / b.ema13 * 100 : 0);
                pend_entries.push_back({sd.stem, Side::LONG_STOCK, i, sc});

                // CALL overlay
                if (call_ok(b) && !positions.count(pos_key(sd.stem, Side::LONG_CALL)))
                    pend_entries.push_back({sd.stem, Side::LONG_CALL, i, sc + 10});
            }

            // LONG PUT (bear)
            if (allow_put && put_opt_ok(b) &&
                !positions.count(pos_key(sd.stem, Side::LONG_PUT)) &&
                count_side(Side::LONG_PUT) < MAX_PUTS) {
                double sc = (!isnan_d(b.adx) ? b.adx : 0);
                pend_entries.push_back({sd.stem, Side::LONG_PUT, i, sc});
            }
        }

        // EOD mark
        stock_val = call_val = put_val = 0;
        int n_long = 0, n_call = 0, n_put = 0;
        for (auto& kv : positions) {
            auto* sd = by_stem[kv.second.stem];
            if (!sd) continue;
            auto dit = sd->date_idx.find(day);
            if (dit == sd->date_idx.end()) continue;
            double px = sd->bars[dit->second].close;
            if (!kv.second.is_option) {
                stock_val += kv.second.shares * px;
                ++n_long;
            } else {
                double pu = OPTION_PREMIUM * kv.second.entry;
                double n = pu > 1e-12 ? kv.second.opt_premium / pu : 0;
                double v = (kv.second.side == Side::LONG_CALL)
                         ? call_ps(px, kv.second.entry) * n
                         : put_ps(px, kv.second.entry) * n;
                if (kv.second.side == Side::LONG_CALL) { call_val += v; ++n_call; }
                else { put_val += v; ++n_put; }
            }
        }
        DayLog dl;
        dl.date = day;
        dl.cash = cash;
        dl.stock_val = stock_val;
        dl.call_val = call_val;
        dl.put_val = put_val;
        dl.equity = cash + stock_val + call_val + put_val;
        dl.n_long = n_long; dl.n_call = n_call; dl.n_put = n_put;
        logs.push_back(dl);
    }

    // liquidate
    for (auto& kv : positions) {
        auto* sd = by_stem[kv.second.stem];
        if (!sd || sd->bars.empty()) continue;
        double px = sd->bars.back().close;
        Position& pos = kv.second;
        double pnl = 0;
        if (!pos.is_option) {
            cash += pos.shares * px;
            pnl = px / pos.entry - 1.0;
        } else {
            double pu = OPTION_PREMIUM * pos.entry;
            double n = pu > 1e-12 ? pos.opt_premium / pu : 0;
            double v = (pos.side == Side::LONG_CALL)
                     ? call_ps(px, pos.entry) * n
                     : put_ps(px, pos.entry) * n;
            cash += max(0.0, v);
            pnl = pos.opt_premium > 0 ? v / pos.opt_premium - 1 : -1;
        }
        TradeRec tr;
        tr.stem = pos.stem;
        tr.type = side_str(pos.side);
        tr.entry_date = pos.entry_date;
        tr.exit_date = sd->bars.back().date;
        tr.holding_days = days_between(tr.entry_date, tr.exit_date);
        tr.entry_px = pos.entry;
        tr.exit_px = px;
        tr.pnl = pnl;
        tr.mfe = pos.is_option ? 0 : pos.peak_high / pos.entry - 1.0;
        tr.mae = pos.is_option ? -1 : pos.trough_low / pos.entry - 1.0;
        tr.exit_reason = "end_of_data";
        trades.push_back(tr);
    }
    positions.clear();

    // PORTFOLIO
    {
        ofstream f(OUT_DIR + "/PORTFOLIO_equity.csv");
        f << fixed << setprecision(6);
        f << "time_key,equity,cash,stock_value,call_value,put_value,n_long,n_call,n_put\n";
        for (auto& d : logs)
            f << d.date << "," << d.equity / INIT_CAPITAL << ","
              << d.cash << "," << d.stock_val << "," << d.call_val << ","
              << d.put_val << "," << d.n_long << "," << d.n_call << ","
              << d.n_put << "\n";
    }

    // trades
    {
        ofstream f(OUT_DIR + "/trades.csv");
        f << fixed << setprecision(6);
        f << "code,type,entry_date,exit_date,holding_days,entry_px,exit_px,"
          << "pnl_pct,mfe_pct,mae_pct,exit_reason\n";
        for (auto& t : trades)
            f << t.stem << "," << t.type << "," << t.entry_date << ","
              << t.exit_date << "," << t.holding_days << ","
              << t.entry_px << "," << t.exit_px << ","
              << t.pnl << "," << t.mfe << "," << t.mae << ","
              << t.exit_reason << "\n";
    }

    // metrics by type
    auto calc_leg = [&](const string& type) {
        vector<double> pnls;
        for (auto& t : trades) if (t.type == type) pnls.push_back(t.pnl);
        return pnls;
    };
    auto leg_ret = [&](const vector<double>& pnls) {
        // approximate compounded product of (1+pnl) is not portfolio — report sum/avg
        if (pnls.empty()) return 0.0;
        double s = 0; for (double p : pnls) s += p;
        return s;
    };

    vector<double> stock_pnls = calc_leg("LONG_STOCK");
    vector<double> call_pnls  = calc_leg("LONG_CALL");
    vector<double> put_pnls   = calc_leg("LONG_PUT");

    double final_eq = logs.empty() ? INIT_CAPITAL : logs.back().equity;
    double total_ret = final_eq / INIT_CAPITAL - 1.0;

    vector<double> rets;
    for (size_t i = 1; i < logs.size(); ++i)
        if (logs[i-1].equity > 0)
            rets.push_back(logs[i].equity / logs[i-1].equity - 1.0);
    double mean_r = 0, std_r = 0, down_std = 0;
    if (!rets.empty()) {
        mean_r = accumulate(rets.begin(), rets.end(), 0.0) / rets.size();
        for (double r : rets) std_r += (r - mean_r) * (r - mean_r);
        std_r = sqrt(std_r / rets.size());
        int nd = 0;
        for (double r : rets) if (r < 0) { down_std += r*r; ++nd; }
        down_std = nd ? sqrt(down_std / nd) : 0;
    }
    double sharpe = std_r > 1e-12 ? mean_r / std_r * sqrt(252.0) : 0;
    double sortino = down_std > 1e-12 ? mean_r / down_std * sqrt(252.0) : 0;
    double peak = 0, max_dd = 0;
    for (auto& d : logs) {
        double eq = d.equity / INIT_CAPITAL;
        peak = max(peak, eq);
        if (peak > 0) max_dd = min(max_dd, (eq - peak) / peak);
    }
    double cagr = 0;
    if (logs.size() > 1) {
        double years = max(1.0, logs.size() / 252.0);
        cagr = pow(final_eq / INIT_CAPITAL, 1.0 / years) - 1.0;
    }

    int n_tr = (int)trades.size(), n_win = 0, n_loss = 0;
    double sum_w = 0, sum_l = 0, sum_hold = 0;
    vector<double> winners, losers;
    map<string,int> exit_cnt, type_cnt;
    for (auto& t : trades) {
        exit_cnt[t.exit_reason]++;
        type_cnt[t.type]++;
        sum_hold += t.holding_days;
        if (t.pnl > 0) { ++n_win; sum_w += t.pnl; winners.push_back(t.pnl); }
        else { ++n_loss; sum_l += t.pnl; losers.push_back(t.pnl); }
    }
    double win_rate = n_tr ? (double)n_win / n_tr : 0;
    double pf = (sum_l < 0) ? sum_w / fabs(sum_l) : (sum_w > 0 ? 99 : 0);
    double expc = n_tr ? (sum_w + sum_l) / n_tr : 0;
    double avg_w = n_win ? sum_w / n_win : 0;
    double avg_l = n_loss ? sum_l / n_loss : 0;

    cout << fixed << setprecision(4);
    cout << "\n==================== METRICS ====================\n";
    cout << "Total Return (combined): " << total_ret * 100 << "%\n";
    cout << "CAGR                   : " << cagr * 100 << "%\n";
    cout << "Sharpe                 : " << sharpe << "\n";
    cout << "Sortino                : " << sortino << "\n";
    cout << "Max Drawdown           : " << max_dd * 100 << "%\n";
    cout << "Win Rate               : " << win_rate * 100 << "%\n";
    cout << "Profit Factor          : " << pf << "\n";
    cout << "Expectancy             : " << expc * 100 << "%\n";
    cout << "Avg Winner             : " << avg_w * 100 << "%\n";
    cout << "Avg Loser              : " << avg_l * 100 << "%\n";
    cout << "Trades                 : " << n_tr << "\n";
    cout << "Avg Hold Days          : " << (n_tr ? sum_hold / n_tr : 0) << "\n";
    cout << "Portfolio Eq           : " << final_eq / INIT_CAPITAL << "x\n";
    cout << "\nBy type (sum of trade returns):\n";
    cout << "  LONG_STOCK : n=" << type_cnt["LONG_STOCK"]
         << "  sum_pnl=" << leg_ret(stock_pnls) << "\n";
    cout << "  LONG_CALL  : n=" << type_cnt["LONG_CALL"]
         << "  sum_pnl=" << leg_ret(call_pnls) << "\n";
    cout << "  LONG_PUT   : n=" << type_cnt["LONG_PUT"]
         << "  sum_pnl=" << leg_ret(put_pnls) << "\n";
    cout << "\nExit reasons:\n";
    for (auto& kv : exit_cnt)
        cout << "  " << left << setw(14) << kv.first << " " << kv.second << "\n";

    auto by_pnl = trades;
    sort(by_pnl.begin(), by_pnl.end(),
         [](const TradeRec& a, const TradeRec& b) { return a.pnl > b.pnl; });
    cout << "\n==================== TOP 20 ====================\n";
    for (int i = 0; i < min(20, (int)by_pnl.size()); ++i) {
        auto& t = by_pnl[i];
        cout << left << setw(12) << t.stem << setw(12) << t.type
             << right << setw(10) << t.pnl * 100 << "%  " << t.exit_reason << "\n";
    }
    cout << "\n==================== BOTTOM 20 ====================\n";
    int n = (int)by_pnl.size();
    for (int i = n - 1; i >= max(0, n - 20); --i) {
        auto& t = by_pnl[i];
        cout << left << setw(12) << t.stem << setw(12) << t.type
             << right << setw(10) << t.pnl * 100 << "%  " << t.exit_reason << "\n";
    }

    {
        ofstream f(OUT_DIR + "/metrics.txt");
        f << fixed << setprecision(4);
        f << "HK Momentum Trend V4-LS Metrics\n";
        f << "Total Return: " << total_ret * 100 << "%\n";
        f << "CAGR: " << cagr * 100 << "%\n";
        f << "Sharpe: " << sharpe << "\n";
        f << "Sortino: " << sortino << "\n";
        f << "Max DD: " << max_dd * 100 << "%\n";
        f << "Win Rate: " << win_rate * 100 << "%\n";
        f << "Profit Factor: " << pf << "\n";
        f << "Stock trades: " << type_cnt["LONG_STOCK"] << " sum=" << leg_ret(stock_pnls) << "\n";
        f << "Call trades: " << type_cnt["LONG_CALL"] << " sum=" << leg_ret(call_pnls) << "\n";
        f << "Put trades: " << type_cnt["LONG_PUT"] << " sum=" << leg_ret(put_pnls) << "\n";
    }

    // minimal per-stock CSVs for compatibility
    ofstream fsum(OUT_DIR + "/summary_all_stocks.csv");
    fsum << "code,n_trades,sum_pnl\n";
    map<string, pair<int,double>> agg;
    for (auto& t : trades) {
        agg[t.stem].first++;
        agg[t.stem].second += t.pnl;
    }
    for (auto& kv : agg)
        fsum << kv.first << "," << kv.second.first << "," << kv.second.second << "\n";
    fsum.close();

    cout << "\nWrote PORTFOLIO_equity.csv, trades.csv, metrics.txt\nDone.\n";
    return 0;
}