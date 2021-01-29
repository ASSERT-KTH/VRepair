import re
import sys
import itertools
import difflib
import codecs
import subprocess
from pathlib import Path
from multiprocessing import Pool
from unidiff import PatchSet

def get_token_pair_diff(pre_version_file,pre_version_file_str,
                          post_version_file_str, num_tokens):
    try:
        pre_token_per_line = pre_version_file_str.replace(' ','\n') 
        post_token_per_line = post_version_file_str.replace(' ','\n') 

        diff = list(difflib.unified_diff(pre_token_per_line.splitlines(True), post_token_per_line.splitlines(True), fromfile=str(pre_version_file), tofile='post_version',  n=1000000))
        if diff:
            if num_tokens == 1000: 
                return (pre_version_file_str,post_version_file_str)
            # States:
            #  0: start
            #  1,2: preamble processing
            #  3: idle (no current delta)
            #  100: gathering delete
            #  101 to 10x: post-delete tokens
            #  200: gathering modify
            #  201 to 20x: post-modify tokens
            #  300: gathering addition
            #  301 to 30x: post-addition tokens
            state = 0;  # Start state
            src = ""
            src_line = ""
            bugtag = False
            tgt = ""
            pre_tokens = ["<S2SV_null>"] * num_tokens
            post_tokens = ["<S2SV_null>"] * num_tokens
            for t in diff:
                t = t.replace('\n','')
                if t.startswith("--- ") or t.startswith("+++ ") or t.startswith("@@ "):
                    if state > 2:
                        print(f'ERROR: preamble line {t} occurred in unexpected location')
                    state += 1 # State will be 3 at start of real tokens
                elif state < 3:
                    print(f'ERROR: token line {t} occurred before preamble done')
                elif t.startswith(" "):
                    if t != " //<S2SV>":
                        src_line += t[1:] + ' '
                    elif bugtag:
                        src += "<S2SV_StartBug> "+src_line+"<S2SV_EndBug> "
                        bugtag = False
                        src_line = ""
                        continue
                    else:
                        src += src_line
                        src_line = ""
                        continue
                    if state == 3: # Continue idle state
                        pre_tokens = pre_tokens[1:num_tokens] + [t[1:]] 
                    elif state % 100 == num_tokens-1:
                        post_tokens = post_tokens[1:num_tokens] + [t[1:]]
                        if state >= 300: # addition
                            tgt += '<S2SV_ModStart> '+' '.join(pre_tokens)+' '+' '.join(new_tokens)+' '
                        elif state >= 200: # modify
                            tgt += '<S2SV_ModStart> '+' '.join(pre_tokens)+' '+' '.join(new_tokens)+' <S2SV_ModEnd> '+' '.join(post_tokens)+' '
                        elif state >= 100: # delete
                            tgt += '<S2SV_ModStart> '+' '.join(pre_tokens)+' <S2SV_ModEnd> '+' '.join(post_tokens)+' '
                        state = 3
                        pre_tokens=post_tokens
                    else:
                        state += 1   # Advance post_token count
                        post_tokens = post_tokens[1:num_tokens] + [t[1:]]
                elif t.startswith("-"):
                    if t != "-//<S2SV>":
                        src_line += t[1:] + ' '
                    elif bugtag:
                        src += "<S2SV_StartBug> "+src_line+"<S2SV_EndBug> "
                        bugtag = False
                        src_line = ""
                        continue
                    else:
                        src += src_line
                        src_line = ""
                        continue
                    if state == 3: # Enter from idle state
                        bugtag=True
                        state = 100 # Assume delete at first
                        new_tokens = []
                    elif state >= 300: # Addition changes to modification
                        new_tokens += post_tokens[num_tokens - (state % 100):num_tokens]
                        state = 200
                    elif state >= 200: # Accumulate any post tokens we may have
                        new_tokens += post_tokens[num_tokens - (state % 100):num_tokens]
                        state = 200
                    elif state > 100: # Post count after delete changes to modification
                        new_tokens += post_tokens[num_tokens - (state % 100):num_tokens]
                        state = 200
                    post_tokens = ["<S2SV_null>"] * num_tokens
                        
                elif t.startswith("+"):
                    if t == "+//<S2SV>":
                        continue
                    if state == 3: # Enter from idle state
                        bugtag=True
                        state = 300 # Assume addition at first
                        new_tokens = [t[1:]]
                    elif state >= 300: 
                        new_tokens += post_tokens[num_tokens - (state % 100):num_tokens]+[t[1:]]
                        if state > 300: # Check if we started accumulating post tokens
                            state = 200 
                    elif state >= 200: # accumulate any post tokens we may have
                        new_tokens += post_tokens[num_tokens - (state % 100):num_tokens]+[t[1:]]
                        state = 200 # Modified
                    elif state >= 100: # delete changes to modify
                        new_tokens += post_tokens[num_tokens - (state % 100):num_tokens]+[t[1:]]
                        state = 200 # Change to modified
                    post_tokens = ["<S2SV_null>"] * num_tokens
                    
            # Fix end-of-file post tokens by putting <S2SV_null> at end
            post_tokens = post_tokens[num_tokens-(state % 100):num_tokens]+ \
                          post_tokens[0:num_tokens-(state % 100)]
            
            if state >= 300: # addition
                tgt += '<S2SV_ModStart> '+' '.join(pre_tokens)+' '+' '.join(new_tokens)+' '
            elif state >= 200: # modify
                tgt += '<S2SV_ModStart> '+' '.join(pre_tokens)+' '+' '.join(new_tokens)+' <S2SV_ModEnd> '+' '.join(post_tokens)+' '
            elif state >= 100: # delete
                tgt += '<S2SV_ModStart> '+' '.join(pre_tokens)+' <S2SV_ModEnd> '+' '.join(post_tokens)+' '
            if not tgt:
                print(f'ERROR: {pre_version_file_str} found no target changes in {diff}')
            return (src.strip(),tgt.strip())
        else:
            print(f'No diff found for {pre_version_file}')
            sys.exit(2)
    except Exception as e:
        print("Get token pair fail: "+str(e))

