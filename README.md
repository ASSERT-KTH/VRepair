# VRepair

This repository provides data and source files related to our paper on "Neural Transfer Learning for Repairing Security Vulnerabilities in C Code", see <https://arxiv.org/abs/2104.08308>

In this paper, we address the problem of automatic repair of software vulnerabilities with deep learning. The major problem with data-driven vulnerability repair is that the few existing datasets of known confirmed vulnerabilities consist of only a few thousand examples. However, training a deep learning model often requires hundreds of thousands of examples. In this work, we leverage the intuition that the bug fixing task and the vulnerability fixing task are related, and the knowledge learned from bug fixes can be transferred to fixing vulnerabilities. In the machine learning community, this technique is called transfer learning. In this paper, we propose an approach for repairing security vulnerabilities named VRepair which is based on transfer learning. VRepair is first trained on a large bug fix corpus, and is then tuned on a vulnerability fix dataset, which is an order of magnitudes smaller. In our experiments, we show that a model trained only on a bug fix corpus can already fix some vulnerabilities. Then, we demonstrate that transfer learning improves the ability to repair vulnerable C functions. In the end, we present evidence that transfer learning produces more stable and superior neural models for vulnerability repair.

## Software versions

 * Python: 3.9.1
 * Clang: 13.0.0
 * GCC: 10.2.0
 * OpenNMT-py: 1.2.0

## Git file descriptions
 * data/Context3/OnlyFirstLine: Data used to train source domain task. Includes single and multiline bug fixes from GitHub with only the first line identified as buggy with special tokens.
 * data/PreSpecial*: 2 Directories with 2 denoising passes on each function used to provide denoising pretraining samples.
 * fine_tune_data: Target domain data of vulnerability fixes.
 * src: scripts used to process data and create training configuration files.

## Example data processing for big\_vul

```bash
# Set up environment as needed
source env.sh
cd fine_tune_data/big_vul

# Extract token data from C source files
python ../../src/extract.py commits > extract.out 2>&1

# Generate src/tgt raw files from tokenized data
python ../../src/gensrctgt.py commits 3 -meta commits_metadata.csv > gensrctgt.out 2>&1

# Create random split on data
python ../../src/process_fine_tune_data.py --src_file=SrcTgt/commits.src.txt --tgt_file=SrcTgt/commits.tgt.txt --meta_file=SrcTgt/commits.meta.txt --max_src_length=1000 --max_tgt_length=100 --generate_random --is_big_vul --output_dir=. > process.out 2>&1
```

## CWE type descriptions for top 10 CWE labels in both Big-Vul and CVEfixes test datasets

| CWE ID  | Big-Vul   | CVEfixes  | Description                                                                               |
| :------ | --------: | --------: | :---------------------------------------------------------------------------------------- |
| CWE-119 | 187       | 102       | Improper Restriction of Operations within the Bounds of a Memory Buffer                   |
| CWE-20  | 51        | 55        | Improper Input Validation                                                                 |
| CWE-125 | 45        | 55        | Out-of-bounds Read                                                                        |
| CWE-264 | 32        | 26        | Permissions, Priveledges, and Access Controls                                             |
| CWE-399 | 29        | 26        | Resource Management Errors                                                                |
| CWE-200 | 28        | 22        | Exposure of Sensitive Information to an Unauthorized Actor                                |
| CWE-476 | 26        | 39        | NULL Pointer Dereference                                                                  |
| CWE-284 | 26        |  7        | Improper Access Control                                                                   |
| CWE-189 | 26        | 11        | Numeric Errors                                                                            |
| CWE-362 | 26        | 30        | Concurrent Execution using Shared Resource with Improper Synchronization (Race Condition) |
| CWE-190 | 15        | 29        | Integer Overflow or Wraparound                                                            |
| CWE-787 |  8        | 24        | Out-of-bounds Write                                                                       |

