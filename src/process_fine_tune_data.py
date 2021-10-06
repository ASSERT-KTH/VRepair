import argparse
import collections
import datetime
from dateutil import parser as dateutil_parser
import math
import random
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description='Generate fine tune data for seq2seq training.')
parser.add_argument('--src_file', action="store",
                    dest='src_file', help="Path to src_file")
parser.add_argument('--tgt_file', action="store",
                    dest='tgt_file', help="Path to tgt_file")
parser.add_argument('--meta_file', action="store",
                    dest='meta_file', help="Path to meta_file")
parser.add_argument('--max_src_length', action='store',
                    dest='max_src_length', type=int, help='Maximum src token length')
parser.add_argument('--max_tgt_length', action='store',
                    dest='max_tgt_length', type=int, help='Maximum tgt token length')
parser.add_argument('--generate_random', action='store_true', dest='generate_random',
                    help='Generate random split.')
parser.add_argument('--generate_time', action='store_true', dest='generate_time',
                    help='Generate split based on time.')
parser.add_argument('--generate_frequency', action='store_true', dest='generate_frequency',
                    help='Generate split based on cwe frequency.')
parser.add_argument('--is_big_vul', action='store_true', dest='is_big_vul',
                    help='If set generate fine tune data for big_vul, default to cve_fixes.'
                         'This only impact the date on time split.')
parser.add_argument('--output_dir', action='store', dest='output_dir',
                    default='./', help='Output directory')


def read_all_data(src_file, tgt_file, meta_file):
    src_list = []
    tgt_list = []
    meta_list = []
    # Read all data as they are.
    with open(src_file) as f:
        src_list.extend(f.read().splitlines())
    with open(tgt_file) as f:
        tgt_list.extend(f.read().splitlines())
    with open(meta_file) as f:
        meta_list.extend(f.read().splitlines())

    assert len(src_list) == len(tgt_list) == len(meta_list)

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


def generate_random_split(src_list, tgt_list, meta_list, output_dir):
    triples = list(zip(src_list, tgt_list, meta_list))
    random.shuffle(triples)
    src_list, tgt_list, meta_list = zip(*triples)

    num_examples = len(src_list)
    max_train_index = math.floor(0.7 * num_examples)
    max_valid_index = math.floor(0.8 * num_examples)

    train_src_filename = 'random_fine_tune_train.src.txt'
    train_tgt_filename = 'random_fine_tune_train.tgt.txt'
    train_meta_filename = 'random_fine_tune_train.meta.txt'

    valid_src_filename = 'random_fine_tune_valid.src.txt'
    valid_tgt_filename = 'random_fine_tune_valid.tgt.txt'
    valid_meta_filename = 'random_fine_tune_valid.meta.txt'

    test_src_filename = 'random_fine_tune_test.src.txt'
    test_tgt_filename = 'random_fine_tune_test.tgt.txt'
    test_meta_filename = 'random_fine_tune_test.meta.txt'

    with open(output_dir / train_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(src_list[:max_train_index])+'\n')
    with open(output_dir / train_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(tgt_list[:max_train_index])+'\n')
    with open(output_dir / train_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(meta_list[:max_train_index])+'\n')

    with open(output_dir / valid_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(src_list[max_train_index:max_valid_index])+'\n')
    with open(output_dir / valid_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(tgt_list[max_train_index:max_valid_index])+'\n')
    with open(output_dir / valid_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(meta_list[max_train_index:max_valid_index])+'\n')

    with open(output_dir / test_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(src_list[max_valid_index:])+'\n')
    with open(output_dir / test_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(tgt_list[max_valid_index:])+'\n')
    with open(output_dir / test_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(meta_list[max_valid_index:])+'\n')


def generate_time_split(src_list, tgt_list, meta_list, output_dir, is_big_vul):
    train_src_list = []
    train_tgt_list = []
    train_meta_list = []

    valid_src_list = []
    valid_tgt_list = []
    valid_meta_list = []

    test_src_list = []
    test_tgt_list = []
    test_meta_list = []

    if is_big_vul:
        train_date_max = datetime.date(2017, 6, 1)
        valid_date_max = datetime.date(2018, 1, 1)
    else:
        train_date_max = datetime.date(2018, 6, 1)
        valid_date_max = datetime.date(2019, 6, 1)
    for src, tgt, meta in zip(src_list, tgt_list, meta_list):
        create_time_token = meta.strip().split(',')[4]
        if create_time_token:
            create_date = dateutil_parser.parse(create_time_token).date()
            if create_date < train_date_max:
                train_src_list.append(src)
                train_tgt_list.append(tgt)
                train_meta_list.append(meta)
            elif create_date < valid_date_max:
                valid_src_list.append(src)
                valid_tgt_list.append(tgt)
                valid_meta_list.append(meta)
            else:
                test_src_list.append(src)
                test_tgt_list.append(tgt)
                test_meta_list.append(meta)

    train_src_filename = 'year_fine_tune_train.src.txt'
    train_tgt_filename = 'year_fine_tune_train.tgt.txt'
    train_meta_filename = 'year_fine_tune_train.meta.txt'

    valid_src_filename = 'year_fine_tune_valid.src.txt'
    valid_tgt_filename = 'year_fine_tune_valid.tgt.txt'
    valid_meta_filename = 'year_fine_tune_valid.meta.txt'

    test_src_filename = 'year_fine_tune_test.src.txt'
    test_tgt_filename = 'year_fine_tune_test.tgt.txt'
    test_meta_filename = 'year_fine_tune_test.meta.txt'

    with open(output_dir / train_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_src_list)+'\n')
    with open(output_dir / train_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_tgt_list)+'\n')
    with open(output_dir / train_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_meta_list)+'\n')

    with open(output_dir / valid_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_src_list)+'\n')
    with open(output_dir / valid_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_tgt_list)+'\n')
    with open(output_dir / valid_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_meta_list)+'\n')

    with open(output_dir / test_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_src_list)+'\n')
    with open(output_dir / test_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_tgt_list)+'\n')
    with open(output_dir / test_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_meta_list)+'\n')


