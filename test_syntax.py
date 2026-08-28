#!/usr/bin/env python3
"""
简单的语法检查脚本
"""
import os
import re

def check_c_syntax(file_path):
    """检查C文件的基本语法"""
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()

        # 检查基本的括号匹配
        stack = []
        errors = []

        for i, line in enumerate(content.split('\n'), 1):
            for j, char in enumerate(line):
                if char == '{':
                    stack.append((i, j + 1))
                elif char == '}':
                    if not stack:
                        errors.append(f"Line {i}, Col {j + 1}: Unmatched '}}'")
                    else:
                        stack.pop()

        # 检查未匹配的括号
        for line, col in stack:
            errors.append(f"Line {line}, Col {col}: Unmatched '{{'")

        return errors
    except Exception as e:
        return [f"Error reading file: {str(e)}"]

def main():
    # 检查主要源文件
    files_to_check = [
        'main/app_main.c',
        'main/src/app_state.c'
    ]

    all_errors = []

    for file_path in files_to_check:
        if os.path.exists(file_path):
            print(f"\nChecking {file_path}...")
            errors = check_c_syntax(file_path)
            if errors:
                print("Syntax errors found:")
                for error in errors:
                    print(f"  {error}")
                all_errors.extend(errors)
            else:
                print("  No syntax errors detected")
        else:
            print(f"  File not found: {file_path}")

    if all_errors:
        print(f"\nTotal errors: {len(all_errors)}")
    else:
        print("\nAll files passed basic syntax check!")

if __name__ == "__main__":
    main()