#!/usr/bin/env python3
import subprocess
import json
import os

def get_uncovered_lines(filename):
    """Use jq to get uncovered line numbers for a specific file"""
    cmd = [
        'jq', '-r',
        f'.. | objects | select(.name == "{filename}") | .coverage | to_entries | map(select(.value == 0)) | map(.key + 1) | .[]',
        'covdir'
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        line_numbers = [int(line.strip()) for line in result.stdout.strip().split('\n') if line.strip()]
        return line_numbers
    except subprocess.CalledProcessError:
        return []

def get_coverage_summary(filename):
    """Get coverage summary for a file"""
    cmd = [
        'jq', '-r',
        f'.. | objects | select(.name == "{filename}") | {{name, coveragePercent, linesCovered, linesTotal, linesMissed}}',
        'covdir'
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return json.loads(result.stdout.strip())
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        return None

def group_into_ranges(line_numbers):
    """Group consecutive line numbers into ranges"""
    if not line_numbers:
        return []
    
    ranges = []
    start = line_numbers[0]
    end = line_numbers[0]
    
    for line_num in line_numbers[1:]:
        if line_num == end + 1:
            end = line_num
        else:
            ranges.append((start, end))
            start = end = line_num
    
    ranges.append((start, end))
    return ranges

def read_source_lines(filename, start_line, end_line):
    """Read specific lines from source file"""
    source_path = f'contrib/envoy/extensions/load_balancing_policies/peak_ewma/v3alpha/source/{filename}'
    
    if not os.path.exists(source_path):
        return None
    
    try:
        with open(source_path, 'r') as f:
            lines = f.readlines()
            
        # Extract the requested range (convert to 0-based indexing)
        start_idx = max(0, start_line - 1)
        end_idx = min(len(lines), end_line)
        
        return lines[start_idx:end_idx]
    except:
        return None

def main():
    peak_ewma_files = ['peak_ewma_lb.cc', 'peak_ewma_lb.h']
    
    print("Peak EWMA Coverage Analysis")
    print("=" * 50)
    
    for filename in peak_ewma_files:
        print(f"\n=== {filename} ===")
        
        # Get coverage summary
        summary = get_coverage_summary(filename)
        if summary:
            print(f"Coverage: {summary['coveragePercent']}% ({summary['linesCovered']}/{summary['linesTotal']} lines)")
            print(f"Lines missed: {summary['linesMissed']}")
        
        # Get uncovered lines
        uncovered_lines = get_uncovered_lines(filename)
        if uncovered_lines:
            ranges = group_into_ranges(uncovered_lines)
            print(f"Uncovered ranges: {ranges}")
            
            # Show source code for each range
            for start, end in ranges:
                print(f"\nLines {start}-{end}:")
                source_lines = read_source_lines(filename, start, end)
                if source_lines:
                    for i, line in enumerate(source_lines):
                        line_num = start + i
                        print(f"  {line_num:3d}: {line.rstrip()}")
                else:
                    print(f"  Could not read source file")
        else:
            print("All lines covered!")

if __name__ == "__main__":
    main()