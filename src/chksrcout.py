import sys
import itertools
import difflib
import codecs
import subprocess
import re
from pathlib import Path
from multiprocessing import Pool
from unidiff import PatchSet

def check_out(pre_version_file_str, post_version_file_str, out_beam, num_tokens, sample):
    try:
        pre_version_tokens = pre_version_file_str.replace(' //<S2SV>','').split(' ') + ["<S2SV_null>"] * num_tokens

        parse_error=True
        special = re.compile("^<S2SV_Mod(Start|End)>$")
        # Check each beam position
        for out_str in out_beam:
            chk_file_str=""
            pre_tokens = ["<S2SV_null>"] * num_tokens
            out_tokens = out_str.split(' ')
            out_pre = out_tokens[1:num_tokens+1]
            out_idx = num_tokens+1
            i = 0;
            while i <= len(pre_version_tokens)-num_tokens:
                if ' '.join(out_pre) == ' '.join(pre_tokens): # Found match
                    while out_idx < len(out_tokens) and not special.match(out_tokens[out_idx]):
                        chk_file_str+=out_tokens[out_idx]+' '
                        out_idx+=1
                    if out_idx >= len(out_tokens): # Must have been Add action
                        out_pre = ['<S2SV_OUTNOMATCH>'] * num_tokens
                    else:
                        out_pre = out_tokens[out_idx+1:out_idx+num_tokens+1]
                        if (out_tokens[out_idx] == '<S2SV_ModEnd>'):
                            # Post token sequence is at least num_tokens+1 forward
                            # Jumping num_tokens+1 is safe because of while loop
                            pre_tokens = pre_version_tokens[i+1:i+num_tokens+1]
                            i+=num_tokens+1
                            while i <= len(pre_version_tokens) and ' '.join(out_pre) != ' '.join(pre_tokens): # No match yet
                                pre_tokens = pre_tokens[1:num_tokens]+[pre_version_tokens[i]]
                                i+=1
                            if i <= len(pre_version_tokens)-num_tokens:
                                chk_file_str += ' '.join(pre_tokens)+' '
                            else: # End-of-function special case
                                while pre_tokens[0] != '<S2SV_null>':
                                    chk_file_str += pre_tokens[0]+' '
                                    pre_tokens = pre_tokens[1:]
                            out_idx += num_tokens+1
                            if out_idx >= len(out_tokens): # Must have been Add action
                                out_pre = ['<S2SV_OUTNOMATCH>'] * num_tokens
                            else:
                                out_pre = out_tokens[out_idx+1:out_idx+num_tokens+1]
                                out_idx += num_tokens+1
                        else:
                            pre_tokens = ['<S2SV_NOMATCH>'] * num_tokens
                            out_idx += num_tokens+1
                else:
                    pre_tokens = pre_tokens[1:num_tokens]+[pre_version_tokens[i]]
                    if i < len(pre_version_tokens)-num_tokens:
                        chk_file_str+=pre_version_tokens[i]+' '
                    i+=1
            # Check if all delta tokens were processed
            if out_idx == len(out_tokens):
                parse_error=False
            if chk_file_str[:-1] == post_version_file_str.replace(' //<S2SV>',''):
                break
        if chk_file_str[:-1] == post_version_file_str.replace(' //<S2SV>',''):
            return f'Result for sample {sample} PASSED'
        elif parse_error:
            return f'Result for sample {sample} PARSE ERROR'
        else:
            return f'Result for sample {sample} FAILED'
    except Exception as e:
        print("Check_out fail: "+str(e))

def main(argv):
    bug_fix_pair_path = argv[1]
    num_tokens = int(argv[2])
    if num_tokens < 2:
        print("Usage: python chksrcout.py BugFixTokenDir num_tokens")
        print("       num_tokens must be 2 or more")
        sys.exit(2)
    root_path = Path(bug_fix_pair_path)
    pre_version_files = []
    post_version_files = []
    chk_lines=""

    for day in root_path.iterdir():
        for commit_id in day.iterdir():
            pre_version = commit_id / 'pre_version'
            for pre_version_file in pre_version.glob('**/*.tokens'):
                post_version_file = Path(str(pre_version_file).replace('pre_version', 'post_version'))
                if not post_version_file.exists():
                    print('No matching files: {} and {}'.format(str(pre_version_file), str(post_version_file)))
                    continue
                pre_version_files.append(pre_version_file)
                post_version_files.append(post_version_file)

    files = list(zip(pre_version_files, post_version_files))
    files.sort(key=lambda files: str(files[0]))
    
    out_path = root_path.parent / 'SrcTgt' / (root_path.stem + '.tgt.txt')
    out_lines = open(out_path).read().split('\n')

    beam_group=0
    beam_width=1
    for pre_version_file, post_version_file in files:
        print(f'Sample {beam_group}: {pre_version_file}', flush=True)
        out_beam = out_lines[beam_group*beam_width:beam_group*beam_width+beam_width]
        pre_version_file_str = open(pre_version_file).read()
        post_version_file_str = open(post_version_file).read()
        if pre_version_file_str.endswith(' '):
            pre_version_file_str=pre_version_file_str[:-1]
        if post_version_file_str.endswith(' '):
            post_version_file_str=post_version_file_str[:-1]
        # This if statement could be adjusted if any data pruning is desired
        if True:
            chk_lines += check_out(pre_version_file_str, post_version_file_str, out_beam, num_tokens,beam_group) +'\n'
            beam_group+=1
    
    chk_path = root_path.parent / 'SrcTgt' / (root_path.stem + '.chk.txt')

    with codecs.open(chk_path, 'w', 'utf-8') as f:
        f.write(chk_lines)

if __name__=="__main__":
    main(sys.argv)

