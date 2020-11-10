import argparse
import math
import random
import sys
from pathlib import Path

parser = argparse.ArgumentParser(
    description='Generate data suitble for seq2seq training.')
parser.add_argument('-data_dir', action='store',
                    dest='data_dir', help='Path to all raw data')
parser.add_argument('-max_src_length', action='store',
                    dest='max_src_length', type=int, help='Maximum src token length')
parser.add_argument('-max_tgt_length', action='store',
                    dest='max_tgt_length', type=int, help='Maximum tgt token length')
parser.add_argument('--split_range', action='store', nargs='+',
                    dest='split_range', type=float,
                    help='train/valid/test data range, must sum up to 100')
parser.add_argument('--output_dir', action='store', dest='output_dir',
                    default='./', help='Output directory')


def read_all_data(data_dir):
    src_list = []
    tgt_list = []
    # Read all data as they are.
    for year in ('2017', '2018'):
        for month in ('01', '02', '03', '04', '05', '06', '07', '08', '09', '10', '11', '12'):
            src_filename = f'BugFixTokenPairs_{year}_{month}.src.txt'
            tgt_filename = f'BugFixTokenPairs_{year}_{month}.tgt.txt'

            with open(data_dir / src_filename) as f:
                src_list.extend(f.read().splitlines())
            with open(data_dir / tgt_filename) as f:
                tgt_list.extend(f.read().splitlines())

    # Remove instances where the src or tgt is whitespace only.
    src_nonempty_list = []
    tgt_nonempty_list = []
    for src, tgt in zip(src_list, tgt_list):
        if src.strip() and tgt.strip():
            src_nonempty_list.append(src)
            tgt_nonempty_list.append(tgt)
    return src_nonempty_list, tgt_nonempty_list


def remove_duplicate(src_list, tgt_list):
    src_unique_list = []
    tgt_unique_list = []
    unique_pairs = set()

    for src, tgt in zip(src_list, tgt_list):
        if (src, tgt) not in unique_pairs:
            unique_pairs.add((src, tgt))
            src_unique_list.append(src)
            tgt_unique_list.append(tgt)

    return src_unique_list, tgt_unique_list


def remove_long_sequence(src_list, tgt_list, max_src_length, max_tgt_length):
    src_suitable_list = []
    tgt_suitable_list = []

    for src, tgt in zip(src_list, tgt_list):
        if len(src.split(' ')) <= max_src_length and len(tgt.split(' ')) <= max_tgt_length:
            src_suitable_list.append(src)
            tgt_suitable_list.append(tgt)

    return src_suitable_list, tgt_suitable_list


def split_train_val_test(src_list, tgt_list, split_range):
    src_tgt_pairs = list(zip(src_list, tgt_list))
    random.shuffle(src_tgt_pairs)
    src_list, tgt_list = zip(*src_tgt_pairs)

    list_count = len(src_list)
    train_index = math.floor((split_range[0] / 100) * list_count)
    validation_index = math.floor((split_range[1] / 100) * list_count)
    validation_index += train_index

    train_src_list = src_list[:train_index]
    train_tgt_list = tgt_list[:train_index]
    valid_src_list = src_list[train_index:validation_index]
    valid_tgt_list = tgt_list[train_index:validation_index]
    test_src_list = src_list[validation_index:list_count]
    test_tgt_list = tgt_list[validation_index:list_count]

    return (train_src_list, train_tgt_list,
            valid_src_list, valid_tgt_list,
            test_src_list, test_tgt_list)


def main(argv):
    args = parser.parse_args(argv)

    data_dir = Path(args.data_dir).resolve()
    src_list, tgt_list = read_all_data(data_dir)
    src_list, tgt_list = remove_duplicate(src_list, tgt_list)
    src_list, tgt_list = remove_long_sequence(
        src_list, tgt_list, args.max_src_length, args.max_tgt_length)
    (train_src_list, train_tgt_list, valid_src_list, valid_tgt_list,
     test_src_list, test_tgt_list) = split_train_val_test(
        src_list, tgt_list, args.split_range)

    train_src_filename = 'BugFix_train_src.txt'
    train_tgt_filename = 'BugFix_train_tgt.txt'
    valid_src_filename = 'BugFix_valid_src.txt'
    valid_tgt_filename = 'BugFix_valid_tgt.txt'
    test_src_filename = 'BugFix_test_src.txt'
    test_tgt_filename = 'BugFix_test_tgt.txt'

    output_dir = Path(args.output_dir).resolve()
    with open(output_dir / train_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_src_list))
    with open(output_dir / train_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_tgt_list))
    with open(output_dir / valid_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_src_list))
    with open(output_dir / valid_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_tgt_list))
    with open(output_dir / test_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_src_list))
    with open(output_dir / test_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_tgt_list))


if __name__ == '__main__':
    main(sys.argv[1:])
