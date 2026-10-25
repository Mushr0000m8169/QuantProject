/*
 * backtester_with_options.cpp
 *
 * Based on original backtester.cpp signal logic, plus Options (Call) trading.
 *
 * Stock leg  : same rules as current backtester.cpp
 * Options leg: on the same buy signal, buy an ATM-ish Call;
 *              close the Call on the same exit signal / stop.
 *
 * Option P&L model (no live options chain needed):
 *   - Pay premium = OPTION_PREMIUM * spot at entry
 *   - Intrinsic at exit = max(spot_exit - spot_entry, 0)
 *   - Call PnL% = (intrinsic - premium) / premium
 *     → defined risk: max loss = -100% of premium capital
 *   - Optional leverage multiplier on the option capital slice
 *
 * Compile:
 *   g++ -std=c++17 -O2 -o bt_opt backtester_with_options.cpp
 *
 * Run:
 *   ./bt_opt --daily-dir d_history --list monitorCodeList.csv --out testResult_opt
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

using namespace std;
namespace fs = std::filesystem;

// ===================== tunable parameters =====================
double STOCK_STOP       = 0.03;   // stock hard stop 3%
double OPTION_PREMIUM   = 0.05;   // call premium as % of spot (e.g. 5%)
double OPTION_STOP      = 0.50;   // option stop: lose 50% of premium → exit
double OPTION_ALLOC     = 0.30;   // fraction of capital allocated to options (rest to stock)
double OPTION_LEVERAGE  = 1.0;    // extra multiplier on option P&L (1 = no extra)
// ==============================================================

struct DailyBar {
    string date;
    double open = NAN, high = NAN, low = NAN, close = NAN;
    double K = NAN, D = NAN, J = NAN;
    double ema5 = NAN, ema13 = NAN;
    double mid_bb_d = NAN;

    int signal = 0;                 // 1=buy, -1=sell (shared by stock & option)

    // ---- stock leg ----
    int    stock_pos = 0;
    double stock_entry = NAN;
    double stock_exit  = NAN;
    double stock_pnl   = 0.0;
    string stock_exit_reason;
    double stock_ret   = 0.0;
    double stock_equity = 1.0;

    // ---- option (call) leg ----
    int    opt_pos = 0;
    double opt_entry_spot = NAN;    // underlying price when call bought
    double opt_premium_paid = NAN;  // premium in price units
    double opt_exit_spot = NAN;
    double opt_pnl = 0.0;           // return on premium capital (-1 ~ +inf)
    string opt_exit_reason;
    double opt_ret = 0.0;           // daily marked return contribution
    double opt_equity = 1.0;

    // combined
    double combined_equity = 1.0;
    double bh_equity = 1.0;
};

vector<string> split(const string& s, char delim = ',') {
    vector<string> tokens;
    string token;
    istringstream ss(s);
    while (getline(ss, token, delim)) tokens.push_back(token);
    return tokens;
}

double to_d(const string& s) {
    if (s.empty() || s == "nan" || s == "NaN" || s == "null") return NAN;
    try { return stod(s); } catch (...) { return NAN; }
}

string code_to_filename(const string& code) {
    string name;
    for (char c : code) if (c != '.') name += c;
    return name + ".csv";
}

bool load_daily(const string& path, vector<DailyBar>& bars) {
    ifstream fin(path);
    if (!fin) return false;
    string line;
    getline(fin, line);
    auto header = split(line);
    map<string, int> col;
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
        auto t = split(line);
        if (t.size() < 5) continue;
        DailyBar b;
        string tk = gets(t, "time_key");
        b.date = (tk.size() >= 10) ? tk.substr(0, 10) : tk;
        b.open  = get(t, "open");
        b.close = get(t, "close");
        b.high  = get(t, "high");
        b.low   = get(t, "low");
        b.ema5  = get(t, "ema5");
        b.ema13 = get(t, "ema13");
        b.K     = get(t, "k");
        b.D     = get(t, "d");
        b.J     = get(t, "j");
        b.mid_bb_d = get(t, "boll_mid");
        bars.push_back(b);
    }
    return !bars.empty();
}

// ---------- same signal logic as backtester.cpp ----------
void generate_signals(vector<DailyBar>& daily) {
    for (size_t i = 1; i < daily.size(); ++i) {
        bool bull = !isnan(daily[i].mid_bb_d) && daily[i].close > daily[i].mid_bb_d;
        bool bear = !isnan(daily[i].mid_bb_d) && daily[i].close < daily[i].mid_bb_d;

        bool kdj_golden =
            !isnan(daily[i].K) && !isnan(daily[i].D) && !isnan(daily[i].J) &&
            !isnan(daily[i-1].K) && !isnan(daily[i-1].D) && !isnan(daily[i-1].J) &&
            (daily[i-1].D >= daily[i-1].K) && (daily[i-1].D >= daily[i-1].J) &&
            (daily[i].K > daily[i].D) && (daily[i].J > daily[i].D);

        bool kdj_death =
            !isnan(daily[i].K) && !isnan(daily[i].D) && !isnan(daily[i].J) &&
            !isnan(daily[i-1].K) && !isnan(daily[i-1].D) && !isnan(daily[i-1].J) &&
            (daily[i-1].K >= daily[i-1].D) && (daily[i-1].J >= daily[i-1].D) &&
            (daily[i].K < daily[i].D) && (daily[i].J < daily[i].D);

        bool ema_golden =
            !isnan(daily[i].ema5) && !isnan(daily[i].ema13) &&
            !isnan(daily[i-1].ema5) && !isnan(daily[i-1].ema13) &&
            (daily[i].ema5 > daily[i].ema13) && (daily[i-1].ema5 <= daily[i-1].ema13);

        if (bull) {
            if (kdj_golden) daily[i].signal = 1;
            if (kdj_death)  daily[i].signal = -1;
        }
        if (bear) {
            if (ema_golden) daily[i].signal = 1;
            if (kdj_death)  daily[i].signal = -1;
        }
    }
}

// Intrinsic value of a call struck at entry spot (ATM at purchase)
double call_intrinsic(double spot_now, double spot_entry) {
    return max(0.0, spot_now - spot_entry);
}

// Mark-to-market value of the call as fraction of premium paid
// value / premium  -  1  =  unrealized return on option capital
double call_mtm_return(double spot_now, double spot_entry, double premium_paid) {
    if (premium_paid <= 0) return 0.0;
    double intrinsic = call_intrinsic(spot_now, spot_entry);
    return intrinsic / premium_paid - 1.0;
}

void run_backtest(vector<DailyBar>& daily) {
    int stock_pos = 0;
    double stock_ep = 0.0;

    int opt_pos = 0;
    double opt_entry_spot = 0.0;
    double opt_premium = 0.0;

    for (size_t i = 1; i < daily.size(); ++i) {
        // ========== STOCK stop-loss ==========
        if (stock_pos == 1) {
            double stop_px = stock_ep * (1.0 - STOCK_STOP);
            if (daily[i].low <= stop_px) {
                double exit_px = (daily[i].open < stop_px) ? daily[i].open : stop_px;
                daily[i].stock_exit = exit_px;
                daily[i].stock_pnl  = exit_px / stock_ep - 1.0;
                daily[i].stock_exit_reason = "stop_loss";
                stock_pos = 0;
            } else if (daily[i].close <= stop_px) {
                daily[i].stock_exit = daily[i].close;
                daily[i].stock_pnl  = daily[i].close / stock_ep - 1.0;
                daily[i].stock_exit_reason = "stop_loss";
                stock_pos = 0;
            }
        }

        // ========== OPTION stop (lose OPTION_STOP of premium) ==========
        if (opt_pos == 1) {
            double mtm = call_mtm_return(daily[i].low, opt_entry_spot, opt_premium);
            // also check open gap
            double mtm_open = call_mtm_return(daily[i].open, opt_entry_spot, opt_premium);
            if (mtm <= -OPTION_STOP || mtm_open <= -OPTION_STOP) {
                double spot_exit = (mtm_open <= -OPTION_STOP) ? daily[i].open : daily[i].low;
                // approximate exit when premium has lost OPTION_STOP
                daily[i].opt_exit_spot = spot_exit;
                daily[i].opt_pnl = max(-1.0, call_mtm_return(spot_exit, opt_entry_spot, opt_premium));
                daily[i].opt_exit_reason = "opt_stop";
                opt_pos = 0;
            }
        }

        daily[i].stock_pos = stock_pos;
        daily[i].opt_pos   = opt_pos;

        int prev_sig = daily[i - 1].signal;

        // ----- shared entry -----
        if (prev_sig == 1) {
            if (stock_pos == 0) {
                stock_pos = 1;
                stock_ep = daily[i].open;
                daily[i].stock_entry = stock_ep;
                daily[i].stock_pos = 1;
            }
            if (opt_pos == 0) {
                opt_pos = 1;
                opt_entry_spot = daily[i].open;
                opt_premium = OPTION_PREMIUM * opt_entry_spot;
                daily[i].opt_entry_spot = opt_entry_spot;
                daily[i].opt_premium_paid = opt_premium;
                daily[i].opt_pos = 1;
            }
        }
        // ----- shared exit (signal) -----
        else if (prev_sig == -1) {
            if (stock_pos == 1) {
                daily[i].stock_exit = daily[i].open;
                daily[i].stock_pnl  = daily[i].open / stock_ep - 1.0;
                daily[i].stock_exit_reason = "signal";
                stock_pos = 0;
                daily[i].stock_pos = 0;
            }
            if (opt_pos == 1) {
                daily[i].opt_exit_spot = daily[i].open;
                daily[i].opt_pnl = call_mtm_return(daily[i].open, opt_entry_spot, opt_premium);
                daily[i].opt_exit_reason = "signal";
                opt_pos = 0;
                daily[i].opt_pos = 0;
            }
        }
    }

    // force close at end
    if (!daily.empty()) {
        size_t last = daily.size() - 1;
        if (stock_pos == 1) {
            daily[last].stock_exit = daily[last].close;
            daily[last].stock_pnl  = daily[last].close / stock_ep - 1.0;
            daily[last].stock_exit_reason = "end_of_data";
            daily[last].stock_pos = 0;
        }
        if (opt_pos == 1) {
            daily[last].opt_exit_spot = daily[last].close;
            daily[last].opt_pnl = call_mtm_return(daily[last].close, opt_entry_spot, opt_premium);
            daily[last].opt_exit_reason = "end_of_data";
            daily[last].opt_pos = 0;
        }
    }

    // ----- equity curves -----
    // stock_equity: fully invested when in stock
    // opt_equity  : capital on option premium when in call
    // combined    : (1-OPTION_ALLOC)*stock + OPTION_ALLOC*opt
    daily[0].stock_equity = 1.0;
    daily[0].opt_equity = 1.0;
    daily[0].combined_equity = 1.0;
    daily[0].bh_equity = 1.0;

    for (size_t i = 1; i < daily.size(); ++i) {
        // stock daily return when held
        if (daily[i - 1].stock_pos == 1)
            daily[i].stock_ret = daily[i].close / daily[i - 1].close - 1.0;
        daily[i].stock_equity = daily[i - 1].stock_equity * (1.0 + daily[i].stock_ret);

        // option: mark-to-market change in call value / premium
        if (daily[i - 1].opt_pos == 1) {
            // find the entry spot & premium for the open position
            // (walk back to last entry)
            double entry_spot = NAN, prem = NAN;
            for (int j = (int)i - 1; j >= 0; --j) {
                if (!isnan(daily[j].opt_entry_spot)) {
                    entry_spot = daily[j].opt_entry_spot;
                    prem = daily[j].opt_premium_paid;
                    break;
                }
            }
            if (!isnan(entry_spot) && prem > 0) {
                double mtm_prev = call_mtm_return(daily[i - 1].close, entry_spot, prem);
                double mtm_now  = call_mtm_return(daily[i].close, entry_spot, prem);
                // daily change in MTM return, scaled by leverage
                daily[i].opt_ret = (mtm_now - mtm_prev) * OPTION_LEVERAGE;
                // floor at -1 on the day of total loss
                if (daily[i].opt_ret < -1.0) daily[i].opt_ret = -1.0;
            }
        }
        daily[i].opt_equity = daily[i - 1].opt_equity * (1.0 + daily[i].opt_ret);

        // combined portfolio
        double stock_w = 1.0 - OPTION_ALLOC;
        double opt_w   = OPTION_ALLOC;
        double comb_ret = stock_w * daily[i].stock_ret + opt_w * daily[i].opt_ret;
        daily[i].combined_equity = daily[i - 1].combined_equity * (1.0 + comb_ret);

        double bh = daily[i].close / daily[i - 1].close - 1.0;
        daily[i].bh_equity = daily[i - 1].bh_equity * (1.0 + bh);
    }
}

void write_result(const string& path, const vector<DailyBar>& bars) {
    ofstream fout(path);
    fout << fixed << setprecision(6);
    fout << "time_key,open,high,low,close,ema5,ema13,K,D,J,boll_mid,signal,"
         << "stock_pos,stock_entry,stock_exit,stock_pnl,stock_exit_reason,stock_ret,stock_equity,"
         << "opt_pos,opt_entry_spot,opt_premium_paid,opt_exit_spot,opt_pnl,opt_exit_reason,opt_ret,opt_equity,"
         << "combined_equity,bh_equity\n";
    for (const auto& b : bars) {
        fout << b.date << ","
             << b.open << "," << b.high << "," << b.low << "," << b.close << ","
             << b.ema5 << "," << b.ema13 << ","
             << b.K << "," << b.D << "," << b.J << "," << b.mid_bb_d << ","
             << b.signal << ","
             << b.stock_pos << "," << b.stock_entry << "," << b.stock_exit << ","
             << b.stock_pnl << "," << b.stock_exit_reason << ","
             << b.stock_ret << "," << b.stock_equity << ","
             << b.opt_pos << "," << b.opt_entry_spot << "," << b.opt_premium_paid << ","
             << b.opt_exit_spot << "," << b.opt_pnl << "," << b.opt_exit_reason << ","
             << b.opt_ret << "," << b.opt_equity << ","
             << b.combined_equity << "," << b.bh_equity << "\n";
    }
}

struct Summary {
    string code;
    int stock_trades = 0, opt_trades = 0;
    double stock_eq = 1, opt_eq = 1, combined_eq = 1, bh_eq = 1;
    double stock_win = 0, opt_win = 0;
};

Summary make_summary(const string& code, const vector<DailyBar>& bars) {
    Summary s;
    s.code = code;
    int sw = 0, ow = 0;
    for (const auto& b : bars) {
        if (b.stock_pnl != 0 && !isnan(b.stock_pnl)) {
            ++s.stock_trades;
            if (b.stock_pnl > 0) ++sw;
        }
        if (b.opt_pnl != 0 && !isnan(b.opt_pnl)) {
            ++s.opt_trades;
            if (b.opt_pnl > 0) ++ow;
        }
    }
    s.stock_win = s.stock_trades ? (double)sw / s.stock_trades : 0;
    s.opt_win   = s.opt_trades   ? (double)ow / s.opt_trades   : 0;
    if (!bars.empty()) {
        s.stock_eq    = bars.back().stock_equity;
        s.opt_eq      = bars.back().opt_equity;
        s.combined_eq = bars.back().combined_equity;
        s.bh_eq       = bars.back().bh_equity;
    }
    return s;
}

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    string list_path = "/Users/markp/Desktop/QuantProject@3.12/monitorCodeList.csv";
    string daily_dir = "/Users/markp/Desktop/QuantProject@3.12/d_history";
    string out_dir   = "/Users/markp/Desktop/QuantProject@3.12/AggressiveFolder";

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        auto ns = [&](string& v) { if (i+1 < argc) v = argv[++i]; };
        auto nd = [&](double& v) { if (i+1 < argc) v = stod(argv[++i]); };
        if (a == "--list") ns(list_path);
        else if (a == "--daily-dir") ns(daily_dir);
        else if (a == "--out") ns(out_dir);
        else if (a == "--stock-stop") nd(STOCK_STOP);
        else if (a == "--opt-premium") nd(OPTION_PREMIUM);
        else if (a == "--opt-stop") nd(OPTION_STOP);
        else if (a == "--opt-alloc") nd(OPTION_ALLOC);
        else if (a == "--opt-leverage") nd(OPTION_LEVERAGE);
    }

    if (!fs::exists(list_path)) {
        for (const string& c : {"monitorCodeList.csv",
                                "/Users/markp/Desktop/QuantProject@3.12/monitorCodeList.csv"}) {
            if (fs::exists(c)) { list_path = c; break; }
        }
    }

    vector<string> codes;
    ifstream flist(list_path);
    if (!flist) { cerr << "Cannot open " << list_path << "\n"; return 1; }
    string line;
    getline(flist, line);
    while (getline(flist, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty()) codes.push_back(line);
    }

    fs::create_directories(out_dir);

    cout << "============================================================\n";
    cout << "  Backtester + Options (Call)  – based on original signals\n";
    cout << "============================================================\n";
    cout << "  Stock stop      : " << STOCK_STOP * 100 << "%\n";
    cout << "  Option premium  : " << OPTION_PREMIUM * 100 << "% of spot\n";
    cout << "  Option stop     : lose " << OPTION_STOP * 100 << "% of premium\n";
    cout << "  Option alloc    : " << OPTION_ALLOC * 100 << "% of capital\n";
    cout << "  Option leverage : " << OPTION_LEVERAGE << "x\n";
    cout << "  Stocks          : " << codes.size() << "\n";
    cout << "------------------------------------------------------------\n";

    vector<Summary> summaries;
    int ok = 0, fail = 0;

    for (size_t i = 0; i < codes.size(); ++i) {
        const string& code = codes[i];
        string fname = code_to_filename(code);
        string path = daily_dir + "/" + fname;
        if (!fs::exists(path)) {
            string p2 = daily_dir + "/" + code + ".csv";
            if (fs::exists(p2)) path = p2;
            else {
                cout << "[" << (i+1) << "/" << codes.size() << "] " << code << " – missing\n";
                ++fail; continue;
            }
        }

        vector<DailyBar> bars;
        if (!load_daily(path, bars) || bars.size() < 30) {
            cout << "[" << (i+1) << "/" << codes.size() << "] " << code << " – load fail\n";
            ++fail; continue;
        }

        generate_signals(bars);
        run_backtest(bars);

        string out_path = out_dir + "/" + fname.substr(0, fname.size()-4) + "_backtest.csv";
        write_result(out_path, bars);

        Summary s = make_summary(code, bars);
        summaries.push_back(s);
        cout << "[" << (i+1) << "/" << codes.size() << "] " << code
             << "  stock=" << fixed << setprecision(3) << s.stock_eq << "x"
             << "  opt=" << s.opt_eq << "x"
             << "  combined=" << s.combined_eq << "x"
             << "  (S trades=" << s.stock_trades << " O trades=" << s.opt_trades << ")\n";
        ++ok;
    }

    sort(summaries.begin(), summaries.end(),
         [](const Summary& a, const Summary& b) { return a.combined_eq > b.combined_eq; });

    ofstream fsum(out_dir + "/summary_options.csv");
    fsum << "code,stock_trades,opt_trades,stock_win_rate,opt_win_rate,"
         << "stock_equity,opt_equity,combined_equity,bh_equity\n";
    fsum << fixed << setprecision(6);
    for (const auto& s : summaries) {
        fsum << s.code << "," << s.stock_trades << "," << s.opt_trades << ","
             << s.stock_win << "," << s.opt_win << ","
             << s.stock_eq << "," << s.opt_eq << "," << s.combined_eq << "," << s.bh_eq << "\n";
    }

    cout << "------------------------------------------------------------\n";
    cout << "Done. Success: " << ok << "  Failed: " << fail << "\n";
    cout << "Results → " << out_dir << "\n";
    return 0;
}