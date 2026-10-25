/*
 * correlationTracker.cpp
 * -------------------------------------------------------------
 * Statistical-arbitrage / pairs signal generator.
 *
 * For every pair of stocks in the monitor list:
 *   1. Load daily closes from d_history/
 *   2. Compute rolling Pearson correlation of log-returns
 *   3. When correlation is high enough, monitor the log-price ratio
 *   4. Compute rolling z-score of the ratio
 *   5. Generate mean-reversion signals:
 *        z > +ENTRY_Z  → short the ratio (short A, long B)
 *        z < -ENTRY_Z  → long  the ratio (long  A, short B)
 *        |z| < EXIT_Z  → close the pair
 *
 * Output (one CSV per pair) goes into correlationResult/
 *
 * Compile:
 *   g++ -O2 -std=c++17 correlationTracker.cpp -o correlationTracker
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
const string DATA_DIR      = "d_history";                 // daily CSVs
const string WEEKLY_DIR    = "w_history";                 // weekly CSVs
const string CODE_LIST     = "monitorCodeList.csv";
const string OUT_DIR       = "correlationResult";

const int    CORR_WINDOW   = 60;     // daily rolling correlation (trading days)
const int    Z_WINDOW      = 20;     // daily z-score window
const int    W_CORR_WINDOW = 20;     // weekly rolling correlation (weeks)
const double MIN_CORR      = 0.70;   // min daily correlation to trade
const double MIN_CORR_W    = 0.60;   // min weekly correlation (extra filter)
const double ENTRY_Z       = 2.0;    // enter when |z| exceeds this
const double EXIT_Z        = 0.5;    // exit when |z| falls back below this
// ==================================================

struct Bar {
    string date;
    double close;
};

struct SignalRow {
    string date;
    double close_a, close_b;
    double log_ratio;
    double zscore;
    double corr;        // daily correlation
    double corr_w;      // weekly correlation (forward-filled onto daily)
    int    signal;      // +1 long ratio, -1 short ratio, 0 flat
    int    position;    // current position after signal
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

// Convert "HK.00100" → "HK00100" (dots removed) for filename matching
string code_to_filename_stem(const string& code) {
    string s;
    for (char c : code) if (c != '.') s.push_back(c);
    return s;
}

// Load OHLC CSV from a given directory (d_history or w_history)
// Supports filenames: HK.00100.csv  OR  HK00100.csv
// Columns: code,name,time_key,open,close,high,low,...
vector<Bar> load_stock_from(const string& dir, const string& code) {
    string stem = code_to_filename_stem(code);

    vector<string> candidates = {
        dir + "/" + code + ".csv",
        dir + "/" + stem + ".csv",
        dir + "/" + code + "_daily.csv",
        dir + "/" + stem + "_daily.csv",
        dir + "/" + code + "_weekly.csv",
        dir + "/" + stem + "_weekly.csv",
    };

    string path;
    for (auto& p : candidates) {
        if (fs::exists(p)) { path = p; break; }
    }

    if (path.empty() && fs::exists(dir)) {
        for (auto& entry : fs::directory_iterator(dir)) {
            string name = entry.path().filename().string();
            if (name.find(stem) != string::npos &&
                name.size() >= 4 &&
                name.substr(name.size() - 4) == ".csv") {
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
    int date_col = -1, close_col = -1;
    for (size_t i = 0; i < header.size(); ++i) {
        string h = header[i];
        for (auto& c : h) c = (char)tolower(c);
        if (h == "time_key" || h == "date" || h.find("time") != string::npos)
            date_col = (int)i;
        if (h == "close" || h == "adj_close" || h == "adjclose")
            close_col = (int)i;
    }
    if (date_col  < 0) date_col  = 2;
    if (close_col < 0) close_col = 4;

    vector<Bar> bars;
    while (getline(fin, line)) {
        if (line.empty()) continue;
        auto t = split(line);
        if ((int)t.size() <= max(date_col, close_col)) continue;
        Bar b;
        b.date  = t[date_col].substr(0, 10);
        b.close = to_d(t[close_col]);
        if (!isnan_d(b.close) && b.close > 0.0) bars.push_back(b);
    }
    return bars;
}

vector<Bar> load_stock(const string& code) {
    return load_stock_from(DATA_DIR, code);
}

vector<Bar> load_stock_weekly(const string& code) {
    return load_stock_from(WEEKLY_DIR, code);
}

// Read monitor list (one code per line, or first column of CSV)
vector<string> load_codes() {
    vector<string> codes;
    ifstream fin(CODE_LIST);
    if (!fin) {
        cerr << "Cannot open " << CODE_LIST << " – using demo pair HK.00700 / HK.00941\n";
        return {"HK.00700", "HK.00941"};
    }
    string line;
    bool first = true;
    while (getline(fin, line)) {
        if (line.empty()) continue;
        auto t = split(line);
        string c = t[0];
        // skip header if it looks like one
        if (first) {
            first = false;
            string lower = c;
            for (auto& ch : lower) ch = tolower(ch);
            if (lower.find("code") != string::npos || lower == "symbol") continue;
        }
        // strip whitespace
        c.erase(remove_if(c.begin(), c.end(), ::isspace), c.end());
        if (!c.empty()) codes.push_back(c);
    }
    return codes;
}

// Align two series on common dates
void align(const vector<Bar>& a, const vector<Bar>& b,
           vector<string>& dates, vector<double>& ca, vector<double>& cb) {
    map<string, double> ma, mb;
    for (auto& x : a) ma[x.date] = x.close;
    for (auto& x : b) mb[x.date] = x.close;

    for (auto& [d, pa] : ma) {
        if (mb.count(d)) {
            dates.push_back(d);
            ca.push_back(pa);
            cb.push_back(mb[d]);
        }
    }
}

// Pearson correlation of two vectors (no NaNs expected)
double pearson(const vector<double>& x, const vector<double>& y) {
    int n = (int)x.size();
    if (n < 3) return NAN;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; ++i) {
        sx  += x[i];  sy  += y[i];
        sxx += x[i]*x[i]; syy += y[i]*y[i];
        sxy += x[i]*y[i];
    }
    double num = n * sxy - sx * sy;
    double den = sqrt((n*sxx - sx*sx) * (n*syy - sy*sy));
    if (den < 1e-12) return NAN;
    return num / den;
}

// Compute rolling weekly correlation, then forward-fill onto daily dates
vector<double> weekly_corr_on_daily(
    const vector<Bar>& wA, const vector<Bar>& wB,
    const vector<string>& daily_dates)
{
    vector<string> wdates;
    vector<double> wa, wb;
    align(wA, wB, wdates, wa, wb);
    int Nw = (int)wdates.size();

    // weekly log returns
    vector<double> rA(Nw, NAN), rB(Nw, NAN);
    for (int i = 1; i < Nw; ++i) {
        rA[i] = log(wa[i]) - log(wa[i-1]);
        rB[i] = log(wb[i]) - log(wb[i-1]);
    }

    // rolling weekly correlation
    vector<double> wcorr(Nw, NAN);
    for (int i = W_CORR_WINDOW; i < Nw; ++i) {
        vector<double> xa, xb;
        for (int j = i - W_CORR_WINDOW + 1; j <= i; ++j) {
            if (!isnan_d(rA[j]) && !isnan_d(rB[j])) {
                xa.push_back(rA[j]);
                xb.push_back(rB[j]);
            }
        }
        wcorr[i] = pearson(xa, xb);
    }

    // forward-fill onto daily dates (asof merge)
    vector<double> result(daily_dates.size(), NAN);
    size_t wi = 0;
    for (size_t di = 0; di < daily_dates.size(); ++di) {
        while (wi + 1 < wdates.size() && wdates[wi + 1] <= daily_dates[di])
            ++wi;
        if (wi < wdates.size() && wdates[wi] <= daily_dates[di])
            result[di] = wcorr[wi];
    }
    return result;
}

void process_pair(const string& codeA, const string& codeB,
                  const vector<Bar>& barsA, const vector<Bar>& barsB,
                  const vector<Bar>& wBarsA, const vector<Bar>& wBarsB) {
    vector<string> dates;
    vector<double> ca, cb;
    align(barsA, barsB, dates, ca, cb);
    int N = (int)dates.size();
    if (N < max(CORR_WINDOW, Z_WINDOW) + 5) {
        cerr << "  Pair " << codeA << "-" << codeB << ": not enough daily data (" << N << ")\n";
        return;
    }

    // log prices & log returns (daily)
    vector<double> logA(N), logB(N), retA(N, NAN), retB(N, NAN);
    for (int i = 0; i < N; ++i) {
        logA[i] = log(ca[i]);
        logB[i] = log(cb[i]);
        if (i > 0) {
            retA[i] = logA[i] - logA[i-1];
            retB[i] = logB[i] - logB[i-1];
        }
    }

    // log ratio
    vector<double> ratio(N);
    for (int i = 0; i < N; ++i) ratio[i] = logA[i] - logB[i];

    // rolling daily correlation of returns
    vector<double> corr(N, NAN);
    for (int i = CORR_WINDOW; i < N; ++i) {
        vector<double> xa, xb;
        for (int j = i - CORR_WINDOW + 1; j <= i; ++j) {
            if (!isnan_d(retA[j]) && !isnan_d(retB[j])) {
                xa.push_back(retA[j]);
                xb.push_back(retB[j]);
            }
        }
        corr[i] = pearson(xa, xb);
    }

    // weekly correlation forward-filled onto daily
    vector<double> corr_w(N, NAN);
    if (!wBarsA.empty() && !wBarsB.empty()) {
        corr_w = weekly_corr_on_daily(wBarsA, wBarsB, dates);
    }

    // rolling z-score of ratio
    vector<double> zscore(N, NAN);
    for (int i = Z_WINDOW - 1; i < N; ++i) {
        double sum = 0, sum2 = 0;
        for (int j = i - Z_WINDOW + 1; j <= i; ++j) {
            sum  += ratio[j];
            sum2 += ratio[j] * ratio[j];
        }
        double mean = sum / Z_WINDOW;
        double var  = sum2 / Z_WINDOW - mean * mean;
        double std  = sqrt(max(0.0, var));
        if (std > 1e-8) zscore[i] = (ratio[i] - mean) / std;
    }

    // generate signals – both daily AND weekly correlation required
    vector<SignalRow> rows(N);
    int pos = 0;
    for (int i = 0; i < N; ++i) {
        rows[i].date      = dates[i];
        rows[i].close_a   = ca[i];
        rows[i].close_b   = cb[i];
        rows[i].log_ratio = ratio[i];
        rows[i].zscore    = zscore[i];
        rows[i].corr      = corr[i];
        rows[i].corr_w    = corr_w[i];
        rows[i].signal    = 0;

        bool daily_ok  = !isnan_d(corr[i])   && corr[i]   >= MIN_CORR;
        bool weekly_ok = !isnan_d(corr_w[i]) && corr_w[i] >= MIN_CORR_W;
        // if no weekly data at all, fall back to daily-only
        if (wBarsA.empty() || wBarsB.empty()) weekly_ok = true;

        bool tradable = daily_ok && weekly_ok && !isnan_d(zscore[i]);

        if (tradable) {
            if (pos == 0) {
                if (zscore[i] > ENTRY_Z) {
                    rows[i].signal = -1;
                    pos = -1;
                } else if (zscore[i] < -ENTRY_Z) {
                    rows[i].signal = +1;
                    pos = +1;
                }
            } else {
                if (abs(zscore[i]) < EXIT_Z) {
                    rows[i].signal = 0;
                    pos = 0;
                }
                // force exit if either correlation collapses
                if (corr[i] < MIN_CORR * 0.7 ||
                    (!isnan_d(corr_w[i]) && corr_w[i] < MIN_CORR_W * 0.7)) {
                    rows[i].signal = 0;
                    pos = 0;
                }
            }
        } else {
            if (pos != 0) {
                rows[i].signal = 0;
                pos = 0;
            }
        }
        rows[i].position = pos;
    }

    // only keep pairs that produced at least one entry signal
    bool has_signal = false;
    for (auto& r : rows) {
        if (r.signal != 0) { has_signal = true; break; }
    }
    if (!has_signal) return;

    // write CSV
    string safeA = codeA, safeB = codeB;
    for (char& c : safeA) if (c == '.') c = '_';
    for (char& c : safeB) if (c == '.') c = '_';
    string out_path = OUT_DIR + "/" + safeA + "_" + safeB + "_pair.csv";

    ofstream fout(out_path);
    fout << fixed << setprecision(6);
    fout << "time_key,close_a,close_b,log_ratio,zscore,corr,corr_w,signal,position\n";
    for (auto& r : rows) {
        fout << r.date << ","
             << r.close_a << "," << r.close_b << ","
             << r.log_ratio << ","
             << (isnan_d(r.zscore) ? string("nan") : to_string(r.zscore)) << ","
             << (isnan_d(r.corr)   ? string("nan") : to_string(r.corr))   << ","
             << (isnan_d(r.corr_w) ? string("nan") : to_string(r.corr_w)) << ","
             << r.signal << "," << r.position << "\n";
    }
    fout.close();
    cout << "  Wrote " << out_path << "  (" << N << " rows)\n";
}

int main() {
    cout << "=== Correlation Tracker / Pairs Signal Generator ===\n";
    cout << "Daily corr window=" << CORR_WINDOW
         << "  Weekly corr window=" << W_CORR_WINDOW
         << "  MIN_CORR(d)=" << MIN_CORR
         << "  MIN_CORR(w)=" << MIN_CORR_W << "\n";
    fs::create_directories(OUT_DIR);

    auto codes = load_codes();
    cout << "Loaded " << codes.size() << " codes\n";

    // load daily + weekly once
    map<string, vector<Bar>> data_d, data_w;
    for (auto& c : codes) {
        auto bars = load_stock(c);
        if (bars.size() < 100) {
            cerr << "  Skip " << c << " daily (only " << bars.size() << " bars)\n";
            continue;
        }
        data_d[c] = move(bars);

        auto wbars = load_stock_weekly(c);
        if (wbars.size() < (size_t)W_CORR_WINDOW + 2) {
            cerr << "  Warn " << c << " weekly only " << wbars.size()
                 << " bars – weekly filter relaxed for this name\n";
        } else {
            data_w[c] = move(wbars);
        }
        cout << "  Loaded " << c
             << "  daily=" << data_d[c].size()
             << "  weekly=" << (data_w.count(c) ? data_w[c].size() : 0) << "\n";
    }

    vector<string> valid;
    for (auto& [c, _] : data_d) valid.push_back(c);

    int pair_count = 0;
    for (size_t i = 0; i < valid.size(); ++i) {
        for (size_t j = i + 1; j < valid.size(); ++j) {
            cout << "Processing pair " << valid[i] << " / " << valid[j] << " ...\n";
            const vector<Bar> empty;
            const auto& wA = data_w.count(valid[i]) ? data_w[valid[i]] : empty;
            const auto& wB = data_w.count(valid[j]) ? data_w[valid[j]] : empty;
            process_pair(valid[i], valid[j],
                         data_d[valid[i]], data_d[valid[j]],
                         wA, wB);
            ++pair_count;
        }
    }
    cout << "Done. " << pair_count << " pairs processed → " << OUT_DIR << "/\n";
    return 0;
}