def main(argv):
    bug_fix_pair_path = argv[1]
    num_tokens = int(argv[2])
    if len(argv) > 3:
        if len(argv) > 4 and argv[3] == "-late":
            metadata_file = Path(argv[4])
            late=True
        else:
            metadata_file = Path(argv[3])
            late=False
        if not metadata_file.exists():
            # Force usage error message
            num_tokens=0
    else:
        metadata_file = None
    if num_tokens < 2:
        print("Usage: python gensrctgt.py BugFixTokenDir num_tokens [-late] [metadata]")
        print("       num_tokens must be 2 or more")
        print("       num_tokens set to 1000 results in whole file as target")
        print("       late will process CVE-2019 entries, otherwise CVE-2019 are omitted")
        print("       metadata optionally includes CWE numbers for each commit")
        sys.exit(2)
    root_path = Path(bug_fix_pair_path)
    pre_version_files = []
    post_version_files = []
    src_lines=""
    tgt_lines=""

    hash_to_cwe = {}
    if metadata_file:
        metadata_lines = open(metadata_file).read().split("\n")
        for l in metadata_lines:
            latesearch = re.search(r', *(CVE-2019-[0123456789])',l)
            search = re.search(r'/([0123456789abcdef]+) *,.*, *(CWE-[0123456789]*) *,',l)
            if not search:
                search = re.search(r'/(CVE-[0123456789]*-[0123456789]*).* (CWE-[0123456789]*)',l)
            if search:
                if latesearch and not late or not latesearch and late:
                    hash_to_cwe[search.group(1)] = "IGNORE"
                else:
                    hash_to_cwe[search.group(1)] = search.group(2)

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

    for pre_version_file, post_version_file in files:
        print(pre_version_file, flush=True)
        pre_version_file_str = open(pre_version_file).read()
        post_version_file_str = open(post_version_file).read()
        if pre_version_file_str.endswith(' '):
            pre_version_file_str=pre_version_file_str[:-1]
        if post_version_file_str.endswith(' '):
            post_version_file_str=post_version_file_str[:-1]
        (src, tgt) = get_token_pair_diff(pre_version_file, pre_version_file_str, post_version_file_str, num_tokens)
        if hash_to_cwe:
            search = re.search(r'/([0123456789abcdef]+)/',str(pre_version_file))
            if search and search.group(1) in hash_to_cwe:
                if hash_to_cwe[search.group(1)] != "IGNORE":
                    src_lines += hash_to_cwe[search.group(1)]+' '+src+'\n'
                    tgt_lines += tgt+'\n'
            else:
                search = re.search(r'/(CVE-[0123456789]+-[0123456789]+)/',str(pre_version_file))
                if search and search.group(1) in hash_to_cwe:
                    if hash_to_cwe[search.group(1)] != "IGNORE":
                        src_lines += hash_to_cwe[search.group(1)]+' '+src+'\n'
                        tgt_lines += tgt+'\n'
                else:
                    if not late:
                        src_lines += 'CWE-000 '+src+'\n'
                        tgt_lines += tgt+'\n'
        else:
            src_lines += src+'\n'
            tgt_lines += tgt+'\n'
    
    if late:
        src_path = root_path.parent / 'SrcTgt' / (root_path.stem + '_2019.src.txt')
        tgt_path = root_path.parent / 'SrcTgt' / (root_path.stem + '_2019.tgt.txt')
    else:
        src_path = root_path.parent / 'SrcTgt' / (root_path.stem + '.src.txt')
        tgt_path = root_path.parent / 'SrcTgt' / (root_path.stem + '.tgt.txt')

    src_path.parent.mkdir(parents=True, exist_ok=True)

    with codecs.open(src_path, 'w', 'utf-8') as f:
        f.write(src_lines)

    with codecs.open(tgt_path, 'w', 'utf-8') as f:
        f.write(tgt_lines)

if __name__=="__main__":
    main(sys.argv)

