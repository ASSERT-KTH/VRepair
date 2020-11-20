import sys
import itertools
import difflib
import codecs
import subprocess
from pathlib import Path
from multiprocessing import Pool
from unidiff import PatchSet

def main(argv):
    samples = set()
    for year in ("2017","2018"):
        for month in ("01","02","03","04","05","06","07","08","09","10","11","12"):
            print(f'y={year},m={month}')
            src_nodup=""
            tgt_nodup=""
            src_file = Path(f'BugFixTokenPairs_{year}_{month}.src.txt')
            tgt_file = Path(f'BugFixTokenPairs_{year}_{month}.tgt.txt')
            src_lines = open(src_file).read().split("\n")
            tgt_lines = open(tgt_file).read().split("\n")
            assert (len(src_lines) == len(tgt_lines))
            for (src,tgt) in list(zip(src_lines,tgt_lines)):
                if not (src,tgt) in samples and len(src) > 1 and len(tgt) > 1:
                    samples.add((src,tgt))
                    src_nodup+=src+'\n'
                    tgt_nodup+=tgt+'\n'

            src_path = Path(f'BugFixNoDup_{year}_{month}.src.txt')
            tgt_path = Path(f'BugFixNoDup_{year}_{month}.tgt.txt')
            with codecs.open(src_path, 'w', 'utf-8') as f:
                f.write(src_nodup)

            with codecs.open(tgt_path, 'w', 'utf-8') as f:
                f.write(tgt_nodup)

if __name__=="__main__":
    main(sys.argv)
