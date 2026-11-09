import argparse
import csv
import statistics


def summarize(rows):
    frametimes = [r['frametime_us'] for r in rows]
    n = len(frametimes)
    p99 = statistics.quantiles(frametimes, n=100, method='inclusive')[98] if n > 1 else frametimes[0]
    return {
        'frames': n,
        'avg_fps': n / (sum(frametimes) / 1e6),
        'p99_ms': p99 / 1000,
        'pct_within_16_7': 100.0 * sum(ft <= 16700 for ft in frametimes) / n,
        'frames_over_50ms': sum(ft > 50000 for ft in frametimes),
        'shadercreate_ms_total': sum(r['shadercreate_us'] for r in rows) / 1000,
    }


def load(path):
    with open(path, newline='') as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('csv')
    parser.add_argument('--from-s', type=float, default=0)
    parser.add_argument('--to-s', type=float)
    args = parser.parse_args()

    rows = load(args.csv)
    start = rows[0]['timestamp_us']
    rows = [r for r in rows if (r['timestamp_us'] - start) / 1e6 >= args.from_s
            and (args.to_s is None or (r['timestamp_us'] - start) / 1e6 <= args.to_s)]
    for key, value in summarize(rows).items():
        print(f'{key}: {value:.2f}' if isinstance(value, float) else f'{key}: {value}')


if __name__ == '__main__':
    main()
