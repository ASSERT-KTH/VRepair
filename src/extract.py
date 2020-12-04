import sys
import itertools
import difflib
import codecs
import subprocess
import clang.cindex
from pathlib import Path
from clang.cindex import CursorKind
from multiprocessing import Pool
from unidiff import PatchSet
import traceback

def tokenize_pre_and_post(tmpfile,path_to_diff):
    try:
        patch = PatchSet.from_filename(tmpfile, encoding='utf-8')

        pre_version_function = [line[1:] for line in patch[0][0].source]
        post_version_function = [line[1:] for line in patch[0][0].target]

        pre_version_function_str = ''.join(pre_version_function)
        post_version_function_str = ''.join(post_version_function)

        if ''.join(pre_version_function_str) == ''.join(post_version_function_str):
            return

        index = clang.cindex.Index.create()
        tu_pre = index.parse('tmp.c', unsaved_files=[('tmp.c', pre_version_function_str)])
        tu_post = index.parse('tmp.c', unsaved_files=[('tmp.c', post_version_function_str)])

        pre_version_function_path = path_to_diff.parent / 'pre_version' / (path_to_diff.stem + '.tokens')
        post_version_function_path = path_to_diff.parent / 'post_version' / (path_to_diff.stem + '.tokens')

        pre_version_function_path = Path(str(pre_version_function_path).replace('BugFixFunction', 'BugFixToken'))
        post_version_function_path = Path(str(post_version_function_path).replace('BugFixFunction', 'BugFixToken'))

        pre_version_function_path.parent.mkdir(parents=True, exist_ok=True)
        post_version_function_path.parent.mkdir(parents=True, exist_ok=True)

        pre_tokens = ""
        post_tokens = ""
        for token in tu_pre.cursor.get_tokens():
            pre_tokens+=repr(token.spelling.replace(' ', '<S2SV_blank>'))[1:-1] + ' '
        for token in tu_post.cursor.get_tokens():
            post_tokens+=repr(token.spelling.replace(' ', '<S2SV_blank>'))[1:-1] + ' '
        if pre_tokens == post_tokens:
            return

        with codecs.open(pre_version_function_path, 'w', 'utf-8') as f:
            f.write(pre_tokens)

        with codecs.open(post_version_function_path, 'w', 'utf-8') as f:
            f.write(post_tokens)
    except Exception as e:
        print("Tokenize error: " + str(e))
        print(traceback.format_exc())
        return

def removeComment(path_to_file,tmpfile):
    result = subprocess.run(["gcc", "-fpreprocessed", "-dD", "-E", "-P", str(path_to_file)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output_path = Path(tmpfile)
    try:
        no_comment_file_content = result.stdout.decode('utf-8')
        with codecs.open(output_path, 'w', 'utf-8') as f:
            f.write(no_comment_file_content)
            f.close()
    except Exception as e:
        print("Error removing comments: "+str(e))

def get_func_pair_diff(pre_version_file, post_version_file):
    try:
        index = clang.cindex.Index.create()

        removeComment(pre_version_file,"/tmp/S2SV_pre.c")
        removeComment(post_version_file,"/tmp/S2SV_post.c")

        pre_version_file_str = open("/tmp/S2SV_pre.c").read().splitlines(True)
        post_version_file_str = open("/tmp/S2SV_post.c").read().splitlines(True)

        pre_version_file_tu = index.parse("/tmp/S2SV_pre.c")
        post_version_file_tu = index.parse("/tmp/S2SV_post.c")

        pre_version_file_func_decl_cursor = []
        post_version_file_func_decl_cursor = []

        for cursor in pre_version_file_tu.cursor.walk_preorder():
            if cursor.kind == CursorKind.FUNCTION_DECL:
                pre_version_file_func_decl_cursor.append(cursor)
        for cursor in post_version_file_tu.cursor.walk_preorder():
            if cursor.kind == CursorKind.FUNCTION_DECL:
                post_version_file_func_decl_cursor.append(cursor)

        func_decl_cursor_key = lambda c: (c.spelling, c.location.line)

        pre_version_file_func_decl_cursor.sort(key=func_decl_cursor_key)
        post_version_file_func_decl_cursor.sort(key=func_decl_cursor_key)

        pre_index = 0
        post_index = 0
        pre_max_index = len(pre_version_file_func_decl_cursor)
        post_max_index = len(post_version_file_func_decl_cursor)

    except Exception as e:
        print(traceback.format_exc())
        return

    while(pre_index < pre_max_index):
        try:
            pre_func_decl_cursor = pre_version_file_func_decl_cursor[pre_index]
            pre_func_start_line_number = pre_func_decl_cursor.extent.start.line-1
            pre_func_end_line_number = pre_func_decl_cursor.extent.end.line
            for index in range(post_index, post_max_index):
                post_func_decl_cursor = post_version_file_func_decl_cursor[index]
                post_func_start_line_number = post_func_decl_cursor.extent.start.line-1
                post_func_end_line_number = post_func_decl_cursor.extent.end.line
                if(pre_func_decl_cursor.spelling == post_func_decl_cursor.spelling and
                   pre_version_file_lines[pre_func_end_line_number-1].strip()[-1] == '}' and
                   post_version_file_lines[post_func_end_line_number-1].strip()[-1] == '}'):
                    pre_func_decl_cursor_lines = pre_version_file_lines[pre_func_start_line_number:pre_func_end_line_number]
                    post_func_decl_cursor_lines = post_version_file_lines[post_func_start_line_number:post_func_end_line_number]
                    diff = list(difflib.unified_diff(pre_func_decl_cursor_lines, post_func_decl_cursor_lines, fromfile=pre_func_decl_cursor.spelling+'.function', tofile=pre_func_decl_cursor.spelling+'.function',  n=1000000))
                    if diff:
                        func_decl_diff_dir = Path(str(pre_version_file.parent.parent).replace('BugFix', 'BugFixFunction'))
                        func_decl_diff_file = func_decl_diff_dir / (pre_version_file.name + '__' + pre_func_decl_cursor.spelling + '__' + str(pre_func_decl_cursor.location.line) + '.diff')
                        with codecs.open("/tmp/S2SV_func.diff", 'w', 'utf-8') as f:
                            f.write(''.join(diff))
                        tokenize_pre_and_post("/tmp/S2SV_func.diff",func_decl_diff_file)
                    post_index = index + 1
                    break
            pre_index += 1
        except Exception as e:
            pre_index += 1
            print(traceback.format_exc())
            continue

def main(argv):
    bug_fix_pair_path = argv[1]
    root_path = Path(bug_fix_pair_path)
    pre_version_files = []
    post_version_files = []
    for day in root_path.iterdir():
        for commit_id in day.iterdir():
            pre_version = commit_id / 'pre_version'
            for pre_version_file in pre_version.glob('**/*.c'):
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
        get_func_pair_diff(pre_version_file, post_version_file)

if __name__=="__main__":
    main(sys.argv)
