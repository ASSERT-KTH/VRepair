import argparse
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
    for file_path in data_dir.rglob('*.src.txt'):
        with open(file_path) as f:
            src_list.extend(f.read().splitlines())
    for file_path in data_dir.rglob('*.tgt.txt'):
        with open(file_path) as f:
            tgt_list.extend(f.read().splitlines())
    for file_path in data_dir.rglob('*.meta.txt'):
        with open(file_path) as f:
            meta_list.extend(f.read().splitlines())

    # Remove instances where the src or tgt is whitespace only.
    src_nonempty_list = []
    tgt_nonempty_list = []
    meta_nonempty_list = []
    # For CWE, we gather the top 16 numbers and assign all other cases to 000.
    # (these account for over 90% of cases in TokenPairs_commits)
    # The perl script to find these is:
    #  perl -e 'while (<>) {/^(CWE-\d+) / && $n{$1}++; }; foreach $i (keys(%n)) { print "$i: $n{$i}\n" }' VRepair/data/Context3/BugFixTokenPairs_commits.src.txt | perl -ne 'if (/: (\d+) *$/) { if ($1 > 20) {$n+=$1; print $_}}'
    cwe_set={ 'CWE-835', 'CWE-476', 'CWE-59', 'CWE-269', 'CWE-284',
              'CWE-399', 'CWE-119', 'CWE-20', 'CWE-787', 'CWE-190',
              'CWE-400', 'CWE-416', 'CWE-200', 'CWE-264', 'CWE-125',
              'CWE-189', 'CWE-000'}
    for src, tgt, meta in zip(src_list, tgt_list, meta_list):
        if src.strip() and tgt.strip() and meta.strip():
            if src.startswith('CWE-'):
                cwe = src.split()[0]
                if cwe in cwe_set:
                    src_nonempty_list.append(src)
                else:
                    src_nonempty_list.append(src.replace(cwe,'CWE-000'))
            else:
                src_nonempty_list.append("CWE-000 "+src)
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
    for after, now in ((("2019-(0[23456789]|10|11|12)-","2019-01-"),
                        ("2019-(0[3456789]|10|11|12)-","2019-02-"),
                        ("2019-(0[456789]|10|11|12)-","2019-03-"),
                        ("2019-(0[56789]|10|11|12)-","2019-04-"),
                        ("2019-(0[6789]|10|11|12)-","2019-05-"),
                        ("2019-(0[789]|10|11|12)-","2019-06-"),
                        ("2019-(0[89]|10|11|12)-","2019-07-"),
                        ("2019-(09|10|11|12)-","2019-08-"),
                        ("2019-(10|11|12)-","2019-09-"),
                        ("2019-(11|12)-","2019-10-"))):
        src_before_list=[]
        tgt_before_list=[]
        src_now_list=[]
        tgt_now_list=[]
        for src, tgt, meta in zip(src_list, tgt_list, meta_list):
            search=re.search(","+after,meta)
            if not search:
                search=re.search(","+now,meta)
                if search:
                    src_now_list.append(src)
                    tgt_now_list.append(tgt)
                else:
                    src_before_list.append(src)
                    tgt_before_list.append(tgt)
        src_filename = f'BugFix-{now}src.txt'
        tgt_filename = f'BugFix-{now}tgt.txt'
        with open(output_dir / src_filename, encoding='utf-8', mode='w') as f:
            f.write('\n'.join(src_now_list) + '\n')
        with open(output_dir / tgt_filename, encoding='utf-8', mode='w') as f:
            f.write('\n'.join(tgt_now_list) + '\n')
        src_filename = f'BugFix-before{now}src.txt'
        tgt_filename = f'BugFix-before{now}tgt.txt'
        with open(output_dir / src_filename, encoding='utf-8', mode='w') as f:
            f.write('\n'.join(src_before_list) + '\n')
        with open(output_dir / tgt_filename, encoding='utf-8', mode='w') as f:
            f.write('\n'.join(tgt_before_list) + '\n')
            

if __name__ == '__main__':
    main(sys.argv[1:])
