import argparse
import re
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
parser.add_argument('--num_tokens', action='store', type=int,
                    dest='num_tokens', help='Number of context tokens')
parser.add_argument('--vrepair_beam', action='store', type=int, default=50,
                    dest='vrepair_beam', help='VRepair beam size')
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


def check_out(src_str, tgt_str, out_beam, num_tokens, vrepair_beam):
    raw_tokens = src_str.split(' ')[1:] + ["<S2SV_null>"] * num_tokens
    src_tokens = []
    endbug=0
    # Process out start/end tokens and track range for pattern start
    for i in range(len(raw_tokens)):
        if raw_tokens[i] == "<S2SV_StartBug>":
            if not src_tokens:
                src_tokens = raw_tokens[max(i-num_tokens,0):i]
        elif raw_tokens[i] == "<S2SV_EndBug>":
            # If StartBug was token 0, we need to grab tokens
            if not src_tokens:
                src_tokens = raw_tokens[1:i]
            if not endbug:
                endbug=len(src_tokens)
        elif src_tokens:
            src_tokens.append(raw_tokens[i])

    special = re.compile("^<S2SV_Mod(Start|End)>$")
    # Check each beam position
    beampos=1
    total_matches = 0
    for out_str in out_beam:
        firstfail=[0]*32
        counts=[0]*32
        pass_modnum=0
        # Process out_str multiple times to find how many interpretations it has
        while total_matches <= vrepair_beam:
            pre_tokens = ["<S2SV_null>"] * num_tokens
            out_tokens = out_str.split(' ')
            out_pre = out_tokens[1:num_tokens+1]
            out_idx = num_tokens+1
            modnum=1
            skip=-1
            i = 0;
            while i < len(src_tokens):
                if modnum == 1 and i > endbug or skip > 4:
                    out_idx=len(out_tokens)+1 # Flag error
                    break
                if ' '.join(out_pre) == ' '.join(pre_tokens): # Found match
                    skip+=1
                if ' '.join(out_pre) == ' '.join(pre_tokens) and skip==counts[modnum]:
                    if out_idx == len(out_tokens):
                        out_idx=len(out_tokens)+1 # Flag error
                        break
                    skip=-1
                    modnum+=1
                    while out_idx < len(out_tokens) and not special.match(out_tokens[out_idx]):
                        out_idx+=1
                    if out_idx >= len(out_tokens): # Must have been Add action
                        out_pre = ['<S2SV_OUTNOMATCH>'] * num_tokens
                    else:
                        out_pre = out_tokens[out_idx+1:out_idx+num_tokens+1]
                        if (out_tokens[out_idx] == '<S2SV_ModEnd>'):
                            # Post token sequence is at least num_tokens+1 forward
                            # Jumping num_tokens+1 is safe because of while loop
                            pre_tokens = src_tokens[i+1:i+num_tokens+1]
                            i+=num_tokens+1
                            skip=0
                            while i < len(src_tokens) and (' '.join(out_pre) != ' '.join(pre_tokens) or skip != counts[modnum]): # No match yet
                                if (' '.join(out_pre) == ' '.join(pre_tokens)):
                                    skip += 1
                                pre_tokens = pre_tokens[1:num_tokens]+[src_tokens[i]]
                                i+=1
                            if ' '.join(out_pre) != ' '.join(pre_tokens) or skip != counts[modnum]:
                                out_idx=len(out_tokens)+1 # Flag error
                                break
                            if i > endbug and skip > 3:
                                out_idx=len(out_tokens)+1 # Flag error
                                break
                            skip=-1
                            modnum+=1
                            out_idx += num_tokens+1
                            if out_idx >= len(out_tokens):
                                out_pre = ['<S2SV_OUTNOMATCH>'] * num_tokens
                            else:
                                out_pre = out_tokens[out_idx+1:out_idx+num_tokens+1]
                                out_idx += num_tokens+1
                        else:
                            pre_tokens = ['<S2SV_NOMATCH>'] * num_tokens
                            out_idx += num_tokens+1
                else:
                    pre_tokens = pre_tokens[1:num_tokens]+[src_tokens[i]]
                    i+=1
            # print (f'DBG: out_str= {out_str}, src_str= {src_str}, out_idx= {out_idx}, modnum={modnum},lenout={len(out_tokens)},src_tok={src_tokens},endbug={endbug}')
            # Check if all delta tokens were processed
            if out_idx == len(out_tokens) and modnum>1:
                total_matches+=1
                if pass_modnum >0 and pass_modnum != modnum:
                    print(f'ERROR: pass_modnum != modnum ({pass_modnum} != {modnum})')
                    print(f'     : out_str={out_str},src_str={src_str},counts={counts}')
                    sys.exit(2)
                pass_modnum = modnum
                for i in range(1,modnum):
                    if counts[i]+1 != firstfail[i]:
                        counts[i]+=1
                        break
                    else:
                        counts[i]=0
            else:
                # Check if first position attempt failed and stop search
                if pass_modnum == 0:
                    break
                # pass_modnum is one more than the actual number of mods in the output
                for i in range(1,pass_modnum):
                    # Note that counts[pass_modnum] will increment on final loop
                    if counts[i] != 0:
                        firstfail[i] = max(counts[i],firstfail[i])
                        counts[i]=0
                        counts[i+1]+=1
                        break
                for i in range(1,pass_modnum):
                    if counts[i] != 0 and counts[i] == firstfail[i]:
                        counts[i]=0
                        counts[i+1]+=1
                if counts[pass_modnum] == 1:
                    break   # Done with mod position searches
        if total_matches <= vrepair_beam:
            if out_str == tgt_str:
                return True
            else:
                beampos+=1
        else:
            return False
    return False



