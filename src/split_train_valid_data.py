import argparse
import math
from pathlib import Path
import random
import sys

parser = argparse.ArgumentParser(
    description='Split src and tgt file into train, validation and test split.')
parser.add_argument('--src_file', action='store', dest='src_file')
parser.add_argument('--tgt_file', action='store', dest='tgt_file')
parser.add_argument('--split_range', action='store', nargs='+', dest='split_range', type=float)
parser.add_argument('--output_dir', action='store', dest='output_dir', default='./')

def main(argv):
    args = parser.parse_args(argv)

    src_file = Path(args.src_file)
    tgt_file = Path(args.tgt_file)
    assert(src_file.exists() and tgt_file.exists())
    split_range = args.split_range
    assert(sum(split_range) == 100)
    output_dir = Path(args.output_dir)
    assert(output_dir.exists())

    with open(src_file) as f:
        src_lines = f.readlines()
    with open(tgt_file) as f:
        tgt_lines = f.readlines()
    assert(len(src_lines) == len(tgt_lines))

    src_tgt_pairs = list(zip(src_lines, tgt_lines))
    random.shuffle(src_tgt_pairs)
    src_lines, tgt_lines = zip(*src_tgt_pairs)

    lines_count = len(src_lines)
    train_index = math.floor((split_range[0]/100) * lines_count)
    validation_index = math.floor((split_range[1]/100) * lines_count)
    validation_index += train_index

    src_file_prefix = src_file.stem
    tgt_file_prefix = tgt_file.stem
    src_train_filename = src_file_prefix + '_train.txt'
    tgt_train_filename = tgt_file_prefix + '_train.txt'
    src_validation_filename = src_file_prefix + '_validation.txt'
    tgt_validation_filename = tgt_file_prefix + '_validation.txt'
    src_test_filename = src_file_prefix + '_test.txt'
    tgt_test_filename = tgt_file_prefix + '_test.txt'

    with open(output_dir / src_train_filename, encoding='utf-8', mode='w') as f:
        for i in range(train_index):
            f.write(src_lines[i])
    with open(output_dir / tgt_train_filename, encoding='utf-8', mode='w') as f:
        for i in range(train_index):
            f.write(tgt_lines[i])
    with open(output_dir / src_validation_filename, encoding='utf-8', mode='w') as f:
        for i in range(train_index, validation_index):
            f.write(src_lines[i])
    with open(output_dir / tgt_validation_filename, encoding='utf-8', mode='w') as f:
        for i in range(train_index, validation_index):
            f.write(tgt_lines[i])
    with open(output_dir / src_test_filename, encoding='utf-8', mode='w') as f:
        for i in range(validation_index, lines_count):
            f.write(src_lines[i])
    with open(output_dir / tgt_test_filename, encoding='utf-8', mode='w') as f:
        for i in range(validation_index, lines_count):
            f.write(tgt_lines[i])




if __name__ == '__main__':
    main(sys.argv[1:])
