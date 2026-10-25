# ============================================================
# QuantProject Makefile
# ============================================================

CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall
LDFLAGS  :=

# ---------- C++ targets ----------
BINARIES := backtester momentum calculateIndicators AggressiveTester deleteFile \
            correlationTracker correlationBacktester summarizeResults sentimentMomentum

# ---------- Directories ----------
TEST_RESULT_DIR   := testResult
CORR_RESULT_DIR   := correlationResult

# ============================================================
# Default target
# ============================================================
.PHONY: all
all: $(BINARIES)

# ---------- Compile rules ----------
backtester: backTester.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# Portfolio equity engine (HK Momentum Trend V4-LS)
# Source: momentumTrend.cpp  →  binary: momentum
momentum: momentumTrend.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

calculateIndicators: calculateIndicators.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

AggressiveTester: AggressiveTester.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

deleteFile: deleteFile.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

correlationTracker: correlationTracker.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

correlationBacktester: correlationBacktester.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

summarizeResults: summarizeResults.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

sentimentMomentum: sentimentMomentum.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# ============================================================
# Run pipelines
# ============================================================

# Classic / experimental engine in backTester.cpp
.PHONY: summarize
summarize: backtester summarizeResults
	./backtester
	./summarizeResults

# Portfolio equity strategy (momentumTrend.cpp → ./momentum)
.PHONY: run-momentum summarize-momentum
run-momentum: momentum
	./momentum

summarize-momentum: momentum summarizeResults
	./momentum
	./summarizeResults

# Sentiment momentum pipeline
.PHONY: summarize-sentiment
summarize-sentiment: sentimentMomentum summarizeResults
	./sentimentMomentum
	./summarizeResults

# ============================================================
# Python helpers
# ============================================================
.PHONY: plot plot-single plot-corr cumulative update filter test

plot:
	python3 plotResults.py

plot-single:
	python3 singlePlot.py

plot-corr:
	python3 plotCorrelation.py

cumulative:
	python3 cumulative.py

update:
	python3 dailyUpdate.py

# Preferred history fetch (Futu OpenD)
.PHONY: fetch fetch-full
fetch:
	python3 fetchHistory.py --list monitorCodeList.csv --days 60

fetch-full:
	python3 fetchHistory.py --list monitorCodeList.csv --full --days 400

filter:
	python3 filter.py

test:
	python3 test.py

# ============================================================
# Correlation / pairs pipeline
# ============================================================
.PHONY: corr-signals corr-backtest corr

corr-signals: correlationTracker
	./correlationTracker

corr-backtest: correlationBacktester
	./correlationBacktester

corr: corr-signals corr-backtest

# ============================================================
# Clean
# ============================================================
.PHONY: clean clean-png clean-corr clean-all

clean:
	rm -f $(BINARIES)
	@echo "Binaries removed."

# Remove only PNG files in testResult/ (keeps CSVs)
clean-png:
	@if [ -d "$(TEST_RESULT_DIR)" ]; then \
		find $(TEST_RESULT_DIR) -type f -name "*.png" -print -delete; \
		echo "PNG files in $(TEST_RESULT_DIR)/ removed."; \
	else \
		echo "Directory $(TEST_RESULT_DIR)/ does not exist."; \
	fi

clean-corr:
	@if [ -d "$(CORR_RESULT_DIR)" ]; then \
		find $(CORR_RESULT_DIR) -type f \( -name "*_pair.csv" -o -name "*_equity.csv" -o -name "pairs_summary.csv" \) -print -delete; \
		echo "Correlation result CSVs removed."; \
	else \
		echo "Directory $(CORR_RESULT_DIR)/ does not exist."; \
	fi

clean-all: clean clean-png clean-corr
	@echo "Full clean done."

# ============================================================
# Help
# ============================================================
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make / make all          - build all C++ binaries"
	@echo "  make momentum            - build portfolio equity engine (momentumTrend.cpp)"
	@echo "  make run-momentum        - build + run ./momentum → testResult/"
	@echo "  make summarize-momentum  - run momentum + summarizeResults"
	@echo "  make backtester          - build backTester.cpp"
	@echo "  make summarize           - run backtester + summarizeResults"
	@echo "  make summarize-sentiment - sentimentMomentum + summarize"
	@echo "  make plot                - plotResults.py (uses testResult/)"
	@echo "  make fetch / fetch-full  - refresh d_history / w_history via Futu"
	@echo "  make corr                - pairs signals + backtest"
	@echo "  make clean               - remove binaries"
	@echo "  make clean-png           - remove PNGs in testResult only"
	@echo "  make clean-all           - binaries + PNGs + corr CSVs"
