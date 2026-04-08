#!/usr/bin/env python3

"""
MB-2: read_peek() vs wolfSSL_read() Overhead - Graph Generation

Generates line chart showing overhead vs payload size
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


class MB2Analyzer:
    def __init__(self, csv_file):
        self.csv_file = csv_file
        self.data = {}
        self.load_data()
    
    def load_data(self):
        """Load CSV results"""
        if not os.path.exists(self.csv_file):
            print(f"ERROR: {self.csv_file} not found")
            sys.exit(1)
        
        with open(self.csv_file, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                size = int(row['payload_size_bytes']) if 'payload_size_bytes' in row else int(row['payload_size'])
                config = row['config']
                avg_time = float(row['avg_time_per_iteration_us'] if 'avg_time_per_iteration_us' in row else row['avg_time_per_iter'])
                stddev_time = float(row['stddev_us'] if 'stddev_us' in row else row['stddev'])
                
                if size not in self.data:
                    self.data[size] = {}
                
                self.data[size][config] = {'avg': avg_time, 'stddev': stddev_time}
    
    def plot_overhead(self, output_file='plot_mb2.png'):
        """Create line chart of overhead vs payload size"""
        sizes = sorted(self.data.keys())
        
        overhead_values = []
        overhead_stds = []
        
        for size in sizes:
            if 'A' in self.data[size] and 'B' in self.data[size]:
                a = self.data[size]['A']
                b = self.data[size]['B']
                overhead = b['avg'] - a['avg']
                overhead_std = np.sqrt(a['stddev']**2 + b['stddev']**2)
                overhead_values.append(overhead)
                overhead_stds.append(overhead_std)
            else:
                overhead_values.append(0)
                overhead_stds.append(0)
        
        # Create figure
        fig, ax = plt.subplots(figsize=(12, 7))
        
        # Convert sizes to display names
        size_labels = ['256 B', '1 KiB', '4 KiB']
        x_pos = np.arange(len(sizes))
        
        # Plot line with error bars
        ax.errorbar(x_pos, overhead_values, yerr=overhead_stds, 
                   fmt='o-', linewidth=2.5, markersize=10, 
                   capsize=8, capthick=2, 
                   label='Overhead (peek cost)',
                   color='#e74c3c', ecolor='#c0392b', elinewidth=2)
        
        # Add value labels
        for i, (x, y, std) in enumerate(zip(x_pos, overhead_values, overhead_stds)):
            ax.text(x, y + std + 50, f'{y:.0f} µs', 
                   ha='center', va='bottom', fontsize=11, fontweight='bold')
        
        # Labels and title
        ax.set_xlabel('Payload Size', fontsize=13, fontweight='bold')
        ax.set_ylabel('Overhead (µs)', fontsize=13, fontweight='bold')
        ax.set_title('MB-2: tls_read_peek() Overhead vs Payload Size\nIncrease in time due to double decryption',
                    fontsize=15, fontweight='bold', pad=20)
        
        # X-axis
        ax.set_xticks(x_pos)
        ax.set_xticklabels(size_labels, fontsize=12)
        
        # Grid
        ax.grid(True, alpha=0.3, linestyle='--', linewidth=0.7)
        ax.set_axisbelow(True)
        
        # Legend
        ax.legend(loc='upper left', fontsize=12, framealpha=0.95)
        
        # Set y-axis to start at 0
        ax.set_ylim(bottom=0)
        
        # Tight layout
        plt.tight_layout()
        
        # Save
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"[✓] Graph saved: {output_file}")
        
        pdf_file = output_file.replace('.png', '.pdf')
        plt.savefig(pdf_file, dpi=300, bbox_inches='tight')
        print(f"[✓] Graph saved: {pdf_file}")
        
        return fig, ax
    
    def print_summary(self):
        """Print summary statistics"""
        print("\n" + "="*80)
        print("MB-2: read_peek() vs wolfSSL_read() Overhead Analysis")
        print("="*80 + "\n")
        
        for size in sorted(self.data.keys()):
            print(f"Payload Size: {size} bytes")
            print("-" * 80)
            
            if 'A' in self.data[size]:
                a = self.data[size]['A']
                print(f"  Config A (read only):    {a['avg']:8.2f} µs ± {a['stddev']:6.2f} µs")
            
            if 'B' in self.data[size]:
                b = self.data[size]['B']
                print(f"  Config B (peek + read):  {b['avg']:8.2f} µs ± {b['stddev']:6.2f} µs")
            
            if 'A' in self.data[size] and 'B' in self.data[size]:
                a = self.data[size]['A']
                b = self.data[size]['B']
                overhead_us = b['avg'] - a['avg']
                overhead_pct = (overhead_us / a['avg']) * 100 if a['avg'] > 0 else 0
                
                print(f"\n  Overhead:                {overhead_us:8.2f} µs ({overhead_pct:+.1f}%)")
                
                if 80 <= overhead_pct <= 120:
                    print("  ✓ EXPECTED - roughly 100% (peek costs one decryption)")
                elif overhead_pct < 50:
                    print("  ⚠ UNEXPECTED - overhead seems low")
                elif overhead_pct > 200:
                    print("  ⚠ UNEXPECTED - overhead seems too high")
            
            print()
        
        print("="*80 + "\n")


def main():
    parser = argparse.ArgumentParser(
        description='MB-2: read_peek() vs wolfSSL_read() - Graph Generator'
    )
    parser.add_argument('--csv', default='results_mb2.csv',
                       help='Input CSV file')
    parser.add_argument('--output', default='plot_mb2.png',
                       help='Output graph file')
    parser.add_argument('--no-display', action='store_true',
                       help='Do not display graph interactively')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.csv):
        print(f"ERROR: CSV file not found: {args.csv}")
        sys.exit(1)
    
    print(f"Loading results from: {args.csv}\n")
    
    analyzer = MB2Analyzer(args.csv)
    analyzer.print_summary()
    
    print("Generating graph...")
    fig, ax = analyzer.plot_grouped_bars(args.output) if hasattr(analyzer, 'plot_grouped_bars') else analyzer.plot_overhead(args.output)
    
    if not args.no_display:
        try:
            plt.show()
        except:
            print("[WARN] Could not display graph interactively")
    
    print("\n[✓] MB-2 analysis complete!")


if __name__ == '__main__':
    main()
