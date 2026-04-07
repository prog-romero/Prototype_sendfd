#!/usr/bin/env python3

"""
MB-1: TLS Handshake Rate Benchmark - Graph Generation

Generates grouped bar chart showing:
  - Configuration A (Vanilla wolfSSL)
  - Configuration B (With keylog callback/libtlspeek)

For handshake counts: 1000, 5000, 10000

Includes error bars (standard deviation if multiple runs available)
"""

import csv
import sys
import os
import argparse
from pathlib import Path

try:
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy required. Install with:")
    print("  pip3 install matplotlib numpy")
    sys.exit(1)


class MB1Analyzer:
    def __init__(self, csv_file):
        self.csv_file = csv_file
        self.data = {}
        self.load_data()
    
    def load_data(self):
        """Load CSV results into organized structure"""
        if not os.path.exists(self.csv_file):
            print(f"ERROR: {self.csv_file} not found")
            sys.exit(1)
        
        # Structure: data[count][config] = {'avg': float, 'stddev': float}
        with open(self.csv_file, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                count = int(row['num_handshakes'])
                config = row['config']
                avg_time = float(row['avg_time_per_handshake_us'])
                stddev_time = float(row['stddev_time_us']) if 'stddev_time_us' in row else 0.0
                
                if count not in self.data:
                    self.data[count] = {}
                
                self.data[count][config] = {'avg': avg_time, 'stddev': stddev_time}
    
    def calculate_stats(self):
        """Extract statistics from data (already calculated in CSV)"""
        stats = {}
        for count in self.data:
            stats[count] = {}
            for config in self.data[count]:
                data_point = self.data[count][config]
                stats[count][config] = {
                    'mean': data_point['avg'],
                    'std': data_point['stddev']
                }
        return stats
    
    def plot_grouped_bars(self, output_file='plot_mb1.png'):
        """Create grouped bar chart"""
        stats = self.calculate_stats()
        
        # Sort counts for x-axis
        counts = sorted(stats.keys())
        
        # Prepare data
        x = np.arange(len(counts))
        width = 0.35  # Width of bars
        
        means_a = [stats[c]['A']['mean'] for c in counts]
        stds_a = [stats[c]['A']['std'] for c in counts]
        
        means_b = [stats[c]['B']['mean'] for c in counts]
        stds_b = [stats[c]['B']['std'] for c in counts]
        
        # Create figure
        fig, ax = plt.subplots(figsize=(12, 6))
        
        # Plot bars
        bars_a = ax.bar(x - width/2, means_a, width, label='Config A: Vanilla wolfSSL',
                       color='#3498db', alpha=0.8, edgecolor='black', linewidth=1.5,
                       yerr=stds_a, capsize=5, error_kw={'elinewidth': 2, 'capthick': 2})
        
        bars_b = ax.bar(x + width/2, means_b, width, label='Config B: With keylog callback',
                       color='#e74c3c', alpha=0.8, edgecolor='black', linewidth=1.5,
                       yerr=stds_b, capsize=5, error_kw={'elinewidth': 2, 'capthick': 2})
        
        # Labels and title
        ax.set_xlabel('Number of Handshakes', fontsize=12, fontweight='bold')
        ax.set_ylabel('Time per Handshake (µs)', fontsize=12, fontweight='bold')
        ax.set_title('MB-1: TLS 1.3 Handshake Rate Comparison\nlibtlspeek Overhead Analysis',
                    fontsize=14, fontweight='bold', pad=20)
        
        # X-axis
        ax.set_xticks(x)
        ax.set_xticklabels([f'{c:,}' for c in counts], fontsize=11)
        
        # Grid
        ax.grid(axis='y', alpha=0.3, linestyle='--', linewidth=0.7)
        ax.set_axisbelow(True)
        
        # Legend
        ax.legend(loc='upper left', fontsize=11, framealpha=0.95)
        
        # Add value labels on bars
        for bars in [bars_a, bars_b]:
            for bar in bars:
                height = bar.get_height()
                ax.text(bar.get_x() + bar.get_width()/2., height,
                       f'{height:.1f}', ha='center', va='bottom', fontsize=9)
        
        # Tight layout
        plt.tight_layout()
        
        # Save
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"[✓] Graph saved: {output_file}")
        
        # Also save as PDF
        pdf_file = output_file.replace('.png', '.pdf')
        plt.savefig(pdf_file, dpi=300, bbox_inches='tight')
        print(f"[✓] Graph saved: {pdf_file}")
        
        return fig, ax
    
    def print_summary(self):
        """Print summary statistics"""
        stats = self.calculate_stats()
        
        print("\n" + "="*70)
        print("MB-1: TLS Handshake Rate Benchmark - Summary")
        print("="*70)
        
        for count in sorted(stats.keys()):
            print(f"\nN = {count:,} handshakes:")
            print("-" * 70)
            
            if 'A' in stats[count]:
                a = stats[count]['A']
                print(f"  Config A (Vanilla):     {a['mean']:8.2f} µs ± {a['std']:6.2f} µs")
            
            if 'B' in stats[count]:
                b = stats[count]['B']
                print(f"  Config B (Keylog):      {b['mean']:8.2f} µs ± {b['std']:6.2f} µs")
            
            if 'A' in stats[count] and 'B' in stats[count]:
                a = stats[count]['A']['mean']
                b = stats[count]['B']['mean']
                overhead_us = b - a
                overhead_pct = (overhead_us / a) * 100
                
                print(f"\n  Overhead:               {overhead_us:8.2f} µs ({overhead_pct:+.1f}%)")
                
                if overhead_pct < 5:
                    print("  ✓ MINIMAL overhead (<5%) - excellent!")
                elif overhead_pct < 10:
                    print("  ~ Acceptable overhead (5-10%)")
                else:
                    print("  ⚠ Significant overhead (>10%)")
        
        print("\n" + "="*70 + "\n")


def main():
    parser = argparse.ArgumentParser(
        description='MB-1: TLS Handshake Rate Benchmark - Graph Generator'
    )
    parser.add_argument('--csv', default='results_mb1.csv',
                       help='Input CSV file (default: results_mb1.csv)')
    parser.add_argument('--output', default='plot_mb1.png',
                       help='Output graph file (default: plot_mb1.png)')
    parser.add_argument('--no-display', action='store_true',
                       help='Do not display graph interactively')
    
    args = parser.parse_args()
    
    # Check if CSV exists
    if not os.path.exists(args.csv):
        print(f"ERROR: CSV file not found: {args.csv}")
        print("\nRun the benchmark first:")
        print(f"  bash run_mb1.sh")
        sys.exit(1)
    
    print(f"Loading results from: {args.csv}")
    
    # Analyze
    analyzer = MB1Analyzer(args.csv)
    
    # Print summary
    analyzer.print_summary()
    
    # Plot
    print("Generating graph...")
    fig, ax = analyzer.plot_grouped_bars(args.output)
    
    # Display if requested
    if not args.no_display:
        try:
            plt.show()
        except:
            print("[WARN] Could not display graph interactively (running headless?)")
    
    print("\n[✓] MB-1 analysis complete!")


if __name__ == '__main__':
    main()
