#!/bin/bash

for sweep in {0..26};do
  echo parameter sweep $sweep
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/bigger_pre_train_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/bigger_pre_train_context3_models/${sweep}_parameter_sweep/train_config.yml)
  vs=$(grep 'src_vocab_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/bigger_pre_train_context3_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}, ${vs}"
  echo "Pre-train + fine tune"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/bigger_pre_train_context3_models/big_vul_random_test_data/big_vul_random_fine_tune_test.tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/bigger_pre_train_context3_models/${sweep}_parameter_sweep/big_vul_random_fine_tune/big_vul_random_fine_tune_test.src.txt.predictions
  echo
done
