#!/bin/bash

# ==========================================================
# 🧪 HELIOS vs. TCP: THE ULTIMATE STRESS TEST
# ==========================================================

DURATION=10
SERVER_IP="127.0.0.1"
LOG_FILE="benchmark_results.csv"

# 1. SETUP & COMPILATION
# ----------------------------------------------------------
echo "[Setup] Cleaning old processes..."
pkill -9 tcp_benchmark 2>/dev/null
pkill -9 rudp_server 2>/dev/null
pkill -9 rudp_client 2>/dev/null
sudo tc qdisc del dev lo root 2>/dev/null # Clear network rules

echo "[Setup] Compiling..."

# --- COMPILE TCP BASELINE ---
# (Standard TCP benchmark file)
if [ -f "tcp_benchmark.cpp" ]; then
    g++ -O3 -pthread tcp_benchmark.cpp -o tcp_benchmark
else
    echo "⚠️ Warning: tcp_benchmark.cpp not found. TCP tests might fail."
fi

# --- COMPILE HELIOS RUDP (Direct G++) ---
echo ">> Compiling Helios Server & Client..."
# Adding -pthread just in case your networking uses threads, 
# but sticking to your requested -O3 flag.
g++ -std=c++20 -O3 -pthread server_main.cpp -o rudp_server
g++ -std=c++20 -O3 -pthread client_main.cpp -o rudp_client

if [ ! -f "./rudp_server" ] || [ ! -f "./rudp_client" ]; then
    echo "❌ Compilation Failed. Check if server_main.cpp/client_main.cpp exist."
    exit 1
fi

# Initialize Result Log
echo "SCENARIO,PROTOCOL,LOSS(%),DELAY(ms),SPEED(MB/s)" > $LOG_FILE

# 2. THE TEST ENGINE
# ----------------------------------------------------------
run_scenario() {
    SCENARIO_NAME=$1
    LOSS=$2
    DELAY=$3
    
    echo "=========================================================="
    echo " 🧪 SCENARIO: $SCENARIO_NAME"
    echo "    (Conditions: $LOSS% Loss, ${DELAY}ms Delay)"
    echo "=========================================================="

    # Apply Network Conditions (NetEm)
    sudo tc qdisc del dev lo root 2>/dev/null
    
    NETEM_CMD="sudo tc qdisc add dev lo root netem"
    if [ "$LOSS" != "0" ]; then NETEM_CMD="$NETEM_CMD loss ${LOSS}%"; fi
    if [ "$DELAY" != "0" ]; then NETEM_CMD="$NETEM_CMD delay ${DELAY}ms"; fi
    
    # Only run tc if we have actual constraints
    if [ "$LOSS" != "0" ] || [ "$DELAY" != "0" ]; then
        $NETEM_CMD
    fi

    # --- RUN TCP ---
    if [ -f "./tcp_benchmark" ]; then
        echo -n "👉 TCP Baseline:   "
        ./tcp_benchmark server > server_log.txt &
        SERVER_PID=$!
        sleep 2
        ./tcp_benchmark client $SERVER_IP $DURATION > /dev/null
        kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null
        
        TCP_SPEED=$(grep "SPEED" server_log.txt | awk '{print $2}')
        if [ -z "$TCP_SPEED" ]; then TCP_SPEED="0.00"; fi
        echo "${TCP_SPEED} MB/s"
        echo "$SCENARIO_NAME,TCP,$LOSS,$DELAY,$TCP_SPEED" >> $LOG_FILE
        rm server_log.txt
    fi

    # --- RUN HELIOS ---
    echo -n "👉 Helios RUDP:    "
    ./rudp_server > rudp_server_log.txt &
    SERVER_PID=$!
    sleep 2
    ./rudp_client $SERVER_IP $DURATION > /dev/null
    kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null
    
    HELIOS_SPEED=$(grep "SPEED" rudp_server_log.txt | awk '{print $2}')
    if [ -z "$HELIOS_SPEED" ]; then HELIOS_SPEED="0.00"; fi
    echo "${HELIOS_SPEED} MB/s"
    echo "$SCENARIO_NAME,HELIOS,$LOSS,$DELAY,$HELIOS_SPEED" >> $LOG_FILE
    rm rudp_server_log.txt
    
    echo ""
}

# 3. EXECUTE TEST SUITE
# ----------------------------------------------------------

# 1. Ideal Conditions
run_scenario "Ideal_Localhost" "0" "0"

# 2. Light Internet Loss (Common WiFi)
run_scenario "Spotty_WiFi" "1" "0"

# 3. Cross-Country Latency (No Loss, just Lag)
run_scenario "Cross_Country" "0" "50"

# 4. Heavy Congestion (The "TCP Killer")
run_scenario "Heavy_Congestion" "5" "0"

# 5. The Nightmare (Loss + Lag)
run_scenario "Worst_Case" "2" "50"


# 4. SUMMARY REPORT
# ----------------------------------------------------------
sudo tc qdisc del dev lo root 2>/dev/null # Cleanup
echo ""
echo "=========================================================="
echo " 📊 FINAL SUMMARY REPORT"
echo "=========================================================="
column -s, -t < $LOG_FILE
echo "=========================================================="