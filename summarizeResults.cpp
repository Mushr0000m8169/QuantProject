/*
 * summarizeResults.cpp
 * -------------------------------------------------------------
 * Read all testResult/*_backtest.csv files and write
 * testResult/summary_all_stocks.csv with overall strategy
 * performance metrics.
 *
 * Also prints a short console summary (avg equity, avg MDD, etc.).
 *
 * Compile:
 *   g++ -O2 -std=c++17 summarizeResults.cpp -o summarizeResults
 *
 * Run (after backtester):
 *   ./summarizeResults
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
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

const string RESULT_DIR = "testResult";
const string OUT_CSV    = "testResult/summary_all_stocks.csv";

struct StockStats {
    string code;
    int n_trades = 0;
    int n_wins = 0;
    int n_stops = 0;
    double win_rate = 0;
    double final_equity = 1.0;
    double bh_equity = 1.0;
    double total_ret = 0;
    double excess_ret = 0;
    double max_drawdown = 0;
    bool ok = false;
};

vector<string> split(const string& s, char delim = ',') {
    vector<string> t;
    string item;
    istringstream ss(s);
    while (getline(ss, item, delim)) t.push_back(item);
    return t;
}

double to_d(const string& s) {
    if (s.empty() || s == "nan" || s == "NaN" || s == "NaN") return NAN;
    try { return stod(s); } catch (...) { return NAN; }
}

bool isnan_d(double x) { return x != x; }

StockStats analyze_one(const string& path) {
    StockStats st;
    // code from filename: HK00998_backtest.csv → HK00998
    string stem = fs::path(path).stem().string();
    const string suffix = "_backtest";
    if (stem.size() > suffix.size() &&
        stem.substr(stem.size() - suffix.size()) == suffix)
        st.code = stem.substr(0, stem.size() - suffix.size());
    else
        st.code = stem;

    ifstream fin(path);
    if (!fin) return st;

    string line;
    getline(fin, line); // header
    auto header = split(line);

    auto col = [&](const string& name) -> int {
        for (size_t i = 0; i < header.size(); ++i) {
            string h = header[i];
            for (char& c : h) c = (char)tolower(c);
            string n = name;
            for (char& c : n) c = (char)tolower(c);
            if (h == n) return (int)i;
        }
        return -1;
    };

    int i_equity  = col("equity");
    int i_bh      = col("bh_equity");
    int i_pnl     = col("pnl");
    int i_reason  = col("exit_reason");

    if (i_equity < 0) {
        cerr << "  Skip " << st.code << " – no equity column\n";
        return st;
    }

    double peak = 1.0, mdd = 0.0;
    double last_eq = 1.0, last_bh = 1.0;

    while (getline(fin, line)) {
        if (line.empty()) continue;
        auto t = split(line);
        if ((int)t.size() <= i_equity) continue;

        double eq = to_d(t[i_equity]);
        if (!isnan_d(eq)) {
            last_eq = eq;
            if (eq > peak) peak = eq;
            double dd = (eq - peak) / peak;
            if (dd < mdd) mdd = dd;
        }

        if (i_bh >= 0 && i_bh < (int)t.size()) {
            double bh = to_d(t[i_bh]);
            if (!isnan_d(bh)) last_bh = bh;
        }

        if (i_pnl >= 0 && i_pnl < (int)t.size()) {
            double pnl = to_d(t[i_pnl]);
            // treat non-zero pnl as a closed trade
            if (!isnan_d(pnl) && pnl != 0.0) {
                ++st.n_trades;
                if (pnl > 0) ++st.n_wins;
                if (i_reason >= 0 && i_reason < (int)t.size() &&
                    t[i_reason] == "stop_loss")
                    ++st.n_stops;
            }
        }
    }

    st.final_equity = last_eq;
    st.bh_equity    = last_bh;
    st.total_ret    = last_eq - 1.0;
    st.excess_ret   = last_eq - last_bh;
    st.max_drawdown = mdd;
    st.win_rate     = st.n_trades ? (double)st.n_wins / st.n_trades : 0.0;
    st.ok = true;
    return st;
}

int main() {
    cout << "=== Summarize Backtest Results ===\n";

    if (!fs::exists(RESULT_DIR)) {
        cerr << "Directory not found: " << RESULT_DIR << "\n";
        cerr << "Run the backtester first.\n";
        return 1;
    }

    vector<string> files;
    for (auto& entry : fs::directory_iterator(RESULT_DIR)) {
        string name = entry.path().filename().string();
        if (name.size() > 13 &&
            name.substr(name.size() - 13) == "_backtest.csv")
            files.push_back(entry.path().string());
    }
    sort(files.begin(), files.end());
    cout << "Found " << files.size() << " backtest files\n";

    vector<StockStats> all;
    for (auto& f : files) {
        auto st = analyze_one(f);
        if (st.ok) {
            all.push_back(st);
            cout << "  " << st.code
                 << "  equity=" << fixed << setprecision(4) << st.final_equity
                 << "  trades=" << st.n_trades
                 << "  win%=" << setprecision(1) << st.win_rate * 100.0
                 << "  MDD=" << setprecision(2) << st.max_drawdown * 100.0 << "%\n";
        }
    }

    // sort by final equity descending
    sort(all.begin(), all.end(),
         [](const StockStats& a, const StockStats& b) {
             return a.final_equity > b.final_equity;
         });

    ofstream sf(OUT_CSV);
    if (!sf) {
        cerr << "Cannot write " << OUT_CSV << "\n";
        return 1;
    }
    sf << fixed << setprecision(6);
    sf << "code,n_trades,n_wins,win_rate,n_stops,final_equity,bh_equity,"
       << "total_ret,excess_ret,max_drawdown\n";

    double sum_eq = 0, sum_bh = 0, sum_mdd = 0, sum_wr = 0;
    int total_trades = 0, total_wins = 0;
    for (auto& s : all) {
        sf << s.code << ","
           << s.n_trades << "," << s.n_wins << "," << s.win_rate << ","
           << s.n_stops << ","
           << s.final_equity << "," << s.bh_equity << ","
           << s.total_ret << "," << s.excess_ret << ","
           << s.max_drawdown << "\n";
        sum_eq += s.final_equity;
        sum_bh += s.bh_equity;
        sum_mdd += s.max_drawdown;
        sum_wr += s.win_rate;
        total_trades += s.n_trades;
        total_wins   += s.n_wins;
    }
    sf.close();

    int n = (int)all.size();
    cout << "\n=== Overall Strategy Performance ===\n";
    if (n > 0) {
        cout << fixed << setprecision(4);
        cout << "  Stocks tested      : " << n << "\n";
        cout << "  Total trades       : " << total_trades << "\n";
        cout << "  Overall win rate   : "
             << (total_trades ? 100.0 * total_wins / total_trades : 0) << "%\n";
        cout << "  Avg final equity   : " << (sum_eq / n) << "x\n";
        cout << "  Avg buy & hold     : " << (sum_bh / n) << "x\n";
        cout << "  Avg excess return  : " << ((sum_eq - sum_bh) / n) << "\n";
        cout << "  Avg max drawdown   : " << (sum_mdd / n * 100.0) << "%\n";
        cout << "  Avg win rate       : " << (sum_wr / n * 100.0) << "%\n";
        cout << "\n  Top 5 by equity:\n";
        for (int i = 0; i < min(5, n); ++i)
            cout << "    " << all[i].code << "  " << all[i].final_equity << "x\n";
    }
    cout << "\n  Summary written: " << OUT_CSV << "\n";
    return 0;
}