def generate_frequency_split(src_list, tgt_list, meta_list, output_dir):
    all_cwe = []
    for meta in meta_list:
        cwe_token = meta.strip().split(',')[2]
        if cwe_token:
            all_cwe.append(cwe_token)
    cwe_counter = collections.Counter(all_cwe)
    top_10_cwe = [counts[0] for counts in cwe_counter.most_common(10)]
    all_data_dict = {}
    for cwe in top_10_cwe:
        all_data_dict[cwe] = {'src_list': [], 'tgt_list': [], 'meta_list': []}
    for src, tgt, meta in zip(src_list, tgt_list, meta_list):
        cwe_token = src.split(' ')[0]
        if cwe_token in all_data_dict:
            all_data_dict[cwe_token]['src_list'].append(src)
            all_data_dict[cwe_token]['tgt_list'].append(tgt)
            all_data_dict[cwe_token]['meta_list'].append(meta)

    train_src_list = []
    train_tgt_list = []
    train_meta_list = []

    valid_src_list = []
    valid_tgt_list = []
    valid_meta_list = []

    test_src_list = []
    test_tgt_list = []
    test_meta_list = []

    for cwe in all_data_dict:
        num_examples = len(all_data_dict[cwe]['src_list'])
        max_train_index = math.floor(0.7 * num_examples)
        max_valid_index = math.floor(0.8 * num_examples)

        train_src_list.extend(all_data_dict[cwe]['src_list'][:max_train_index])
        train_tgt_list.extend(all_data_dict[cwe]['tgt_list'][:max_train_index])
        train_meta_list.extend(all_data_dict[cwe]['meta_list'][:max_train_index])

        valid_src_list.extend(all_data_dict[cwe]['src_list'][max_train_index:max_valid_index])
        valid_tgt_list.extend(all_data_dict[cwe]['tgt_list'][max_train_index:max_valid_index])
        valid_meta_list.extend(all_data_dict[cwe]['meta_list'][max_train_index:max_valid_index])

        test_src_list.extend(all_data_dict[cwe]['src_list'][max_valid_index:])
        test_tgt_list.extend(all_data_dict[cwe]['tgt_list'][max_valid_index:])
        test_meta_list.extend(all_data_dict[cwe]['meta_list'][max_valid_index:])

    train_src_filename = 'frequency_fine_tune_train.src.txt'
    train_tgt_filename = 'frequency_fine_tune_train.tgt.txt'
    train_meta_filename = 'frequency_fine_tune_train.meta.txt'

    valid_src_filename = 'frequency_fine_tune_valid.src.txt'
    valid_tgt_filename = 'frequency_fine_tune_valid.tgt.txt'
    valid_meta_filename = 'frequency_fine_tune_valid.meta.txt'

    test_src_filename = 'frequency_fine_tune_test.src.txt'
    test_tgt_filename = 'frequency_fine_tune_test.tgt.txt'
    test_meta_filename = 'frequency_fine_tune_test.meta.txt'

    with open(output_dir / train_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_src_list)+'\n')
    with open(output_dir / train_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_tgt_list)+'\n')
    with open(output_dir / train_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(train_meta_list)+'\n')

    with open(output_dir / valid_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_src_list)+'\n')
    with open(output_dir / valid_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_tgt_list)+'\n')
    with open(output_dir / valid_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(valid_meta_list)+'\n')

    with open(output_dir / test_src_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_src_list)+'\n')
    with open(output_dir / test_tgt_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_tgt_list)+'\n')
    with open(output_dir / test_meta_filename, encoding='utf-8', mode='w') as f:
        f.write('\n'.join(test_meta_list)+'\n')


def main(argv):
    args = parser.parse_args(argv)

    output_dir = Path(args.output_dir).resolve()
    src_list, tgt_list, meta_list = read_all_data(args.src_file, args.tgt_file, args.meta_file)
    src_list, tgt_list, meta_list = remove_duplicate(src_list, tgt_list, meta_list)
    src_list, tgt_list, meta_list = remove_long_sequence(
        src_list, tgt_list, meta_list, args.max_src_length, args.max_tgt_length)

    if args.generate_random:
        generate_random_split(src_list, tgt_list, meta_list, output_dir)
    if args.generate_time:
        generate_time_split(src_list, tgt_list, meta_list, output_dir, args.is_big_vul)
    if args.generate_frequency:
        generate_frequency_split(src_list, tgt_list, meta_list, output_dir)


if __name__ == '__main__':
    main(sys.argv[1:])
