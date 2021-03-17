import argparse
import sys
from collections import Counter
from pathlib import Path

parser = argparse.ArgumentParser(
    description='Compare predictions against the ground truth and output CWE analysis')
parser.add_argument('--prediction_file', action='store',
                    dest='prediction_file', help='Path to the prediction file')
parser.add_argument('--src_file', action='store',
                    dest='src_file', help='Test src file')
parser.add_argument('--tgt_file', action='store',
                    dest='tgt_file', help='Test tgt file')
parser.add_argument('--top_n_cwe', action='store', type=int, default=10,
                    dest='top_n_cwe', help='Top n CWE ID to analyze, default to 10')


def get_top_n_cwe(top_n_cwe, src_file):
    cwe_id_list = []
    with open(src_file) as f:
        for line in f:
            cwe_id_token = line.split(' ')[0]
            cwe_id_list.append(cwe_id_token)

    top_cwe_id_counter = Counter(cwe_id_list).most_common(top_n_cwe+1)
    top_cwe_id_list = []
    for (id, count) in top_cwe_id_counter:
        if id == 'CWE-000':
            continue
        if len(top_cwe_id_list) == top_n_cwe:
            break
        top_cwe_id_list.append(id)
    return cwe_id_list, top_cwe_id_list



def main(argv):
    args = parser.parse_args(argv)

    prediction_file = Path(args.prediction_file).resolve()
    src_file = Path(args.src_file).resolve()
    tgt_file = Path(args.tgt_file).resolve()
    top_n_cwe = args.top_n_cwe

    for file in [prediction_file, src_file, tgt_file]:
        assert(file.exists())

    cwe_id_list, top_cwe_id_list = get_top_n_cwe(top_n_cwe, src_file)

    with open(prediction_file) as f:
        prediction_lines = f.read().splitlines()
    with open(tgt_file) as f:
        tgt_lines = f.read().splitlines()

    assert(len(prediction_lines) % len(tgt_lines) == 0)
    beam_size = int(len(prediction_lines) / len(tgt_lines))

    cwe_id_stat_dict = {}
    for cwe_id in top_cwe_id_list:
        cwe_id_stat_dict[cwe_id] = {'correct': 0, 'total': 0}

    for i in range(len(tgt_lines)):
        tgt_line = tgt_lines[i]

        # Remove all whitespaces
        tgt_line = ''.join(tgt_line.split())
        if cwe_id_list[i] not in top_cwe_id_list:
            continue

        for j in range(beam_size):
            prediction_line = prediction_lines[i*beam_size+j]
            # Remove all whitespaces
            prediction_line = ''.join(prediction_line.split())
            if(prediction_line == tgt_line):
                cwe_id_stat_dict[cwe_id_list[i]]['correct'] += 1

        cwe_id_stat_dict[cwe_id_list[i]]['total'] += 1

    print(cwe_id_stat_dict)





if __name__ == '__main__':
    main(sys.argv[1:])
