#!/bin/bash

for sweep in {0..26};do
  echo parameter sweep $sweep
  echo "Source domain training"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  vs=$(grep 'src_vocab_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}, ${vs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_context3_more_parameters_models/big_vul_random_test_data/big_vul_random_fine_tune_test.tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/big_vul_random_fine_tune_test.src.txt.predictions
  echo "Target domain training"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  vs=$(grep 'src_vocab_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}, ${vs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_big_vul_random_fine_tune_context3_models/big_vul_random_test_data/big_vul_random_fine_tune_test.tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_11/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/big_vul_random_fine_tune_test.src.txt.predictions
  echo
done
