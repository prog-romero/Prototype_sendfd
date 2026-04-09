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
        """Create grouped bar chart showing both Config A and Config B"""
        sizes = sorted(self.data.keys())
        
        config_a_values = []
        config_b_values = []
        config_a_stds = []
        config_b_stds = []
        overhead_percentages = []
        
        for size in sizes:
            if 'A' in self.data[size]:
                a = self.data[size]['A']
                config_a_values.append(a['avg'])
                config_a_stds.append(a['stddev'])
            else:
                config_a_values.append(0)
                config_a_stds.append(0)
            
            if 'B' in self.data[size]:
                b = self.data[size]['B']
                config_b_values.append(b['avg'])
                config_b_stds.append(b['stddev'])
            else:
                config_b_values.append(0)
                config_b_stds.append(0)
            
            if 'A' in self.data[size] and 'B' in self.data[size]:
                a = self.data[size]['A']
                b = self.data[size]['B']
                overhead = b['avg'] - a['avg']
                overhead_pct = (overhead / a['avg'] * 100) if a['avg'] > 0 else 0
                overhead_percentages.append(overhead_pct)
            else:
                overhead_percentages.append(0)
        
        # Create figure
        fig, ax = plt.subplots(figsize=(15, 8))
        
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
        bar_width = 0.35
        
        # Plot grouped bars
        bars_a = ax.bar(x_pos - bar_width/2, config_a_values, bar_width,
                       yerr=config_a_stds, capsize=5,
                       label='Config A: read() only',
                       color='#3498db', edgecolor='#2980b9', linewidth=1.5, alpha=0.85,
                       error_kw={'elinewidth': 2, 'ecolor': '#2980b9', 'capthick': 2})
        
        bars_b = ax.bar(x_pos + bar_width/2, config_b_values, bar_width,
                       yerr=config_b_stds, capsize=5,
                       label='Config B: peek() + read()',
                       color='#e74c3c', edgecolor='#c0392b', linewidth=1.5, alpha=0.85,
                       error_kw={'elinewidth': 2, 'ecolor': '#c0392b', 'capthick': 2})
        
        # Add value labels on bars
        for i, (bar_a, bar_b, pct) in enumerate(zip(bars_a, bars_b, overhead_percentages)):
            # Config A value
            height_a = bar_a.get_height()
            ax.text(bar_a.get_x() + bar_a.get_width()/2, height_a + config_a_stds[i] + 5,
                   f'{config_a_values[i]:.1f}µs',
                   ha='center', va='bottom', fontsize=9, fontweight='bold', color='#2980b9')
            
            # Config B value
            height_b = bar_b.get_height()
            ax.text(bar_b.get_x() + bar_b.get_width()/2, height_b + config_b_stds[i] + 5,
                   f'{config_b_values[i]:.1f}µs',
                   ha='center', va='bottom', fontsize=9, fontweight='bold', color='#c0392b')
            
            # Overhead percentage centered between the two bars
            max_height = max(height_a + config_a_stds[i], height_b + config_b_stds[i])
            ax.text(x_pos[i], max_height + 25,
                   f'+{pct:.1f}%',
                   ha='center', va='bottom', fontsize=11, fontweight='bold',
                   color='#27ae60',
                   bbox=dict(boxstyle='round,pad=0.5', facecolor='#f0f0f0', edgecolor='#27ae60', linewidth=2))
        
        # Labels and title
        ax.set_xlabel('Payload Size', fontsize=13, fontweight='bold')
        ax.set_ylabel('Time (µs) ', fontsize=13, fontweight='bold')
        ax.set_title('MB-2: Configuration Comparison - read() Only vs peek() + read()',
                    fontsize=15, fontweight='bold', pad=20)
        
        # Add unit conversion note on the graph
        conversion_text = ''
        ax.text(0.02, 0.98, conversion_text, transform=ax.transAxes,
               fontsize=10, verticalalignment='top',
               bbox=dict(boxstyle='round', facecolor='#ecf0f1', alpha=0.8, edgecolor='#34495e', linewidth=1.5),
               family='monospace')
        
        # X-axis
        ax.set_xticks(x_pos)
        ax.set_xticklabels(size_labels, fontsize=12)
        
        # Grid
        ax.grid(True, alpha=0.3, linestyle='--', linewidth=0.7, axis='y')
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
    fig, ax = analyzer.plot_overhead(args.output)
    
    if not args.no_display:
        try:
            plt.show()
        except:
            print("[WARN] Could not display graph interactively")
    
    print("\n[✓] MB-2 analysis complete!")


if __name__ == '__main__':
    main()
