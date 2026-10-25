// calculate_indicators.cpp
// Compile: g++ -std=c++17 -O2 calculate_indicators.cpp -o calculate_indicators
// Usage:   ./calculate_indicators [data_folder]
//          (default data_folder = current directory)

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <map>

namespace fs = std::filesystem;

struct Bar {
    std::string code;
    std::string name;
    std::string time_key;
    double open = 0, close = 0, high = 0, low = 0;
    double pe_ratio = 0, turnover_rate = 0;
    double volume = 0, turnover = 0;
    double change_rate = 0, last_close = 0;

    // indicators
    double ema5 = NAN;
    double ema13 = NAN;
    double k = NAN, d = NAN, j = NAN;
    double boll_mid = NAN, boll_upper = NAN, boll_lower = NAN;
};

// -------------------- helpers --------------------
std::vector<std::string> split(const std::string& s, char delim = ',') {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream ss(s);
    while (std::getline(ss, token, delim)) {
        tokens.push_back(token);
    }
    return tokens;
}

double to_double(const std::string& s) {
    try { return std::stod(s); }
    catch (...) { return 0.0; }
}

// Simple SMA
double sma(const std::vector<double>& data, int end_idx, int period) {
    if (end_idx + 1 < period) return NAN;
    double sum = 0.0;
    for (int i = end_idx - period + 1; i <= end_idx; ++i)
        sum += data[i];
    return sum / period;
}

// Standard deviation (population)
double stddev(const std::vector<double>& data, int end_idx, int period, double mean) {
    if (end_idx + 1 < period) return NAN;
    double sum_sq = 0.0;
    for (int i = end_idx - period + 1; i <= end_idx; ++i) {
        double diff = data[i] - mean;
        sum_sq += diff * diff;
    }
    return std::sqrt(sum_sq / period);
}

// -------------------- indicator calculation --------------------
void calculate_indicators(std::vector<Bar>& bars) {
    const int n = static_cast<int>(bars.size());
    if (n == 0) return;

    std::vector<double> closes(n), highs(n), lows(n);
    for (int i = 0; i < n; ++i) {
        closes[i] = bars[i].close;
        highs[i]  = bars[i].high;
        lows[i]   = bars[i].low;
    }

    // ---------- EMA5 & EMA13 ----------
    const double k5  = 2.0 / (5  + 1);
    const double k13 = 2.0 / (13 + 1);

    // initialize with SMA
    if (n >= 5) {
        double sum = 0;
        for (int i = 0; i < 5; ++i) sum += closes[i];
        bars[4].ema5 = sum / 5.0;
        for (int i = 5; i < n; ++i)
            bars[i].ema5 = closes[i] * k5 + bars[i-1].ema5 * (1.0 - k5);
    }

    if (n >= 13) {
        double sum = 0;
        for (int i = 0; i < 13; ++i) sum += closes[i];
        bars[12].ema13 = sum / 13.0;
        for (int i = 13; i < n; ++i)
            bars[i].ema13 = closes[i] * k13 + bars[i-1].ema13 * (1.0 - k13);
    }

    // ---------- KDJ (9,3,3) ----------
    const int kdj_n = 9;
    double prev_k = 50.0, prev_d = 50.0;   // common initial value

    for (int i = 0; i < n; ++i) {
        if (i + 1 < kdj_n) {
            bars[i].k = bars[i].d = bars[i].j = NAN;
            continue;
        }

        // Highest High / Lowest Low of last 9 bars
        double hh = *std::max_element(highs.begin() + (i - kdj_n + 1), highs.begin() + i + 1);
        double ll = *std::min_element(lows.begin()  + (i - kdj_n + 1), lows.begin()  + i + 1);

        double rsv = 50.0;
        if (hh != ll)
            rsv = (closes[i] - ll) / (hh - ll) * 100.0;

        // K = 2/3 * prevK + 1/3 * RSV
        double k = (2.0/3.0) * prev_k + (1.0/3.0) * rsv;
        // D = 2/3 * prevD + 1/3 * K
        double d = (2.0/3.0) * prev_d + (1.0/3.0) * k;
        double j = 3.0 * k - 2.0 * d;

        bars[i].k = k;
        bars[i].d = d;
        bars[i].j = j;

        prev_k = k;
        prev_d = d;
    }

    // ---------- Bollinger Bands (n=40, sd=2) ----------
    const int boll_n = 20;
    const double boll_sd = 2.0;

    for (int i = 0; i < n; ++i) {
        if (i + 1 < boll_n) {
            bars[i].boll_mid = bars[i].boll_upper = bars[i].boll_lower = NAN;
            continue;
        }
        double mid = sma(closes, i, boll_n);
        double sd  = stddev(closes, i, boll_n, mid);

        bars[i].boll_mid   = mid;
        bars[i].boll_upper = mid + boll_sd * sd;
        bars[i].boll_lower = mid - boll_sd * sd;
    }
}

