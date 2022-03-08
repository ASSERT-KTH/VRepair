# VRepair

This repository provides data and source files related to our paper on "Neural Transfer Learning for Repairing Security Vulnerabilities in C Code" (IEEE Transactions on Software Engineering, 2022), see <https://arxiv.org/pdf/2104.08308>

```bibtex
@article{vrepair,
 title = {Neural Transfer Learning for Repairing Security Vulnerabilities in C Code},
 journal = {IEEE Transactions on Software Engineering},
 year = {2022},
 doi = {10.1109/TSE.2019.2940179},
 author = {Zimin Chen and Steve Kommrusch and Martin Monperrus},
 url = {https://arxiv.org/pdf/2104.08308},
}


```


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

## Trained models

The trained models are currently uploaded to OneDrive:
* https://kth-my.sharepoint.com/:u:/g/personal/zimin_ug_kth_se/EV9uVzTRIF5HlMrRNHPHjfoBOivr63Is023EFzNe1Ax9zg?e=dEck8z
* https://kth-my.sharepoint.com/:u:/g/personal/zimin_ug_kth_se/EVy6M7UZ0rVPr3c28wxKH2cBYZPKGSPqb5_lqLOrduzvlQ?e=lgn8TV
* https://kth-my.sharepoint.com/:u:/g/personal/zimin_ug_kth_se/EVTHzJRcIW9Ak5PVn38RYX8BEWH5zyKlSlJh9KgKXU3SYw?e=WgSkHp
* https://kth-my.sharepoint.com/:u:/g/personal/zimin_ug_kth_se/ESU_mesDK3lPq2aF4kve2_4BwM41wyFarIOe9E8JpD2dLw?e=jpJeZl
* https://kth-my.sharepoint.com/:u:/g/personal/zimin_ug_kth_se/ESEhDRyeJ9pGtB7kEqjEfYUBPL19rR3g7ASYuUUqb5y-LQ?e=mu2umn
* https://kth-my.sharepoint.com/:u:/g/personal/zimin_ug_kth_se/Edn05UsQUmpDrGZNJK6GmuIBPdIFp4_llmFuU7RegWYhCw?e=u5Y35p

Once decompressed, the models are stored in different folder corresponding to different training strategies:
* `ablation_study_models`: Models trained for ablation study
* `all_buggy_lines_context3_models`: Models trained with all buggy lines identified
* `bigger_pre_train_context3_models`: Models trained with pre-training (token masking) and then target domain training
* `no_buggy_lines_context3_models`: Models trained with no buggy lines identified
* `one_block_context3_models`: Models trained where the buggy lines is in a contigoues block of lines
* `only_first_line_big_vul_random_fine_tune_context3_models`: Models trained only on Big-Vul^{rand}
* `only_first_line_big_vul_year_fine_tune_context3_models`: Models trained only on Big-Vul^{year}
* `only_first_line_context3_more_parameters_models`: The main apporach, source domain training + target domain training (ie. transfer learning)
* `only_first_line_context3_rnn_models`: Models trained with LSTM instead of transformer
* `only_first_line_cve_fixes_random_fine_tune_context3_models`: Models trained only on CVEfixes^{rand}
* `only_first_line_cve_fixes_year_fine_tune_context3_models`: Models trained only on CVEfixes^{year}


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

## CWE type descriptions for our extracted function pairs from both Big-Vul and CVEfixes datasets

| CWE ID  | Big-Vul | CVEfixes | Description                                                                                 |
|---------|---------|----------|---------------------------------------------------------------------------------------------|
| CWE-119 | 993     | 461      | Improper Restriction of Operations within the Bounds of a Memory Buffer                     |
| CWE-20  | 228     | 297      | Improper Input Validation                                                                   |
| CWE-125 | 224     | 333      | Out-of-bounds Read                                                                          |
| CWE-264 | 177     | 149      | Permissions, Privileges, and Access Controls                                                |
| CWE-200 | 150     | 147      | Exposure of Sensitive Information to an Unauthorized Actor                                  |
| CWE-399 | 143     | 109      | Resource Management Errors                                                                  |
| CWE-476 | 125     | 180      | NULL Pointer Dereference                                                                    |
| CWE-416 | 114     | 102      | Use After Free                                                                              |
| CWE-362 | 104     | 149      | Concurrent Execution using Shared Resource with Improper Synchronization ('Race Condition') |
| CWE-284 | 102     | 32       | Improper Access Control                                                                     |
| CWE-190 | 94      | 144      | Integer Overflow or Wraparound                                                              |
| CWE-189 | 92      | 78       | Numeric Errors                                                                              |
| CWE-787 | 54      | 136      | Out-of-bounds Write                                                                         |
| CWE-415 | 22      | 38       | Double Free                                                                                 |
| CWE-772 | 8       | 35       | Missing Release of Resource after Effective Lifetime                                        |

