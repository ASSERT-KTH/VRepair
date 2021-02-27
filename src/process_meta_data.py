import argparse
import datetime
import dateutil
from dateutil import parser as dateutil_parser
import math
import random
import sys
import re
from pathlib import Path

parser = argparse.ArgumentParser(
    description='Generate data suitble for seq2seq training.')
parser.add_argument('--data_dir', action='store',
                    dest='data_dir', help='Path to all raw data')
parser.add_argument('--max_src_length', action='store',
                    dest='max_src_length', type=int, help='Maximum src token length')
parser.add_argument('--max_tgt_length', action='store',
                    dest='max_tgt_length', type=int, help='Maximum tgt token length')
parser.add_argument('--output_dir', action='store', dest='output_dir',
                    default='./', help='Output directory')


def read_all_data(data_dir):
    src_list = []
    tgt_list = []
    meta_list = []
    # Read all data as they are.
    for file_path in sorted(data_dir.glob('*fine.src.txt')):
        with open(file_path) as f:
            src_list.extend(f.read().splitlines())
    for file_path in sorted(data_dir.glob('*fine.tgt.txt')):
        with open(file_path) as f:
            tgt_list.extend(f.read().splitlines())
    for file_path in sorted(data_dir.glob('*fine.meta.txt')):
        with open(file_path) as f:
            meta_list.extend(f.read().splitlines())

    # Remove instances where the src or tgt is whitespace only.
    src_nonempty_list = []
    tgt_nonempty_list = []
    meta_nonempty_list = []
    # For CWE, we gather the top 15 numbers and assign all other cases to 000.
    # (these account for over 80% of cases in commits_meta.csv)
    cwe_set={ 'CWE-119', 'CWE-125', 'CWE-20', 'CWE-200', 'CWE-264',
              'CWE-476', 'CWE-399', 'CWE-189', 'CWE-416', 'CWE-190',
              'CWE-362', 'CWE-787', 'CWE-284', 'CWE-772', 'CWE-415',
              'CWE-000'}
    for src, tgt, meta in zip(src_list, tgt_list, meta_list):
        if src.strip() and tgt.strip() and meta.strip():
            if src.startswith('CWE-'):
                cwe = src.split()[0]
                if cwe in cwe_set:
                    src_nonempty_list.append(src)
                else:
                    src_nonempty_list.append(src.replace(cwe,'CWE-000'))
            else:
                print (f'ERROR! All source lines should start with CWE: {src}')
                sys.exit(2)
            tgt_nonempty_list.append(tgt)
            meta_nonempty_list.append(meta)
    return src_nonempty_list, tgt_nonempty_list, meta_nonempty_list


def remove_duplicate(src_list, tgt_list, meta_list):
    src_unique_list = []
    tgt_unique_list = []
    meta_unique_list = []
    unique_pairs = set()

    for src, tgt, meta in zip(src_list, tgt_list, meta_list):
        if (src, tgt) not in unique_pairs:
            unique_pairs.add((src, tgt))
            src_unique_list.append(src)
            tgt_unique_list.append(tgt)
            meta_unique_list.append(meta)
    return src_unique_list, tgt_unique_list, meta_unique_list


def remove_long_sequence(src_list, tgt_list, meta_list, max_src_length, max_tgt_length):
    src_suitable_list = []
    tgt_suitable_list = []
    meta_suitable_list = []

    for src, tgt, meta in zip(src_list, tgt_list, meta_list):
        if len(src.split(' ')) <= max_src_length and len(tgt.split(' ')) <= max_tgt_length:
            src_suitable_list.append(src)
            tgt_suitable_list.append(tgt)
            meta_suitable_list.append(meta)

    return src_suitable_list, tgt_suitable_list, meta_suitable_list

def main(argv):
    args = parser.parse_args(argv)

    data_dir = Path(args.data_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    src_list, tgt_list, meta_list = read_all_data(data_dir)
    src_list, tgt_list, meta_list = remove_duplicate(src_list, tgt_list, meta_list)
    src_list, tgt_list, meta_list = remove_long_sequence(
        src_list, tgt_list, meta_list, args.max_src_length, args.max_tgt_length)
    triples = list(zip(src_list, tgt_list, meta_list))
    random.shuffle(triples)
    src_list, tgt_list, meta_list = zip(*triples)
    for year in (2019, ):
        for month in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10):
            src_before_train_list=[]
            tgt_before_train_list=[]
            src_before_valid_list=[]
            tgt_before_valid_list=[]
            src_now_list=[]
            tgt_now_list=[]
            for src, tgt, meta in zip(src_list, tgt_list, meta_list):
                create_time_token = meta.strip().split(',')[4]
                if create_time_token:
                    create_date = dateutil_parser.parse(create_time_token).date()

                    valid_date_max = datetime.date(year, month, 1)
                    train_date_max = valid_date_max - dateutil.relativedelta.relativedelta(months=6)

                    if create_date < valid_date_max:
                        src_before_train_list.append(src)
                        tgt_before_train_list.append(tgt)
                        if create_date >= train_date_max:
                            src_before_valid_list.append(src)
                            tgt_before_valid_list.append(tgt)
                    elif create_date.year == year and create_date.month == month:
                        src_now_list.append(src)
                        tgt_now_list.append(tgt)

            src_filename = f'BugFix-before-{year}-{month}-train-src.txt'
            tgt_filename = f'BugFix-before-{year}-{month}-train-tgt.txt'
            with open(output_dir / src_filename, encoding='utf-8', mode='w') as f:
                f.write('\n'.join(src_before_train_list) + '\n')
            with open(output_dir / tgt_filename, encoding='utf-8', mode='w') as f:
                f.write('\n'.join(tgt_before_train_list) + '\n')
            src_filename = f'BugFix-before-{year}-{month}-valid-src.txt'
            tgt_filename = f'BugFix-before-{year}-{month}-valid-tgt.txt'
            with open(output_dir / src_filename, encoding='utf-8', mode='w') as f:
                f.write('\n'.join(src_before_valid_list) + '\n')
            with open(output_dir / tgt_filename, encoding='utf-8', mode='w') as f:
                f.write('\n'.join(tgt_before_valid_list) + '\n')
            src_filename = f'BugFix-{year}-{month}-src.txt'
            tgt_filename = f'BugFix-{year}-{month}-tgt.txt'
            with open(output_dir / src_filename, encoding='utf-8', mode='w') as f:
                f.write('\n'.join(src_now_list) + '\n')
            with open(output_dir / tgt_filename, encoding='utf-8', mode='w') as f:
                f.write('\n'.join(tgt_now_list) + '\n')


if __name__ == '__main__':
    main(sys.argv[1:])
