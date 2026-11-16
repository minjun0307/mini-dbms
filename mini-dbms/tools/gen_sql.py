#!/usr/bin/env python3
"""대용량 INSERT SQL 파일 생성기

    python3 tools/gen_sql.py 1000000 > big.sql            # 한 문장에 한 행
    python3 tools/gen_sql.py 1000000 --batch 1000 > big.sql
    ./build/mdb -q big.sql                                 # CLI 로 그대로 실행
"""
import argparse
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rows", type=int)
    ap.add_argument("--batch", type=int, default=1, help="rows per INSERT statement")
    ap.add_argument("--table", default="users")
    args = ap.parse_args()

    out = sys.stdout
    i = 1
    while i <= args.rows:
        end = min(i + args.batch - 1, args.rows)
        values = ", ".join(f"('user_{k}', {18 + (k * 7) % 60})" for k in range(i, end + 1))
        out.write(f"INSERT INTO {args.table} (name, age) VALUES {values};\n")
        i = end + 1


if __name__ == "__main__":
    main()
