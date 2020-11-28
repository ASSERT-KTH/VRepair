import getopt
import sys

def usage():
    print("Usage: python compare.py --src [src_file] --tgt [tgt_file] [-v]")
    print(" the -v option will print out all passing cases")
    print("Example: python compare.py --src pred-test.txt --tgt tgt-test.txt")

def main():
    verbose=False
    try:
        opts, args = getopt.getopt(sys.argv[1:], "hv", ["src=", "tgt=", "help"])
    except getopt.GetoptError as err:
        print(err)
        usage()
        sys.exit(2)
    for opt, arg in opts:
        if opt in ("--src"):
            src_file = arg
        elif opt in ("-v"):
            verbose = True
        elif opt in ("--tgt"):
            tgt_file = arg
        elif opt in ("-h", "--help"):
            usage()
            sys.exit()

    # Opens prediction file (src_file) and ground-truth file (tgt_file)
    with open(src_file, "r") as file:
        src_lines = file.readlines()
    with open(tgt_file, "r") as file:
        tgt_orig = file.readlines()
    if verbose:
        with open(tgt_file.replace("tgt","src"), "r") as file:
            bug_orig = file.readlines()

    # Michele format, convert to 1 line per prediction
    src_lines = [line for beam_line in src_lines for line in beam_line.split("<SEP>")]

    # Remove leading and trailing spaces
    src_lines = [line.strip() for line in src_lines]
    tgt_lines = [line.strip() for line in tgt_orig]

    # Check if we have multiple predictions
    assert(len(src_lines) % len(tgt_lines) == 0)

    beam_size = int(len(src_lines)/len(tgt_lines))

    # Count for correct prediction
    correct_count = 0

    for i in range(len(tgt_lines)):
        tgt_line = tgt_lines[i]

        # Remove all whitespaces
        tgt_line = ''.join(tgt_line.split())
        for j in range(beam_size):
            src_line = src_lines[i*beam_size+j]

            # Remove all whitespaces
            src_line = ''.join(src_line.split())
            if(src_line == tgt_line):
                correct_count += 1
                if verbose:
                    print((bug_orig[i]+"<BUG2FIX>"+tgt_orig[i]).replace('\n',''))
                break
            if(j == beam_size-1 and verbose):
                print((bug_orig[i]+"<BUG2FIX>"+tgt_orig[i]+"<FIRSTFAIL>"+src_lines[i*beam_size]).replace('\n',''))

    # Print the result
    print(str(correct_count) + " out of " + str(len(tgt_lines)) + " are correct")
    print(str(correct_count/len(tgt_lines)*100)+"%")



if __name__=="__main__":
    main()
