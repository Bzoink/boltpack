#!/usr/bin/env python3
import sqlite3
import argparse

def main():
    parser = argparse.ArgumentParser(description="Fluff SQLite Analyzer")
    parser.add_argument("--db", default="fluff_trace.sqlite", help="Input SQLite database")
    args = parser.parse_args()

    try:
        conn = sqlite3.connect(args.db)
        cursor = conn.cursor()
        
        print("=== FLUFF PROTOTYPE ANALYSIS ===")
        
        cursor.execute("SELECT count(*) FROM parse_trace")
        total = cursor.fetchone()[0]
        print(f"Total Parser Decisions: {total:,}")
        if total == 0:
            return
            
        cursor.execute("SELECT count(*) FROM parse_trace WHERE action='literal'")
        lits = cursor.fetchone()[0]
        
        cursor.execute("SELECT count(*) FROM parse_trace WHERE action='match'")
        matches = cursor.fetchone()[0]
        
        cursor.execute("SELECT count(*) FROM parse_trace WHERE action='rep_match'")
        rep_matches = cursor.fetchone()[0]
        
        print(f"Literals:    {lits:,} ({lits/total*100:.2f}%)")
        print(f"Matches:     {matches:,} ({matches/total*100:.2f}%)")
        print(f"Rep-Matches: {rep_matches:,} ({rep_matches/total*100:.2f}%)")
        
        cursor.execute("SELECT avg(length), max(length) FROM parse_trace WHERE action IN ('match', 'rep_match')")
        avg_len, max_len = cursor.fetchone()
        if avg_len:
            print(f"Average Match Length: {avg_len:.2f}")
            print(f"Max Match Length:     {max_len}")
            
        cursor.execute("SELECT count(*) FROM parse_trace WHERE is_lazy=1")
        lazy = cursor.fetchone()[0]
        print(f"Lazy Evaluations Triggered (Literals replacing Matches): {lazy:,}")
        
        print("\n--- Match Distance Distribution ---")
        buckets = [
            (1, 64), 
            (65, 256), 
            (257, 4096), 
            (4097, 32768), 
            (32769, 65536)
        ]
        for low, high in buckets:
            cursor.execute("SELECT count(*) FROM parse_trace WHERE action='match' AND distance BETWEEN ? AND ?", (low, high))
            c = cursor.fetchone()[0]
            if matches > 0:
                print(f"  {low:5d} - {high:5d} : {c:7,} ({c/matches*100:5.2f}%)")

        print("\n--- Smallest Matches Optimization Targets ---")
        for code_len in range(4, min(10, max_len + 1)):
            cursor.execute("SELECT count(*) FROM parse_trace WHERE action='match' AND length=?", (code_len,))
            c = cursor.fetchone()[0]
            if matches > 0:
                print(f"  Length={code_len:02d}: {c:7,} matches")

        conn.close()
    except sqlite3.Error as e:
        print(f"Database error: {e}")

if __name__ == "__main__":
    main()
