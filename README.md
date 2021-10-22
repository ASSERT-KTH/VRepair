# VRepair

This repository provides data and source files related to our paper on "Neuaral Transfer Learning for Repairing Security Vulnerabilities in C Code": https://arxiv.org/abs/2104.08308v1

In this paper, we address the problem of automatic repair of software vulnerabilities with deep learning. The major problem with data-driven vulnerability repair is that the few existing datasets of known confirmed vulnerabilities consist of only a few thousand examples. However, training a deep learning model often requires hundreds of thousands of examples. In this work, we leverage the intuition that the bug fixing task and the vulnerability fixing task are related, and the knowledge learned from bug fixes can be transferred to fixing vulnerabilities. In the machine learning community, this technique is called transfer learning. In this paper, we propose an approach for repairing security vulnerabilities named VRepair which is based on transfer learning. VRepair is first trained on a large bug fix corpus, and is then tuned on a vulnerability fix dataset, which is an order of magnitudes smaller. In our experiments, we show that a model trained only on a bug fix corpus can already fix some vulnerabilities. Then, we demonstrate that transfer learning improves the ability to repair vulnerable C functions. In the end, we present evidence that transfer learning produces more stable and superior neural models for vulnerability repair.

## SW versions

 * Python: 3.9.1
 * Clang: 13.0.0
 * GCC: 10.2.0
 * OpenNMT-py: 1.2.0

## Git file descriptions
 * data/Context3/OnlyFirstLine: Data used to train source domain task. Includes single and multiline bug fixes from GitHub with only the first line identified as buggy with special tokens.
 * data/PreSpecial*: 2 Directories with 2 denoising passes on each function used to provide denoising pretraining samples.
 * fine_tune_data: Target domain data of vulnerability fixes.
 * src: scripts used to process data and create training configuration files.

## CWE type discributions

------------------------------------------
| CWE ID | Big-Vul | CVEfixes | Description |
------------------------------------------
| CWE-119 | 187 | 102 | xxx |
| CWE-20 | 51 | 55 | xxx |
------------------------------------------