// -------------------- CSV I/O --------------------
bool load_csv(const std::string& path, std::vector<Bar>& bars) {
    std::ifstream fin(path);
    if (!fin) return false;

    std::string line;
    // skip header
    if (!std::getline(fin, line)) return false;

    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        auto tokens = split(line);
        if (tokens.size() < 13) continue;

        Bar b;
        b.code          = tokens[0];
        b.name          = tokens[1];
        b.time_key      = tokens[2];
        b.open          = to_double(tokens[3]);
        b.close         = to_double(tokens[4]);
        b.high          = to_double(tokens[5]);
        b.low           = to_double(tokens[6]);
        b.pe_ratio      = to_double(tokens[7]);
        b.turnover_rate = to_double(tokens[8]);
        b.volume        = to_double(tokens[9]);
        b.turnover      = to_double(tokens[10]);
        b.change_rate   = to_double(tokens[11]);
        b.last_close    = to_double(tokens[12]);

        bars.push_back(b);
    }
    return !bars.empty();
}

void save_csv(const std::string& path, const std::vector<Bar>& bars) {
    std::ofstream fout(path);
    if (!fout) {
        std::cerr << "Cannot write " << path << std::endl;
        return;
    }

    // header
    fout << "code,name,time_key,open,close,high,low,pe_ratio,turnover_rate,volume,turnover,"
         << "change_rate,last_close,"
         << "ema5,ema13,k,d,j,boll_mid,boll_upper,boll_lower\n";

    fout << std::fixed << std::setprecision(6);

    for (const auto& b : bars) {
        fout << b.code << ","
             << b.name << ","
             << b.time_key << ","
             << b.open << ","
             << b.close << ","
             << b.high << ","
             << b.low << ","
             << b.pe_ratio << ","
             << b.turnover_rate << ","
             << b.volume << ","
             << b.turnover << ","
             << b.change_rate << ","
             << b.last_close << ",";

        auto print_val = [&](double v) {
            if (std::isnan(v)) fout << "";
            else fout << v;
        };

        print_val(b.ema5);     fout << ",";
        print_val(b.ema13);    fout << ",";
        print_val(b.k);        fout << ",";
        print_val(b.d);        fout << ",";
        print_val(b.j);        fout << ",";
        print_val(b.boll_mid); fout << ",";
        print_val(b.boll_upper); fout << ",";
        print_val(b.boll_lower);
        fout << "\n";
    }
}

// -------------------- main --------------------
int main(int argc, char* argv[]) {
    std::string data_dir = ".";
    if (argc >= 2) data_dir = argv[1];

    // 1. read stock list
    std::vector<std::string> codes;
    {
        std::ifstream fin("monitorCodeList.csv");
        if (!fin) {
            std::cerr << "Cannot open monitorCodeList.csv\n";
            return 1;
        }
        std::string line;
        std::getline(fin, line); // header
        while (std::getline(fin, line)) {
            if (line.empty()) continue;
            // remove possible \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            codes.push_back(line);
        }
    }

    std::cout << "Found " << codes.size() << " stocks in list\n";

    int success = 0, failed = 0;

    for (const auto& code : codes) {
        // try different possible filenames

        // Remove dot from code (HK.00100 → HK00100)
        std::string code_no_dot = code;
        code_no_dot.erase(std::remove(code_no_dot.begin(), code_no_dot.end(), '.'), code_no_dot.end());

        std::vector<std::string> candidates = {
        data_dir + "/" + code_no_dot + ".csv",   // HK00100.csv
        data_dir + "/" + code + ".csv",          // HK.00100.csv
        data_dir + "/" + code.substr(3) + ".csv" // 00100.csv
        };

        std::string found_path;
        for (const auto& p : candidates) {
            if (fs::exists(p)) {
                found_path = p;
                break;
            }
        }

        if (found_path.empty()) {
            std::cerr << "[SKIP] No CSV found for " << code << " (tried: " 
                    << code_no_dot << ".csv)" << std::endl;
            ++failed;
            continue;
        }

        std::vector<Bar> bars;
        if (!load_csv(found_path, bars)) {
            std::cerr << "[FAIL] Cannot load " << found_path << std::endl;
            ++failed;
            continue;
        }

        // sort by time_key just in case
        std::sort(bars.begin(), bars.end(),
                  [](const Bar& a, const Bar& b){ return a.time_key < b.time_key; });

        calculate_indicators(bars);

        // output file
        std::string out_name = code;
        // replace '.' with nothing for cleaner filename
        out_name.erase(std::remove(out_name.begin(), out_name.end(), '.'), out_name.end());
        std::string out_path = data_dir + "/" + out_name + ".csv";

        save_csv(out_path, bars);
        std::cout << "[OK] " << code << "  (" << bars.size() << " bars) -> " << out_path << std::endl;
        ++success;
    }

    std::cout << "\nDone. Success: " << success << "  Failed/Skipped: " << failed << std::endl;
    return 0;
}