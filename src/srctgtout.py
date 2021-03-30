import sys
import itertools
import difflib
import codecs
import subprocess
import re
from pathlib import Path
from multiprocessing import Pool
from unidiff import PatchSet

def check_out(src_str, tgt_str, out_beam, num_tokens, sample):
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
    for out_str in out_beam:
        firstfail=[0]*32
        counts=[0]*32
        matches=0
        pass_modnum=0
        # Process out_str multiple times to find how many interpretations it has
        while matches < 1000:
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
                matches+=1
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
        if matches > 1000:
          matches = 1000; # Overflow flag
        if (out_str == tgt_str):
            print (f'Sample {sample} beam pos {beampos} PASS with {matches} matches')
            if (matches == 0):
                print (f'ERROR: found zero matches for passing {out_str} in {src_str}')
        else:
            print (f'Sample {sample} beam pos {beampos} FAIL with {matches} matches')
        beampos+=1

def main(argv):
    if (len(argv) != 5) :
        num_tokens=0  # Flag usage error
    else:
        src_file = argv[1]
        tgt_file = argv[2]
        out_file = argv[3]
        num_tokens = int(argv[4])
    if num_tokens < 2:
        print("Usage: python srctgtout.py src_file tgt_file out_file num_tokens")
        print("       num_tokens must be 2 or more")
        sys.exit(2)
    src_lines = open(src_file).read().split("\n")[:-1]
    tgt_lines = open(tgt_file).read().split("\n")[:-1]
    out_lines = open(out_file).read().split("\n")[:-1]
    assert(len(out_lines) % len(tgt_lines) == 0)
    beam_width = int(len(out_lines)/len(tgt_lines))

    for i in range(len(src_lines)):
        src=src_lines[i]
        tgt=tgt_lines[i]
        out=out_lines[i*beam_width:(i+1)*beam_width]
        check_out(src, tgt, out, num_tokens, i+1)

if __name__=="__main__":
    main(sys.argv)