def main(argv):
    args = parser.parse_args(argv)

    prediction_file = Path(args.prediction_file).resolve()
    src_file = Path(args.src_file).resolve()
    tgt_file = Path(args.tgt_file).resolve()
    top_n_cwe = args.top_n_cwe
    num_tokens = args.num_tokens
    vrepair_beam = args.vrepair_beam

    for file in [prediction_file, src_file, tgt_file]:
        assert(file.exists())

    cwe_id_list, top_cwe_id_list = get_top_n_cwe(top_n_cwe, src_file)

    with open(prediction_file) as f:
        prediction_lines = f.read().splitlines()
    with open(tgt_file) as f:
        tgt_lines = f.read().splitlines()
    with open(src_file) as f:
        src_lines = f.read().splitlines()

    assert(len(prediction_lines) % len(tgt_lines) == 0)
    beam_size = int(len(prediction_lines) / len(tgt_lines))

    cwe_id_stat_dict = {}
    for cwe_id in top_cwe_id_list:
        cwe_id_stat_dict[cwe_id] = {'correct': 0, 'total': 0}
    cwe_id_stat_dict['all'] = {'correct': 0, 'total': 0}

    for i in range(len(tgt_lines)):
        tgt_line = tgt_lines[i]
        src_line = src_lines[i]
        out = prediction_lines[i*beam_size:(i+1)*beam_size]
        correct = check_out(src_line, tgt_line, out, num_tokens, vrepair_beam)

        cwe_id = src_line.split(' ')[0]
        if correct:
            cwe_id_stat_dict['all']['correct'] += 1
            cwe_id_stat_dict['all']['total'] += 1
            if cwe_id in cwe_id_stat_dict:
                cwe_id_stat_dict[cwe_id]['correct'] += 1
                cwe_id_stat_dict[cwe_id]['total'] += 1
        else:
            cwe_id_stat_dict['all']['total'] += 1
            if cwe_id in cwe_id_stat_dict:
                cwe_id_stat_dict[cwe_id]['total'] += 1

    print(cwe_id_stat_dict)


if __name__ == '__main__':
    main(sys.argv[1:])
