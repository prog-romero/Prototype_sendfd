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
            lines = f.readlines()
        
        # Find where CSV data starts (after header lines)
        csv_start = 0
        for i, line in enumerate(lines):
            # Skip header lines and empty lines
            if line.strip() and not line.startswith('═') and not line.startswith('│') and not line.startswith('['):
                # Check if this line looks like CSV data (starts with number)
                if line.strip()[0].isdigit():
                    csv_start = i
                    break
        
        # Parse CSV data (format: payload_size,config,iterations,total_time,avg_time,stddev,failed_count)
        for line in lines[csv_start:]:
            line = line.strip()
            if not line or line.startswith('['):
                continue
            
            try:
                parts = line.split(',')
                if len(parts) >= 5:
                    size = int(parts[0])
                    config = parts[1].strip()
                    # avg_time is at index 4
                    avg_time = float(parts[4])
                    # stddev is at index 5
                    stddev_time = float(parts[5]) if len(parts) > 5 else 0.0
                    
                    if size not in self.data:
                        self.data[size] = {}
                    
                    self.data[size][config] = {'avg': avg_time, 'stddev': stddev_time}
            except (ValueError, IndexError):
                continue
    
    def plot_overhead(self, output_file='plot_mb2.png'):
        """Create line chart of overhead vs payload size"""
        sizes = sorted(self.data.keys())
        
        overhead_values = []
        overhead_stds = []
        overhead_percentages = []
        
        for size in sizes:
            if 'A' in self.data[size] and 'B' in self.data[size]:
                a = self.data[size]['A']
                b = self.data[size]['B']
                overhead = b['avg'] - a['avg']
                overhead_std = np.sqrt(a['stddev']**2 + b['stddev']**2)
                overhead_pct = (overhead / a['avg'] * 100) if a['avg'] > 0 else 0
                overhead_values.append(overhead)
                overhead_stds.append(overhead_std)
                overhead_percentages.append(overhead_pct)
            else:
                overhead_values.append(0)
                overhead_stds.append(0)
                overhead_percentages.append(0)
        
        # Create figure
        fig, ax = plt.subplots(figsize=(14, 8))
        
        # Generate display labels for sizes
        def size_to_label(size_bytes):
            if size_bytes >= 1024 * 1024:
                return f"{size_bytes // (1024*1024)} MB"
            elif size_bytes >= 1024:
                return f"{size_bytes // 1024} KB"
            else:
                return f"{size_bytes} B"
        
        size_labels = [size_to_label(s) for s in sizes]
        x_pos = np.arange(len(sizes))
        
        # Plot line with error bars
        ax.errorbar(x_pos, overhead_values, yerr=overhead_stds, 
                   fmt='o-', linewidth=2.5, markersize=10, 
                   capsize=8, capthick=2, 
                   label='Overhead (peek cost)',
                   color='#e74c3c', ecolor='#c0392b', elinewidth=2)
        
        # Add value labels WITH PERCENTAGES [showing the % overhead]
        for i, (x, y, std, pct) in enumerate(zip(x_pos, overhead_values, overhead_stds, overhead_percentages)):
            # Absolute [actual numerical] value in µs
            ax.text(x, y + std + 50, f'{y:.1f} µs', 
                   ha='center', va='bottom', fontsize=10, fontweight='bold', color='#2c3e50')
            # Percentage label [showing the % value]
            ax.text(x, y + std + 100, f'({pct:.1f}%)', 
                   ha='center', va='bottom', fontsize=11, fontweight='bold', 
                   color='#e74c3c', bbox=dict(boxstyle='round,pad=0.3', facecolor='yellow', alpha=0.3))
        
        # Labels and title
        ax.set_xlabel('Payload Size', fontsize=13, fontweight='bold')
        ax.set_ylabel('Overhead (µs)', fontsize=13, fontweight='bold')
        
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
